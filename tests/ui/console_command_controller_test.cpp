#include <algorithm>
#include <memory>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "console_command_controller.h"
#include "console_mode_widget.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_folder_import_event_adapter.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::cli
{
namespace
{

class EmptyWorkerProfileClient final : public core::client::IWorkerProfileClient
{
public:
    // 목적: Local-only console test에 빈 Worker profile 목록 제공
    // 입력: 없음
    // 출력: 빈 application-level profile snapshot
    [[nodiscard]] core::client::WorkerProfileListResult listWorkerProfiles() override
    {
        return core::client::WorkerProfileListResult::success({});
    }

    // 목적: test double에서 지원하지 않는 profile 생성 거부
    // 입력: command: 미사용 create command
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileResult createWorkerProfile(
        const core::client::CreateWorkerProfileCommand&) override
    {
        return unsupported();
    }

    // 목적: test double에서 지원하지 않는 profile 갱신 거부
    // 입력: command: 미사용 update command
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileResult updateWorkerProfile(
        const core::client::UpdateWorkerProfileCommand&) override
    {
        return unsupported();
    }

    // 목적: test double에서 지원하지 않는 profile 제거 거부
    // 입력: id: 미사용 profile identity
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileRemoveResult removeWorkerProfile(
        const core::client::WorkerProfileId&) override
    {
        return core::client::WorkerProfileRemoveResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Worker profile mutation is unavailable in this test."});
    }

    // 목적: test double에서 지원하지 않는 enabled 변경 거부
    // 입력: command: 미사용 enabled command
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileResult setWorkerProfileEnabled(
        const core::client::SetWorkerProfileEnabledCommand&) override
    {
        return unsupported();
    }

private:
    // 목적: 반복되는 unsupported profile mutation result 생성
    // 입력: 없음
    // 출력: InvalidArgument WorkerProfileResult
    [[nodiscard]] static core::client::WorkerProfileResult unsupported()
    {
        return core::client::WorkerProfileResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Worker profile mutation is unavailable in this test."});
    }
};

class ExportDefaultsClientStub final : public core::client::IExportDefaultsClient
{
public:
    // 목적: console test에 mutable application-wide Export default snapshot 제공
    // 입력: 없음
    // 출력: current in-memory snapshot
    [[nodiscard]] core::client::ExportDefaultsResult exportDefaults() const override
    {
        return core::client::ExportDefaultsResult::success(m_defaults);
    }

    // 목적: 성공한 console Export raster option을 in-memory snapshot에 기록
    // 입력: options: 저장할 raster option
    // 출력: 같은 저장값
    [[nodiscard]] core::client::ExportRasterDefaultsResult saveRasterExportDefaults(
        const core::client::ExportRasterOptions& options) override
    {
        m_defaults.rasterOptions = options;
        return core::client::ExportRasterDefaultsResult::success(options);
    }

    // 목적: accepted console Export placement를 in-memory snapshot에 기록
    // 입력: defaults: 저장할 placement와 optional Worker identity
    // 출력: 같은 저장값
    [[nodiscard]] core::client::ExportExecutionDefaultsResult saveExportExecutionDefaults(
        const core::client::ExportExecutionDefaults& defaults) override
    {
        m_defaults.execution = defaults;
        return core::client::ExportExecutionDefaultsResult::success(defaults);
    }

private:
    core::client::ExportDefaultsSnapshot m_defaults;
};

// 목적: widget test에서 입력 field의 returnPressed signal 호출
// 입력: input: command 문자열이 설정된 line edit
// 출력: signal 호출 성공 여부
[[nodiscard]] bool submitCommand(QLineEdit& input)
{
    return QMetaObject::invokeMethod(&input, "returnPressed", Qt::DirectConnection);
}

TEST(ConsoleCommandControllerTest, CompletesImportOnControllerThread)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile photo(QDir(directory.path()).filePath(QStringLiteral("photo.png")));
    ASSERT_TRUE(photo.open(QIODevice::WriteOnly));
    photo.close();
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.db"));
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_FALSE(catalogOrchestrator.openCatalog(catalogPath).hasError());
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    settings::QtExportSettingsAdapter exportDefaults(settings);
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QEventLoop eventLoop;
    QString output;
    QThread* completionThread = nullptr;
    QObject::connect(&controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) {
        output = line;
        completionThread = QThread::currentThread();
        eventLoop.quit();
    });
    const QString command =
        QStringLiteral("catalog import --catalog \"%1\" --folder \"%2\"").arg(catalogPath, directory.path());

    ASSERT_TRUE(controller.tryExecute(command));
    EXPECT_TRUE(controller.isBusy());
    if (output.isEmpty())
    {
        QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
        eventLoop.exec();
    }

    ASSERT_FALSE(output.isEmpty());
    EXPECT_FALSE(controller.isBusy());
    EXPECT_EQ(controller.thread(), completionThread);
    EXPECT_TRUE(output.contains(QStringLiteral("scanned 1, stored 1")));
    const core::orchestration::CatalogPhotoPageResult photos =
        catalogOrchestrator.queryPhotos(core::catalog::CatalogPhotoPageRequest{});
    ASSERT_FALSE(photos.hasError());
    ASSERT_EQ(1, photos.value().photos.size());
    EXPECT_EQ(QStringLiteral("photo.png"), photos.value().photos.constFirst().displayName);
}

TEST(ConsoleCommandControllerTest, RejectsImportWhenRequestedCatalogIsNotActive)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString activeCatalogPath = QDir(directory.path()).filePath(QStringLiteral("active.db"));
    const QString otherCatalogPath = QDir(directory.path()).filePath(QStringLiteral("other.db"));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_FALSE(catalogOrchestrator.openCatalog(activeCatalogPath).hasError());
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    ExportDefaultsClientStub exportDefaults;
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QString output;
    QObject::connect(
        &controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) { output = line; });

    const QString command =
        QStringLiteral("catalog import --catalog \"%1\" --folder \"%2\"").arg(otherCatalogPath, directory.path());

    EXPECT_TRUE(controller.tryExecute(command));
    EXPECT_FALSE(controller.isBusy());
    EXPECT_TRUE(output.contains(QStringLiteral("Open the requested catalog")));
    EXPECT_FALSE(QFile::exists(otherCatalogPath));
}

TEST(ConsoleCommandControllerTest, RejectsImportResultAfterActiveCatalogChanges)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile photo(QDir(directory.path()).filePath(QStringLiteral("photo.png")));
    ASSERT_TRUE(photo.open(QIODevice::WriteOnly));
    photo.close();
    const QString firstCatalogPath = QDir(directory.path()).filePath(QStringLiteral("first.db"));
    const QString secondCatalogPath = QDir(directory.path()).filePath(QStringLiteral("second.db"));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_FALSE(catalogOrchestrator.openCatalog(firstCatalogPath).hasError());
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    ExportDefaultsClientStub exportDefaults;
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QEventLoop eventLoop;
    QString output;
    QObject::connect(&controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) {
        output = line;
        eventLoop.quit();
    });
    const QString command =
        QStringLiteral("catalog import --catalog \"%1\" --folder \"%2\"").arg(firstCatalogPath, directory.path());

    ASSERT_TRUE(controller.tryExecute(command));
    ASSERT_TRUE(controller.isBusy());
    const core::orchestration::CatalogSessionState closed = catalogOrchestrator.closeCatalog();
    ASSERT_FALSE(closed.isOpen);
    ASSERT_FALSE(catalogOrchestrator.openCatalog(secondCatalogPath).hasError());
    QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();

    EXPECT_FALSE(controller.isBusy());
    EXPECT_TRUE(output.contains(QStringLiteral("active catalog changed")));
    const core::orchestration::CatalogPhotoPageResult photos =
        catalogOrchestrator.queryPhotos(core::catalog::CatalogPhotoPageRequest{});
    ASSERT_FALSE(photos.hasError());
    EXPECT_TRUE(photos.value().photos.isEmpty());
}

TEST(ConsoleCommandControllerTest, ReportsValidationErrorsWithoutStartingWorker)
{
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    ExportDefaultsClientStub exportDefaults;
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QString output;
    int busyChangeCount = 0;
    QObject::connect(
        &controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) { output = line; });
    QObject::connect(
        &controller, &ConsoleCommandController::busyChanged, &controller, [&busyChangeCount] { ++busyChangeCount; });

    EXPECT_TRUE(controller.tryExecute(QStringLiteral("catalog import --catalog only.db")));

    EXPECT_FALSE(output.isEmpty());
    EXPECT_EQ(0, busyChangeCount);
    EXPECT_FALSE(controller.isBusy());
}

TEST(ConsoleCommandControllerTest, CompletesRasterExportOnControllerThread)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString inputPath = QDir(directory.path()).filePath(QStringLiteral("input.png"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.png"));
    QImage inputImage(QSize(20, 10), QImage::Format_RGBA8888);
    inputImage.fill(Qt::red);
    ASSERT_TRUE(inputImage.save(inputPath));

    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    settings::QtExportSettingsAdapter exportDefaults(settings);
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QEventLoop eventLoop;
    QString output;
    QObject::connect(&controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) {
        output = line;
        eventLoop.quit();
    });

    const QString command = QStringLiteral("export raster --input \"%1\" --output \"%2\" --format png "
                                           "--color-space display-p3 --max-dimension 8")
                                .arg(inputPath, outputPath);
    ASSERT_TRUE(controller.tryExecute(command));
    if (output.isEmpty())
    {
        QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
        eventLoop.exec();
    }

    EXPECT_FALSE(controller.isBusy());
    EXPECT_TRUE(output.contains(QStringLiteral("Raster export complete:")));
    EXPECT_TRUE(QFile::exists(outputPath));
    const core::client::ExportDefaultsResult restored = exportDefaults.exportDefaults();
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::ExportOutputColorSpace::DisplayP3, restored.value().rasterOptions.outputColorSpace);
}

TEST(ConsoleCommandControllerTest, CompletesBatchExportThroughSharedOrchestrator)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString inputFolderPath = QDir(directory.path()).filePath(QStringLiteral("input"));
    const QString outputFolderPath = QDir(directory.path()).filePath(QStringLiteral("output"));
    ASSERT_TRUE(QDir().mkpath(inputFolderPath));
    ASSERT_TRUE(QDir().mkpath(outputFolderPath));
    QImage inputImage(QSize(20, 10), QImage::Format_RGBA8888);
    inputImage.fill(Qt::green);
    ASSERT_TRUE(inputImage.save(QDir(inputFolderPath).filePath(QStringLiteral("first.png"))));
    ASSERT_TRUE(inputImage.save(QDir(inputFolderPath).filePath(QStringLiteral("second.png"))));

    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    settings::QtExportSettingsAdapter exportDefaults(settings);
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QEventLoop eventLoop;
    QString output;
    QVector<int> completedProgress;
    QObject::connect(&exportOrchestrator,
                     &core::orchestration::ExportOrchestrator::exportProgressed,
                     &controller,
                     [&](const core::orchestration::ExportProgress& progress) {
                         completedProgress.push_back(progress.completedCount);
                     });
    QObject::connect(&controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) {
        output = line;
        eventLoop.quit();
    });

    const QString command = QStringLiteral("export batch --input-folder \"%1\" --output-folder \"%2\" "
                                           "--format png --workers 2 --max-dimension 8")
                                .arg(inputFolderPath, outputFolderPath);
    ASSERT_TRUE(controller.tryExecute(command));
    if (output.isEmpty())
    {
        QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
        eventLoop.exec();
    }

    EXPECT_FALSE(controller.isBusy());
    EXPECT_TRUE(output.contains(QStringLiteral("total 2, succeeded 2, failed 0")));
    ASSERT_FALSE(completedProgress.isEmpty());
    EXPECT_TRUE(std::is_sorted(completedProgress.cbegin(), completedProgress.cend()));
    EXPECT_EQ(2, completedProgress.constLast());
    EXPECT_TRUE(QFile::exists(QDir(outputFolderPath).filePath(QStringLiteral("first-png.png"))));
    EXPECT_TRUE(QFile::exists(QDir(outputFolderPath).filePath(QStringLiteral("second-png.png"))));
}

TEST(ConsoleCommandControllerTest, ReportsRawExportFailureOnControllerThread)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    ExportDefaultsClientStub exportDefaults;
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleCommandController controller(
        catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    QEventLoop eventLoop;
    QString output;
    QObject::connect(&controller, &ConsoleCommandController::outputReady, &controller, [&](const QString& line) {
        output = line;
        eventLoop.quit();
    });

    const QString command = QStringLiteral("export raw --input \"%1\" --output \"%2\" --format png")
                                .arg(QDir(directory.path()).filePath(QStringLiteral("missing.raw")),
                                     QDir(directory.path()).filePath(QStringLiteral("output.png")));
    ASSERT_TRUE(controller.tryExecute(command));
    if (output.isEmpty())
    {
        QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
        eventLoop.exec();
    }

    EXPECT_FALSE(controller.isBusy());
    EXPECT_TRUE(output.contains(QStringLiteral("Raster export failed:")));
}

TEST(ConsoleModeWidgetTest, PreservesHelpClearAndGuiCommands)
{
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    EmptyWorkerProfileClient workerProfiles;
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    ExportDefaultsClientStub exportDefaults;
    facade::QtFolderImportEventAdapter folderEventAdapter(catalogOrchestrator);
    ConsoleModeWidget widget(catalogOrchestrator, folderEventAdapter, exportAdapter, exportAdapter, exportDefaults);
    auto* input = widget.findChild<QLineEdit*>();
    auto* output = widget.findChild<QPlainTextEdit*>();
    ASSERT_NE(nullptr, input);
    ASSERT_NE(nullptr, output);
    bool exitRequested = false;
    QObject::connect(&widget, &ConsoleModeWidget::exitRequested, &widget, [&exitRequested] { exitRequested = true; });

    input->setText(QStringLiteral("help"));
    ASSERT_TRUE(submitCommand(*input));
    EXPECT_TRUE(output->toPlainText().contains(QStringLiteral("Commands: help, clear, gui")));

    input->setText(QStringLiteral("clear"));
    ASSERT_TRUE(submitCommand(*input));
    EXPECT_TRUE(output->toPlainText().isEmpty());

    input->setText(QStringLiteral("gui"));
    ASSERT_TRUE(submitCommand(*input));
    EXPECT_TRUE(exitRequested);
}

}  // namespace
}  // namespace flexraw::ui::cli
