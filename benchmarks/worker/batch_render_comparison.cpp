#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>

#include "current_process_memory_probe.h"
#include "develop_params.h"
#include "job_scheduler.h"
#include "measurement_csv.h"
#include "operation_types.h"
#include "render_job_runner.h"
#include "resolved_render_pipeline.h"
#include "stage_timer.h"
#include "supported_extensions.h"

namespace
{

struct BatchConfiguration
{
    QString inputDirectory;
    QString outputDirectory;
    QString runId;
    QString target;
    std::uint32_t concurrency{2};
    std::uint32_t warmupCount{1};
};

struct BatchJobResult
{
    QString sourceLabel;
    bool succeeded{false};
    QString errorMessage;
    std::uint32_t width{0};
    std::uint32_t height{0};
    flexraw::core::measurement::RenderStats render;
    std::uint64_t endToEndNanoseconds{0};
};

// 목적: benchmark 진단과 CSV에 사용할 QString을 UTF-8 표준 문자열로 변환
// 입력: value: 변환할 Qt 문자열
// 출력: UTF-8 byte를 보유한 std::string
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

// 목적: batch benchmark에 공통 적용할 대표 develop parameter 생성
// 입력: 없음
// 출력: 단일 RAW benchmark와 동일한 비기본 DevelopParams
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

// 목적: 입력 폴더에서 deterministic 순서의 지원 RAW batch manifest 생성
// 입력: inputDirectory: 단일 폴더 범위의 source root
// 출력: 파일명 대소문자를 정규화해 정렬한 absolute RAW 목록
[[nodiscard]] std::vector<QFileInfo> discoverRawFiles(const QString& inputDirectory)
{
    const QDir directory(inputDirectory);
    const QFileInfoList entries = directory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    std::vector<QFileInfo> files;
    files.reserve(static_cast<std::size_t>(entries.size()));
    for (const QFileInfo& entry : entries)
    {
        if (flexraw::core::util::classifyExtension(entry.suffix()) == flexraw::core::types::SupportedFileKind::Raw)
        {
            files.push_back(entry);
        }
    }
    return files;
}

// 목적: source와 고유 output에 대표 보정·출력 option을 결합
// 입력: sourcePath/outputPath: resolved local file 경로
// 출력: scheduler가 실행할 resolved render request
[[nodiscard]] flexraw::core::render::ResolvedRenderRequest makeRequest(const QString& sourcePath,
                                                                       const QString& outputPath)
{
    flexraw::core::render::ResolvedRenderRequest request;
    request.sourcePath = sourcePath;
    request.outputPath = outputPath;
    request.developParams = makeRepresentativeParams();
    request.outputOptions.includeMetadata = false;
    return request;
}

// 목적: command-line option을 검증된 batch configuration으로 변환
// 입력: application/configuration: Qt argument owner와 출력 configuration
// 출력: 필수 directory와 양의 concurrency/warm-up이 유효하면 true
[[nodiscard]] bool parseConfiguration(QCoreApplication& application, BatchConfiguration& configuration)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Measures bounded local Worker batch concurrency."));
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("input-dir"), QStringLiteral("Directory containing supported RAW sources."), QStringLiteral("path")});
    parser.addOption(
        {QStringLiteral("output-dir"), QStringLiteral("Existing output directory."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("concurrency"),
                      QStringLiteral("Bounded Worker running limit."),
                      QStringLiteral("count"),
                      QStringLiteral("2")});
    parser.addOption({QStringLiteral("warmup"),
                      QStringLiteral("Warm-up render count before batch timing."),
                      QStringLiteral("count"),
                      QStringLiteral("1")});
    parser.addOption({QStringLiteral("run-id"),
                      QStringLiteral("Stable CSV run label."),
                      QStringLiteral("id"),
                      QStringLiteral("batch-manual-1")});
    parser.addOption(
        {QStringLiteral("target"), QStringLiteral("Machine and architecture label."), QStringLiteral("name")});
    parser.process(application);

    const QFileInfo inputInfo(parser.value(QStringLiteral("input-dir")));
    const QFileInfo outputInfo(parser.value(QStringLiteral("output-dir")));
    bool concurrencyOk = false;
    bool warmupOk = false;
    const uint concurrency = parser.value(QStringLiteral("concurrency")).toUInt(&concurrencyOk);
    const uint warmup = parser.value(QStringLiteral("warmup")).toUInt(&warmupOk);
    if (!inputInfo.isDir() || !outputInfo.isDir() || !concurrencyOk || concurrency == 0 || !warmupOk)
    {
        std::cerr
            << "--input-dir/--output-dir must exist, --concurrency must be positive, and --warmup non-negative.\n";
        return false;
    }

    configuration.inputDirectory = inputInfo.absoluteFilePath();
    configuration.outputDirectory = outputInfo.absoluteFilePath();
    configuration.concurrency = concurrency;
    configuration.warmupCount = warmup;
    configuration.runId = parser.value(QStringLiteral("run-id"));
    configuration.target = parser.value(QStringLiteral("target"));
    return true;
}

// 목적: optional warm-up으로 library와 filesystem 초기화 비용을 batch 측정에서 분리
// 입력: configuration/source/runner: 출력 위치, 대표 source와 동기 실행자
// 출력: 모든 warm-up render가 성공하면 true
[[nodiscard]] bool runWarmup(const BatchConfiguration& configuration,
                             const QFileInfo& source,
                             const flexraw::worker::runtime::IRenderJobRunner& runner)
{
    for (std::uint32_t index = 1; index <= configuration.warmupCount; ++index)
    {
        const QString outputPath = QDir(configuration.outputDirectory)
                                       .filePath(QStringLiteral("warmup-%1.jpg").arg(index, 4, 10, QLatin1Char('0')));
        flexraw::core::types::CancellationSource cancellation;
        const flexraw::worker::runtime::RenderJobExecutionResult result =
            runner.execute(makeRequest(source.absoluteFilePath(), outputPath), cancellation.token());
        if (result.hasError() || result.value().hasError())
        {
            std::cerr << "Warm-up render failed.\n";
            return false;
        }
    }
    return true;
}

// 목적: terminal scheduler outcome을 CSV record용 immutable 결과로 변환
// 입력: outcome/sourceLabel/endToEndNanoseconds: 실행 결과와 source·queue 포함 시간
// 출력: success/failure timing과 output dimensions를 포함한 결과
[[nodiscard]] BatchJobResult decodeOutcome(const flexraw::worker::runtime::RenderJobOutcome& outcome,
                                           QString sourceLabel,
                                           const std::uint64_t endToEndNanoseconds)
{
    BatchJobResult result;
    result.sourceLabel = std::move(sourceLabel);
    result.endToEndNanoseconds = endToEndNanoseconds;
    if (outcome.result.hasError())
    {
        result.errorMessage = outcome.result.error().message;
        return result;
    }

    const flexraw::core::render::ResolvedRenderPipelineResult& pipelineResult = outcome.result.value();
    if (pipelineResult.hasError())
    {
        result.errorMessage = pipelineResult.error().cause.message;
        result.render = pipelineResult.error().stats;
        return result;
    }

    result.succeeded = true;
    result.render = pipelineResult.value().stats;
    QImageReader reader(pipelineResult.value().artifact.outputPath);
    const QSize size = reader.size();
    result.width = static_cast<std::uint32_t>(std::max(size.width(), 0));
    result.height = static_cast<std::uint32_t>(std::max(size.height(), 0));
    return result;
}

// 목적: scheduler 결과 전체를 versioned per-job CSV와 batch summary로 출력
// 입력: configuration/results/scheduler/makespan: 공통 context와 측정 결과
// 출력: 성공 job 수가 전체와 같으면 true
[[nodiscard]] bool writeResults(const BatchConfiguration& configuration,
                                const std::vector<BatchJobResult>& results,
                                const flexraw::core::measurement::SchedulerStats scheduler,
                                const std::uint64_t makespanNanoseconds,
                                const std::optional<flexraw::platform::ProcessMemorySnapshot>& processMemory)
{
    flexraw::core::measurement::writeRenderMeasurementCsvHeader(std::cout);
    std::size_t succeeded = 0;
    for (std::size_t index = 0; index < results.size(); ++index)
    {
        const BatchJobResult& result = results[index];
        flexraw::core::measurement::RenderMeasurementRecord record;
        record.runId = toUtf8(configuration.runId);
        record.mode = "worker-scheduler-batch";
        record.target = toUtf8(configuration.target);
        record.sourceLabel = toUtf8(result.sourceLabel);
        record.width = result.width;
        record.height = result.height;
        record.concurrency = configuration.concurrency;
        record.sampleIndex = static_cast<std::uint32_t>(index + 1);
        record.succeeded = result.succeeded;
        if (processMemory.has_value())
        {
            record.peakRssBytes = processMemory->peakResidentBytes;
        }
        record.scheduler = scheduler;
        record.render = result.render;
        record.endToEndNanoseconds = result.endToEndNanoseconds;
        flexraw::core::measurement::writeRenderMeasurementCsvRow(std::cout, record);
        succeeded += result.succeeded ? 1U : 0U;
        if (!result.succeeded)
        {
            std::cerr << "failed,source=" << toUtf8(result.sourceLabel) << ",message=" << toUtf8(result.errorMessage)
                      << '\n';
        }
    }
    std::cout.flush();
    const double makespanSeconds = static_cast<double>(makespanNanoseconds) / 1'000'000'000.0;
    const double throughput = makespanSeconds > 0.0 ? static_cast<double>(succeeded) * 60.0 / makespanSeconds : 0.0;
    std::cerr << "summary,jobs=" << results.size() << ",succeeded=" << succeeded
              << ",concurrency=" << configuration.concurrency << ",makespan_ns=" << makespanNanoseconds
              << ",throughput_images_per_minute=" << throughput << ",queued_high_water=" << scheduler.queuedHighWater
              << ",running_high_water=" << scheduler.runningHighWater;
    if (processMemory.has_value())
    {
        std::cerr << ",current_rss_bytes=" << processMemory->residentBytes
                  << ",peak_rss_bytes=" << processMemory->peakResidentBytes;
    }
    else
    {
        std::cerr << ",current_rss_bytes=unavailable,peak_rss_bytes=unavailable";
    }
    std::cerr << '\n';
    return succeeded == results.size();
}

// 목적: deterministic manifest를 실제 bounded JobScheduler에서 병렬 실행
// 입력: configuration/files/runner: 실행 option, source manifest와 processing runner
// 출력: 모든 job이 성공하면 true
[[nodiscard]] bool runBatch(const BatchConfiguration& configuration,
                            const std::vector<QFileInfo>& files,
                            const flexraw::worker::runtime::IRenderJobRunner& runner,
                            const flexraw::platform::IProcessMemoryProbe& processMemoryProbe)
{
    flexraw::worker::runtime::JobScheduler scheduler(runner, configuration.concurrency, files.size());
    std::mutex mutex;
    std::condition_variable completedCondition;
    std::vector<BatchJobResult> results(files.size());
    std::vector<std::uint64_t> submittedAt(files.size());
    std::size_t completed = 0;
    const flexraw::core::measurement::StageTimer batchTimer;

    for (std::size_t index = 0; index < files.size(); ++index)
    {
        const QFileInfo& source = files[index];
        const QString outputName =
            QStringLiteral("%1-%2.jpg").arg(index + 1, 4, 10, QLatin1Char('0')).arg(source.completeBaseName());
        const QString outputPath = QDir(configuration.outputDirectory).filePath(outputName);
        submittedAt[index] = batchTimer.elapsedNanoseconds();
        flexraw::worker::runtime::ScheduledRenderJob job;
        job.key = {1, {static_cast<std::uint64_t>(index + 1)}};
        job.outputRelativePath = outputName;
        job.request = makeRequest(source.absoluteFilePath(), outputPath);
        const flexraw::worker::runtime::SubmitStatus status = scheduler.submit(
            std::move(job),
            [&, index, sourceLabel = source.fileName()](flexraw::worker::runtime::RenderJobOutcome outcome) mutable {
                const std::uint64_t endToEnd = batchTimer.elapsedNanoseconds() - submittedAt[index];
                BatchJobResult result = decodeOutcome(outcome, std::move(sourceLabel), endToEnd);
                {
                    const std::scoped_lock lock(mutex);
                    results[index] = std::move(result);
                    ++completed;
                    std::cerr << "progress,completed=" << completed << ",total=" << files.size()
                              << ",source=" << toUtf8(results[index].sourceLabel)
                              << ",end_to_end_ns=" << results[index].endToEndNanoseconds << '\n';
                }
                completedCondition.notify_one();
            });
        if (status != flexraw::worker::runtime::SubmitStatus::Accepted)
        {
            std::cerr << "Scheduler rejected batch job " << index + 1 << ".\n";
            scheduler.shutdown();
            return false;
        }
    }

    {
        std::unique_lock lock(mutex);
        completedCondition.wait(lock, [&]() { return completed == files.size(); });
    }
    const std::uint64_t makespanNanoseconds = batchTimer.elapsedNanoseconds();
    const flexraw::worker::runtime::WorkerRuntimeSnapshot snapshot = scheduler.snapshot();
    const std::optional<flexraw::platform::ProcessMemorySnapshot> processMemory = processMemoryProbe.snapshot();
    scheduler.shutdown();
    return writeResults(configuration, results, snapshot.highWater, makespanNanoseconds, processMemory);
}

// 목적: batch benchmark의 Qt runtime, manifest, pipeline과 scheduler 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: 성공 0, configuration 2, manifest 3, warm-up 4, batch failure 5
[[nodiscard]] int runApplication(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    BatchConfiguration configuration;
    if (!parseConfiguration(application, configuration))
    {
        return 2;
    }
    const std::vector<QFileInfo> files = discoverRawFiles(configuration.inputDirectory);
    if (files.empty())
    {
        std::cerr << "No supported RAW files found in --input-dir.\n";
        return 3;
    }

    flexraw::core::render::ResolvedRenderPipeline pipeline;
    flexraw::worker::runtime::PipelineRenderJobRunner runner(pipeline);
    const std::unique_ptr<flexraw::platform::IProcessMemoryProbe> processMemoryProbe =
        flexraw::platform::createCurrentProcessMemoryProbe();
    if (!runWarmup(configuration, files.front(), runner))
    {
        return 4;
    }
    return runBatch(configuration, files, runner, *processMemoryProbe) ? 0 : 5;
}

}  // namespace

int main(int argc, char* argv[])
{
    return runApplication(argc, argv);
}
