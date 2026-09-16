#include <filesystem>
#include <string>
#include <system_error>

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_orchestrator.h"
#include "catalog_session_orchestrator.h"
#include "editor_client.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

class TestEditorClient final : public client::IEditorClient
{
public:
    // 목적: test가 지정한 authoritative Editor snapshot 반환
    // 입력: 없음
    // 출력: current test snapshot
    [[nodiscard]] client::EditorSnapshot editorSnapshot() const override
    {
        return snapshot;
    }

    // 목적: Catalog Session test에서 Editor source activation interface 충족
    // 입력: command: 선택할 source metadata
    // 출력: source가 선택된 test snapshot
    [[nodiscard]] client::EditorResult activateSource(const client::ActivateEditorSourceCommand& command) override
    {
        snapshot.hasSelection = true;
        snapshot.source =
            client::EditorSourceSnapshot{command.sourceLocator, command.extension, command.displayName, command.kind};
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 Editor selection command interface 충족
    // 입력: command: 선택 identity
    // 출력: 선택된 test snapshot
    [[nodiscard]] client::EditorResult selectPhoto(const client::SelectEditorPhotoCommand& command) override
    {
        snapshot.hasSelection = true;
        snapshot.photoId = command.photoId;
        return client::EditorResult::success(snapshot);
    }

    // 목적: session transition이 clean Editor selection을 정리했는지 기록
    // 입력: 없음
    // 출력: selection과 dirty/Adjustment가 제거된 snapshot
    [[nodiscard]] client::EditorResult clearEditorSelection() override
    {
        ++clearCount;
        snapshot = {};
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 Editor update interface 충족
    // 입력: command: 적용할 Develop parameter
    // 출력: parameter와 dirty state가 반영된 snapshot
    [[nodiscard]] client::EditorResult updateDevelopParams(const client::UpdateDevelopParamsCommand& command) override
    {
        snapshot.params = command.params;
        snapshot.dirty = true;
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 active Adjustment state 구성
    // 입력: 없음
    // 출력: Adjustment가 활성화된 snapshot
    [[nodiscard]] client::EditorResult beginAdjustment() override
    {
        snapshot.adjustmentActive = true;
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 active Adjustment state 종료
    // 입력: 없음
    // 출력: Adjustment가 종료된 snapshot
    [[nodiscard]] client::EditorResult endAdjustment() override
    {
        snapshot.adjustmentActive = false;
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 미사용 undo interface 충족
    // 입력: 없음
    // 출력: 변경하지 않은 snapshot
    [[nodiscard]] client::EditorResult undoDevelop() override
    {
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 미사용 redo interface 충족
    // 입력: 없음
    // 출력: 변경하지 않은 snapshot
    [[nodiscard]] client::EditorResult redoDevelop() override
    {
        return client::EditorResult::success(snapshot);
    }

    // 목적: Catalog Session test에서 dirty state를 저장된 상태로 전환
    // 입력: 없음
    // 출력: dirty가 해제된 snapshot
    [[nodiscard]] client::EditorResult saveDevelopState() override
    {
        snapshot.dirty = false;
        return client::EditorResult::success(snapshot);
    }

    client::EditorSnapshot snapshot;
    int clearCount{0};
};

// 목적: Qt path를 byte 길이가 보존된 UTF-8 client path로 변환
// 입력: path: test temporary Catalog path
// 출력: Qt-free command path
[[nodiscard]] std::string toClientPath(const QString& path)
{
    const QByteArray utf8 = path.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: existing-open test에 사용할 빈 Catalog file을 생성하고 connection 종료
// 입력: path: 생성할 Catalog path
// 출력: schema migration까지 완료되면 true
[[nodiscard]] bool createCatalogFile(const QString& path)
{
    catalog::CatalogDatabaseOpenResult opened = catalog::CatalogDatabase::open(path);
    return opened.hasValue();
}

// 목적: 같은 기존 Catalog를 가리키는 별도 locator를 hard link로 생성
// 입력: existingPath/linkPath: 원본 Catalog와 생성할 alias 경로
// 출력: hard link 생성 성공 여부
[[nodiscard]] bool createHardLink(const QString& existingPath, const QString& linkPath)
{
    std::error_code error;
    std::filesystem::create_hard_link(
        QFileInfo(existingPath).filesystemAbsoluteFilePath(), QFileInfo(linkPath).filesystemAbsoluteFilePath(), error);
    return !error;
}

TEST(CatalogSessionOrchestratorTest, CreatesClosesAndOpensCatalogThroughQtFreeContract)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("새-목록.flexraw-catalog"));
    CatalogOrchestrator catalogOrchestrator;
    TestEditorClient editorClient;
    CatalogSessionOrchestrator sessionOrchestrator(catalogOrchestrator, editorClient);
    client::ICatalogSessionClient& sessionClient = sessionOrchestrator;

    const client::CatalogSessionResult created = sessionClient.openCatalog(
        {toClientPath(catalogPath), client::CatalogOpenMode::CreateNew, client::CatalogReplacementPolicy::Reject});

    ASSERT_TRUE(created.hasValue());
    EXPECT_TRUE(created.value().isOpen);
    EXPECT_EQ(toClientPath(QFileInfo(catalogPath).absoluteFilePath()), created.value().catalogPath);
    const client::CatalogSessionResult sameCatalog = sessionClient.openCatalog(
        {toClientPath(catalogPath), client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});
    ASSERT_TRUE(sameCatalog.hasValue());
    EXPECT_EQ(created.value(), sameCatalog.value());
    const client::CatalogSessionResult closed = sessionClient.closeCatalog();
    ASSERT_TRUE(closed.hasValue());
    EXPECT_FALSE(closed.value().isOpen);
    EXPECT_TRUE(closed.value().catalogPath.empty());
    const client::CatalogSessionResult closedAgain = sessionClient.closeCatalog();
    ASSERT_TRUE(closedAgain.hasValue());
    EXPECT_EQ(closed.value(), closedAgain.value());
    const client::CatalogSessionResult reopened = sessionClient.openCatalog(
        {toClientPath(catalogPath), client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});
    ASSERT_TRUE(reopened.hasValue());
    EXPECT_EQ(created.value(), reopened.value());
}

TEST(CatalogSessionOrchestratorTest, EnforcesExplicitCreateAndOpenIntentBeforeMutation)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString existingPath = QDir(directory.path()).filePath(QStringLiteral("existing.flexraw-catalog"));
    const QString missingPath = QDir(directory.path()).filePath(QStringLiteral("missing.flexraw-catalog"));
    ASSERT_TRUE(createCatalogFile(existingPath));
    CatalogOrchestrator catalogOrchestrator;
    TestEditorClient editorClient;
    CatalogSessionOrchestrator sessionClient(catalogOrchestrator, editorClient);

    const client::CatalogSessionResult empty = sessionClient.openCatalog(
        {{}, client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});
    const client::CatalogSessionResult missing = sessionClient.openCatalog(
        {toClientPath(missingPath), client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});
    const client::CatalogSessionResult duplicate = sessionClient.openCatalog(
        {toClientPath(existingPath), client::CatalogOpenMode::CreateNew, client::CatalogReplacementPolicy::Reject});

    ASSERT_TRUE(empty.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, empty.error().code);
    ASSERT_TRUE(missing.hasError());
    EXPECT_EQ(client::ClientErrorCode::NotFound, missing.error().code);
    EXPECT_FALSE(QFileInfo::exists(missingPath));
    ASSERT_TRUE(duplicate.hasError());
    EXPECT_EQ(client::ClientErrorCode::Conflict, duplicate.error().code);
    EXPECT_FALSE(sessionClient.catalogSnapshot().isOpen);
}

TEST(CatalogSessionOrchestratorTest, TreatsExistingHardLinkAliasAsTheActiveCatalog)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("identity.flexraw-catalog"));
    const QString aliasPath = QDir(directory.path()).filePath(QStringLiteral("identity-alias.flexraw-catalog"));
    ASSERT_TRUE(createCatalogFile(catalogPath));
    if (!createHardLink(catalogPath, aliasPath))
    {
        GTEST_SKIP() << "The test filesystem does not support hard links.";
    }
    CatalogOrchestrator catalogOrchestrator;
    TestEditorClient editorClient;
    CatalogSessionOrchestrator sessionClient(catalogOrchestrator, editorClient);
    const client::CatalogSessionResult opened = sessionClient.openCatalog(
        {toClientPath(catalogPath), client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});
    ASSERT_TRUE(opened.hasValue());

    const client::CatalogSessionResult sameCatalog = sessionClient.openCatalog(
        {toClientPath(aliasPath), client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});

    ASSERT_TRUE(sameCatalog.hasValue());
    EXPECT_EQ(opened.value(), sameCatalog.value());
    EXPECT_EQ(0, editorClient.clearCount);
}

TEST(CatalogSessionOrchestratorTest, RequiresExplicitReplacementAndPreservesPreviousOnOpenFailure)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstPath = QDir(directory.path()).filePath(QStringLiteral("first.flexraw-catalog"));
    const QString secondPath = QDir(directory.path()).filePath(QStringLiteral("second.flexraw-catalog"));
    const QString invalidPath = QDir(directory.path()).filePath(QStringLiteral("invalid.flexraw-catalog"));
    ASSERT_TRUE(createCatalogFile(firstPath));
    ASSERT_TRUE(createCatalogFile(secondPath));
    QFile invalidFile(invalidPath);
    ASSERT_TRUE(invalidFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(invalidFile.write("not-a-sqlite-catalog"), 20);
    invalidFile.close();
    CatalogOrchestrator catalogOrchestrator;
    TestEditorClient editorClient;
    CatalogSessionOrchestrator sessionClient(catalogOrchestrator, editorClient);
    ASSERT_TRUE(sessionClient
                    .openCatalog({toClientPath(firstPath),
                                  client::CatalogOpenMode::OpenExisting,
                                  client::CatalogReplacementPolicy::Reject})
                    .hasValue());

    const client::CatalogSessionResult rejected = sessionClient.openCatalog(
        {toClientPath(secondPath), client::CatalogOpenMode::OpenExisting, client::CatalogReplacementPolicy::Reject});
    ASSERT_TRUE(rejected.hasError());
    EXPECT_EQ(client::ClientErrorCode::Conflict, rejected.error().code);
    EXPECT_EQ(toClientPath(QFileInfo(firstPath).absoluteFilePath()), sessionClient.catalogSnapshot().catalogPath);

    const client::CatalogSessionResult failed =
        sessionClient.openCatalog({toClientPath(invalidPath),
                                   client::CatalogOpenMode::OpenExisting,
                                   client::CatalogReplacementPolicy::ReplaceCurrent});
    ASSERT_TRUE(failed.hasError());
    EXPECT_EQ(client::ClientErrorCode::DatabaseError, failed.error().code);
    EXPECT_TRUE(sessionClient.catalogSnapshot().isOpen);
    EXPECT_EQ(toClientPath(QFileInfo(firstPath).absoluteFilePath()), sessionClient.catalogSnapshot().catalogPath);

    const client::CatalogSessionResult replaced =
        sessionClient.openCatalog({toClientPath(secondPath),
                                   client::CatalogOpenMode::OpenExisting,
                                   client::CatalogReplacementPolicy::ReplaceCurrent});
    ASSERT_TRUE(replaced.hasValue());
    EXPECT_EQ(toClientPath(QFileInfo(secondPath).absoluteFilePath()), replaced.value().catalogPath);
}

TEST(CatalogSessionOrchestratorTest, BlocksDirtyOrActiveAdjustmentTransitionWithoutLosingState)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstPath = QDir(directory.path()).filePath(QStringLiteral("first.flexraw-catalog"));
    const QString secondPath = QDir(directory.path()).filePath(QStringLiteral("second.flexraw-catalog"));
    ASSERT_TRUE(createCatalogFile(firstPath));
    ASSERT_TRUE(createCatalogFile(secondPath));
    CatalogOrchestrator catalogOrchestrator;
    TestEditorClient editorClient;
    CatalogSessionOrchestrator sessionClient(catalogOrchestrator, editorClient);
    ASSERT_TRUE(sessionClient
                    .openCatalog({toClientPath(firstPath),
                                  client::CatalogOpenMode::OpenExisting,
                                  client::CatalogReplacementPolicy::Reject})
                    .hasValue());
    editorClient.snapshot.hasSelection = true;
    editorClient.snapshot.dirty = true;

    const client::CatalogSessionResult closeBlocked = sessionClient.closeCatalog();
    const client::CatalogSessionResult replaceBlocked =
        sessionClient.openCatalog({toClientPath(secondPath),
                                   client::CatalogOpenMode::OpenExisting,
                                   client::CatalogReplacementPolicy::ReplaceCurrent});

    ASSERT_TRUE(closeBlocked.hasError());
    EXPECT_EQ(client::ClientErrorCode::Conflict, closeBlocked.error().code);
    ASSERT_TRUE(replaceBlocked.hasError());
    EXPECT_EQ(client::ClientErrorCode::Conflict, replaceBlocked.error().code);
    EXPECT_EQ(0, editorClient.clearCount);
    EXPECT_EQ(toClientPath(QFileInfo(firstPath).absoluteFilePath()), sessionClient.catalogSnapshot().catalogPath);

    editorClient.snapshot.dirty = false;
    editorClient.snapshot.adjustmentActive = true;
    const client::CatalogSessionResult adjustmentBlocked = sessionClient.closeCatalog();
    ASSERT_TRUE(adjustmentBlocked.hasError());
    EXPECT_EQ(client::ClientErrorCode::Conflict, adjustmentBlocked.error().code);
    EXPECT_EQ(0, editorClient.clearCount);
    EXPECT_EQ(toClientPath(QFileInfo(firstPath).absoluteFilePath()), sessionClient.catalogSnapshot().catalogPath);

    editorClient.snapshot.adjustmentActive = false;
    const client::CatalogSessionResult replaced =
        sessionClient.openCatalog({toClientPath(secondPath),
                                   client::CatalogOpenMode::OpenExisting,
                                   client::CatalogReplacementPolicy::ReplaceCurrent});
    ASSERT_TRUE(replaced.hasValue());
    EXPECT_EQ(1, editorClient.clearCount);
    EXPECT_FALSE(editorClient.snapshot.hasSelection);
}

}  // namespace
}  // namespace flexraw::core::orchestration
