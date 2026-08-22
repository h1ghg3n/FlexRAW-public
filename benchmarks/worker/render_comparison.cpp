#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QImageReader>
#include <QString>
#include <QTcpSocket>
#include <QThread>

#include "frame_codec.h"
#include "frame_parser.h"
#include "local_render_executor.h"
#include "measurement_csv.h"
#include "render_payload_codec.h"
#include "resolved_render_pipeline.h"
#include "stage_timer.h"
#include "worker_application_context.h"
#include "worker_path_resolver.h"

namespace
{

constexpr int DefaultTimeoutMilliseconds = 300000;
constexpr quint16 DefaultWorkerPort = 47331;
constexpr std::uint32_t MaximumSampleCount = 1000;

enum class BenchmarkMode
{
    Direct,
    Loopback,
    Remote,
    Both,
};

struct BenchmarkConfiguration
{
    QString inputPath;
    QString outputDirectory;
    QString runId;
    QString target;
    QString remoteHost{QStringLiteral("127.0.0.1")};
    QString remoteSourceRelativePath;
    quint16 remotePort{DefaultWorkerPort};
    BenchmarkMode mode{BenchmarkMode::Both};
    std::uint32_t warmupCount{1};
    std::uint32_t sampleCount{5};
};

struct BenchmarkSample
{
    bool succeeded{false};
    QString errorMessage;
    QString outputPath;
    flexraw::core::measurement::RenderStats render;
    flexraw::core::measurement::SchedulerStats scheduler;
    std::uint64_t endToEndNanoseconds{0};
};

// 목적: benchmark CLI와 진단에 사용할 번역 가능한 문자열 생성
// 입력: sourceText: source language 문자열
// 출력: WorkerRenderBenchmark context로 번역된 QString
[[nodiscard]] QString trBenchmark(const char* const sourceText)
{
    return QCoreApplication::translate("WorkerRenderBenchmark", sourceText);
}

// 목적: QString을 CSV record와 stderr에 보존할 UTF-8 standard string으로 변환
// 입력: value: 변환할 Qt text
// 출력: UTF-8 byte sequence
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// 목적: benchmark option을 지정 상한 안의 unsigned count로 변환
// 입력: value/name: option 원문과 진단 이름, allowZero: 0 허용 여부
// 출력: 성공 여부와 parsed count
[[nodiscard]] bool parseCount(const QString& value,
                              const QString& name,
                              const bool allowZero,
                              std::uint32_t& parsedValue)
{
    bool parsed = false;
    const qulonglong count = value.toULongLong(&parsed);
    const qulonglong minimum = allowZero ? 0U : 1U;
    if (!parsed || count < minimum || count > MaximumSampleCount)
    {
        std::cerr << toUtf8(trBenchmark("Option %1 must be an integer from %2 through %3.")
                                .arg(name)
                                .arg(minimum)
                                .arg(MaximumSampleCount))
                  << '\n';
        return false;
    }
    parsedValue = static_cast<std::uint32_t>(count);
    return true;
}

// 목적: direct/loopback/both option을 benchmark 실행 mode로 변환
// 입력: value: --mode 원문
// 출력: 성공 여부와 parsed mode
[[nodiscard]] bool parseMode(const QString& value, BenchmarkMode& mode)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("direct"))
    {
        mode = BenchmarkMode::Direct;
        return true;
    }
    if (normalized == QStringLiteral("loopback"))
    {
        mode = BenchmarkMode::Loopback;
        return true;
    }
    if (normalized == QStringLiteral("remote"))
    {
        mode = BenchmarkMode::Remote;
        return true;
    }
    if (normalized == QStringLiteral("both"))
    {
        mode = BenchmarkMode::Both;
        return true;
    }
    std::cerr << toUtf8(trBenchmark("--mode must be direct, loopback, remote, or both.")) << '\n';
    return false;
}

// 목적: remote Worker TCP port option을 유효한 16-bit port로 변환
// 입력: value: --remote-port 원문, parsedPort: 성공 시 갱신할 port
// 출력: 1~65535 범위의 정수이면 true
[[nodiscard]] bool parsePort(const QString& value, quint16& parsedPort)
{
    bool parsed = false;
    const qulonglong port = value.toULongLong(&parsed);
    if (!parsed || port == 0 || port > std::numeric_limits<quint16>::max())
    {
        std::cerr << toUtf8(trBenchmark("--remote-port must be an integer from 1 through 65535.")) << '\n';
        return false;
    }
    parsedPort = static_cast<quint16>(port);
    return true;
}

// 목적: W3 Direct/Loopback/Remote 비교에 공통 적용할 대표 develop parameter 생성
// 입력: 없음
// 출력: Core validation 범위 안의 비기본 DevelopParams
[[nodiscard]] flexraw::core::types::DevelopParams makeRepresentativeParams()
{
    flexraw::core::types::DevelopParams params;
    params.exposureEv = 0.35F;
    params.contrast = 0.20F;
    params.highlights = -0.25F;
    params.shadows = 0.25F;
    params.whites = 0.10F;
    params.blacks = -0.10F;
    params.saturation = 0.10F;
    params.vibrance = 0.20F;
    params.whiteBalanceMode = flexraw::core::types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 6200.0F;
    params.whiteBalanceTint = 0.05F;
    params.clarity = 0.15F;
    params.dehaze = 0.10F;
    params.sharpeningAmount = 0.25F;
    params.sharpeningRadius = 1.2F;
    params.sharpeningDetail = 0.20F;
    params.sharpeningMasking = 0.10F;
    params.luminanceNoiseReduction = 0.15F;
    params.colorNoiseReduction = 0.15F;
    params.toneCurveShadows = -0.05F;
    params.toneCurveDarks = -0.05F;
    params.toneCurveLights = 0.05F;
    params.toneCurveHighlights = 0.05F;
    return params;
}

// 목적: 같은 input semantics를 사용하는 resolved render request 생성
// 입력: sourcePath/outputPath: machine-local 경로
// 출력: 대표 DevelopParams와 JPEG option이 적용된 request
[[nodiscard]] flexraw::core::render::ResolvedRenderRequest makeResolvedRequest(const QString& sourcePath,
                                                                               const QString& outputPath)
{
    flexraw::core::render::ResolvedRenderRequest request;
    request.sourcePath = sourcePath;
    request.outputPath = outputPath;
    request.developParams = makeRepresentativeParams();
    request.outputOptions.includeMetadata = false;
    return request;
}

// 목적: warm-up과 measured output이 충돌하지 않는 root-relative JPEG 이름 생성
// 입력: modeLabel/sampleIndex/warmup: 실행 mode, 1-based 순번과 warm-up 여부
// 출력: Worker root와 local output directory에서 공통 사용 가능한 파일 이름
[[nodiscard]] QString makeOutputFileName(const QString& modeLabel, const std::uint32_t sampleIndex, const bool warmup)
{
    return QStringLiteral("%1-%2-%3.jpg")
        .arg(modeLabel, warmup ? QStringLiteral("warmup") : QStringLiteral("sample"))
        .arg(sampleIndex, 4, 10, QLatin1Char('0'));
}

// 목적: 선택한 render mode의 sample 한 건을 versioned CSV row로 출력
// 입력: configuration/modeLabel/sampleIndex/sample: 공통 context와 측정 결과
// 출력: stdout에 CSV row 추가
void writeSampleRecord(const BenchmarkConfiguration& configuration,
                       const QString& modeLabel,
                       const std::uint32_t sampleIndex,
                       const BenchmarkSample& sample)
{
    flexraw::core::measurement::RenderMeasurementRecord record;
    record.runId = toUtf8(configuration.runId);
    record.mode = toUtf8(modeLabel);
    record.target = toUtf8(configuration.target);
    record.sourceLabel = toUtf8(QFileInfo(configuration.inputPath).fileName());
    record.concurrency = 1;
    record.sampleIndex = sampleIndex;
    record.succeeded = sample.succeeded;
    record.scheduler = sample.scheduler;
    record.render = sample.render;
    record.endToEndNanoseconds = sample.endToEndNanoseconds;
    if (sample.succeeded)
    {
        QImageReader reader(sample.outputPath);
        const QSize outputSize = reader.size();
        record.width = static_cast<std::uint32_t>(std::max(outputSize.width(), 0));
        record.height = static_cast<std::uint32_t>(std::max(outputSize.height(), 0));
    }
    flexraw::core::measurement::writeRenderMeasurementCsvRow(std::cout, record);
    std::cout.flush();
}

// 목적: 긴 RAW benchmark의 현재 sample 상태를 CSV와 분리된 stderr에 즉시 출력
// 입력: modeLabel/phase/sampleIndex/state/sample: 실행 위치와 선택적 완료 timing
// 출력: stderr에 한 줄을 기록하고 flush
void writeProgress(const QString& modeLabel,
                   const QString& phase,
                   const std::uint32_t sampleIndex,
                   const QString& state,
                   const BenchmarkSample* const sample = nullptr)
{
    std::cerr << "progress,mode=" << toUtf8(modeLabel) << ",phase=" << toUtf8(phase) << ",sample=" << sampleIndex
              << ",state=" << toUtf8(state);
    if (sample != nullptr)
    {
        std::cerr << ",end_to_end_ns=" << sample->endToEndNanoseconds;
    }
    std::cerr << '\n' << std::flush;
}

class RenderProtocolClient final
{
public:
    // 목적: benchmark client를 지정 Worker endpoint에 연결
    // 입력: host/port: Worker TCP host와 port
    // 출력: timeout 전에 연결되면 true
    [[nodiscard]] bool connectToServer(const QString& host, const quint16 port)
    {
        m_socket.connectToHost(host, port);
        static_cast<void>(waitUntil([this]() {
            return m_socket.state() == QAbstractSocket::ConnectedState ||
                   (m_socket.state() == QAbstractSocket::UnconnectedState &&
                    m_socket.error() != QAbstractSocket::UnknownSocketError);
        }));
        return m_socket.state() == QAbstractSocket::ConnectedState;
    }

    // 목적: render request를 전송하고 accepted부터 terminal frame까지 client-observed latency 측정
    // 입력: payload/jobId/outputPath: wire request, session-local identity와 artifact 절대 경로
    // 출력: pipeline stats와 end-to-end timing을 포함한 sample
    [[nodiscard]] BenchmarkSample execute(const flexraw::worker::runtime::RenderRequestPayload& payload,
                                          const flexraw::worker::protocol::JobId jobId,
                                          const QString& outputPath)
    {
        BenchmarkSample sample;
        sample.outputPath = outputPath;
        m_frames.clear();
        m_errorMessage.clear();

        const flexraw::worker::runtime::EncodePayloadResult encodedPayload =
            flexraw::worker::runtime::encodeRenderRequestPayload(payload);
        if (encodedPayload.hasError())
        {
            sample.errorMessage = encodedPayload.error().message;
            return sample;
        }
        const flexraw::worker::protocol::EncodeFrameResult encodedFrame = flexraw::worker::protocol::encodeFrame(
            {flexraw::worker::protocol::MessageType::RenderRequest, jobId, encodedPayload.value()});
        if (encodedFrame.hasError())
        {
            sample.errorMessage = encodedFrame.error().message;
            return sample;
        }

        const flexraw::core::measurement::StageTimer endToEndTimer;
        if (m_socket.write(encodedFrame.value()) != encodedFrame.value().size())
        {
            sample.errorMessage = m_socket.errorString();
            sample.endToEndNanoseconds = endToEndTimer.elapsedNanoseconds();
            return sample;
        }
        const bool completed = waitUntil([this, jobId]() {
            readAvailable();
            return !m_errorMessage.isEmpty() || hasTerminalFrame(jobId);
        });
        sample.endToEndNanoseconds = endToEndTimer.elapsedNanoseconds();
        if (!completed)
        {
            sample.errorMessage = trBenchmark("Timed out waiting for the Worker render result.");
            return sample;
        }
        if (!m_errorMessage.isEmpty())
        {
            sample.errorMessage = m_errorMessage;
            return sample;
        }
        return decodeOutcome(jobId, std::move(sample));
    }

private:
    // 목적: Qt event를 처리하며 socket predicate를 bounded wait
    // 입력: predicate: 완료 조건
    // 출력: timeout 전에 조건이 충족되면 true
    template<typename Predicate> [[nodiscard]] bool waitUntil(Predicate&& predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < DefaultTimeoutMilliseconds)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        return predicate();
    }

    // 목적: socket receive bytes를 incremental parser에 공급
    // 입력: 없음
    // 출력: 완성 frame 누적 또는 terminal protocol error 기록
    void readAvailable()
    {
        const QByteArray bytes = m_socket.readAll();
        if (bytes.isEmpty())
        {
            if (m_socket.state() == QAbstractSocket::UnconnectedState &&
                m_socket.error() != QAbstractSocket::UnknownSocketError)
            {
                m_errorMessage = m_socket.errorString();
            }
            return;
        }
        flexraw::worker::protocol::ParseFramesOutcome outcome = m_parser.append(bytes);
        m_frames.insert(m_frames.end(),
                        std::make_move_iterator(outcome.frames.begin()),
                        std::make_move_iterator(outcome.frames.end()));
        if (outcome.terminalError.has_value())
        {
            m_errorMessage = outcome.terminalError->message;
        }
    }

    // 목적: 지정 JobId의 terminal response 수신 여부 확인
    // 입력: jobId: 현재 benchmark request identity
    // 출력: success/failure/busy frame이 있으면 true
    [[nodiscard]] bool hasTerminalFrame(const flexraw::worker::protocol::JobId jobId) const
    {
        return std::any_of(m_frames.cbegin(), m_frames.cend(), [jobId](const auto& frame) {
            if (frame.jobId != jobId)
            {
                return false;
            }
            return frame.messageType == flexraw::worker::protocol::MessageType::RenderSucceeded ||
                   frame.messageType == flexraw::worker::protocol::MessageType::RenderFailed ||
                   frame.messageType == flexraw::worker::protocol::MessageType::ServerBusy ||
                   frame.messageType == flexraw::worker::protocol::MessageType::ResourceBusy;
        });
    }

    // 목적: accepted와 terminal response를 BenchmarkSample로 변환
    // 입력: jobId/sample: 기대 identity와 이미 측정된 output/timing
    // 출력: success/failure stats와 진단을 반영한 sample
    [[nodiscard]] BenchmarkSample decodeOutcome(const flexraw::worker::protocol::JobId jobId,
                                                BenchmarkSample sample) const
    {
        bool accepted = false;
        for (const flexraw::worker::protocol::ProtocolFrame& frame : m_frames)
        {
            if (frame.jobId != jobId)
            {
                sample.errorMessage = trBenchmark("Worker response JobId did not match the request.");
                return sample;
            }
            if (frame.messageType == flexraw::worker::protocol::MessageType::JobAccepted)
            {
                accepted = true;
                continue;
            }
            if (frame.messageType == flexraw::worker::protocol::MessageType::RenderSucceeded)
            {
                const auto decoded = flexraw::worker::runtime::decodeRenderSucceededPayload(frame.payload);
                if (decoded.hasError())
                {
                    sample.errorMessage = decoded.error().message;
                    return sample;
                }
                sample.succeeded = accepted;
                sample.render = decoded.value().stats;
                if (!accepted)
                {
                    sample.errorMessage = trBenchmark("Worker success arrived without JobAccepted.");
                }
                return sample;
            }
            if (frame.messageType == flexraw::worker::protocol::MessageType::RenderFailed)
            {
                const auto decoded = flexraw::worker::runtime::decodeRenderFailedPayload(frame.payload);
                if (decoded.hasError())
                {
                    sample.errorMessage = decoded.error().message;
                    return sample;
                }
                sample.render = decoded.value().stats;
                sample.errorMessage = decoded.value().cause.message;
                return sample;
            }
            if (frame.messageType == flexraw::worker::protocol::MessageType::ServerBusy)
            {
                const auto decoded = flexraw::worker::runtime::decodeServerBusyPayload(frame.payload);
                sample.errorMessage = decoded.hasValue() ? decoded.value().message : decoded.error().message;
                return sample;
            }
            if (frame.messageType == flexraw::worker::protocol::MessageType::ResourceBusy)
            {
                const auto decoded = flexraw::worker::runtime::decodeResourceBusyPayload(frame.payload);
                sample.errorMessage = decoded.hasValue() ? decoded.value().message : decoded.error().message;
                return sample;
            }
        }
        sample.errorMessage = trBenchmark("Worker response did not contain a terminal frame.");
        return sample;
    }

    QTcpSocket m_socket;
    flexraw::worker::protocol::FrameParser m_parser;
    std::vector<flexraw::worker::protocol::ProtocolFrame> m_frames;
    QString m_errorMessage;
};

// 목적: 공통 warm-up/measured 반복 정책으로 benchmark mode 실행
// 입력: configuration/modeLabel/executeSample: 실행 context와 sample callback
// 출력: 모든 warm-up과 measured sample이 성공하면 true
template<typename ExecuteSample>
[[nodiscard]] bool runSeries(const BenchmarkConfiguration& configuration,
                             const QString& modeLabel,
                             ExecuteSample&& executeSample)
{
    for (std::uint32_t index = 1; index <= configuration.warmupCount; ++index)
    {
        writeProgress(modeLabel, QStringLiteral("warmup"), index, QStringLiteral("started"));
        const BenchmarkSample sample = executeSample(index, true);
        writeProgress(modeLabel, QStringLiteral("warmup"), index, QStringLiteral("completed"), &sample);
        if (!sample.succeeded)
        {
            std::cerr << toUtf8(
                             trBenchmark("%1 warm-up %2 failed: %3").arg(modeLabel).arg(index).arg(sample.errorMessage))
                      << '\n';
            return false;
        }
    }
    for (std::uint32_t index = 1; index <= configuration.sampleCount; ++index)
    {
        writeProgress(modeLabel, QStringLiteral("measured"), index, QStringLiteral("started"));
        const BenchmarkSample sample = executeSample(index, false);
        writeProgress(modeLabel, QStringLiteral("measured"), index, QStringLiteral("completed"), &sample);
        writeSampleRecord(configuration, modeLabel, index, sample);
        if (!sample.succeeded)
        {
            std::cerr << toUtf8(
                             trBenchmark("%1 sample %2 failed: %3").arg(modeLabel).arg(index).arg(sample.errorMessage))
                      << '\n';
            return false;
        }
    }
    return true;
}

// 목적: caller thread의 LocalRenderExecutor로 Direct series 실행
// 입력: configuration: input/output와 반복 횟수
// 출력: 모든 sample 성공 여부
[[nodiscard]] bool runDirectSeries(const BenchmarkConfiguration& configuration)
{
    const flexraw::core::render::ResolvedRenderPipeline pipeline;
    const flexraw::core::render::LocalRenderExecutor executor(pipeline);
    const flexraw::core::types::CancellationSource cancellation;
    return runSeries(configuration, QStringLiteral("direct-local"), [&](const std::uint32_t index, const bool warmup) {
        BenchmarkSample sample;
        sample.outputPath =
            QDir(configuration.outputDirectory).filePath(makeOutputFileName(QStringLiteral("direct"), index, warmup));
        const flexraw::core::render::ResolvedRenderRequest request =
            makeResolvedRequest(configuration.inputPath, sample.outputPath);
        const flexraw::core::measurement::StageTimer endToEndTimer;
        const flexraw::core::render::ResolvedRenderPipelineResult result =
            executor.execute(request, cancellation.token());
        sample.endToEndNanoseconds = endToEndTimer.elapsedNanoseconds();
        sample.succeeded = result.hasValue();
        sample.render = result.hasValue() ? result.value().stats : result.error().stats;
        if (result.hasError())
        {
            sample.errorMessage = result.error().cause.message;
        }
        return sample;
    });
}

// 목적: in-process localhost Worker를 통해 Loopback series 실행
// 입력: configuration: input/output와 반복 횟수
// 출력: server/client setup과 모든 sample 성공 여부
[[nodiscard]] bool runLoopbackSeries(const BenchmarkConfiguration& configuration)
{
    const QFileInfo inputInfo(configuration.inputPath);
    flexraw::worker::runtime::WorkerPathResolver::CreateResult resolver =
        flexraw::worker::runtime::WorkerPathResolver::create({inputInfo.absolutePath(), configuration.outputDirectory});
    if (resolver.hasError())
    {
        std::cerr << toUtf8(resolver.error().message) << '\n';
        return false;
    }

    flexraw::worker::app::WorkerApplicationConfiguration workerConfiguration;
    workerConfiguration.maximumConcurrency = 1;
    workerConfiguration.queueCapacity = 1;
    workerConfiguration.server.session.inactivityTimeoutMilliseconds = DefaultTimeoutMilliseconds;
    flexraw::worker::app::WorkerApplicationContext context(std::move(resolver.value()), workerConfiguration);
    if (!context.listen(QHostAddress::LocalHost, 0))
    {
        std::cerr << toUtf8(context.errorString()) << '\n';
        return false;
    }

    RenderProtocolClient client;
    if (!client.connectToServer(QStringLiteral("127.0.0.1"), context.serverPort()))
    {
        std::cerr << toUtf8(trBenchmark("Unable to connect to the in-process loopback Worker.")) << '\n';
        return false;
    }

    flexraw::worker::protocol::JobId nextJobId = 1;
    const bool succeeded =
        runSeries(configuration, QStringLiteral("loopback-worker"), [&](const std::uint32_t index, const bool warmup) {
            const QString outputFileName = makeOutputFileName(QStringLiteral("loopback"), index, warmup);
            flexraw::worker::runtime::RenderRequestPayload payload;
            payload.sourceRelativePath = QDir::fromNativeSeparators(inputInfo.fileName());
            payload.outputRelativePath = outputFileName;
            payload.developParams = makeRepresentativeParams();
            payload.outputOptions.includeMetadata = false;
            BenchmarkSample sample =
                client.execute(payload, nextJobId++, QDir(configuration.outputDirectory).filePath(outputFileName));
            sample.scheduler = context.runtimeSnapshot().highWater;
            return sample;
        });
    context.shutdown();
    return succeeded;
}

// 목적: 별도 process의 Worker endpoint를 통해 pre-staged Remote series 실행
// 입력: configuration: local fixture view, remote endpoint/path와 반복 횟수
// 출력: client 연결과 모든 sample 성공 여부
[[nodiscard]] bool runRemoteSeries(const BenchmarkConfiguration& configuration)
{
    RenderProtocolClient client;
    if (!client.connectToServer(configuration.remoteHost, configuration.remotePort))
    {
        std::cerr << toUtf8(trBenchmark("Unable to connect to the external Worker at %1:%2.")
                                .arg(configuration.remoteHost)
                                .arg(configuration.remotePort))
                  << '\n';
        return false;
    }

    flexraw::worker::protocol::JobId nextJobId = 1;
    return runSeries(configuration, QStringLiteral("remote-worker"), [&](const std::uint32_t index, const bool warmup) {
        const QString outputFileName = makeOutputFileName(QStringLiteral("remote"), index, warmup);
        flexraw::worker::runtime::RenderRequestPayload payload;
        payload.sourceRelativePath = configuration.remoteSourceRelativePath;
        payload.outputRelativePath = outputFileName;
        payload.developParams = makeRepresentativeParams();
        payload.outputOptions.includeMetadata = false;
        return client.execute(payload, nextJobId++, QDir(configuration.outputDirectory).filePath(outputFileName));
    });
}

// 목적: benchmark command line을 검증된 configuration으로 변환
// 입력: application: process arguments를 소유한 Qt application, configuration: 출력 위치
// 출력: 실행 가능한 option이면 true
[[nodiscard]] bool parseConfiguration(QCoreApplication& application, BenchmarkConfiguration& configuration)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        trBenchmark("Compares direct, localhost, and external Worker RAW render latency."));
    parser.addHelpOption();
    parser.addOption(
        {{QStringLiteral("i"), QStringLiteral("input")}, trBenchmark("Camera RAW source path."), trBenchmark("path")});
    parser.addOption({{QStringLiteral("o"), QStringLiteral("output-dir")},
                      trBenchmark("Existing output directory."),
                      trBenchmark("path")});
    parser.addOption({QStringLiteral("mode"),
                      trBenchmark("direct, loopback, remote, or both."),
                      trBenchmark("name"),
                      QStringLiteral("both")});
    parser.addOption({QStringLiteral("warmup"),
                      trBenchmark("Warm-up renders excluded from CSV."),
                      trBenchmark("count"),
                      QStringLiteral("1")});
    parser.addOption({QStringLiteral("samples"),
                      trBenchmark("Measured renders per selected mode."),
                      trBenchmark("count"),
                      QStringLiteral("5")});
    parser.addOption({QStringLiteral("run-id"),
                      trBenchmark("Stable label shared by emitted CSV rows."),
                      trBenchmark("id"),
                      QStringLiteral("manual-1")});
    parser.addOption({QStringLiteral("target"),
                      trBenchmark("Machine and architecture label."),
                      trBenchmark("name"),
                      QStringLiteral("windows-x64")});
    parser.addOption({QStringLiteral("remote-host"),
                      trBenchmark("External Worker host used by remote mode."),
                      trBenchmark("host"),
                      QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("remote-port"),
                      trBenchmark("External Worker TCP port used by remote mode."),
                      trBenchmark("port"),
                      QString::number(DefaultWorkerPort)});
    parser.addOption({QStringLiteral("remote-source-relative-path"),
                      trBenchmark("Worker source-root-relative path; defaults to the input file name."),
                      trBenchmark("path")});
    parser.process(application);

    const QString inputValue = parser.value(QStringLiteral("input")).trimmed();
    const QString outputValue = parser.value(QStringLiteral("output-dir")).trimmed();
    if (inputValue.isEmpty() || outputValue.isEmpty())
    {
        std::cerr << toUtf8(trBenchmark("Both --input and --output-dir are required.")) << '\n';
        return false;
    }
    configuration.inputPath = QFileInfo(inputValue).absoluteFilePath();
    configuration.outputDirectory = QDir(outputValue).absolutePath();
    configuration.runId = parser.value(QStringLiteral("run-id"));
    configuration.target = parser.value(QStringLiteral("target"));
    configuration.remoteHost = parser.value(QStringLiteral("remote-host")).trimmed();
    configuration.remoteSourceRelativePath = parser.value(QStringLiteral("remote-source-relative-path")).trimmed();
    if (configuration.remoteSourceRelativePath.isEmpty())
    {
        configuration.remoteSourceRelativePath = QFileInfo(configuration.inputPath).fileName();
    }
    if (!QFileInfo(configuration.inputPath).isFile())
    {
        std::cerr << toUtf8(trBenchmark("--input must name an existing camera RAW file.")) << '\n';
        return false;
    }
    if (!QFileInfo(configuration.outputDirectory).isDir())
    {
        std::cerr << toUtf8(trBenchmark("--output-dir must name an existing directory.")) << '\n';
        return false;
    }
    if (configuration.remoteHost.isEmpty())
    {
        std::cerr << toUtf8(trBenchmark("--remote-host must not be empty.")) << '\n';
        return false;
    }
    return parseMode(parser.value(QStringLiteral("mode")), configuration.mode) &&
           parsePort(parser.value(QStringLiteral("remote-port")), configuration.remotePort) &&
           parseCount(
               parser.value(QStringLiteral("warmup")), QStringLiteral("--warmup"), true, configuration.warmupCount) &&
           parseCount(
               parser.value(QStringLiteral("samples")), QStringLiteral("--samples"), false, configuration.sampleCount);
}

// 목적: 선택된 render mode를 동일 configuration으로 순차 실행
// 입력: configuration: 검증된 benchmark option
// 출력: 모든 선택 mode가 성공하면 process code 0
[[nodiscard]] int runBenchmark(const BenchmarkConfiguration& configuration)
{
    flexraw::core::measurement::writeRenderMeasurementCsvHeader(std::cout);
    std::cout.flush();
    if ((configuration.mode == BenchmarkMode::Direct || configuration.mode == BenchmarkMode::Both) &&
        !runDirectSeries(configuration))
    {
        return 3;
    }
    if ((configuration.mode == BenchmarkMode::Loopback || configuration.mode == BenchmarkMode::Both) &&
        !runLoopbackSeries(configuration))
    {
        return 4;
    }
    if (configuration.mode == BenchmarkMode::Remote && !runRemoteSeries(configuration))
    {
        return 5;
    }
    return 0;
}

}  // namespace

// 목적: W3 Direct/Loopback/Remote benchmark용 Qt runtime과 CLI 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: 성공 0, configuration 2, Direct 3, Loopback 4, Remote 5
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Worker Render Benchmark"));

    BenchmarkConfiguration configuration;
    if (!parseConfiguration(application, configuration))
    {
        return 2;
    }
    return runBenchmark(configuration);
}
