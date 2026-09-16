#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "client_error.h"
#include "client_result.h"
#include "editor_client.h"

namespace flexraw::core::client
{

struct EditorEventSequence
{
    std::uint64_t value{0};

    bool operator==(const EditorEventSequence&) const = default;
};

struct EditorStateEvent
{
    EditorEventSequence sequence;
    bool initial{false};
    EditorSnapshot snapshot;

    bool operator==(const EditorStateEvent&) const = default;
};

using EditorStateCallback = std::function<void(const EditorStateEvent&)>;

class IEditorStateSubscription
{
public:
    // 목적: implementation별 subscription resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IEditorStateSubscription() = default;

    // 목적: queued event와 이후 Editor state callback 전달을 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using EditorStateSubscriptionHandle = std::shared_ptr<IEditorStateSubscription>;
using EditorStateSubscriptionResult = ClientResult<EditorStateSubscriptionHandle, ClientError>;

class IEditorStateEventSource
{
public:
    // 목적: implementation별 event source resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IEditorStateEventSource() = default;

    // 목적: adapter delivery context에서 Editor immutable snapshot event 구독
    // 입력: callback: 초기 snapshot과 이후 state 변경을 받을 consumer 함수
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual EditorStateSubscriptionResult subscribeToEditorState(EditorStateCallback callback) = 0;
};

}  // namespace flexraw::core::client
