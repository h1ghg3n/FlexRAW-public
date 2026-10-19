#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QUuid>

#include "current_path_identity_service.h"
#include "current_process_memory_probe.h"
#include "current_system_memory_probe.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "measurement_csv.h"
#include "remote_export_execution_adapter.h"
#include "remote_render_executor.h"
#include "resolved_render_pipeline.h"
#include "shared_storage_locator.h"
#include "stage_timer.h"
#include "supported_extensions.h"

namespace
{

constexpr std::uint64_t Mebibyte = 1024ULL * 1024ULL;
constexpr int DefaultTimeoutMilliseconds = 60 * 60 * 1000;

struct PlacementBenchmarkConfiguration
{
    QString inputDirectory;
    QString outputDirectory;
    QString runId{QStringLiteral("export-auto-manual-1")};
    QString target{QStringLiteral("windows-x64+remote")};
    QString remoteHost{QStringLiteral("127.0.0.1")};
    std::uint16_t remotePort{47331};
    flexraw::core::orchestration::ExportPlacementPolicy policy{
        flexraw::core::orchestration::ExportPlacementPolicy::Auto};
    int localConcurrency{4};
    int remoteConcurrency{2};
    int warmupCount{0};
    bool instrumentedLocal{false};
    bool traceLocalAdmission{false};
};

struct TimedLocalAdmission
{
    std::uint64_t elapsedNanoseconds{0};
    flexraw::core::orchestration::ExportLocalAdmissionObservation decision;
};

class LocalAdmissionTrace final
{
public:
    // 목적: warm-up 이후 Local admission 결정의 시간축만 보존
    // 입력: 없음
    // 출력: 이전 sample이 없는 새 측정 구간
    void startMeasurement()
    {
        m_samples.clear();
        m_epoch = std::chrono::steady_clock::now();
        m_measuring = true;
    }

    // 목적: 추가 memory probe 없이 scheduler가 이미 계산한 결정을 저장
    // 입력: decision: 당시의 available memory, cap 및 실제 Local in-flight
    // 출력: benchmark 전용 in-memory sample
    void observe(const flexraw::core::orchestration::ExportLocalAdmissionObservation& decision)
    {
        if (m_measuring)
        {
            const auto elapsed = std::chrono::steady_clock::now() - m_epoch;
            m_samples.push_back(
                {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                 decision});
        }
    }

    // 목적: 측정 종료 후에만 admission 결정 log를 stderr로 출력
    // 입력: 없음
    // 출력: 별도 log로 보존 가능한 구조화된 sample line
    void writeLog() const
    {
        for (const TimedLocalAdmission& sample : m_samples)
        {
            const auto& decision = sample.decision;
            std::cerr << "local_admission,time_ns=" << sample.elapsedNanoseconds << ",available_bytes=";
            if (decision.availableMemoryBytes.has_value())
            {
                std::cerr << *decision.availableMemoryBytes;
            }
            else
            {
                std::cerr << "unknown";
            }
            std::cerr << ",configured_slots=" << decision.configuredSlots << ",cpu_slots=" << decision.cpuSlots
                      << ",memory_slots=" << decision.memorySlots << ",effective_slots=" << decision.effectiveSlots
                      << ",local_in_flight=" << decision.localInFlight << '\n';
        }
    }

private:
    std::vector<TimedLocalAdmission> m_samples;
    std::chrono::steady_clock::time_point m_epoch{};
    bool m_measuring{false};
};

struct LocalExecutionSample
{
    std::uint64_t startedNanoseconds{0};
    std::uint64_t finishedNanoseconds{0};
    flexraw::core::measurement::RenderStats render;
};

class MeasuredLocalExportPipeline final : public flexraw::core::orchestration::IExportPipeline
{
public:
    // 목적: 기존 Export 준비/검증과 단계 계측용 RAW renderer를 함께 조립
    // 입력: pathIdentityService: benchmark보다 오래 살아야 하는 OS path dependency
    // 출력: LocalOnly 전용 계측 pipeline
    explicit MeasuredLocalExportPipeline(const flexraw::platform::IPathIdentityService& pathIdentityService)
        : m_delegate(pathIdentityService)
    {}

    // 목적: warm-up 이후의 실제 Local 실행만 계측하도록 상태 초기화
    // 입력: 없음
    // 출력: job timing과 in-flight high-water의 새 측정 구간
    void startMeasurement()
    {
        QMutexLocker lock(&m_mutex);
        m_samples.clear();
        m_running = 0;
        m_runningHighWater = 0;
        m_epoch = std::chrono::steady_clock::now();
        m_measuring = true;
    }

    // 목적: 준비된 item의 기존 source/output validation과 develop state 해석 보존
    // 입력: request: Export 요청, cancellationToken: 중단 상태
    // 출력: FileExportPipeline의 원래 준비 결과
    [[nodiscard]] flexraw::core::orchestration::ExportPreparationResult prepare(
        const flexraw::core::orchestration::ExportRequest& request,
        const flexraw::core::types::CancellationToken& cancellationToken) const override
    {
        return m_delegate.prepare(request, cancellationToken);
    }

    // 목적: 같은 Local slot에서 stage timer가 있는 RAW renderer를 실행하고 실제 실행 구간 관측
    // 입력: item: 준비된 source/develop/output, cancellationToken: 중단 상태
    // 출력: RenderStats를 별도 보관한 item 성공 또는 실패
    [[nodiscard]] flexraw::core::orchestration::ExportItemResult executeItem(
        const flexraw::core::orchestration::PreparedExportItem& item,
        const flexraw::core::types::CancellationToken& cancellationToken) const override
    {
        if (item.preparationError.has_value() ||
            item.request.source.kind != flexraw::core::types::SupportedFileKind::Raw)
        {
            return m_delegate.executeItem(item, cancellationToken);
        }

        ActiveJob sample(*this, item.request.source.path);
        flexraw::core::render::ResolvedRenderRequest request{
            item.request.source.path,
            item.request.outputPath,
            item.request.developParams.value_or(flexraw::core::types::DevelopParams{}),
            item.request.options};
        const flexraw::core::render::ResolvedRenderPipelineResult result =
            m_renderer.execute(request, cancellationToken);
        if (result.hasError())
        {
            sample.render = result.error().stats;
            return {item.request.source.path,
                    item.request.outputPath,
                    false,
                    result.error().cause,
                    flexraw::core::orchestration::ExportItemFailureKind::Execution};
        }
        sample.render = result.value().stats;
        return {item.request.source.path, result.value().artifact.outputPath, true, {}};
    }

    // 목적: benchmark에서 사용하지 않는 synchronous legacy call을 기존 구현에 위임
    // 입력: request/cancellationToken/progress: legacy Export 인자
    // 출력: FileExportPipeline의 원래 request-level 결과
    [[nodiscard]] flexraw::core::orchestration::ExportPipelineResult execute(
        const flexraw::core::orchestration::ExportRequest& request,
        const flexraw::core::types::CancellationToken& cancellationToken,
        const flexraw::core::orchestration::ExportProgressCallback& progress) const override
    {
        return m_delegate.execute(request, cancellationToken, progress);
    }

    // 목적: 완료된 RAW job의 stage 및 실행 시작·종료 측정값 조회
    // 입력: sourcePath: prepared source identity
    // 출력: 해당 job이 실제 실행됐으면 sample
    [[nodiscard]] std::optional<LocalExecutionSample> sample(const QString& sourcePath) const
    {
        QMutexLocker lock(&m_mutex);
        const auto found = m_samples.constFind(QDir::cleanPath(sourcePath));
        return found == m_samples.cend() ? std::nullopt : std::optional<LocalExecutionSample>(*found);
    }

    // 목적: request 기간에 실제 renderer에 진입한 동시 job 최고치 조회
    // 입력: 없음
    // 출력: measured Local 실행 high-water
    [[nodiscard]] int runningHighWater() const
    {
        QMutexLocker lock(&m_mutex);
        return m_runningHighWater;
    }

private:
    class ActiveJob
    {
    public:
        // 목적: worker가 renderer를 실제 실행하기 시작한 시각과 동시 개수 기록
        // 입력: owner: 측정 owner, sourcePath: item identity
        // 출력: 종료 시점을 보장하는 scope-bound recorder
        ActiveJob(const MeasuredLocalExportPipeline& owner, QString sourcePath)
            : m_owner(owner), m_sourcePath(QDir::cleanPath(sourcePath))
        {
            QMutexLocker lock(&m_owner.m_mutex);
            if (m_owner.m_measuring)
            {
                m_startedNanoseconds = m_owner.elapsedNanoseconds();
                ++m_owner.m_running;
                m_owner.m_runningHighWater = std::max(m_owner.m_runningHighWater, m_owner.m_running);
            }
        }

        // 목적: 실패 또는 예외 시에도 active count를 회수하고 관측값 보존
        // 입력: 없음
        // 출력: source별 timing sample과 감소된 in-flight count
        ~ActiveJob()
        {
            if (!m_startedNanoseconds.has_value())
            {
                return;
            }
            const auto finishedNanoseconds = m_owner.elapsedNanoseconds();
            QMutexLocker lock(&m_owner.m_mutex);
            m_owner.m_samples.insert(m_sourcePath, {*m_startedNanoseconds, finishedNanoseconds, render});
            --m_owner.m_running;
        }

        flexraw::core::measurement::RenderStats render;

    private:
        const MeasuredLocalExportPipeline& m_owner;
        QString m_sourcePath;
        std::optional<std::uint64_t> m_startedNanoseconds;
    };

    // 목적: measured request 시작 이후의 monotonic elapsed time 계산
    // 입력: 없음
    // 출력: nanosecond 단위 상대 시각
    [[nodiscard]] std::uint64_t elapsedNanoseconds() const
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_epoch).count());
    }

    flexraw::core::orchestration::FileExportPipeline m_delegate;
    flexraw::core::render::ResolvedRenderPipeline m_renderer;
    mutable QMutex m_mutex;
    mutable QHash<QString, LocalExecutionSample> m_samples;
    mutable int m_running{0};
    mutable int m_runningHighWater{0};
    bool m_measuring{false};
    std::chrono::steady_clock::time_point m_epoch;
};

struct CompletionSample
{
    QString sourcePath;
    std::uint64_t endToEndNanoseconds{0};
};

struct ObservedExport
{
    std::optional<flexraw::core::orchestration::ExportResult> completed;
    std::optional<flexraw::core::orchestration::ExportIssue> failed;
    QVector<CompletionSample> samples;
    std::uint64_t makespanNanoseconds{0};
    bool cancelled{false};
    bool timedOut{false};
};

// 목적: benchmark 진단과 CSV에 사용할 QString을 UTF-8 표준 문자열로 변환
// 입력: value: 변환할 Qt 문자열
// 출력: UTF-8 byte를 보유한 std::string
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

// 목적: placement benchmark에 공통 적용할 대표 develop parameter 생성
// 입력: 없음
// 출력: 기존 W3 batch와 같은 비기본 DevelopParams
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

// 목적: Local/Remote/Auto CLI 값을 Core placement policy로 변환
// 입력: value: mode option 원문, policy: 변환 결과
// 출력: 지원하는 mode이면 true
[[nodiscard]] bool parsePolicy(const QString& value, flexraw::core::orchestration::ExportPlacementPolicy& policy)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("local"))
    {
        policy = flexraw::core::orchestration::ExportPlacementPolicy::LocalOnly;
        return true;
    }
    if (normalized == QStringLiteral("remote"))
    {
        policy = flexraw::core::orchestration::ExportPlacementPolicy::RemoteOnly;
        return true;
    }
    if (normalized == QStringLiteral("auto"))
    {
        policy = flexraw::core::orchestration::ExportPlacementPolicy::Auto;
        return true;
    }
    return false;
}

// 목적: 양의 정수 CLI option을 bounded int로 변환
// 입력: value/name/maximum: option 원문, 진단 이름과 상한, parsed: 변환 결과
// 출력: 1 이상 maximum 이하이면 true
[[nodiscard]] bool parsePositiveInteger(const QString& value, const QString& name, const int maximum, int& parsed)
{
    bool converted = false;
    const int candidate = value.toInt(&converted);
    if (!converted || candidate < 1 || candidate > maximum)
    {
        std::cerr << toUtf8(name) << " must be between 1 and " << maximum << ".\n";
        return false;
    }
    parsed = candidate;
    return true;
}

// 목적: CLI option을 검증된 placement benchmark configuration으로 변환
// 입력: application/configuration: Qt argument owner와 출력 configuration
// 출력: directory, endpoint, concurrency와 mode가 유효하면 true
[[nodiscard]] bool parseConfiguration(QCoreApplication& application, PlacementBenchmarkConfiguration& configuration)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Measures unified Local/Remote/Auto Export placement."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("input-dir"),
                      QStringLiteral("Directory containing the versioned RAW corpus."),
                      QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("output-dir"), QStringLiteral("Existing shared output directory."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("mode"),
                      QStringLiteral("local, remote, or auto."),
                      QStringLiteral("name"),
                      QStringLiteral("auto")});
    parser.addOption({QStringLiteral("local-concurrency"),
                      QStringLiteral("Desktop Local slot limit."),
                      QStringLiteral("count"),
                      QStringLiteral("4")});
    parser.addOption({QStringLiteral("remote-concurrency"),
                      QStringLiteral("Remote running slot limit."),
                      QStringLiteral("count"),
                      QStringLiteral("2")});
    parser.addOption({QStringLiteral("remote-host"),
                      QStringLiteral("Manual Remote Worker host."),
                      QStringLiteral("host"),
                      QStringLiteral("127.0.0.1")});
    parser.addOption({QStringLiteral("remote-port"),
                      QStringLiteral("Manual Remote Worker port."),
                      QStringLiteral("port"),
                      QStringLiteral("47331")});
    parser.addOption({QStringLiteral("warmup"),
                      QStringLiteral("Single-photo warm-up request count."),
                      QStringLiteral("count"),
                      QStringLiteral("0")});
    parser.addOption({QStringLiteral("instrumented-local"),
                      QStringLiteral("Use measured RAW renderer for LocalOnly stage/in-flight diagnostics.")});
    parser.addOption({QStringLiteral("trace-local-admission"),
                      QStringLiteral("Record Local admission decisions without replacing the production renderer.")});
    parser.addOption(
        {QStringLiteral("run-id"), QStringLiteral("Stable CSV run label."), QStringLiteral("id"), configuration.runId});
    parser.addOption({QStringLiteral("target"),
                      QStringLiteral("Combined execution target label."),
                      QStringLiteral("name"),
                      configuration.target});
    parser.process(application);

    const QFileInfo inputInfo(parser.value(QStringLiteral("input-dir")));
    const QFileInfo outputInfo(parser.value(QStringLiteral("output-dir")));
    if (!inputInfo.isDir() || !outputInfo.isDir() || !outputInfo.isWritable())
    {
        std::cerr << "--input-dir and writable --output-dir must exist.\n";
        return false;
    }

    bool portConverted = false;
    const uint port = parser.value(QStringLiteral("remote-port")).toUInt(&portConverted);
    bool warmupConverted = false;
    const int warmup = parser.value(QStringLiteral("warmup")).toInt(&warmupConverted);
    if (!parsePolicy(parser.value(QStringLiteral("mode")), configuration.policy) || !portConverted || port == 0 ||
        port > 65535 || !warmupConverted || warmup < 0 ||
        !parsePositiveInteger(parser.value(QStringLiteral("local-concurrency")),
                              QStringLiteral("--local-concurrency"),
                              64,
                              configuration.localConcurrency) ||
        !parsePositiveInteger(parser.value(QStringLiteral("remote-concurrency")),
                              QStringLiteral("--remote-concurrency"),
                              64,
                              configuration.remoteConcurrency))
    {
        std::cerr << "Invalid placement mode, endpoint, concurrency, or warm-up count.\n";
        return false;
    }

    configuration.inputDirectory = inputInfo.absoluteFilePath();
    configuration.outputDirectory = outputInfo.absoluteFilePath();
    configuration.remoteHost = parser.value(QStringLiteral("remote-host")).trimmed();
    configuration.remotePort = static_cast<std::uint16_t>(port);
    configuration.warmupCount = warmup;
    configuration.instrumentedLocal = parser.isSet(QStringLiteral("instrumented-local"));
    configuration.traceLocalAdmission = parser.isSet(QStringLiteral("trace-local-admission"));
    if ((configuration.instrumentedLocal || configuration.traceLocalAdmission) &&
        configuration.policy != flexraw::core::orchestration::ExportPlacementPolicy::LocalOnly)
    {
        std::cerr << "Local diagnostics require --mode local.\n";
        return false;
    }
    configuration.runId = parser.value(QStringLiteral("run-id"));
    configuration.target = parser.value(QStringLiteral("target"));
    return true;
}

// 목적: deterministic corpus에서 첫 RAW source를 warm-up과 marker 탐색용으로 선택
// 입력: inputDirectory: corpus folder
// 출력: 지원 RAW가 있으면 첫 absolute QFileInfo
[[nodiscard]] std::optional<QFileInfo> firstRawSource(const QString& inputDirectory)
{
    const QFileInfoList files =
        QDir(inputDirectory).entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& file : files)
    {
        if (flexraw::core::util::classifyExtension(file.suffix()) == flexraw::core::types::SupportedFileKind::Raw)
        {
            return file;
        }
    }
    return std::nullopt;
}

// 목적: 현재 corpus/output marker를 endpoint profile에 best-effort 결합
// 입력: configuration/source: endpoint와 첫 RAW
// 출력: Remote target; marker 오류는 빈 ID로 남겨 adapter가 authoritative 판정
[[nodiscard]] flexraw::core::orchestration::ExportRemoteTarget makeRemoteTarget(
    const PlacementBenchmarkConfiguration& configuration, const QFileInfo& source)
{
    flexraw::core::orchestration::ExportRemoteTarget target;
    target.host = configuration.remoteHost;
    target.port = configuration.remotePort;
    const auto sourceStorage = flexraw::worker::client::SharedStorageLocator::locateSource(source.absoluteFilePath());
    const auto outputStorage = flexraw::worker::client::SharedStorageLocator::locateOutput(
        QDir(configuration.outputDirectory).filePath(QStringLiteral("placement-marker-probe.jpg")));
    if (sourceStorage.hasValue() && outputStorage.hasValue())
    {
        target.expectedSourceStorageId = sourceStorage.value().storageId.toString(QUuid::WithoutBraces);
        target.expectedOutputStorageId = outputStorage.value().storageId.toString(QUuid::WithoutBraces);
    }
    return target;
}

// 목적: benchmark mode와 endpoint를 immutable Export placement option으로 구성
// 입력: configuration/source: mode, endpoint와 storage marker 기준 source
// 출력: LocalOnly 또는 Remote target을 포함한 RemoteOnly/Auto option
[[nodiscard]] flexraw::core::orchestration::ExportPlacementOptions makePlacement(
    const PlacementBenchmarkConfiguration& configuration, const QFileInfo& source)
{
    flexraw::core::orchestration::ExportPlacementOptions placement;
    placement.policy = configuration.policy;
    if (configuration.policy != flexraw::core::orchestration::ExportPlacementPolicy::LocalOnly)
    {
        placement.remoteTarget = makeRemoteTarget(configuration, source);
    }
    return placement;
}

// 목적: 한 Export request를 Qt event loop에서 terminal event까지 관찰
// 입력: orchestrator/request/placement: 실행 authority와 immutable 요청 값
// 출력: progress별 queue E2E, terminal report 또는 issue
[[nodiscard]] ObservedExport executeObserved(flexraw::core::orchestration::ExportOrchestrator& orchestrator,
                                             flexraw::core::orchestration::ExportRequest request,
                                             flexraw::core::orchestration::ExportPlacementOptions placement)
{
    ObservedExport observed;
    QEventLoop eventLoop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(DefaultTimeoutMilliseconds);
    std::optional<flexraw::core::types::RequestId> requestId;
    const flexraw::core::measurement::StageTimer timer;

    const auto progressConnection = QObject::connect(
        &orchestrator,
        &flexraw::core::orchestration::ExportOrchestrator::exportProgressed,
        &eventLoop,
        [&](const flexraw::core::orchestration::ExportProgress& progress) {
            if (requestId.has_value() && progress.requestId == *requestId)
            {
                observed.samples.push_back({progress.currentSourcePath, timer.elapsedNanoseconds()});
                std::cerr << "progress,completed=" << progress.completedCount << ",total=" << progress.totalCount
                          << ",source=" << toUtf8(QFileInfo(progress.currentSourcePath).fileName()) << '\n';
            }
        });
    const auto completedConnection =
        QObject::connect(&orchestrator,
                         &flexraw::core::orchestration::ExportOrchestrator::exportCompleted,
                         &eventLoop,
                         [&](const flexraw::core::orchestration::ExportResult& result) {
                             if (requestId.has_value() && result.requestId == *requestId)
                             {
                                 observed.completed = result;
                                 eventLoop.quit();
                             }
                         });
    const auto failedConnection = QObject::connect(&orchestrator,
                                                   &flexraw::core::orchestration::ExportOrchestrator::exportFailed,
                                                   &eventLoop,
                                                   [&](const flexraw::core::orchestration::ExportIssue& issue) {
                                                       if (requestId.has_value() && issue.requestId == *requestId)
                                                       {
                                                           observed.failed = issue;
                                                           eventLoop.quit();
                                                       }
                                                   });
    const auto cancelledConnection =
        QObject::connect(&orchestrator,
                         &flexraw::core::orchestration::ExportOrchestrator::exportCancelled,
                         &eventLoop,
                         [&](const flexraw::core::types::RequestId cancelledId) {
                             if (requestId.has_value() && cancelledId == *requestId)
                             {
                                 observed.cancelled = true;
                                 eventLoop.quit();
                             }
                         });
    QObject::connect(&timeout, &QTimer::timeout, &eventLoop, [&] {
        observed.timedOut = true;
        if (requestId.has_value())
        {
            (void)orchestrator.cancelExport(*requestId);
        }
        eventLoop.quit();
    });

    const auto submitted = orchestrator.submitExport(std::move(request), std::move(placement));
    if (submitted.hasValue())
    {
        requestId = submitted.value();
        timeout.start();
        eventLoop.exec();
    }
    else
    {
        observed.failed = flexraw::core::orchestration::ExportIssue{0, submitted.error()};
    }
    observed.makespanNanoseconds = timer.elapsedNanoseconds();
    QObject::disconnect(progressConnection);
    QObject::disconnect(completedConnection);
    QObject::disconnect(failedConnection);
    QObject::disconnect(cancelledConnection);
    return observed;
}

// 목적: 지정 placement에서 첫 corpus source를 measured set 밖에서 예열
// 입력: orchestrator/configuration/source/placement/index: 실행 context와 warm-up identity
// 출력: 단일 file request 성공 여부
[[nodiscard]] bool runWarmup(flexraw::core::orchestration::ExportOrchestrator& orchestrator,
                             const PlacementBenchmarkConfiguration& configuration,
                             const QFileInfo& source,
                             const flexraw::core::orchestration::ExportPlacementOptions& placement,
                             const int index)
{
    flexraw::core::orchestration::ExportFileRequest request;
    request.source = {source.absoluteFilePath(),
                      source.suffix().toLower(),
                      source.fileName(),
                      flexraw::core::types::SupportedFileKind::Raw};
    request.outputPath =
        QDir(configuration.outputDirectory)
            .filePath(
                QStringLiteral("warmup-%1-%2.jpg").arg(index, 2, 10, QLatin1Char('0')).arg(source.completeBaseName()));
    request.developParams = makeRepresentativeParams();
    request.options.includeMetadata = false;
    const ObservedExport observed = executeObserved(orchestrator, std::move(request), placement);
    return observed.completed.has_value() && observed.completed->report.failedCount == 0;
}

// 목적: measured corpus용 batch request 생성
// 입력: configuration: input/output과 Local request cap
// 출력: 대표 DevelopParams와 JPEG option을 가진 immutable batch intent
[[nodiscard]] flexraw::core::orchestration::ExportBatchRequest makeBatchRequest(
    const PlacementBenchmarkConfiguration& configuration)
{
    flexraw::core::orchestration::ExportBatchRequest request;
    request.inputFolderPath = configuration.inputDirectory;
    request.outputFolderPath = configuration.outputDirectory;
    request.developParams = makeRepresentativeParams();
    request.workerCount = configuration.localConcurrency;
    request.options.includeMetadata = false;
    return request;
}

// 목적: terminal report와 progress timestamp를 versioned CSV와 summary로 출력
// 입력: configuration/observed/processMemory: 실행 설정, 결과와 peak RSS
// 출력: 모든 item이 성공하고 Local/Remote mode 계약을 만족하면 true
[[nodiscard]] bool writeResults(const PlacementBenchmarkConfiguration& configuration,
                                const ObservedExport& observed,
                                const std::optional<flexraw::platform::ProcessMemorySnapshot>& processMemory,
                                const MeasuredLocalExportPipeline* measuredPipeline)
{
    if (!observed.completed.has_value())
    {
        const QString message = observed.failed.has_value() ? observed.failed->error.message
                                                            : QStringLiteral("Export was cancelled or timed out.");
        std::cerr << "failed,message=" << toUtf8(message) << '\n';
        return false;
    }

    QHash<QString, std::uint64_t> completionTimes;
    for (const CompletionSample& sample : observed.samples)
    {
        completionTimes.insert(QDir::cleanPath(sample.sourcePath), sample.endToEndNanoseconds);
    }

    const auto& report = observed.completed->report;
    int missingLocalSamples = 0;
    flexraw::core::measurement::writeRenderMeasurementCsvHeader(std::cout);
    for (qsizetype index = 0; index < report.items.size(); ++index)
    {
        const auto& item = report.items.at(index);
        flexraw::core::measurement::RenderMeasurementRecord record;
        record.runId = toUtf8(configuration.runId);
        record.mode =
            configuration.policy == flexraw::core::orchestration::ExportPlacementPolicy::LocalOnly    ? "export-local"
            : configuration.policy == flexraw::core::orchestration::ExportPlacementPolicy::RemoteOnly ? "export-remote"
                                                                                                      : "export-auto";
        record.target = toUtf8(configuration.target);
        record.sourceLabel = toUtf8(QFileInfo(item.sourcePath).fileName());
        record.concurrency = static_cast<std::uint32_t>(
            configuration.policy == flexraw::core::orchestration::ExportPlacementPolicy::LocalOnly
                ? configuration.localConcurrency
            : configuration.policy == flexraw::core::orchestration::ExportPlacementPolicy::RemoteOnly
                ? configuration.remoteConcurrency
                : configuration.localConcurrency + configuration.remoteConcurrency);
        record.sampleIndex = static_cast<std::uint32_t>(index + 1);
        record.succeeded = item.succeeded;
        record.endToEndNanoseconds =
            completionTimes.value(QDir::cleanPath(item.sourcePath), observed.makespanNanoseconds);
        if (measuredPipeline != nullptr)
        {
            record.scheduler.runningHighWater = static_cast<std::uint64_t>(measuredPipeline->runningHighWater());
            if (const auto sample = measuredPipeline->sample(item.sourcePath); sample.has_value())
            {
                record.render = sample->render;
                std::cerr << "local_item_timing,source=" << toUtf8(QFileInfo(item.sourcePath).fileName())
                          << ",start_ns=" << sample->startedNanoseconds << ",finish_ns=" << sample->finishedNanoseconds
                          << '\n';
            }
            else
            {
                ++missingLocalSamples;
            }
        }
        if (processMemory.has_value())
        {
            record.peakRssBytes = processMemory->peakResidentBytes;
        }
        if (item.succeeded)
        {
            QImageReader reader(item.outputPath);
            const QSize size = reader.size();
            record.width = static_cast<std::uint32_t>(std::max(size.width(), 0));
            record.height = static_cast<std::uint32_t>(std::max(size.height(), 0));
        }
        flexraw::core::measurement::writeRenderMeasurementCsvRow(std::cout, record);
    }
    std::cout.flush();

    const double makespanSeconds = static_cast<double>(observed.makespanNanoseconds) / 1'000'000'000.0;
    const double throughput =
        makespanSeconds > 0.0 ? static_cast<double>(report.succeededCount) * 60.0 / makespanSeconds : 0.0;
    std::cerr << "summary,jobs=" << report.totalCount << ",succeeded=" << report.succeededCount
              << ",failed=" << report.failedCount << ",makespan_ns=" << observed.makespanNanoseconds
              << ",throughput_images_per_minute=" << throughput << ",local_executed=" << report.scheduling.localExecuted
              << ",remote_executed=" << report.scheduling.remoteExecuted
              << ",remote_attempts=" << report.scheduling.remoteDispatchAttempts
              << ",server_busy=" << report.scheduling.serverBusyCount
              << ",resource_busy=" << report.scheduling.resourceBusyCount
              << ",connection_failed=" << report.scheduling.connectionFailedCount;
    if (processMemory.has_value())
    {
        std::cerr << ",current_rss_bytes=" << processMemory->residentBytes
                  << ",peak_rss_bytes=" << processMemory->peakResidentBytes;
    }
    if (measuredPipeline != nullptr)
    {
        std::cerr << ",actual_local_running_high_water=" << measuredPipeline->runningHighWater()
                  << ",missing_local_samples=" << missingLocalSamples;
    }
    std::cerr << '\n';
    return report.failedCount == 0 && missingLocalSamples == 0;
}

// 목적: production-equivalent Export placement object graph과 measured corpus 실행
// 입력: configuration: corpus, endpoint, slot과 output 설정
// 출력: configuration/processing 성공 여부에 따른 process code
[[nodiscard]] int runBenchmark(const PlacementBenchmarkConfiguration& configuration)
{
    const std::optional<QFileInfo> source = firstRawSource(configuration.inputDirectory);
    if (!source.has_value())
    {
        std::cerr << "No supported RAW files found in --input-dir.\n";
        return 3;
    }

    const std::unique_ptr<flexraw::platform::ISystemMemoryProbe> systemMemoryProbe =
        flexraw::platform::createCurrentSystemMemoryProbe();
    const std::unique_ptr<flexraw::platform::IProcessMemoryProbe> processMemoryProbe =
        flexraw::platform::createCurrentProcessMemoryProbe();
    flexraw::core::orchestration::ExportSchedulingConfiguration scheduling;
    scheduling.localSlotLimit = configuration.localConcurrency;
    scheduling.remoteSlotLimit = configuration.remoteConcurrency;
    scheduling.maximumRemoteDispatchAttempts = 3;
    scheduling.enforceLocalResourceReserve = true;
    scheduling.reservedLogicalProcessors = 4;
    scheduling.memoryReserveBytes = 10ULL * 1024ULL * Mebibyte;
    scheduling.memoryClaimPerJobBytes = 1024ULL * Mebibyte;
    LocalAdmissionTrace admissionTrace;
    if (configuration.traceLocalAdmission)
    {
        scheduling.localAdmissionObserver = [&admissionTrace](const auto& decision) {
            admissionTrace.observe(decision);
        };
    }
    auto remoteAdapter = std::make_unique<flexraw::worker::client::RemoteExportExecutionAdapter>(
        std::make_unique<flexraw::worker::client::RemoteRenderExecutor>());
    const auto pathIdentityService = flexraw::platform::createCurrentPathIdentityService();
    std::unique_ptr<flexraw::core::orchestration::IExportPipeline> pipeline;
    MeasuredLocalExportPipeline* measuredPipeline = nullptr;
    if (configuration.instrumentedLocal)
    {
        auto measured = std::make_unique<MeasuredLocalExportPipeline>(*pathIdentityService);
        measuredPipeline = measured.get();
        pipeline = std::move(measured);
    }
    else
    {
        pipeline = std::make_unique<flexraw::core::orchestration::FileExportPipeline>(*pathIdentityService);
    }
    flexraw::core::orchestration::ExportOrchestrator orchestrator(
        std::move(pipeline), std::move(remoteAdapter), systemMemoryProbe.get(), scheduling);
    const flexraw::core::orchestration::ExportPlacementOptions placement = makePlacement(configuration, *source);

    for (int index = 1; index <= configuration.warmupCount; ++index)
    {
        std::cerr << "warmup,index=" << index << ",state=started\n";
        if (!runWarmup(orchestrator, configuration, *source, placement, index))
        {
            std::cerr << "warmup,index=" << index << ",state=failed\n";
            return 4;
        }
        std::cerr << "warmup,index=" << index << ",state=completed\n";
    }

    if (measuredPipeline != nullptr)
    {
        measuredPipeline->startMeasurement();
    }
    if (configuration.traceLocalAdmission)
    {
        admissionTrace.startMeasurement();
    }
    const ObservedExport observed = executeObserved(orchestrator, makeBatchRequest(configuration), placement);
    const std::optional<flexraw::platform::ProcessMemorySnapshot> processMemory = processMemoryProbe->snapshot();
    const bool succeeded = writeResults(configuration, observed, processMemory, measuredPipeline);
    if (configuration.traceLocalAdmission)
    {
        admissionTrace.writeLog();
    }
    return succeeded ? 0 : 5;
}

}  // namespace

// 목적: unified Export placement benchmark용 Qt runtime과 CLI 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: 성공 0, configuration 2, corpus 3, warm-up 4, measured failure 5
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Export Placement Benchmark"));
    PlacementBenchmarkConfiguration configuration;
    if (!parseConfiguration(application, configuration))
    {
        return 2;
    }
    return runBenchmark(configuration);
}
