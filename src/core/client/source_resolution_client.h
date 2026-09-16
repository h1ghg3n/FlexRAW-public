#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog_photo_client.h"
#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

struct SourceRequestId
{
    std::uint64_t value{0};

    bool operator==(const SourceRequestId&) const = default;
};

enum class SourceRequestKind : std::uint8_t
{
    EstablishBaseline,
    VerifySource,
    AcceptReplacement,
    RegisterReplacementAsNew,
    RelinkSource,
};

struct SourceRequestReceipt
{
    SourceRequestId id;
    SourceRequestKind kind{SourceRequestKind::VerifySource};
    ClientPhotoId photoId;
    std::string sourceLocator;

    bool operator==(const SourceRequestReceipt&) const = default;
};

struct AcceptReplacementCommand
{
    ClientPhotoId photoId;
};

struct RegisterReplacementAsNewCommand
{
    ClientPhotoId photoId;
};

struct RelinkSourceCommand
{
    ClientPhotoId photoId;
    std::string sourceLocator;
};

struct SourceResolutionSnapshot
{
    std::vector<SourceRequestReceipt> activeRequests;

    bool operator==(const SourceResolutionSnapshot&) const = default;
};

struct SourceResolutionUpdate
{
    SourceRequestReceipt receipt;
    CatalogPhotoSnapshot photo;
    std::optional<CatalogPhotoSnapshot> createdPhoto;

    bool operator==(const SourceResolutionUpdate&) const = default;
};

struct SourceResolutionIssue
{
    SourceRequestReceipt receipt;
    ClientError error;

    bool operator==(const SourceResolutionIssue&) const = default;
};

enum class SourceResolutionTerminalState : std::uint8_t
{
    Completed,
    Failed,
    Cancelled,
};

struct SourceResolutionTerminal
{
    SourceRequestReceipt receipt;
    SourceResolutionTerminalState state{SourceResolutionTerminalState::Completed};
    std::optional<ClientError> error;

    bool operator==(const SourceResolutionTerminal&) const = default;
};

struct SourceResolutionEventSequence
{
    std::uint64_t value{0};

    bool operator==(const SourceResolutionEventSequence&) const = default;
};

struct SourceResolutionEvent
{
    SourceResolutionEventSequence sequence;
    bool initial{false};
    SourceResolutionSnapshot snapshot;
    std::optional<SourceRequestReceipt> accepted;
    std::optional<SourceResolutionUpdate> update;
    std::optional<SourceResolutionIssue> issue;
    std::optional<SourceResolutionTerminal> terminal;
};

using SourceRequestResult = ClientResult<SourceRequestReceipt, ClientError>;
using SourceRequestCancelResult = ClientResult<SourceRequestId, ClientError>;
using SourceResolutionCallback = std::function<void(const SourceResolutionEvent&)>;

class ISourceResolutionSubscription
{
public:
    // 목적: implementation별 Source Resolution subscription resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ISourceResolutionSubscription() = default;

    // 목적: queued event와 이후 Source Resolution callback 전달 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using SourceResolutionSubscriptionHandle = std::shared_ptr<ISourceResolutionSubscription>;
using SourceResolutionSubscriptionResult = ClientResult<SourceResolutionSubscriptionHandle, ClientError>;

class ISourceResolutionClient
{
public:
    // 목적: implementation별 Source Resolution command resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ISourceResolutionClient() = default;

    // 목적: 현재 replacement content를 기존 PhotoId의 새 source baseline으로 수용
    // 입력: command: develop state를 유지할 stable Photo identity
    // 출력: accepted background request receipt 또는 capability·session 오류
    [[nodiscard]] virtual SourceRequestResult acceptReplacement(const AcceptReplacementCommand& command) = 0;

    // 목적: 현재 replacement content에 새 PhotoId를 발급하고 기존 identity를 보존
    // 입력: command: source binding을 해제할 기존 stable Photo identity
    // 출력: accepted background request receipt 또는 capability·session 오류
    [[nodiscard]] virtual SourceRequestResult registerReplacementAsNew(
        const RegisterReplacementAsNewCommand& command) = 0;

    // 목적: 기존 PhotoId를 동일 content로 검증할 다른 source locator에 재연결
    // 입력: command: stable Photo identity와 normalized absolute UTF-8 lexical locator
    // 출력: accepted background request receipt 또는 validation·capability·session 오류
    [[nodiscard]] virtual SourceRequestResult relinkSource(const RelinkSourceCommand& command) = 0;

    // 목적: accepted source fingerprint request의 향후 mutation과 event publication 취소 요청
    // 입력: requestId: owner가 발급한 source request identity
    // 출력: 취소된 identity 또는 stale·validation 오류
    [[nodiscard]] virtual SourceRequestCancelResult cancelSourceRequest(SourceRequestId requestId) = 0;
};

class ISourceResolutionEventSource
{
public:
    // 목적: implementation별 Source Resolution event resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ISourceResolutionEventSource() = default;

    // 목적: adapter delivery context에서 initial active requests와 이후 accepted·update·issue·terminal 구독
    // 입력: callback: immutable Source Resolution event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual SourceResolutionSubscriptionResult subscribeToSourceResolution(
        SourceResolutionCallback callback) = 0;
};

}  // namespace flexraw::core::client
