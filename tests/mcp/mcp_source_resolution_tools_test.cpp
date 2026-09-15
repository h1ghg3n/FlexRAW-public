#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QJsonArray>
#include <QJsonDocument>

#include <gtest/gtest.h>

#include "mcp_source_resolution_tools.h"

namespace flexraw::mcp
{
namespace
{

struct TestSubscriptionState
{
    bool active{true};
    core::client::SourceResolutionCallback callback;
};

class TestSubscription final : public core::client::ISourceResolutionSubscription
{
public:
    // 목적: test event source와 MCP adapter가 공유할 subscription state 보관
    // 입력: state: callback과 active flag를 가진 shared state
    // 출력: RAII test subscription handle
    explicit TestSubscription(std::shared_ptr<TestSubscriptionState> state) : m_state(std::move(state)) {}

    // 목적: 마지막 handle 해제 시 이후 test callback 전달 차단
    // 입력: 없음
    // 출력: inactive subscription
    ~TestSubscription() override
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
    std::shared_ptr<TestSubscriptionState> m_state;
};

class TestSourceResolutionEndpoint final : public core::client::ISourceResolutionClient,
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

    // 목적: MCP cancellation identity를 기록하고 configured result 반환
    // 입력: requestId: 취소할 owner request identity
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

    // 목적: configured subscription result와 callback state 생성
    // 입력: callback: publish가 호출할 immutable event consumer
    // 출력: RAII handle 또는 configured error
    [[nodiscard]] core::client::SourceResolutionSubscriptionResult subscribeToSourceResolution(
        core::client::SourceResolutionCallback callback) override
    {
        if (subscribeError.has_value())
        {
            return core::client::SourceResolutionSubscriptionResult::failure(*subscribeError);
        }
        state = std::make_shared<TestSubscriptionState>();
        state->callback = std::move(callback);
        return core::client::SourceResolutionSubscriptionResult::success(std::make_shared<TestSubscription>(state));
    }

    // 목적: active test subscription에 Source Resolution event 전달
    // 입력: event: test가 조립한 ordered lifecycle event
    // 출력: inactive 상태이면 전달 없음
    void publish(const core::client::SourceResolutionEvent& event)
    {
        if (state != nullptr && state->active)
        {
            state->callback(event);
        }
    }

    std::vector<core::client::SourceRequestId> cancellations;
    std::optional<core::client::ClientError> cancelError;
    std::optional<core::client::ClientError> subscribeError;
    std::shared_ptr<TestSubscriptionState> state;

private:
    // 목적: 현재 test 범위 밖 Source command에 일관된 failure 생성
    // 입력: 없음
    // 출력: Unknown SourceRequestResult failure
    [[nodiscard]] static core::client::SourceRequestResult unsupported()
    {
        return core::client::SourceRequestResult::failure(
            {core::client::ClientErrorCode::Unknown, "Test Source command is not configured."});
    }
};

// 목적: test request context를 가진 Source receipt 생성
// 입력: requestId: owner identity
// 출력: stable Photo와 normalized locator가 포함된 receipt
[[nodiscard]] core::client::SourceRequestReceipt makeReceipt(std::uint64_t requestId)
{
    return {{requestId}, core::client::SourceRequestKind::EstablishBaseline, {42}, "C:/photos/source.raw"};
}

TEST(McpSourceResolutionToolsTest, ProjectsOrderedEventsAndKeepsTechnicalFailureOutOfModelOutput)
{
    TestSourceResolutionEndpoint endpoint;
    McpSourceResolutionTools tools(endpoint, endpoint, false);
    ASSERT_TRUE(tools.ready());
    const core::client::SourceRequestReceipt receipt = makeReceipt(9'007'199'254'740'993ULL);
    endpoint.publish({{1}, true, {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    endpoint.publish({{2}, false, {{receipt}}, receipt, std::nullopt, std::nullopt, std::nullopt});
    const core::client::ClientError failure{core::client::ClientErrorCode::NotFound,
                                            "private source path and fingerprint diagnostic"};
    endpoint.publish({{3},
                      false,
                      {},
                      std::nullopt,
                      std::nullopt,
                      core::client::SourceResolutionIssue{receipt, failure},
                      core::client::SourceResolutionTerminal{
                          receipt, core::client::SourceResolutionTerminalState::Failed, failure}});

    const McpToolCallResult result =
        tools.callEvents(QJsonObject{{QStringLiteral("after_sequence"), QStringLiteral("0")}});

    ASSERT_TRUE(result.argumentsValid);
    ASSERT_FALSE(result.isError);
    const QJsonArray events = result.structuredContent.value(QStringLiteral("events")).toArray();
    ASSERT_EQ(events.size(), 3);
    EXPECT_EQ(events[0].toObject().value(QStringLiteral("sequence")).toString(), QStringLiteral("1"));
    EXPECT_EQ(events[1]
                  .toObject()
                  .value(QStringLiteral("accepted"))
                  .toObject()
                  .value(QStringLiteral("request_id"))
                  .toString(),
              QStringLiteral("9007199254740993"));
    EXPECT_EQ(
        events[2].toObject().value(QStringLiteral("terminal")).toObject().value(QStringLiteral("state")).toString(),
        QStringLiteral("failed"));
    EXPECT_EQ(events[2]
                  .toObject()
                  .value(QStringLiteral("terminal"))
                  .toObject()
                  .value(QStringLiteral("error"))
                  .toObject()
                  .value(QStringLiteral("code"))
                  .toString(),
              QStringLiteral("not_found"));
    const QByteArray modelOutput = QJsonDocument(result.structuredContent).toJson(QJsonDocument::Compact);
    EXPECT_FALSE(modelOutput.contains("private source path"));
    EXPECT_NE(result.diagnostic.find("private source path"), std::string::npos);
}

TEST(McpSourceResolutionToolsTest, ProjectsEverySourceStateInUpdateEvents)
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
    TestSourceResolutionEndpoint endpoint;
    McpSourceResolutionTools tools(endpoint, endpoint, false);

    std::uint64_t sequence = 1;
    for (const auto& [state, expectedName] : cases)
    {
        const core::client::SourceRequestReceipt receipt = makeReceipt(sequence);
        core::client::CatalogPhotoSnapshot photo;
        photo.sourceState = state;
        endpoint.publish({{sequence},
                          false,
                          {},
                          std::nullopt,
                          core::client::SourceResolutionUpdate{receipt, photo, std::nullopt},
                          std::nullopt,
                          std::nullopt});

        const McpToolCallResult result =
            tools.callEvents(QJsonObject{{QStringLiteral("after_sequence"), QString::number(sequence - 1)}});
        const QJsonObject projectedPhoto = result.structuredContent.value(QStringLiteral("events"))
                                               .toArray()[0]
                                               .toObject()
                                               .value(QStringLiteral("update"))
                                               .toObject()
                                               .value(QStringLiteral("photo"))
                                               .toObject();
        EXPECT_EQ(QString::fromLatin1(expectedName), projectedPhoto.value(QStringLiteral("source_state")).toString());
        ++sequence;
    }
}

TEST(McpSourceResolutionToolsTest, BoundsRetainedWindowAndSignalsCursorGap)
{
    TestSourceResolutionEndpoint endpoint;
    McpSourceResolutionTools tools(endpoint, endpoint, false);
    ASSERT_TRUE(tools.ready());
    for (std::uint64_t sequence = 1; sequence <= 130; ++sequence)
    {
        endpoint.publish({{sequence}, true, {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    }

    const McpToolCallResult result = tools.callEvents(
        QJsonObject{{QStringLiteral("after_sequence"), QStringLiteral("1")}, {QStringLiteral("limit"), 64}});

    ASSERT_TRUE(result.argumentsValid);
    EXPECT_TRUE(result.structuredContent.value(QStringLiteral("gap")).toBool());
    EXPECT_TRUE(result.structuredContent.value(QStringLiteral("has_more")).toBool());
    EXPECT_EQ(result.structuredContent.value(QStringLiteral("oldest_available_sequence")).toString(),
              QStringLiteral("3"));
    const QJsonArray events = result.structuredContent.value(QStringLiteral("events")).toArray();
    ASSERT_EQ(events.size(), 64);
    EXPECT_EQ(events[0].toObject().value(QStringLiteral("sequence")).toString(), QStringLiteral("3"));

    const McpToolCallResult initialCursorResult =
        tools.callEvents(QJsonObject{{QStringLiteral("after_sequence"), QStringLiteral("0")}});
    EXPECT_TRUE(initialCursorResult.structuredContent.value(QStringLiteral("gap")).toBool());
}

TEST(McpSourceResolutionToolsTest, CancellationRequiresWriteOptInAndPreservesUnsignedIdentity)
{
    TestSourceResolutionEndpoint readOnlyEndpoint;
    McpSourceResolutionTools readOnlyTools(readOnlyEndpoint, readOnlyEndpoint, false);
    const QJsonObject arguments{{QStringLiteral("request_id"), QStringLiteral("18446744073709551615")}};
    const McpToolCallResult denied = readOnlyTools.callCancelRequest(arguments);
    ASSERT_TRUE(denied.isError);
    EXPECT_TRUE(readOnlyEndpoint.cancellations.empty());

    TestSourceResolutionEndpoint writeEndpoint;
    McpSourceResolutionTools writeTools(writeEndpoint, writeEndpoint, true);
    const McpToolCallResult cancelled = writeTools.callCancelRequest(arguments);

    ASSERT_TRUE(cancelled.argumentsValid);
    ASSERT_FALSE(cancelled.isError);
    ASSERT_EQ(writeEndpoint.cancellations.size(), 1U);
    EXPECT_EQ(writeEndpoint.cancellations.front().value, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(cancelled.structuredContent.value(QStringLiteral("cancelled_request_id")).toString(),
              QStringLiteral("18446744073709551615"));
}

TEST(McpSourceResolutionToolsTest, ShutdownUnsubscribesAndSuppressesLateEvent)
{
    TestSourceResolutionEndpoint endpoint;
    McpSourceResolutionTools tools(endpoint, endpoint, true);
    ASSERT_TRUE(tools.ready());
    ASSERT_NE(endpoint.state, nullptr);

    tools.shutdown();
    endpoint.publish({{1}, true, {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    EXPECT_FALSE(endpoint.state->active);
    const McpToolCallResult result = tools.callEvents(QJsonObject{});
    EXPECT_TRUE(result.structuredContent.value(QStringLiteral("events")).toArray().isEmpty());
}

TEST(McpSourceResolutionToolsTest, FailsClosedWhenEventSubscriptionCannotBeCreated)
{
    TestSourceResolutionEndpoint endpoint;
    endpoint.subscribeError =
        core::client::ClientError{core::client::ClientErrorCode::Conflict, "delivery context unavailable"};
    McpSourceResolutionTools tools(endpoint, endpoint, true);

    EXPECT_FALSE(tools.ready());
    EXPECT_EQ(tools.startupDiagnostic(), "delivery context unavailable");
    const McpToolCallResult result = tools.callEvents(QJsonObject{});
    ASSERT_TRUE(result.isError);
    EXPECT_EQ(
        result.structuredContent.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString(),
        QStringLiteral("conflict"));
}

}  // namespace
}  // namespace flexraw::mcp
