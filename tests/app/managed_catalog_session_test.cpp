#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "editor_orchestrator.h"
#include "managed_catalog_session.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"

namespace flexraw::app
{
namespace
{

class DormantPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: app persistence test에서 실행될 필요가 없는 preview 요청을 즉시 종료
    // 입력: request: 미사용, tier: 미사용, cancellationToken: 미사용
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("App persistence test does not render previews.")});
    }
};

// 목적: Qt SQL과 settings test에 필요한 단일 QCoreApplication instance 보장
// 입력: 없음
// 출력: 없음
void ensureCoreApplication()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }

    static int argumentCount = 1;
    static char applicationName[] = "flexraw_app_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
}

// 목적: test별 격리된 INI settings ownership 생성
// 입력: settingsPath: QTemporaryDir 아래 설정 파일 경로
// 출력: ManagedCatalogSession에 전달할 settings 객체
[[nodiscard]] std::unique_ptr<QSettings> makeSettings(const QString& settingsPath)
{
    return std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
}

// 목적: shutdown persistence가 선택할 custom Catalog를 만들고 active session으로 전환
// 입력: session: 현재 기본 Catalog owner, catalogPath: 생성할 custom Catalog 경로
// 출력: custom Catalog open 성공 여부
[[nodiscard]] bool switchToCatalog(ManagedCatalogSession& session, const QString& catalogPath)
{
    (void)session.orchestrator().closeCatalog();
    return session.orchestrator().openCatalog(catalogPath).hasValue();
}

// 목적: corruption recovery test에 사용할 비-SQLite file content 기록
// 입력: catalogPath: 덮어쓸 닫힌 Catalog, content: 기록할 bytes
// 출력: 전체 content를 기록했으면 true
[[nodiscard]] bool overwriteFile(const QString& catalogPath, const QByteArray& content)
{
    QFile file(catalogPath);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

// 목적: recovery 과정이 기존 corrupt file bytes를 바꾸지 않았는지 확인
// 입력: catalogPath: 읽을 file 경로
// 출력: file 전체 bytes, 읽지 못하면 빈 배열
[[nodiscard]] QByteArray readFile(const QString& catalogPath)
{
    QFile file(catalogPath);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

TEST(ManagedCatalogSessionTest, CreatesAndReopensDefaultCatalog)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString settingsPath = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultCatalogPath =
        QDir(directory.path()).filePath(QStringLiteral("managed/Flexraw.flexraw-catalog"));

    {
        ManagedCatalogSession session(makeSettings(settingsPath), defaultCatalogPath);
        const ManagedCatalogStartupState& startup = session.startupState();
        ASSERT_TRUE(startup.session.isOpen);
        EXPECT_EQ(startup.session.catalogPath, QFileInfo(defaultCatalogPath).absoluteFilePath());
        EXPECT_FALSE(startup.usedFallback);
        EXPECT_FALSE(startup.recoveryIssue.has_value());
        EXPECT_FALSE(startup.fatalError.has_value());
        EXPECT_TRUE(QFileInfo::exists(defaultCatalogPath));
    }

    ManagedCatalogSession reopened(makeSettings(settingsPath), defaultCatalogPath);
    const ManagedCatalogStartupState& startup = reopened.startupState();
    ASSERT_TRUE(startup.session.isOpen);
    EXPECT_EQ(startup.requestedCatalogPath, QFileInfo(defaultCatalogPath).absoluteFilePath());
    EXPECT_FALSE(startup.usedFallback);
    EXPECT_FALSE(startup.recoveryIssue.has_value());
}

TEST(ManagedCatalogSessionTest, RemembersLastActiveCatalogAcrossCleanShutdown)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString settingsPath = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultCatalogPath = QDir(directory.path()).filePath(QStringLiteral("managed.flexraw-catalog"));
    const QString customCatalogPath = QDir(directory.path()).filePath(QStringLiteral("custom.flexraw-catalog"));

    {
        ManagedCatalogSession session(makeSettings(settingsPath), defaultCatalogPath);
        ASSERT_TRUE(switchToCatalog(session, customCatalogPath));
    }

    ManagedCatalogSession reopened(makeSettings(settingsPath), defaultCatalogPath);
    ASSERT_TRUE(reopened.startupState().session.isOpen);
    EXPECT_EQ(reopened.startupState().session.catalogPath, QFileInfo(customCatalogPath).absoluteFilePath());
    EXPECT_FALSE(reopened.startupState().usedFallback);
}

TEST(ManagedCatalogSessionTest, RestoresActivatedEditorStateAfterApplicationRestart)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString settingsPath = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultCatalogPath = QDir(directory.path()).filePath(QStringLiteral("managed.flexraw-catalog"));
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("activated.jpg"));
    ASSERT_TRUE(overwriteFile(sourcePath, QByteArray("activated-source")));
    const core::catalog::CatalogEntry entry{
        core::types::makeFileDescriptor(QFileInfo(sourcePath), core::types::SupportedFileKind::RasterImage),
        core::types::FileScanStatus::Ready,
    };
    core::types::PhotoId photoId;
    core::types::DevelopParams editedParams;
    editedParams.exposureEv = 1.2F;
    editedParams.contrast = 0.15F;

    {
        ManagedCatalogSession session(makeSettings(settingsPath), defaultCatalogPath);
        auto pipeline = std::make_unique<DormantPreviewPipeline>();
        core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(pipeline));
        core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, session.orchestrator());
        const core::orchestration::EditorStateResult activated =
            editorOrchestrator.activatePhoto(entry, QSize{640, 480});
        ASSERT_TRUE(activated.hasValue());
        photoId = activated.value().photo.photoId;
        ASSERT_TRUE(core::types::isValidPhotoId(photoId));
        EXPECT_TRUE(activated.value().photo.transientKey.isEmpty());
        ASSERT_TRUE(editorOrchestrator.updateDevelopParams(editedParams, QSize{640, 480}));
        ASSERT_TRUE(editorOrchestrator.saveCurrentPhoto().hasValue());
    }

    ManagedCatalogSession reopened(makeSettings(settingsPath), defaultCatalogPath);
    auto pipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, reopened.orchestrator());
    const core::orchestration::EditorStateResult restored =
        editorOrchestrator.selectCatalogPhoto(photoId, QSize{640, 480});

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(photoId.value, restored.value().photo.photoId.value);
    EXPECT_EQ(editedParams, restored.value().params);
    EXPECT_EQ(1U, restored.value().persistedRevision);
    EXPECT_FALSE(restored.value().dirty);
}

TEST(ManagedCatalogSessionTest, FallsBackWhenLastActiveCatalogIsMissing)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString settingsPath = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultCatalogPath = QDir(directory.path()).filePath(QStringLiteral("managed.flexraw-catalog"));
    const QString missingCatalogPath = QDir(directory.path()).filePath(QStringLiteral("missing.flexraw-catalog"));

    {
        ManagedCatalogSession session(makeSettings(settingsPath), defaultCatalogPath);
        ASSERT_TRUE(switchToCatalog(session, missingCatalogPath));
    }
    ASSERT_TRUE(QFile::remove(missingCatalogPath));

    ManagedCatalogSession recovered(makeSettings(settingsPath), defaultCatalogPath);
    const ManagedCatalogStartupState& startup = recovered.startupState();
    ASSERT_TRUE(startup.session.isOpen);
    EXPECT_EQ(startup.session.catalogPath, QFileInfo(defaultCatalogPath).absoluteFilePath());
    EXPECT_TRUE(startup.usedFallback);
    ASSERT_TRUE(startup.recoveryIssue.has_value());
    EXPECT_EQ(startup.recoveryIssue->catalogPath, QFileInfo(missingCatalogPath).absoluteFilePath());
    EXPECT_EQ(startup.recoveryIssue->error.code, core::types::ErrorCode::NotFound);
    EXPECT_FALSE(startup.fatalError.has_value());
    EXPECT_FALSE(QFileInfo::exists(missingCatalogPath));

    QSettings persistedSettings(settingsPath, QSettings::IniFormat);
    persistedSettings.beginGroup(QStringLiteral("catalog"));
    EXPECT_EQ(persistedSettings.value(QStringLiteral("recovery/unavailablePath")).toString(),
              QFileInfo(missingCatalogPath).absoluteFilePath());
    EXPECT_EQ(persistedSettings.value(QStringLiteral("recovery/errorCode")).toInt(),
              static_cast<int>(core::types::ErrorCode::NotFound));
    persistedSettings.endGroup();
}

TEST(ManagedCatalogSessionTest, FallsBackWithoutReplacingCorruptLastActiveCatalog)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString settingsPath = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultCatalogPath = QDir(directory.path()).filePath(QStringLiteral("managed.flexraw-catalog"));
    const QString corruptCatalogPath = QDir(directory.path()).filePath(QStringLiteral("corrupt.flexraw-catalog"));
    const QByteArray corruptContent("not-a-sqlite-catalog");

    {
        ManagedCatalogSession session(makeSettings(settingsPath), defaultCatalogPath);
        ASSERT_TRUE(switchToCatalog(session, corruptCatalogPath));
    }
    ASSERT_TRUE(overwriteFile(corruptCatalogPath, corruptContent));

    ManagedCatalogSession recovered(makeSettings(settingsPath), defaultCatalogPath);
    const ManagedCatalogStartupState& startup = recovered.startupState();
    ASSERT_TRUE(startup.session.isOpen);
    EXPECT_EQ(startup.session.catalogPath, QFileInfo(defaultCatalogPath).absoluteFilePath());
    EXPECT_TRUE(startup.usedFallback);
    ASSERT_TRUE(startup.recoveryIssue.has_value());
    EXPECT_EQ(startup.recoveryIssue->catalogPath, QFileInfo(corruptCatalogPath).absoluteFilePath());
    EXPECT_EQ(startup.recoveryIssue->error.code, core::types::ErrorCode::DatabaseError);
    EXPECT_FALSE(startup.fatalError.has_value());
    EXPECT_EQ(readFile(corruptCatalogPath), corruptContent);
}

TEST(ManagedCatalogSessionTest, ReportsFatalErrorWithoutOverwritingCorruptDefaultCatalog)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString settingsPath = QDir(directory.path()).filePath(QStringLiteral("settings.ini"));
    const QString defaultCatalogPath = QDir(directory.path()).filePath(QStringLiteral("managed.flexraw-catalog"));
    const QByteArray corruptContent("broken-default-catalog");

    {
        ManagedCatalogSession session(makeSettings(settingsPath), defaultCatalogPath);
        ASSERT_TRUE(session.startupState().session.isOpen);
    }
    ASSERT_TRUE(overwriteFile(defaultCatalogPath, corruptContent));

    ManagedCatalogSession failed(makeSettings(settingsPath), defaultCatalogPath);
    const ManagedCatalogStartupState& startup = failed.startupState();
    EXPECT_FALSE(startup.session.isOpen);
    EXPECT_FALSE(startup.usedFallback);
    ASSERT_TRUE(startup.recoveryIssue.has_value());
    ASSERT_TRUE(startup.fatalError.has_value());
    EXPECT_EQ(startup.fatalError->code, core::types::ErrorCode::DatabaseError);
    EXPECT_EQ(readFile(defaultCatalogPath), corruptContent);
}

}  // namespace
}  // namespace flexraw::app
