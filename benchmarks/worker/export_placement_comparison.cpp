#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
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
                                const std::optional<flexraw::platform::ProcessMemorySnapshot>& processMemory)
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
    std::cerr << '\n';
    return report.failedCount == 0;
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
    auto remoteAdapter = std::make_unique<flexraw::worker::client::RemoteExportExecutionAdapter>(
        std::make_unique<flexraw::worker::client::RemoteRenderExecutor>());
    const auto pathIdentityService = flexraw::platform::createCurrentPathIdentityService();
    flexraw::core::orchestration::ExportOrchestrator orchestrator(
        std::make_unique<flexraw::core::orchestration::FileExportPipeline>(*pathIdentityService),
        std::move(remoteAdapter),
        systemMemoryProbe.get(),
        scheduling);
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

    const ObservedExport observed = executeObserved(orchestrator, makeBatchRequest(configuration), placement);
    const std::optional<flexraw::platform::ProcessMemorySnapshot> processMemory = processMemoryProbe->snapshot();
    return writeResults(configuration, observed, processMemory) ? 0 : 5;
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
