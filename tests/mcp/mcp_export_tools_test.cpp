#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>

#include <gtest/gtest.h>

#include "mcp_export_tools.h"

namespace flexraw::mcp
{
namespace
{

class TestEditorClient final : public core::client::IEditorClient
{
public:
    // 목적: test가 지정한 immutable Editor snapshot 반환
    // 입력: 없음
    // 출력: current test selection과 Develop state
    [[nodiscard]] core::client::EditorSnapshot editorSnapshot() const override
    {
        return snapshot;
    }

    // 목적: Export tool test 범위 밖 source activation 명시적 거부
    // 입력: command: 미사용 command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult activateSource(const core::client::ActivateEditorSourceCommand&) override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 selection command 명시적 거부
    // 입력: command: 미사용 command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult selectPhoto(const core::client::SelectEditorPhotoCommand&) override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 selection clear 명시적 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult clearEditorSelection() override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 Develop update 명시적 거부
    // 입력: command: 미사용 command
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult updateDevelopParams(
        const core::client::UpdateDevelopParamsCommand&) override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 adjustment start 명시적 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult beginAdjustment() override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 adjustment finish 명시적 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult endAdjustment() override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 undo 명시적 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult undoDevelop() override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 redo 명시적 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult redoDevelop() override
    {
        return unsupported();
    }

    // 목적: Export tool test 범위 밖 save 명시적 거부
    // 입력: 없음
    // 출력: Unknown test failure
    [[nodiscard]] core::client::EditorResult saveDevelopState() override
    {
        return unsupported();
    }

    core::client::EditorSnapshot snapshot;

private:
    // 목적: 미사용 Editor command의 test failure 생성
    // 입력: 없음
    // 출력: Unknown EditorResult
    [[nodiscard]] static core::client::EditorResult unsupported()
    {
        return core::client::EditorResult::failure(
            {core::client::ClientErrorCode::Unknown, "Test Editor command is not configured."});
    }
};

struct TestExportSubscriptionState
{
    bool active{true};
    core::client::ExportCallback callback;
};

class TestExportSubscription final : public core::client::IExportSubscription
{
public:
    // 목적: test endpoint와 MCP adapter가 공유할 subscription state 보관
    // 입력: state: callback과 active flag를 가진 shared state
    // 출력: RAII test subscription handle
    explicit TestExportSubscription(std::shared_ptr<TestExportSubscriptionState> state) : m_state(std::move(state)) {}

    // 목적: 마지막 handle 해제 시 이후 callback 전달 차단
    // 입력: 없음
    // 출력: inactive subscription
    ~TestExportSubscription() override
    {
        unsubscribe();
    }

    // 목적: test callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: inactive state와 해제된 callback
    void unsubscribe() noexcept override
    {
        m_state->active = false;
        m_state->callback = {};
    }

    // 목적: test subscription callback 전달 가능 상태 조회
    // 입력: 없음
    // 출력: active이면 true
    [[nodiscard]] bool isActive() const noexcept override
    {
        return m_state->active;
    }

private:
    std::shared_ptr<TestExportSubscriptionState> m_state;
};

class TestExportEndpoint final : public core::client::IExportClient, public core::client::IExportEventSource
{
public:
    // 목적: submitted command 기록 후 configured receipt/error 반환
    // 입력: command: MCP adapter가 조립한 Local Export command
    // 출력: configured submission result
    [[nodiscard]] core::client::ExportSubmissionResult submitExport(
        const core::client::SubmitExportCommand& command) override
    {
        submissions.push_back(command);
        if (submitError.has_value())
        {
            return core::client::ExportSubmissionResult::failure(*submitError);
        }
        return core::client::ExportSubmissionResult::success({{requestId}});
    }

    // 목적: cancellation identity 기록 후 configured result 반환
    // 입력: cancelledRequestId: MCP adapter가 복원한 identity
    // 출력: configured cancellation result
    [[nodiscard]] core::client::ExportCancellationResult cancelExport(
        core::client::ExportRequestId cancelledRequestId) override
    {
        cancellations.push_back(cancelledRequestId);
        return core::client::ExportCancellationResult::success(cancelledRequestId);
    }

    // 목적: test에서 사용하지 않는 direct snapshot 조회에 빈 값 반환
    // 입력: requestId: optional filter
    // 출력: empty Export snapshot
    [[nodiscard]] core::client::ExportSnapshotResult exportSnapshot(
        std::optional<core::client::ExportRequestId>) const override
    {
        return core::client::ExportSnapshotResult::success({});
    }

    // 목적: configured subscription result와 callback state 생성
    // 입력: callback: publish가 호출할 immutable event consumer
    // 출력: RAII handle 또는 configured error
    [[nodiscard]] core::client::ExportSubscriptionResult subscribeToExports(
        core::client::ExportCallback callback) override
    {
        if (subscribeError.has_value())
        {
            return core::client::ExportSubscriptionResult::failure(*subscribeError);
        }
        state = std::make_shared<TestExportSubscriptionState>();
        state->callback = std::move(callback);
        return core::client::ExportSubscriptionResult::success(std::make_shared<TestExportSubscription>(state));
    }

    // 목적: active test subscription에 Export event 전달
    // 입력: event: test가 조립한 ordered lifecycle event
    // 출력: inactive 상태이면 전달 없음
    void publish(const core::client::ExportEvent& event)
    {
        if (state != nullptr && state->active)
        {
            state->callback(event);
        }
    }

    std::uint64_t requestId{17};
    std::vector<core::client::SubmitExportCommand> submissions;
    std::vector<core::client::ExportRequestId> cancellations;
    std::optional<core::client::ClientError> submitError;
    std::optional<core::client::ClientError> subscribeError;
    std::shared_ptr<TestExportSubscriptionState> state;
};

// 목적: Local Export가 가능한 current Editor snapshot 생성
// 입력: 없음
// 출력: raster source와 in-memory Develop state를 가진 selection
[[nodiscard]] core::client::EditorSnapshot selectedEditorSnapshot()
{
    core::client::EditorSnapshot snapshot;
    snapshot.hasSelection = true;
    snapshot.photoId = {42};
    snapshot.source = core::client::EditorSourceSnapshot{
        "C:/photos/source.png", "png", "source.png", core::client::CatalogFileKind::RasterImage};
    snapshot.params.exposureEv = 1.25F;
    snapshot.params.contrast = 0.2F;
    snapshot.sourceProcessingAllowed = true;
    return snapshot;
}

TEST(McpExportToolsTest, SubmitsCurrentSelectionAsLocalJpegWithInMemoryDevelopState)
{
    TestEditorClient editor;
    editor.snapshot = selectedEditorSnapshot();
    TestExportEndpoint endpoint;
    McpExportTools tools(editor, endpoint, endpoint, true);

    const McpToolCallResult result = tools.callSubmitSelectedPhoto(
        QJsonObject{{QStringLiteral("output_locator"), QStringLiteral("C:/exports/result.jpeg")},
                    {QStringLiteral("jpeg_quality"), 84}});

    ASSERT_TRUE(result.argumentsValid);
    ASSERT_FALSE(result.isError);
    EXPECT_EQ(result.structuredContent.value(QStringLiteral("request_id")).toString(), QStringLiteral("17"));
    ASSERT_EQ(endpoint.submissions.size(), 1U);
    const core::client::SubmitExportCommand& command = endpoint.submissions.front();
    EXPECT_EQ(command.placement.policy, core::client::ExportPlacementPolicy::LocalOnly);
    ASSERT_TRUE(std::holds_alternative<core::client::ExportFileRequest>(command.request));
    const core::client::ExportFileRequest& file = std::get<core::client::ExportFileRequest>(command.request);
    EXPECT_EQ(file.outputLocator, "C:/exports/result.jpeg");
    ASSERT_TRUE(file.developParams.has_value());
    EXPECT_FLOAT_EQ(file.developParams->exposureEv, 1.25F);
    EXPECT_FLOAT_EQ(file.developParams->contrast, 0.2F);
    EXPECT_EQ(file.options.format, core::client::ExportRasterFormat::Jpeg);
    EXPECT_EQ(file.options.jpegQuality, 84);
}

TEST(McpExportToolsTest, RejectsWriteWithoutOptInSelectionOrJpegLocator)
{
    TestEditorClient editor;
    TestExportEndpoint endpoint;
    McpExportTools readOnlyTools(editor, endpoint, endpoint, false);
    const QJsonObject validArguments{{QStringLiteral("output_locator"), QStringLiteral("C:/exports/result.jpg")}};
    EXPECT_TRUE(readOnlyTools.callSubmitSelectedPhoto(validArguments).isError);

    McpExportTools writeTools(editor, endpoint, endpoint, true);
    EXPECT_TRUE(writeTools.callSubmitSelectedPhoto(validArguments).isError);
    editor.snapshot = selectedEditorSnapshot();
    const McpToolCallResult wrongExtension = writeTools.callSubmitSelectedPhoto(
        QJsonObject{{QStringLiteral("output_locator"), QStringLiteral("C:/exports/result.png")}});
    EXPECT_FALSE(wrongExtension.argumentsValid);
    const McpToolCallResult fractionalQuality = writeTools.callSubmitSelectedPhoto(
        QJsonObject{{QStringLiteral("output_locator"), QStringLiteral("C:/exports/result.jpg")},
                    {QStringLiteral("jpeg_quality"), 84.5}});
    EXPECT_FALSE(fractionalQuality.argumentsValid);
    EXPECT_TRUE(endpoint.submissions.empty());
}

TEST(McpExportToolsTest, ProjectsCompletedArtifactAndKeepsTechnicalFailureOutOfModelOutput)
{
    TestEditorClient editor;
    TestExportEndpoint endpoint;
    McpExportTools tools(editor, endpoint, endpoint, true);
    const core::client::ClientError failure{core::client::ClientErrorCode::DecodeFailed,
                                            "private source decoder diagnostic"};
    core::client::ExportReport report;
    report.totalCount = 1;
    report.failedCount = 1;
    report.items.push_back(
        {"C:/private/source.raw", "C:/exports/result.jpg", false, failure, core::client::ExportFailureKind::Execution});
    endpoint.publish({{1}, true, {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    endpoint.publish({{2},
                      false,
                      {},
                      std::nullopt,
                      std::nullopt,
                      core::client::ExportResult{{17}, report},
                      std::nullopt,
                      std::nullopt});

    const McpToolCallResult result =
        tools.callEvents(QJsonObject{{QStringLiteral("after_sequence"), QStringLiteral("0")}});

    ASSERT_TRUE(result.argumentsValid);
    const QJsonArray events = result.structuredContent.value(QStringLiteral("events")).toArray();
    ASSERT_EQ(events.size(), 2);
    const QJsonObject item = events[1]
                                 .toObject()
                                 .value(QStringLiteral("completed"))
                                 .toObject()
                                 .value(QStringLiteral("report"))
                                 .toObject()
                                 .value(QStringLiteral("items"))
                                 .toArray()[0]
                                 .toObject();
    EXPECT_EQ(item.value(QStringLiteral("output_locator")).toString(), QStringLiteral("C:/exports/result.jpg"));
    EXPECT_EQ(item.value(QStringLiteral("failure_kind")).toString(), QStringLiteral("execution"));
    const QByteArray modelOutput = QJsonDocument(result.structuredContent).toJson(QJsonDocument::Compact);
    EXPECT_FALSE(modelOutput.contains("private source decoder"));
    EXPECT_NE(result.diagnostic.find("private source decoder"), std::string::npos);
}

TEST(McpExportToolsTest, CancelsPrecisionSafeIdentityAndShutsDownSubscription)
{
    TestEditorClient editor;
    TestExportEndpoint endpoint;
    McpExportTools tools(editor, endpoint, endpoint, true);
    const McpToolCallResult cancelled =
        tools.callCancelRequest(QJsonObject{{QStringLiteral("request_id"), QStringLiteral("18446744073709551615")}});

    ASSERT_TRUE(cancelled.argumentsValid);
    ASSERT_EQ(endpoint.cancellations.size(), 1U);
    EXPECT_EQ(endpoint.cancellations.front().value, std::numeric_limits<std::uint64_t>::max());
    tools.shutdown();
    ASSERT_NE(endpoint.state, nullptr);
    EXPECT_FALSE(endpoint.state->active);
}

TEST(McpExportToolsTest, FailsClosedWhenEventSubscriptionCannotBeCreated)
{
    TestEditorClient editor;
    TestExportEndpoint endpoint;
    endpoint.subscribeError =
        core::client::ClientError{core::client::ClientErrorCode::Conflict, "Export delivery context unavailable"};
    McpExportTools tools(editor, endpoint, endpoint, true);

    EXPECT_FALSE(tools.ready());
    EXPECT_EQ(tools.startupDiagnostic(), "Export delivery context unavailable");
    EXPECT_TRUE(tools.callEvents(QJsonObject{}).isError);
}

}  // namespace
}  // namespace flexraw::mcp
