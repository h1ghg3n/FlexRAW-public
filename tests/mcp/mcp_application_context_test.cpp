#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "catalog_entry.h"
#include "catalog_photo_client.h"
#include "catalog_photo_repository.h"
#include "catalog_project_client.h"
#include "catalog_project_repository.h"
#include "editor_client.h"
#include "mcp_application_context.h"

namespace flexraw::app::mcp
{
namespace
{

// 목적: MCP Catalog integration test에 필요한 단일 QCoreApplication instance 보장
// 입력: 없음
// 출력: 없음
void ensureCoreApplication()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "flexraw_mcp_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
}

TEST(McpApplicationContextTest, OpensExistingCatalogForHeadlessPhotoClient)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("mcp.flexraw-catalog"));
    {
        core::catalog::CatalogDatabaseOpenResult created = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(created.hasValue());
    }

    McpApplicationContext context;
    const core::client::CatalogSessionResult opened = context.openExistingCatalog(catalogPath.toStdString());
    ASSERT_TRUE(opened.hasValue());
    EXPECT_TRUE(opened.value().isOpen);

    const core::client::CatalogPhotoPageResult page =
        context.catalogPhotoClient().queryPhotoPage(core::client::CatalogPhotoPageRequest{});
    ASSERT_TRUE(page.hasValue());
    EXPECT_TRUE(page.value().photos.empty());
    const core::client::CatalogProjectListResult projects = context.catalogProjectClient().listProjects();
    ASSERT_TRUE(projects.hasValue());
    EXPECT_TRUE(projects.value().empty());
    EXPECT_FALSE(context.editorClient().editorSnapshot().hasSelection);
}

TEST(McpApplicationContextTest, RefusesMissingCatalogInsteadOfCreatingIt)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("missing.flexraw-catalog"));

    McpApplicationContext context;
    const core::client::CatalogSessionResult opened = context.openExistingCatalog(catalogPath.toStdString());
    ASSERT_TRUE(opened.hasError());
    EXPECT_EQ(opened.error().code, core::client::ClientErrorCode::NotFound);
}

TEST(McpApplicationContextTest, StandaloneProcessServesCatalogQueryUntilInputEof)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("process.flexraw-catalog"));
    {
        core::catalog::CatalogDatabaseOpenResult created = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(created.hasValue());
    }

    const QString executable = QString::fromUtf8(FLEXRAW_MCP_EXECUTABLE_PATH);
    ASSERT_TRUE(QFileInfo::exists(executable));
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--catalog"), catalogPath});
    process.start();
    ASSERT_TRUE(process.waitForStarted(5'000));
    const QByteArray transcript =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/"
        "call\",\"params\":{\"name\":\"catalog_query_photos\",\"arguments\":{}}}\n";
    ASSERT_EQ(process.write(transcript), transcript.size());
    process.closeWriteChannel();
    ASSERT_TRUE(process.waitForFinished(5'000));
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardError().isEmpty());

    const QByteArray output = process.readAllStandardOutput();
    EXPECT_TRUE(output.contains("\"id\":2"));
    EXPECT_TRUE(output.contains("\"photos\":[]"));
    EXPECT_TRUE(output.contains("\"isError\":false"));
    EXPECT_TRUE(output.contains("catalog_list_projects"));
    EXPECT_TRUE(output.contains("editor_get_state"));
    EXPECT_TRUE(output.contains("source_resolution_events"));
    EXPECT_FALSE(output.contains("editor_select_photo"));
    EXPECT_FALSE(output.contains("editor_set_exposure"));
    EXPECT_FALSE(output.contains("catalog_add_photo_to_project"));
    EXPECT_FALSE(output.contains("source_cancel_request"));
}

TEST(McpApplicationContextTest, StandaloneProcessIdempotentlyAddsProjectMembershipWithWriteOptIn)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("write.flexraw-catalog"));
    core::catalog::ProjectId projectId;
    core::types::PhotoId photoId;
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository photoRepository(*database.value());
        core::catalog::CatalogProjectRepository projectRepository(*database.value());
        const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("member.jpg"));
        const QVector<core::catalog::CatalogEntry> entries{
            {core::types::FileDescriptor{sourcePath,
                                         QStringLiteral("jpg"),
                                         QStringLiteral("member.jpg"),
                                         core::types::SupportedFileKind::RasterImage},
             core::types::FileScanStatus::Ready}};
        ASSERT_TRUE(photoRepository.upsert(entries).hasValue());
        const core::catalog::CatalogPhotoQueryResult photos =
            photoRepository.queryPage(core::catalog::CatalogPhotoPageRequest{});
        ASSERT_TRUE(photos.hasValue());
        ASSERT_EQ(photos.value().photos.size(), 1);
        photoId = photos.value().photos.front().id;
        const core::catalog::CatalogProjectRecordResult project =
            projectRepository.createProject(QStringLiteral("MCP Selection"));
        ASSERT_TRUE(project.hasValue());
        projectId = project.value().id;
    }

    const QString executable = QString::fromUtf8(FLEXRAW_MCP_EXECUTABLE_PATH);
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--catalog"), catalogPath, QStringLiteral("--allow-write")});
    process.start();
    ASSERT_TRUE(process.waitForStarted(5'000));
    const QByteArray addCall =
        QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":%1,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"catalog_add_photo_to_project\",\"arguments\":{\"project_id\":\"%2\",\"photo_id\":\"%3\"}}}\n")
            .arg(3)
            .arg(projectId.value)
            .arg(photoId.value)
            .toUtf8();
    QByteArray repeatedCall = addCall;
    repeatedCall.replace("\"id\":3", "\"id\":4");
    const QByteArray transcript =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":"
        "\"catalog_list_projects\",\"arguments\":{}}}\n" +
        addCall + repeatedCall;
    ASSERT_EQ(process.write(transcript), transcript.size());
    process.closeWriteChannel();
    ASSERT_TRUE(process.waitForFinished(5'000));
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardError().isEmpty());

    const QByteArray output = process.readAllStandardOutput();
    EXPECT_TRUE(output.contains("MCP Selection"));
    EXPECT_EQ(output.count("\"membership\""), 2);
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository photoRepository(*database.value());
        core::catalog::CatalogPhotoPageRequest request;
        request.projectId = projectId;
        const core::catalog::CatalogPhotoQueryResult page = photoRepository.queryPage(request);
        ASSERT_TRUE(page.hasValue());
        ASSERT_EQ(page.value().photos.size(), 1);
        EXPECT_EQ(page.value().photos.front().id.value, photoId.value);
    }
}

TEST(McpApplicationContextTest, StandaloneProcessSelectsAndEditsWithoutPersistingSessionExposure)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("editor.flexraw-catalog"));
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("editor-source.jpg"));
    {
        QFile source(sourcePath);
        ASSERT_TRUE(source.open(QIODevice::WriteOnly));
        ASSERT_EQ(source.write("mcp-editor-source"), 17);
    }

    core::types::PhotoId photoId;
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository photoRepository(*database.value());
        const QVector<core::catalog::CatalogEntry> entries{
            {core::types::FileDescriptor{sourcePath,
                                         QStringLiteral("jpg"),
                                         QStringLiteral("editor-source.jpg"),
                                         core::types::SupportedFileKind::RasterImage},
             core::types::FileScanStatus::Ready}};
        ASSERT_TRUE(photoRepository.upsert(entries).hasValue());
        const core::catalog::CatalogPhotoQueryResult photos =
            photoRepository.queryPage(core::catalog::CatalogPhotoPageRequest{});
        ASSERT_TRUE(photos.hasValue());
        ASSERT_EQ(photos.value().photos.size(), 1);
        photoId = photos.value().photos.front().id;
    }

    const QString executable = QString::fromUtf8(FLEXRAW_MCP_EXECUTABLE_PATH);
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--catalog"), catalogPath, QStringLiteral("--allow-write")});
    process.start();
    ASSERT_TRUE(process.waitForStarted(5'000));
    const QByteArray transcript =
        QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
                       "\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"editor_select_photo\",\"arguments\":{\"photo_id\":\"%1\"}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"editor_set_exposure\",\"arguments\":{\"exposure_ev\":1.25}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"editor_get_state\",\"arguments\":{}}}\n")
            .arg(photoId.value)
            .toUtf8();
    ASSERT_EQ(process.write(transcript), transcript.size());
    process.closeWriteChannel();
    ASSERT_TRUE(process.waitForFinished(5'000));
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardError().isEmpty());

    const QByteArray output = process.readAllStandardOutput();
    EXPECT_TRUE(output.contains(QString::number(photoId.value).toUtf8()));
    EXPECT_TRUE(output.contains("\"exposure_ev\":1.25"));
    EXPECT_TRUE(output.contains("\"dirty\":true"));
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogDevelopRepository developRepository(*database.value());
        const core::catalog::CatalogDevelopStateResult state = developRepository.loadState(photoId);
        ASSERT_TRUE(state.hasValue());
        EXPECT_FALSE(state.value().has_value());
    }
}

TEST(McpApplicationContextTest, StandaloneProcessPublishesCompletedSourceLifecycleThroughBoundedPolling)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("async.flexraw-catalog"));
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("async-source.jpg"));
    {
        QFile source(sourcePath);
        ASSERT_TRUE(source.open(QIODevice::WriteOnly));
        ASSERT_EQ(source.write(QByteArray(1024 * 1024, 'a')), 1024 * 1024);
    }

    core::types::PhotoId photoId;
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository repository(*database.value());
        const QVector<core::catalog::CatalogEntry> entries{
            {core::types::FileDescriptor{sourcePath,
                                         QStringLiteral("jpg"),
                                         QStringLiteral("async-source.jpg"),
                                         core::types::SupportedFileKind::RasterImage},
             core::types::FileScanStatus::Ready}};
        ASSERT_TRUE(repository.upsert(entries).hasValue());
        const core::catalog::CatalogPhotoQueryResult photos =
            repository.queryPage(core::catalog::CatalogPhotoPageRequest{});
        ASSERT_TRUE(photos.hasValue());
        ASSERT_EQ(photos.value().photos.size(), 1);
        photoId = photos.value().photos.front().id;
    }

    QProcess process;
    process.setProgram(QString::fromUtf8(FLEXRAW_MCP_EXECUTABLE_PATH));
    process.setArguments({QStringLiteral("--catalog"), catalogPath, QStringLiteral("--allow-write")});
    process.start();
    ASSERT_TRUE(process.waitForStarted(5'000));
    const QByteArray startup =
        QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
                       "\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"editor_select_photo\",\"arguments\":{\"photo_id\":\"%1\"}}}\n")
            .arg(photoId.value)
            .toUtf8();
    ASSERT_EQ(process.write(startup), startup.size());
    ASSERT_TRUE(process.waitForBytesWritten(2'000));

    QByteArray output;
    bool completed = false;
    for (int attempt = 0; attempt < 50 && !completed; ++attempt)
    {
        const QByteArray poll =
            QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":%1,\"method\":\"tools/call\",\"params\":{\"name\":"
                           "\"source_resolution_events\",\"arguments\":{\"after_sequence\":\"0\"}}}\n")
                .arg(attempt + 3)
                .toUtf8();
        ASSERT_EQ(process.write(poll), poll.size());
        ASSERT_TRUE(process.waitForBytesWritten(2'000));
        if (process.waitForReadyRead(1'000))
        {
            output.append(process.readAllStandardOutput());
        }
        completed = output.contains("\"state\":\"completed\"");
        if (!completed)
        {
            QThread::msleep(5);
        }
    }
    process.closeWriteChannel();
    ASSERT_TRUE(process.waitForFinished(5'000));
    output.append(process.readAllStandardOutput());

    EXPECT_TRUE(completed);
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardError().isEmpty());
    EXPECT_TRUE(output.contains("\"accepted\":{"));
    EXPECT_TRUE(output.contains("\"terminal\":{"));
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository repository(*database.value());
        const core::catalog::CatalogPhotoRecordResult record = repository.findById(photoId);
        ASSERT_TRUE(record.hasValue());
        ASSERT_TRUE(record.value().has_value());
        EXPECT_EQ(record.value()->sourceState, core::catalog::SourceBindingState::Available);
        EXPECT_FALSE(record.value()->fingerprint.sha256.isEmpty());
    }
}

TEST(McpApplicationContextTest, StandaloneProcessStopsSerializedDeliveryAtEofDuringSourceWork)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("eof.flexraw-catalog"));
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("eof-source.jpg"));
    {
        QFile source(sourcePath);
        ASSERT_TRUE(source.open(QIODevice::WriteOnly));
        ASSERT_TRUE(source.resize(64 * 1024 * 1024));
    }

    core::types::PhotoId photoId;
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository repository(*database.value());
        const QVector<core::catalog::CatalogEntry> entries{
            {core::types::FileDescriptor{sourcePath,
                                         QStringLiteral("jpg"),
                                         QStringLiteral("eof-source.jpg"),
                                         core::types::SupportedFileKind::RasterImage},
             core::types::FileScanStatus::Ready}};
        ASSERT_TRUE(repository.upsert(entries).hasValue());
        const core::catalog::CatalogPhotoQueryResult photos =
            repository.queryPage(core::catalog::CatalogPhotoPageRequest{});
        ASSERT_TRUE(photos.hasValue());
        photoId = photos.value().photos.front().id;
    }

    QProcess process;
    process.setProgram(QString::fromUtf8(FLEXRAW_MCP_EXECUTABLE_PATH));
    process.setArguments({QStringLiteral("--catalog"), catalogPath, QStringLiteral("--allow-write")});
    process.start();
    ASSERT_TRUE(process.waitForStarted(5'000));
    const QByteArray transcript =
        QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":"
                       "\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":"
                       "\"editor_select_photo\",\"arguments\":{\"photo_id\":\"%1\"}}}\n")
            .arg(photoId.value)
            .toUtf8();
    ASSERT_EQ(process.write(transcript), transcript.size());
    process.closeWriteChannel();

    ASSERT_TRUE(process.waitForFinished(5'000));
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardError().isEmpty());
    const QList<QByteArray> lines = process.readAllStandardOutput().split('\n');
    int responseCount = 0;
    for (const QByteArray& line : lines)
    {
        if (line.isEmpty())
        {
            continue;
        }
        ++responseCount;
        EXPECT_TRUE(QJsonDocument::fromJson(line).isObject());
    }
    EXPECT_EQ(responseCount, 2);
}

}  // namespace
}  // namespace flexraw::app::mcp
