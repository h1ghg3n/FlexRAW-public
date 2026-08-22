#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <gtest/gtest.h>

#include "export_dialog.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "export_settings.h"
#include "remote_export_execution_adapter.h"
#include "remote_render_executor.h"
#include "shared_storage_locator.h"

namespace flexraw::ui::export_
{
namespace
{

// 목적: graphical export test용 처리 가능한 Editor snapshot 생성
// 입력: sourcePath: fake pipeline에 전달할 source identity
// 출력: unsaved DevelopParams를 포함한 selected EditorState
[[nodiscard]] core::orchestration::EditorState makeEditorState(const QString& sourcePath)
{
    core::orchestration::EditorState state;
    state.hasSelection = true;
    state.source = {
        sourcePath,
        QStringLiteral("arw"),
        QFileInfo(sourcePath).fileName(),
        core::types::SupportedFileKind::Raw,
    };
    state.params.exposureEv = 1.25F;
    state.params.clarity = 0.2F;
    state.sourceProcessingAllowed = true;
    return state;
}

// 목적: Remote export GUI fixture에 shared storage marker 생성
// 입력: rootPath/storageId: marker directory와 expected storage UUID
// 출력: schema 1 marker file write 성공 여부
[[nodiscard]] bool writeStorageMarker(const QString& rootPath, const QUuid& storageId)
{
    const QJsonObject marker{{QStringLiteral("schema"), 1},
                             {QStringLiteral("storage_id"), storageId.toString(QUuid::WithoutBraces)}};
    QFile file(QDir(rootPath).filePath(QString::fromLatin1(worker::client::SharedStorageMarkerFileName)));
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(marker).toJson(QJsonDocument::Compact)) > 0;
}

// 목적: Qt queued callback을 처리하며 비동기 UI 조건을 제한 시간 동안 대기
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: 제한 시간 안에 조건을 만족하면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& predicate, const int timeoutMilliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

class RecordingExportPipeline final : public core::orchestration::IExportPipeline
{
public:
    // 목적: dialog가 제출한 single-file request를 thread-safe하게 기록하고 성공 처리
    // 입력: request: 검증할 request, cancellationToken: 미사용, progress: terminal 누적 상태 callback
    // 출력: request output path를 포함한 한 item 성공 report
    [[nodiscard]] core::orchestration::ExportPipelineResult execute(
        const core::orchestration::ExportRequest& request,
        const core::types::CancellationToken&,
        const core::orchestration::ExportProgressCallback& progress) const override
    {
        const auto fileRequest = std::get<core::orchestration::ExportFileRequest>(request);
        {
            QMutexLocker lock(&m_mutex);
            m_request = fileRequest;
        }
        progress({1, 1, 1, 0, fileRequest.source.path});
        core::orchestration::ExportReport report;
        report.totalCount = 1;
        report.succeededCount = 1;
        report.items.push_back({fileRequest.source.path, fileRequest.outputPath, true, {}});
        return core::orchestration::ExportPipelineResult::success(std::move(report));
    }

    // 목적: worker thread가 기록한 request snapshot 반환
    // 입력: 없음
    // 출력: execute 전이면 빈 optional, 실행 후면 ExportFileRequest 복사본
    [[nodiscard]] std::optional<core::orchestration::ExportFileRequest> request() const
    {
        QMutexLocker lock(&m_mutex);
        return m_request;
    }

private:
    mutable QMutex m_mutex;
    mutable std::optional<core::orchestration::ExportFileRequest> m_request;
};

class CancellableExportPipeline final : public core::orchestration::IExportPipeline
{
public:
    // 목적: cancellation test가 worker 실행 시작을 기다릴 수 있는 semaphore 노출
    // 입력: 없음
    // 출력: 시작 signal을 보유한 semaphore 참조
    [[nodiscard]] QSemaphore& started() noexcept
    {
        return m_started;
    }

    // 목적: cancellation token이 요청될 때까지 deterministic하게 대기
    // 입력: request/progress: 미사용, cancellationToken: 종료 조건
    // 출력: cancellation 확인 후 Cancelled 오류
    [[nodiscard]] core::orchestration::ExportPipelineResult execute(
        const core::orchestration::ExportRequest&,
        const core::types::CancellationToken& cancellationToken,
        const core::orchestration::ExportProgressCallback&) const override
    {
        m_started.release();
        while (!cancellationToken.isCancellationRequested())
        {
            QThread::msleep(1);
        }
        return core::orchestration::ExportPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Fake export cancelled.")});
    }

private:
    mutable QSemaphore m_started;
};

class RecordingItemListPipeline final : public core::orchestration::IExportPipeline
{
public:
    // 목적: 명시적 item-list request를 Orchestrator scheduling용 prepared item으로 변환
    // 입력: request: ExportItemListRequest, cancellationToken: test에서 미사용
    // 출력: 전달 순서를 보존한 PreparedExport 또는 잘못된 request 오류
    [[nodiscard]] core::orchestration::ExportPreparationResult prepare(
        const core::orchestration::ExportRequest& request, const core::types::CancellationToken&) const override
    {
        const auto* itemList = std::get_if<core::orchestration::ExportItemListRequest>(&request);
        if (itemList == nullptr)
        {
            return core::orchestration::ExportPreparationResult::failure(
                {core::types::ErrorCode::InvalidArgument, QStringLiteral("Expected an item-list request.")});
        }

        core::orchestration::PreparedExport prepared;
        for (const core::orchestration::ExportFileRequest& item : itemList->items)
        {
            prepared.items.push_back({item, {}});
        }
        return core::orchestration::ExportPreparationResult::success(std::move(prepared));
    }

    // 목적: scheduler가 실행한 item별 request를 thread-safe하게 기록
    // 입력: item: output 표에서 조립된 immutable request, cancellationToken: 미사용
    // 출력: 같은 source/output identity의 성공 item result
    [[nodiscard]] core::orchestration::ExportItemResult executeItem(
        const core::orchestration::PreparedExportItem& item, const core::types::CancellationToken&) const override
    {
        QMutexLocker lock(&m_mutex);
        m_requests.push_back(item.request);
        return {item.request.source.path, item.request.outputPath, true, {}};
    }

    // 목적: pure virtual compatibility 경로를 test에서 사용하지 않는 failure로 구현
    // 입력: request/cancellationToken/progress: 미사용
    // 출력: 잘못된 직접 호출을 식별하는 failure
    [[nodiscard]] core::orchestration::ExportPipelineResult execute(
        const core::orchestration::ExportRequest&,
        const core::types::CancellationToken&,
        const core::orchestration::ExportProgressCallback&) const override
    {
        return core::orchestration::ExportPipelineResult::failure(
            {core::types::ErrorCode::Unknown, QStringLiteral("Direct execution is not expected.")});
    }

    // 목적: worker thread들이 기록한 item request snapshot 반환
    // 입력: 없음
    // 출력: 실행 완료 시점까지 기록된 request 목록
    [[nodiscard]] QVector<core::orchestration::ExportFileRequest> requests() const
    {
        QMutexLocker lock(&m_mutex);
        return m_requests;
    }

private:
    mutable QMutex m_mutex;
    mutable QVector<core::orchestration::ExportFileRequest> m_requests;
};

class RecordingRemoteExecutor final : public worker::client::IRemoteRenderExecutor
{
public:
    // 목적: Remote export dialog가 제출한 endpoint/request를 기록하고 성공 결과 반환
    // 입력: endpoint/request: 검증할 remote snapshot, cancellationToken: 미사용
    // 출력: wire-relative artifact path를 포함한 성공 result
    [[nodiscard]] worker::client::RemoteRenderResult execute(const worker::client::RemoteRenderEndpoint& endpoint,
                                                             const worker::client::RemoteRenderRequest& request,
                                                             const core::types::CancellationToken&) const override
    {
        return recordSuccessfulExecution(endpoint, request);
    }

    // 목적: Remote placement test에서 Worker acceptance를 publish한 뒤 성공 결과 기록
    // 입력: endpoint/request: 검증할 snapshot, cancellationToken: 미사용, accepted: ownership callback
    // 출력: wire-relative artifact path를 포함한 성공 result
    [[nodiscard]] worker::client::RemoteRenderResult execute(
        const worker::client::RemoteRenderEndpoint& endpoint,
        const worker::client::RemoteRenderRequest& request,
        const core::types::CancellationToken&,
        const worker::client::RemoteRenderAcceptedCallback& accepted) const override
    {
        if (accepted)
        {
            accepted();
        }
        return recordSuccessfulExecution(endpoint, request);
    }

    // 목적: worker thread가 기록한 endpoint snapshot 반환
    // 입력: 없음
    // 출력: execute 전이면 빈 optional, 실행 후면 endpoint 복사본
    [[nodiscard]] std::optional<worker::client::RemoteRenderEndpoint> endpoint() const
    {
        QMutexLocker lock(&m_mutex);
        return m_endpoint;
    }

    // 목적: worker thread가 기록한 wire request snapshot 반환
    // 입력: 없음
    // 출력: execute 전이면 빈 optional, 실행 후면 request 복사본
    [[nodiscard]] std::optional<worker::client::RemoteRenderRequest> request() const
    {
        QMutexLocker lock(&m_mutex);
        return m_request;
    }

private:
    // 목적: endpoint/request를 thread-safe하게 기록하고 deterministic 성공 결과 생성
    // 입력: endpoint/request: Remote adapter가 전달한 wire snapshot
    // 출력: wire-relative output artifact를 포함한 성공 result
    [[nodiscard]] worker::client::RemoteRenderResult recordSuccessfulExecution(
        const worker::client::RemoteRenderEndpoint& endpoint, const worker::client::RemoteRenderRequest& request) const
    {
        QMutexLocker lock(&m_mutex);
        m_endpoint = endpoint;
        m_request = request;
        core::render::ResolvedRenderResult result;
        result.artifact.outputPath = request.outputRelativePath;
        result.artifact.byteSize = 123;
        return worker::client::RemoteRenderResult::success(std::move(result));
    }
    mutable QMutex m_mutex;
    mutable std::optional<worker::client::RemoteRenderEndpoint> m_endpoint;
    mutable std::optional<worker::client::RemoteRenderRequest> m_request;
};

TEST(ExportDialogTest, SubmitsCurrentEditorStateAndPersistsSuccessfulOptions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("current.arw"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("current-output.png"));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto pipeline = std::make_unique<RecordingExportPipeline>();
    RecordingExportPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline));
    const core::orchestration::EditorState editorState = makeEditorState(sourcePath);
    ExportDialog dialog(orchestrator, editorState, settings);

    QLineEdit* outputPathEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit"));
    QComboBox* formatCombo = dialog.findChild<QComboBox*>(QStringLiteral("exportFormatCombo"));
    QSpinBox* pngCompression = dialog.findChild<QSpinBox*>(QStringLiteral("exportPngCompressionSpinBox"));
    QSpinBox* maximumDimension = dialog.findChild<QSpinBox*>(QStringLiteral("exportMaximumDimensionSpinBox"));
    QComboBox* colorSpace = dialog.findChild<QComboBox*>(QStringLiteral("exportColorSpaceCombo"));
    QCheckBox* metadata = dialog.findChild<QCheckBox*>(QStringLiteral("exportMetadataCheckBox"));
    QPushButton* exportButton = dialog.findChild<QPushButton*>(QStringLiteral("exportStartButton"));
    QLabel* statusLabel = dialog.findChild<QLabel*>(QStringLiteral("exportStatusLabel"));
    ASSERT_NE(nullptr, outputPathEdit);
    ASSERT_NE(nullptr, formatCombo);
    ASSERT_NE(nullptr, pngCompression);
    ASSERT_NE(nullptr, maximumDimension);
    ASSERT_NE(nullptr, colorSpace);
    ASSERT_NE(nullptr, metadata);
    ASSERT_NE(nullptr, exportButton);
    ASSERT_NE(nullptr, statusLabel);

    outputPathEdit->setText(outputPath);
    formatCombo->setCurrentIndex(formatCombo->findData(static_cast<int>(core::export_::RasterExportFormat::Png)));
    pngCompression->setValue(3);
    maximumDimension->setValue(2048);
    colorSpace->setCurrentIndex(
        colorSpace->findData(static_cast<int>(core::export_::RasterOutputColorSpace::DisplayP3)));
    metadata->setChecked(false);
    QEventLoop eventLoop;
    QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
    QObject::connect(
        &orchestrator, &core::orchestration::ExportOrchestrator::exportCompleted, &eventLoop, &QEventLoop::quit);

    exportButton->click();
    eventLoop.exec();

    const std::optional<core::orchestration::ExportFileRequest> recorded = pipelineProbe->request();
    ASSERT_TRUE(recorded.has_value());
    EXPECT_EQ(sourcePath, recorded->source.path);
    ASSERT_TRUE(recorded->developParams.has_value());
    EXPECT_EQ(editorState.params, *recorded->developParams);
    EXPECT_TRUE(recorded->catalogPath.isEmpty());
    EXPECT_EQ(QFileInfo(outputPath).absoluteFilePath(), recorded->outputPath);
    EXPECT_EQ(core::export_::RasterExportFormat::Png, recorded->options.format);
    EXPECT_EQ(3, recorded->options.pngCompression);
    EXPECT_EQ(2048, recorded->options.maximumDimension);
    EXPECT_EQ(core::export_::RasterOutputColorSpace::DisplayP3, recorded->options.outputColorSpace);
    EXPECT_FALSE(recorded->options.includeMetadata);
    EXPECT_TRUE(statusLabel->text().contains(QStringLiteral("completed"), Qt::CaseInsensitive));
    EXPECT_FALSE(exportButton->isEnabled());

    settings::ExportSettings exportSettings(settings);
    const core::export_::RasterExportOptions restored = exportSettings.loadRasterDefaults();
    EXPECT_EQ(recorded->options.format, restored.format);
    EXPECT_EQ(recorded->options.pngCompression, restored.pngCompression);
    EXPECT_EQ(recorded->options.maximumDimension, restored.maximumDimension);
    EXPECT_EQ(recorded->options.outputColorSpace, restored.outputColorSpace);
    EXPECT_EQ(recorded->options.includeMetadata, restored.includeMetadata);
}

TEST(ExportDialogTest, ShowsSelectedPhotosAsOutputRowsAndSubmitsOneItemList)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstSource = QDir(directory.path()).filePath(QStringLiteral("first.arw"));
    const QString secondSource = QDir(directory.path()).filePath(QStringLiteral("second.arw"));
    const QString firstOutput = QDir(directory.path()).filePath(QStringLiteral("first-result.jpg"));
    const QString secondOutput = QDir(directory.path()).filePath(QStringLiteral("second-result.jpg"));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto pipeline = std::make_unique<RecordingItemListPipeline>();
    RecordingItemListPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline));
    QVector<ExportDialogSource> sources{
        {makeEditorState(firstSource).source, {}, core::types::DevelopParams{}},
        {makeEditorState(secondSource).source, {}, core::types::DevelopParams{}},
    };
    ExportDialog dialog(orchestrator, std::move(sources), settings);

    QTableWidget* outputTable = dialog.findChild<QTableWidget*>(QStringLiteral("exportOutputTable"));
    QLineEdit* firstOutputEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit"));
    QLineEdit* secondOutputEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit2"));
    QPushButton* exportButton = dialog.findChild<QPushButton*>(QStringLiteral("exportStartButton"));
    QLabel* statusLabel = dialog.findChild<QLabel*>(QStringLiteral("exportStatusLabel"));
    ASSERT_NE(nullptr, outputTable);
    ASSERT_NE(nullptr, firstOutputEdit);
    ASSERT_NE(nullptr, secondOutputEdit);
    ASSERT_NE(nullptr, exportButton);
    ASSERT_NE(nullptr, statusLabel);
    ASSERT_EQ(2, outputTable->rowCount());
    EXPECT_EQ(QStringLiteral("first.arw"), outputTable->item(0, 0)->text());
    EXPECT_EQ(QStringLiteral("second.arw"), outputTable->item(1, 0)->text());

    firstOutputEdit->setText(firstOutput);
    secondOutputEdit->setText(secondOutput);
    exportButton->click();

    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->requests().size() == 2; }));
    ASSERT_TRUE(waitForCondition(
        [&] { return statusLabel->text().contains(QStringLiteral("completed"), Qt::CaseInsensitive); }));
    QVector<core::orchestration::ExportFileRequest> requests = pipelineProbe->requests();
    std::sort(requests.begin(), requests.end(), [](const auto& left, const auto& right) {
        return left.source.path < right.source.path;
    });
    EXPECT_EQ(QFileInfo(firstOutput).absoluteFilePath(), requests.at(0).outputPath);
    EXPECT_EQ(QFileInfo(secondOutput).absoluteFilePath(), requests.at(1).outputPath);
    EXPECT_TRUE(statusLabel->text().contains(QStringLiteral("2 files"), Qt::CaseInsensitive));
}

TEST(ExportDialogTest, SwitchesFormatSpecificControlsAndOutputExtension)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("current.arw"));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    core::orchestration::ExportOrchestrator orchestrator(std::make_unique<RecordingExportPipeline>());
    ExportDialog dialog(orchestrator, makeEditorState(sourcePath), settings);
    QLineEdit* outputPathEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit"));
    QComboBox* formatCombo = dialog.findChild<QComboBox*>(QStringLiteral("exportFormatCombo"));
    QSpinBox* jpegQuality = dialog.findChild<QSpinBox*>(QStringLiteral("exportJpegQualitySpinBox"));
    QSpinBox* pngCompression = dialog.findChild<QSpinBox*>(QStringLiteral("exportPngCompressionSpinBox"));
    QComboBox* tiffCompression = dialog.findChild<QComboBox*>(QStringLiteral("exportTiffCompressionCombo"));
    ASSERT_NE(nullptr, outputPathEdit);
    ASSERT_NE(nullptr, formatCombo);
    ASSERT_NE(nullptr, jpegQuality);
    ASSERT_NE(nullptr, pngCompression);
    ASSERT_NE(nullptr, tiffCompression);
    EXPECT_FALSE(jpegQuality->isHidden());
    EXPECT_TRUE(pngCompression->isHidden());
    EXPECT_TRUE(tiffCompression->isHidden());

    formatCombo->setCurrentIndex(formatCombo->findData(static_cast<int>(core::export_::RasterExportFormat::Png)));

    EXPECT_TRUE(outputPathEdit->text().endsWith(QStringLiteral(".png"), Qt::CaseInsensitive));
    EXPECT_TRUE(jpegQuality->isHidden());
    EXPECT_FALSE(pngCompression->isHidden());
    EXPECT_TRUE(tiffCompression->isHidden());

    formatCombo->setCurrentIndex(formatCombo->findData(static_cast<int>(core::export_::RasterExportFormat::Tiff)));

    EXPECT_TRUE(outputPathEdit->text().endsWith(QStringLiteral(".tif"), Qt::CaseInsensitive));
    EXPECT_TRUE(jpegQuality->isHidden());
    EXPECT_TRUE(pngCompression->isHidden());
    EXPECT_FALSE(tiffCompression->isHidden());
}

TEST(ExportDialogTest, CancelsActiveRequestAndAllowsRetry)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("current.arw"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("current-output.jpg"));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto pipeline = std::make_unique<CancellableExportPipeline>();
    CancellableExportPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline));
    ExportDialog dialog(orchestrator, makeEditorState(sourcePath), settings);
    QLineEdit* outputPathEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit"));
    QPushButton* exportButton = dialog.findChild<QPushButton*>(QStringLiteral("exportStartButton"));
    QPushButton* cancelButton = dialog.findChild<QPushButton*>(QStringLiteral("exportCancelButton"));
    QLabel* statusLabel = dialog.findChild<QLabel*>(QStringLiteral("exportStatusLabel"));
    ASSERT_NE(nullptr, outputPathEdit);
    ASSERT_NE(nullptr, exportButton);
    ASSERT_NE(nullptr, cancelButton);
    ASSERT_NE(nullptr, statusLabel);
    outputPathEdit->setText(outputPath);

    exportButton->click();
    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->started().available() > 0; }));
    ASSERT_TRUE(pipelineProbe->started().tryAcquire());
    EXPECT_FALSE(exportButton->isEnabled());
    EXPECT_TRUE(cancelButton->isEnabled());

    cancelButton->click();

    ASSERT_TRUE(waitForCondition(
        [&] { return statusLabel->text().contains(QStringLiteral("cancelled"), Qt::CaseInsensitive); }));
    EXPECT_TRUE(statusLabel->text().contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));
    EXPECT_TRUE(exportButton->isEnabled());
    EXPECT_EQ(QStringLiteral("Close"), cancelButton->text());
}

TEST(ExportDialogTest, SubmitsRemoteRequestAndRestoresDesktopOutputPath)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("current.arw"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("remote-output.jpg"));
    QFile sourceFile(sourcePath);
    ASSERT_TRUE(sourceFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(1, sourceFile.write("x"));
    sourceFile.close();
    const QUuid storageId = QUuid::createUuid();
    ASSERT_TRUE(writeStorageMarker(directory.path(), storageId));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto remoteExecutor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const remoteProbe = remoteExecutor.get();
    auto remoteAdapter = std::make_unique<worker::client::RemoteExportExecutionAdapter>(std::move(remoteExecutor));
    core::orchestration::ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = 1;
    configuration.remoteSlotLimit = 1;
    configuration.enforceLocalResourceReserve = false;
    core::orchestration::ExportOrchestrator orchestrator(
        std::make_unique<RecordingExportPipeline>(), std::move(remoteAdapter), nullptr, configuration);
    const core::orchestration::EditorState editorState = makeEditorState(sourcePath);
    ExportDialog dialog(orchestrator, editorState, settings);

    QLineEdit* outputPathEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit"));
    QComboBox* executionCombo = dialog.findChild<QComboBox*>(QStringLiteral("exportExecutionCombo"));
    QLineEdit* hostEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportRemoteHostEdit"));
    QSpinBox* portSpinBox = dialog.findChild<QSpinBox*>(QStringLiteral("exportRemotePortSpinBox"));
    QPushButton* exportButton = dialog.findChild<QPushButton*>(QStringLiteral("exportStartButton"));
    QLabel* statusLabel = dialog.findChild<QLabel*>(QStringLiteral("exportStatusLabel"));
    ASSERT_NE(nullptr, outputPathEdit);
    ASSERT_NE(nullptr, executionCombo);
    ASSERT_NE(nullptr, hostEdit);
    ASSERT_NE(nullptr, portSpinBox);
    EXPECT_EQ(nullptr, dialog.findChild<QLineEdit*>(QStringLiteral("exportExpectedSourceStorageIdEdit")));
    EXPECT_EQ(nullptr, dialog.findChild<QLineEdit*>(QStringLiteral("exportExpectedOutputStorageIdEdit")));
    ASSERT_NE(nullptr, exportButton);
    ASSERT_NE(nullptr, statusLabel);

    outputPathEdit->setText(outputPath);
    executionCombo->setCurrentIndex(executionCombo->findData(static_cast<int>(settings::ExportExecutionMode::Remote)));
    hostEdit->setText(QStringLiteral("192.0.2.42"));
    portSpinBox->setValue(49200);
    QEventLoop eventLoop;
    QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
    QObject::connect(
        &orchestrator, &core::orchestration::ExportOrchestrator::exportCompleted, &eventLoop, &QEventLoop::quit);

    exportButton->click();
    eventLoop.exec();

    const std::optional<worker::client::RemoteRenderEndpoint> endpoint = remoteProbe->endpoint();
    const std::optional<worker::client::RemoteRenderRequest> request = remoteProbe->request();
    ASSERT_TRUE(endpoint.has_value());
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(QStringLiteral("192.0.2.42"), endpoint->host);
    EXPECT_EQ(49200, endpoint->port);
    EXPECT_EQ(QStringLiteral("current.arw"), request->sourceRelativePath);
    EXPECT_EQ(QStringLiteral("remote-output.jpg"), request->outputRelativePath);
    EXPECT_EQ(editorState.params, request->developParams);
    EXPECT_TRUE(statusLabel->text().contains(QFileInfo(outputPath).absoluteFilePath()));
    EXPECT_TRUE(statusLabel->text().contains(QStringLiteral("completed"), Qt::CaseInsensitive));
    EXPECT_FALSE(exportButton->isEnabled());

    settings::ExportSettings exportSettings(settings);
    const settings::ExportExecutionDefaults restored = exportSettings.loadExecutionDefaults();
    EXPECT_EQ(settings::ExportExecutionMode::Remote, restored.mode);
    EXPECT_EQ(QStringLiteral("192.0.2.42"), restored.remoteHost);
    EXPECT_EQ(49200, restored.remotePort);
    EXPECT_EQ(storageId.toString(QUuid::WithoutBraces), restored.expectedSourceStorageId);
    EXPECT_EQ(storageId.toString(QUuid::WithoutBraces), restored.expectedOutputStorageId);
}

TEST(ExportDialogTest, AutoWithoutStorageMarkerFallsBackToLocalInsteadOfBlockingInUi)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("current.arw"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("auto-output.jpg"));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto pipeline = std::make_unique<RecordingExportPipeline>();
    RecordingExportPipeline* const localProbe = pipeline.get();
    auto remoteExecutor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const remoteProbe = remoteExecutor.get();
    auto remoteAdapter = std::make_unique<worker::client::RemoteExportExecutionAdapter>(std::move(remoteExecutor));
    core::orchestration::ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = 1;
    configuration.remoteSlotLimit = 1;
    configuration.enforceLocalResourceReserve = false;
    core::orchestration::ExportOrchestrator orchestrator(
        std::move(pipeline), std::move(remoteAdapter), nullptr, configuration);
    ExportDialog dialog(orchestrator, makeEditorState(sourcePath), settings);

    QLineEdit* outputPathEdit = dialog.findChild<QLineEdit*>(QStringLiteral("exportOutputPathEdit"));
    QComboBox* executionCombo = dialog.findChild<QComboBox*>(QStringLiteral("exportExecutionCombo"));
    QPushButton* exportButton = dialog.findChild<QPushButton*>(QStringLiteral("exportStartButton"));
    QLabel* statusLabel = dialog.findChild<QLabel*>(QStringLiteral("exportStatusLabel"));
    ASSERT_NE(nullptr, outputPathEdit);
    ASSERT_NE(nullptr, executionCombo);
    ASSERT_NE(nullptr, exportButton);
    ASSERT_NE(nullptr, statusLabel);

    outputPathEdit->setText(outputPath);
    executionCombo->setCurrentIndex(executionCombo->findData(static_cast<int>(settings::ExportExecutionMode::Auto)));
    QEventLoop eventLoop;
    QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
    QObject::connect(
        &orchestrator, &core::orchestration::ExportOrchestrator::exportCompleted, &eventLoop, &QEventLoop::quit);

    exportButton->click();
    eventLoop.exec();

    ASSERT_TRUE(localProbe->request().has_value());
    EXPECT_FALSE(remoteProbe->request().has_value());
    EXPECT_TRUE(statusLabel->text().contains(QStringLiteral("completed"), Qt::CaseInsensitive));
    settings::ExportSettings exportSettings(settings);
    EXPECT_EQ(settings::ExportExecutionMode::Auto, exportSettings.loadExecutionDefaults().mode);
}

}  // namespace
}  // namespace flexraw::ui::export_
