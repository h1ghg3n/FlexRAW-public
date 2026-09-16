#include <array>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include <gtest/gtest.h>

#include "mcp_catalog_tools.h"
#include "mcp_client_error_projection.h"
#include "mcp_editor_tools.h"
#include "mcp_project_tools.h"
#include "mcp_server.h"
#include "mcp_source_resolution_tools.h"

namespace flexraw::mcp
{
namespace
{

class FakeCatalogPhotoClient final : public core::client::ICatalogPhotoClient
{
public:
    // 목적: test가 지정한 page 또는 error를 반환하고 전달 request 기록
    // 입력: request: MCP adapter가 복원한 bounded query
    // 출력: configured Catalog result
    [[nodiscard]] core::client::CatalogPhotoPageResult queryPhotoPage(
        const core::client::CatalogPhotoPageRequest& request) const override
    {
        requests.push_back(request);
        if (error.has_value())
        {
            return core::client::CatalogPhotoPageResult::failure(*error);
        }
        return core::client::CatalogPhotoPageResult::success(page);
    }

    mutable std::vector<core::client::CatalogPhotoPageRequest> requests;
    core::client::CatalogPhotoPage page;
    std::optional<core::client::ClientError> error;
};

class FakeCatalogProjectClient final : public core::client::ICatalogProjectClient
{
public:
    // 목적: configured Project snapshot 목록 또는 error 반환
    // 입력: 없음
    // 출력: test가 지정한 Project list result
    [[nodiscard]] core::client::CatalogProjectListResult listProjects() const override
    {
        if (error.has_value())
        {
            return core::client::CatalogProjectListResult::failure(*error);
        }
        return core::client::CatalogProjectListResult::success(projects);
    }

    // 목적: 현재 MCP proof가 사용하지 않는 create command를 명시적으로 거부
    // 입력: command: 무시되는 Project create command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::CatalogProjectResult createProject(const core::client::CreateProjectCommand&) override
    {
        return core::client::CatalogProjectResult::failure(
            {core::client::ClientErrorCode::Unknown, "Fake createProject is not configured."});
    }

    // 목적: 현재 MCP proof가 사용하지 않는 rename command를 명시적으로 거부
    // 입력: command: 무시되는 Project rename command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::CatalogProjectResult renameProject(const core::client::RenameProjectCommand&) override
    {
        return core::client::CatalogProjectResult::failure(
            {core::client::ClientErrorCode::Unknown, "Fake renameProject is not configured."});
    }

    // 목적: 현재 MCP proof가 사용하지 않는 delete command를 명시적으로 거부
    // 입력: command: 무시되는 Project delete command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::CatalogProjectDeleteResult deleteProject(
        const core::client::DeleteProjectCommand&) override
    {
        return core::client::CatalogProjectDeleteResult::failure(
            {core::client::ClientErrorCode::Unknown, "Fake deleteProject is not configured."});
    }

    // 목적: Project membership command를 기록하고 configured receipt/error 반환
    // 입력: command: adapter가 복원한 Project와 Photo identity
    // 출력: 같은 identity receipt 또는 configured error
    [[nodiscard]] core::client::CatalogProjectMembershipResult addPhotoToProject(
        const core::client::ProjectPhotoMembershipCommand& command) override
    {
        membershipAdds.push_back(command);
        if (error.has_value())
        {
            return core::client::CatalogProjectMembershipResult::failure(*error);
        }
        return core::client::CatalogProjectMembershipResult::success({command.projectId, command.photoId});
    }

    // 목적: 현재 MCP proof가 사용하지 않는 membership remove command를 명시적으로 거부
    // 입력: command: 무시되는 membership command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::CatalogProjectMembershipResult removePhotoFromProject(
        const core::client::ProjectPhotoMembershipCommand&) override
    {
        return core::client::CatalogProjectMembershipResult::failure(
            {core::client::ClientErrorCode::Unknown, "Fake removePhotoFromProject is not configured."});
    }

    std::vector<core::client::CatalogProjectSnapshot> projects;
    std::vector<core::client::ProjectPhotoMembershipCommand> membershipAdds;
    std::optional<core::client::ClientError> error;
};

class FakeEditorClient final : public core::client::IEditorClient
{
public:
    // 목적: configured immutable Editor snapshot 반환
    // 입력: 없음
    // 출력: test가 지정한 현재 Editor state
    [[nodiscard]] core::client::EditorSnapshot editorSnapshot() const override
    {
        return snapshot;
    }

    // 목적: 현재 proof에서 사용하지 않는 source activation을 명시적으로 거부
    // 입력: command: 미사용 source command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult activateSource(const core::client::ActivateEditorSourceCommand&) override
    {
        return unsupported("activateSource");
    }

    // 목적: adapter가 복원한 Photo selection을 기록하고 snapshot 갱신
    // 입력: command: stable Photo identity
    // 출력: configured error 또는 선택된 snapshot
    [[nodiscard]] core::client::EditorResult selectPhoto(const core::client::SelectEditorPhotoCommand& command) override
    {
        selections.push_back(command);
        if (error.has_value())
        {
            return core::client::EditorResult::failure(*error);
        }
        snapshot.hasSelection = true;
        snapshot.photoId = command.photoId;
        return core::client::EditorResult::success(snapshot);
    }

    // 목적: 현재 proof에서 사용하지 않는 selection clear를 명시적으로 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult clearEditorSelection() override
    {
        return unsupported("clearEditorSelection");
    }

    // 목적: adapter가 조립한 Develop command를 기록하고 snapshot 갱신
    // 입력: command: 전체 frontend-neutral Develop parameter
    // 출력: configured error 또는 dirty snapshot
    [[nodiscard]] core::client::EditorResult updateDevelopParams(
        const core::client::UpdateDevelopParamsCommand& command) override
    {
        updates.push_back(command);
        if (error.has_value())
        {
            return core::client::EditorResult::failure(*error);
        }
        snapshot.params = command.params;
        snapshot.dirty = true;
        ++snapshot.developRevision.value;
        return core::client::EditorResult::success(snapshot);
    }

    // 목적: 현재 proof에서 사용하지 않는 Adjustment 시작을 명시적으로 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult beginAdjustment() override
    {
        return unsupported("beginAdjustment");
    }

    // 목적: 현재 proof에서 사용하지 않는 Adjustment 종료를 명시적으로 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult endAdjustment() override
    {
        return unsupported("endAdjustment");
    }

    // 목적: 현재 proof에서 사용하지 않는 undo를 명시적으로 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult undoDevelop() override
    {
        return unsupported("undoDevelop");
    }

    // 목적: 현재 proof에서 사용하지 않는 redo를 명시적으로 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult redoDevelop() override
    {
        return unsupported("redoDevelop");
    }

    // 목적: 현재 proof에서 사용하지 않는 save를 명시적으로 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult saveDevelopState() override
    {
        return unsupported("saveDevelopState");
    }

    core::client::EditorSnapshot snapshot;
    std::vector<core::client::SelectEditorPhotoCommand> selections;
    std::vector<core::client::UpdateDevelopParamsCommand> updates;
    std::optional<core::client::ClientError> error;

private:
    // 목적: test scope 밖 Editor command에 일관된 failure 생성
    // 입력: operation: 호출된 fake method 이름
    // 출력: Unknown EditorResult failure
    [[nodiscard]] static core::client::EditorResult unsupported(const char* operation)
    {
        return core::client::EditorResult::failure(
            {core::client::ClientErrorCode::Unknown, std::string("Fake ") + operation + " is not configured."});
    }
};

struct FakeSourceSubscriptionState
{
    bool active{true};
    core::client::SourceResolutionCallback callback;
};

class FakeSourceSubscription final : public core::client::ISourceResolutionSubscription
{
public:
    // 목적: fake event source와 MCP adapter가 공유할 subscription state 보관
    // 입력: state: callback과 active flag를 가진 shared state
    // 출력: RAII unsubscribe handle
    explicit FakeSourceSubscription(std::shared_ptr<FakeSourceSubscriptionState> state) : m_state(std::move(state)) {}

    // 목적: 마지막 fake handle 해제 시 이후 callback 전달 차단
    // 입력: 없음
    // 출력: inactive subscription
    ~FakeSourceSubscription() override
    {
        unsubscribe();
    }

    // 목적: fake Source Resolution callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: inactive state와 해제된 callback
    void unsubscribe() noexcept override
    {
        m_state->active = false;
        m_state->callback = {};
    }

    // 목적: fake subscription callback 전달 가능 상태 조회
    // 입력: 없음
    // 출력: active이면 true
    [[nodiscard]] bool isActive() const noexcept override
    {
        return m_state->active;
    }

private:
    std::shared_ptr<FakeSourceSubscriptionState> m_state;
};

class FakeSourceResolutionClient final : public core::client::ISourceResolutionClient,
                                         public core::client::ISourceResolutionEventSource
{
public:
    // 목적: test에서 사용하지 않는 replacement accept command를 명시적으로 거부
    // 입력: command: 미사용 command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::SourceRequestResult acceptReplacement(
        const core::client::AcceptReplacementCommand&) override
    {
        return unsupported();
    }

    // 목적: test에서 사용하지 않는 register-as-new command를 명시적으로 거부
    // 입력: command: 미사용 command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::SourceRequestResult registerReplacementAsNew(
        const core::client::RegisterReplacementAsNewCommand&) override
    {
        return unsupported();
    }

    // 목적: test에서 사용하지 않는 relink command를 명시적으로 거부
    // 입력: command: 미사용 command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::SourceRequestResult relinkSource(const core::client::RelinkSourceCommand&) override
    {
        return unsupported();
    }

    // 목적: adapter가 전달한 cancellation identity 기록
    // 입력: requestId: 취소할 owner identity
    // 출력: 같은 identity 또는 configured error
    [[nodiscard]] core::client::SourceRequestCancelResult cancelSourceRequest(
        core::client::SourceRequestId requestId) override
    {
        cancellations.push_back(requestId);
        if (cancelError.has_value())
        {
            return core::client::SourceRequestCancelResult::failure(*cancelError);
        }
        return core::client::SourceRequestCancelResult::success(requestId);
    }

    // 목적: MCP adapter callback을 fake subscription state에 연결
    // 입력: callback: test가 publish할 immutable event consumer
    // 출력: RAII fake subscription handle
    [[nodiscard]] core::client::SourceResolutionSubscriptionResult subscribeToSourceResolution(
        core::client::SourceResolutionCallback callback) override
    {
        state = std::make_shared<FakeSourceSubscriptionState>();
        state->callback = std::move(callback);
        const std::shared_ptr<FakeSourceSubscriptionState> queuedState = state;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [queuedState] {
                if (!queuedState->active)
                {
                    return;
                }
                queuedState->callback({{1}, true, {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            },
            Qt::QueuedConnection);
        return core::client::SourceResolutionSubscriptionResult::success(
            std::make_shared<FakeSourceSubscription>(state));
    }

    std::vector<core::client::SourceRequestId> cancellations;
    std::optional<core::client::ClientError> cancelError;
    std::shared_ptr<FakeSourceSubscriptionState> state;

private:
    // 목적: 현재 server test 범위 밖 Source command에 일관된 failure 생성
    // 입력: 없음
    // 출력: Unknown SourceRequestResult failure
    [[nodiscard]] static core::client::SourceRequestResult unsupported()
    {
        return core::client::SourceRequestResult::failure(
            {core::client::ClientErrorCode::Unknown, "Fake Source command is not configured."});
    }
};

// 목적: serialized MCP server test에 필요한 process-wide Qt event dispatcher 보장
// 입력: 없음
// 출력: test process lifetime 동안 유지되는 QCoreApplication 참조
[[nodiscard]] QCoreApplication& application()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return *QCoreApplication::instance();
    }
    static int argumentCount = 1;
    static char applicationName[] = "flexraw_mcp_server_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
    return application;
}

// 목적: newline-delimited server input을 fake Catalog/Project/Editor client와 write policy로 실행
// 입력: input: transcript, clients: configured fakes, writesEnabled: process write opt-in
// 출력: stdout/stderr와 process code
[[nodiscard]] std::tuple<int, std::string, std::string> runServer(const std::string& input,
                                                                  FakeCatalogPhotoClient& photoClient,
                                                                  FakeCatalogProjectClient& projectClient,
                                                                  FakeEditorClient& editorClient,
                                                                  bool writesEnabled)
{
    static_cast<void>(application());
    std::istringstream requestStream(input);
    std::ostringstream responseStream;
    std::ostringstream diagnosticStream;
    McpCatalogTools catalogTools(photoClient);
    McpProjectTools projectTools(projectClient, writesEnabled);
    McpEditorTools editorTools(editorClient, writesEnabled);
    FakeSourceResolutionClient sourceClient;
    McpSourceResolutionTools sourceTools(sourceClient, sourceClient, writesEnabled);
    McpServer server(catalogTools,
                     projectTools,
                     editorTools,
                     sourceTools,
                     "0.1.0-test",
                     requestStream,
                     responseStream,
                     diagnosticStream);
    const int exitCode = server.run();
    return {exitCode, responseStream.str(), diagnosticStream.str()};
}

// 목적: Editor를 사용하지 않는 transcript를 기본 fake Editor와 실행
// 입력: input: transcript, photo/project client: configured fakes, writesEnabled: process write opt-in
// 출력: stdout/stderr와 process code
[[nodiscard]] std::tuple<int, std::string, std::string> runServer(const std::string& input,
                                                                  FakeCatalogPhotoClient& photoClient,
                                                                  FakeCatalogProjectClient& projectClient,
                                                                  bool writesEnabled)
{
    FakeEditorClient editorClient;
    return runServer(input, photoClient, projectClient, editorClient, writesEnabled);
}

// 목적: newline-delimited server input을 fake Catalog client로 끝까지 실행
// 입력: input: complete MCP transcript, client: configured fake
// 출력: stdout/stderr와 process code
[[nodiscard]] std::tuple<int, std::string, std::string> runServer(const std::string& input,
                                                                  FakeCatalogPhotoClient& client)
{
    FakeCatalogProjectClient projectClient;
    return runServer(input, client, projectClient, false);
}

// 목적: newline-delimited JSON object stream을 검증 가능한 object 목록으로 parse
// 입력: output: MCP server stdout bytes
// 출력: 각 non-empty line의 JSON object
[[nodiscard]] std::vector<QJsonObject> parseResponses(const std::string& output)
{
    std::vector<QJsonObject> responses;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty())
        {
            responses.push_back(QJsonDocument::fromJson(QByteArray::fromStdString(line)).object());
        }
    }
    return responses;
}

constexpr auto LegacyInitialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})"
    "\n"
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
    "\n";

constexpr auto ModernMetadata =
    R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"test","version":"1"},"io.modelcontextprotocol/clientCapabilities":{}})";

TEST(McpClientErrorProjectionTest, ProjectsEveryCommonCodeWithOperationNeutralText)
{
    struct ExpectedError
    {
        core::client::ClientErrorCode code;
        const char* name;
        const char* message;
    };
    constexpr std::array cases{
        ExpectedError{core::client::ClientErrorCode::Unknown, "unknown", "The operation failed."},
        ExpectedError{
            core::client::ClientErrorCode::InvalidArgument, "invalid_argument", "The request arguments are invalid."},
        ExpectedError{core::client::ClientErrorCode::NotFound, "not_found", "The requested resource was not found."},
        ExpectedError{
            core::client::ClientErrorCode::PermissionDenied, "permission_denied", "The operation is not permitted."},
        ExpectedError{core::client::ClientErrorCode::UnsupportedFormat,
                      "unsupported_format",
                      "The requested format is unsupported."},
        ExpectedError{core::client::ClientErrorCode::ThumbnailUnavailable,
                      "thumbnail_unavailable",
                      "The requested thumbnail is unavailable."},
        ExpectedError{core::client::ClientErrorCode::DecodeFailed,
                      "decode_failed",
                      "The requested content could not be decoded."},
        ExpectedError{
            core::client::ClientErrorCode::DatabaseError, "database_error", "The persistent storage operation failed."},
        ExpectedError{
            core::client::ClientErrorCode::Conflict, "conflict", "The current state conflicts with this request."},
        ExpectedError{core::client::ClientErrorCode::Cancelled, "cancelled", "The operation was cancelled."},
    };

    for (const ExpectedError& expected : cases)
    {
        const QJsonObject projected = clientErrorToJson({expected.code, "private diagnostic"});
        EXPECT_EQ(QString::fromLatin1(expected.name), projected.value(QStringLiteral("code")).toString());
        EXPECT_EQ(QString::fromLatin1(expected.message), projected.value(QStringLiteral("message")).toString());
        EXPECT_FALSE(QJsonDocument(projected).toJson(QJsonDocument::Compact).contains("private diagnostic"));
        EXPECT_FALSE(projected.value(QStringLiteral("message")).toString().contains(QStringLiteral("Catalog")));
    }
}

TEST(McpSourceStateProjectionTest, ProjectsEveryStateAcrossCatalogAndEditorTools)
{
    constexpr std::array cases{
        std::pair{core::client::CatalogSourceState::FingerprintPending, "fingerprint_pending"},
        std::pair{core::client::CatalogSourceState::Available, "available"},
        std::pair{core::client::CatalogSourceState::Missing, "missing"},
        std::pair{core::client::CatalogSourceState::VerificationRequired, "verification_required"},
        std::pair{core::client::CatalogSourceState::IdentityUnverified, "identity_unverified"},
        std::pair{core::client::CatalogSourceState::ReplacementDetected, "replacement_detected"},
        std::pair{core::client::CatalogSourceState::Unreadable, "unreadable"},
        std::pair{core::client::CatalogSourceState::Unlinked, "unlinked"},
    };
    FakeCatalogPhotoClient photoClient;
    FakeEditorClient editorClient;
    McpCatalogTools catalogTools(photoClient);
    McpEditorTools editorTools(editorClient, false);

    for (const auto& [state, expectedName] : cases)
    {
        core::client::CatalogPhotoSnapshot photo;
        photo.sourceState = state;
        photoClient.page.photos = {photo};
        editorClient.snapshot.sourceState = state;

        const McpToolCallResult catalog = catalogTools.callQueryPhotos({});
        const McpToolCallResult editor = editorTools.callGetState({});

        ASSERT_TRUE(catalog.argumentsValid);
        ASSERT_TRUE(editor.argumentsValid);
        const QString expected = QString::fromLatin1(expectedName);
        EXPECT_EQ(expected,
                  catalog.structuredContent.value(QStringLiteral("photos"))
                      .toArray()[0]
                      .toObject()
                      .value(QStringLiteral("source_state"))
                      .toString());
        EXPECT_EQ(expected,
                  editor.structuredContent.value(QStringLiteral("editor"))
                      .toObject()
                      .value(QStringLiteral("source_state"))
                      .toString());
    }
}

TEST(McpServerTest, ListsCatalogToolsAfterLegacyInitialization)
{
    FakeCatalogPhotoClient client;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) + R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" + "\n", client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    const std::vector<QJsonObject> responses = parseResponses(output);
    ASSERT_EQ(responses.size(), 2U);
    const QJsonArray tools =
        responses[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    ASSERT_EQ(tools.size(), 4);
    EXPECT_EQ(tools[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("catalog_query_photos"));
    EXPECT_EQ(tools[1].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("catalog_list_projects"));
    EXPECT_EQ(tools[2].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("editor_get_state"));
    EXPECT_EQ(tools[3].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("source_resolution_events"));
    EXPECT_TRUE(tools[0]
                    .toObject()
                    .value(QStringLiteral("annotations"))
                    .toObject()
                    .value(QStringLiteral("readOnlyHint"))
                    .toBool());
}

TEST(McpServerTest, PumpsQueuedSourceEventBeforePollingOnSerializedDeliveryContext)
{
    FakeCatalogPhotoClient client;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"source_resolution_events","arguments":{"after_sequence":"0"}}})" +
            "\n",
        client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    const std::vector<QJsonObject> responses = parseResponses(output);
    ASSERT_EQ(responses.size(), 2U);
    const QJsonObject structured = responses.back()
                                       .value(QStringLiteral("result"))
                                       .toObject()
                                       .value(QStringLiteral("structuredContent"))
                                       .toObject();
    const QJsonArray events = structured.value(QStringLiteral("events")).toArray();
    ASSERT_EQ(events.size(), 1);
    EXPECT_TRUE(events[0].toObject().value(QStringLiteral("initial")).toBool());
    EXPECT_EQ(events[0].toObject().value(QStringLiteral("sequence")).toString(), QStringLiteral("1"));
}

TEST(McpServerTest, ListsProjectsWithPrecisionSafeIdentity)
{
    FakeCatalogPhotoClient photoClient;
    FakeCatalogProjectClient projectClient;
    projectClient.projects.push_back({{9'007'199'254'740'993LL}, "선택"});
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"catalog_list_projects","arguments":{}}})" +
            "\n",
        photoClient,
        projectClient,
        false);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    const QJsonObject structured = parseResponses(output)
                                       .back()
                                       .value(QStringLiteral("result"))
                                       .toObject()
                                       .value(QStringLiteral("structuredContent"))
                                       .toObject();
    const QJsonObject project = structured.value(QStringLiteral("projects")).toArray()[0].toObject();
    EXPECT_EQ(project.value(QStringLiteral("project_id")).toString(), QStringLiteral("9007199254740993"));
    EXPECT_EQ(project.value(QStringLiteral("name")).toString(), QStringLiteral("선택"));
}

TEST(McpServerTest, HidesAndRejectsWriteToolWithoutExplicitOptIn)
{
    FakeCatalogPhotoClient photoClient;
    FakeCatalogProjectClient projectClient;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) + R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" + "\n" +
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"catalog_add_photo_to_project","arguments":{"project_id":"7","photo_id":"11"}}})" +
            "\n",
        photoClient,
        projectClient,
        false);

    ASSERT_EQ(exitCode, 0);
    EXPECT_TRUE(projectClient.membershipAdds.empty());
    EXPECT_NE(diagnostics.find("MCP write access is disabled"), std::string::npos);
    EXPECT_EQ(output.find("MCP write access is disabled"), std::string::npos);
    const std::vector<QJsonObject> responses = parseResponses(output);
    const QJsonArray tools =
        responses[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    ASSERT_EQ(tools.size(), 4);
    const QJsonObject callResult = responses[2].value(QStringLiteral("result")).toObject();
    EXPECT_TRUE(callResult.value(QStringLiteral("isError")).toBool());
    EXPECT_EQ(callResult.value(QStringLiteral("structuredContent"))
                  .toObject()
                  .value(QStringLiteral("error"))
                  .toObject()
                  .value(QStringLiteral("code"))
                  .toString(),
              QStringLiteral("permission_denied"));
}

TEST(McpServerTest, HidesAndRejectsEditorCommandsWithoutExplicitOptIn)
{
    FakeCatalogPhotoClient photoClient;
    FakeCatalogProjectClient projectClient;
    FakeEditorClient editorClient;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) + R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" + "\n" +
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"editor_select_photo","arguments":{"photo_id":"11"}}})" +
            "\n",
        photoClient,
        projectClient,
        editorClient,
        false);

    ASSERT_EQ(exitCode, 0);
    EXPECT_TRUE(editorClient.selections.empty());
    EXPECT_NE(diagnostics.find("MCP Editor commands are disabled"), std::string::npos);
    EXPECT_EQ(output.find("MCP Editor commands are disabled"), std::string::npos);
    const std::vector<QJsonObject> responses = parseResponses(output);
    const QJsonArray tools =
        responses[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    ASSERT_EQ(tools.size(), 4);
    EXPECT_EQ(tools[2].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("editor_get_state"));
    const QJsonObject error = responses[2]
                                  .value(QStringLiteral("result"))
                                  .toObject()
                                  .value(QStringLiteral("structuredContent"))
                                  .toObject()
                                  .value(QStringLiteral("error"))
                                  .toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toString(), QStringLiteral("permission_denied"));
}

TEST(McpServerTest, HidesAndRejectsSourceCancellationWithoutExplicitOptIn)
{
    FakeCatalogPhotoClient client;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"source_cancel_request","arguments":{"request_id":"7"}}})" +
            "\n",
        client);

    ASSERT_EQ(exitCode, 0);
    EXPECT_NE(diagnostics.find("MCP write access is disabled"), std::string::npos);
    EXPECT_EQ(output.find("MCP write access is disabled"), std::string::npos);
    const QJsonObject error = parseResponses(output)
                                  .back()
                                  .value(QStringLiteral("result"))
                                  .toObject()
                                  .value(QStringLiteral("structuredContent"))
                                  .toObject()
                                  .value(QStringLiteral("error"))
                                  .toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toString(), QStringLiteral("permission_denied"));
}

TEST(McpServerTest, AdvertisesAndCallsIdempotentMembershipToolAfterWriteOptIn)
{
    FakeCatalogPhotoClient photoClient;
    FakeCatalogProjectClient projectClient;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) + R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" + "\n" +
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"catalog_add_photo_to_project","arguments":{"project_id":"7","photo_id":"11"}}})" +
            "\n",
        photoClient,
        projectClient,
        true);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    ASSERT_EQ(projectClient.membershipAdds.size(), 1U);
    EXPECT_EQ(projectClient.membershipAdds[0].projectId.value, 7);
    EXPECT_EQ(projectClient.membershipAdds[0].photoId.value, 11);
    const std::vector<QJsonObject> responses = parseResponses(output);
    const QJsonArray tools =
        responses[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    ASSERT_EQ(tools.size(), 8);
    const QJsonObject writeTool = tools[4].toObject();
    EXPECT_EQ(writeTool.value(QStringLiteral("name")).toString(), QStringLiteral("catalog_add_photo_to_project"));
    EXPECT_EQ(tools[5].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("editor_select_photo"));
    EXPECT_EQ(tools[6].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("editor_set_exposure"));
    EXPECT_EQ(tools[7].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("source_cancel_request"));
    EXPECT_TRUE(
        writeTool.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("idempotentHint")).toBool());
    EXPECT_FALSE(responses[2].value(QStringLiteral("result")).toObject().value(QStringLiteral("isError")).toBool());
}

TEST(McpServerTest, ProjectsImmutableEditorSnapshotWithPrecisionSafeIdentityAndRevisions)
{
    FakeCatalogPhotoClient photoClient;
    FakeCatalogProjectClient projectClient;
    FakeEditorClient editorClient;
    editorClient.snapshot.hasSelection = true;
    editorClient.snapshot.photoId = {9'007'199'254'740'993LL};
    editorClient.snapshot.developRevision = {18'446'744'073'709'551'615ULL};
    editorClient.snapshot.persistedRevision = {9'007'199'254'740'994ULL};
    editorClient.snapshot.source =
        core::client::EditorSourceSnapshot{"C:/사진/원본.raw", "raw", "원본.raw", core::client::CatalogFileKind::Raw};
    editorClient.snapshot.sourceProcessingAllowed = true;
    editorClient.snapshot.sourceState = core::client::CatalogSourceState::Available;
    editorClient.snapshot.params.exposureEv = 0.75F;
    editorClient.snapshot.dirty = true;
    editorClient.snapshot.canUndo = true;

    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"editor_get_state","arguments":{}}})" +
            "\n",
        photoClient,
        projectClient,
        editorClient,
        false);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    const QJsonObject editor = parseResponses(output)
                                   .back()
                                   .value(QStringLiteral("result"))
                                   .toObject()
                                   .value(QStringLiteral("structuredContent"))
                                   .toObject()
                                   .value(QStringLiteral("editor"))
                                   .toObject();
    EXPECT_EQ(editor.value(QStringLiteral("photo_id")).toString(), QStringLiteral("9007199254740993"));
    EXPECT_EQ(editor.value(QStringLiteral("develop_revision")).toString(), QStringLiteral("18446744073709551615"));
    EXPECT_EQ(editor.value(QStringLiteral("persisted_revision")).toString(), QStringLiteral("9007199254740994"));
    EXPECT_EQ(editor.value(QStringLiteral("source")).toObject().value(QStringLiteral("display_name")).toString(),
              QStringLiteral("원본.raw"));
    EXPECT_DOUBLE_EQ(editor.value(QStringLiteral("develop")).toObject().value(QStringLiteral("exposure_ev")).toDouble(),
                     0.75);
    EXPECT_TRUE(editor.value(QStringLiteral("dirty")).toBool());
    EXPECT_TRUE(editor.value(QStringLiteral("can_undo")).toBool());
}

TEST(McpServerTest, SelectsPhotoAndAppliesAbsoluteSessionExposureThroughSharedEditorClient)
{
    FakeCatalogPhotoClient photoClient;
    FakeCatalogProjectClient projectClient;
    FakeEditorClient editorClient;
    editorClient.snapshot.params.contrast = 0.25F;
    editorClient.snapshot.developRevision = {4};

    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"editor_select_photo","arguments":{"photo_id":"42"}}})" +
            "\n" +
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"editor_set_exposure","arguments":{"exposure_ev":1.5}}})" +
            "\n",
        photoClient,
        projectClient,
        editorClient,
        true);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    ASSERT_EQ(editorClient.selections.size(), 1U);
    EXPECT_EQ(editorClient.selections[0].photoId.value, 42);
    ASSERT_EQ(editorClient.updates.size(), 1U);
    EXPECT_FLOAT_EQ(editorClient.updates[0].params.exposureEv, 1.5F);
    EXPECT_FLOAT_EQ(editorClient.updates[0].params.contrast, 0.25F);
    const QJsonObject editor = parseResponses(output)
                                   .back()
                                   .value(QStringLiteral("result"))
                                   .toObject()
                                   .value(QStringLiteral("structuredContent"))
                                   .toObject()
                                   .value(QStringLiteral("editor"))
                                   .toObject();
    EXPECT_EQ(editor.value(QStringLiteral("photo_id")).toString(), QStringLiteral("42"));
    EXPECT_EQ(editor.value(QStringLiteral("develop_revision")).toString(), QStringLiteral("5"));
    EXPECT_TRUE(editor.value(QStringLiteral("dirty")).toBool());
    EXPECT_DOUBLE_EQ(editor.value(QStringLiteral("develop")).toObject().value(QStringLiteral("exposure_ev")).toDouble(),
                     1.5);
}

TEST(McpServerTest, DiscoversAndListsCatalogToolsWithModernPerRequestMetadata)
{
    FakeCatalogPhotoClient client;
    const std::string input =
        std::string(R"({"jsonrpc":"2.0","id":"discover","method":"server/discover","params":{)") + ModernMetadata +
        "}}\n" + R"({"jsonrpc":"2.0","id":"list","method":"tools/list","params":{)" + ModernMetadata + "}}\n";
    const auto [exitCode, output, diagnostics] = runServer(input, client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    const std::vector<QJsonObject> responses = parseResponses(output);
    ASSERT_EQ(responses.size(), 2U);
    const QJsonObject discovery = responses[0].value(QStringLiteral("result")).toObject();
    EXPECT_EQ(discovery.value(QStringLiteral("resultType")).toString(), QStringLiteral("complete"));
    EXPECT_EQ(discovery.value(QStringLiteral("supportedVersions")).toArray()[0].toString(),
              QStringLiteral("2026-07-28"));
    EXPECT_EQ(discovery.value(QStringLiteral("_meta"))
                  .toObject()
                  .value(QStringLiteral("io.modelcontextprotocol/serverInfo"))
                  .toObject()
                  .value(QStringLiteral("name"))
                  .toString(),
              QStringLiteral("Flexraw MCP"));

    const QJsonObject listed = responses[1].value(QStringLiteral("result")).toObject();
    EXPECT_EQ(listed.value(QStringLiteral("resultType")).toString(), QStringLiteral("complete"));
    EXPECT_EQ(listed.value(QStringLiteral("cacheScope")).toString(), QStringLiteral("private"));
    ASSERT_EQ(listed.value(QStringLiteral("tools")).toArray().size(), 4);
}

TEST(McpServerTest, RejectsMixedProtocolErasOnOneStdioConnection)
{
    FakeCatalogPhotoClient client;
    const std::string input = std::string(R"({"jsonrpc":"2.0","id":"discover","method":"server/discover","params":{)") +
                              ModernMetadata + "}}\n" + LegacyInitialize;
    const auto [exitCode, output, diagnostics] = runServer(input, client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    const std::vector<QJsonObject> responses = parseResponses(output);
    ASSERT_EQ(responses.size(), 2U);
    EXPECT_TRUE(responses[0].contains(QStringLiteral("result")));
    EXPECT_EQ(responses[1].value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), -32022);
}

TEST(McpServerTest, ProjectsBoundedCatalogPageWithPrecisionSafeIdentities)
{
    FakeCatalogPhotoClient client;
    core::client::CatalogPhotoSnapshot photo;
    photo.id = {9'007'199'254'740'993LL};
    photo.displayName = "sample.raw";
    photo.lastKnownPath = "C:/photos/sample.raw";
    photo.extension = "raw";
    photo.kind = core::client::CatalogFileKind::Raw;
    photo.scanStatus = core::client::CatalogScanStatus::Ready;
    photo.sourceState = core::client::CatalogSourceState::Available;
    client.page.photos.push_back(photo);
    client.page.nextCursor = core::client::CatalogPhotoPageCursor{"sample.raw", photo.id, std::nullopt, std::nullopt};

    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"catalog_query_photos","arguments":{"page_size":25,"direction":"forward"}}})" +
            "\n",
        client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    ASSERT_EQ(client.requests.size(), 1U);
    EXPECT_EQ(client.requests[0].pageSize, 25);
    const std::vector<QJsonObject> responses = parseResponses(output);
    const QJsonObject result = responses.back().value(QStringLiteral("result")).toObject();
    ASSERT_FALSE(result.value(QStringLiteral("isError")).toBool());
    const QJsonObject structured = result.value(QStringLiteral("structuredContent")).toObject();
    EXPECT_EQ(
        structured.value(QStringLiteral("photos")).toArray()[0].toObject().value(QStringLiteral("photo_id")).toString(),
        QStringLiteral("9007199254740993"));
    EXPECT_EQ(structured.value(QStringLiteral("next_cursor")).toObject().value(QStringLiteral("photo_id")).toString(),
              QStringLiteral("9007199254740993"));
}

TEST(McpServerTest, RejectsInvalidCatalogArgumentsBeforeCallingClient)
{
    FakeCatalogPhotoClient client;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"catalog_query_photos","arguments":{"page_size":0}}})" +
            "\n",
        client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    EXPECT_TRUE(client.requests.empty());
    const QJsonObject error = parseResponses(output).back().value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toInt(), -32602);
}

TEST(McpServerTest, RejectsNonCanonicalDecimalIdentity)
{
    FakeCatalogPhotoClient client;
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"catalog_query_photos","arguments":{"project_id":"01"}}})" +
            "\n",
        client);

    ASSERT_EQ(exitCode, 0);
    ASSERT_TRUE(diagnostics.empty());
    EXPECT_TRUE(client.requests.empty());
    const QJsonObject error = parseResponses(output).back().value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toInt(), -32602);
}

TEST(McpServerTest, KeepsTechnicalErrorOutOfModelVisibleResult)
{
    FakeCatalogPhotoClient client;
    client.error =
        core::client::ClientError{core::client::ClientErrorCode::DatabaseError, "sqlite path and private diagnostics"};
    const auto [exitCode, output, diagnostics] = runServer(
        std::string(LegacyInitialize) +
            R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"catalog_query_photos","arguments":{}}})" +
            "\n",
        client);

    ASSERT_EQ(exitCode, 0);
    EXPECT_NE(diagnostics.find("sqlite path and private diagnostics"), std::string::npos);
    EXPECT_EQ(output.find("sqlite path and private diagnostics"), std::string::npos);
    const QJsonObject result = parseResponses(output).back().value(QStringLiteral("result")).toObject();
    EXPECT_TRUE(result.value(QStringLiteral("isError")).toBool());
    EXPECT_EQ(result.value(QStringLiteral("structuredContent"))
                  .toObject()
                  .value(QStringLiteral("error"))
                  .toObject()
                  .value(QStringLiteral("code"))
                  .toString(),
              QStringLiteral("database_error"));
}

}  // namespace
}  // namespace flexraw::mcp
