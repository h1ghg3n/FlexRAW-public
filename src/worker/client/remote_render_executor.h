#pragma once

#include <functional>

#include "operation_types.h"
#include "remote_render_contracts.h"

namespace flexraw::worker::client
{

using RemoteRenderAcceptedCallback = std::function<void()>;

class IRemoteRenderExecutor
{
public:
    // 목적: injected remote executor implementation을 interface pointer로 안전하게 소멸
    // 입력: 없음
    // 출력: 없음
    virtual ~IRemoteRenderExecutor() = default;

    // 목적: manual endpoint에 한 개 render request를 동기 실행
    // 입력: endpoint: TCP 주소/timeout, request: relative path와 processing 값, cancellationToken: 취소 상태
    // 출력: remote artifact/stats 또는 typed transport/worker 오류
    [[nodiscard]] virtual RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                                     const RemoteRenderRequest& request,
                                                     const core::types::CancellationToken& cancellationToken) const = 0;

    // 목적: Worker JobAccepted 관찰이 필요한 caller에 동기 실행과 accepted event 제공
    // 입력: endpoint/request/token: 실행 값, accepted: 같은 execute thread에서 0~1회 호출하고 저장하지 않는 callback
    // 출력: callback 완료와 내부 실행이 모두 quiescent한 remote artifact/stats 또는 typed transport/worker 오류
    [[nodiscard]] virtual RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                                     const RemoteRenderRequest& request,
                                                     const core::types::CancellationToken& cancellationToken,
                                                     const RemoteRenderAcceptedCallback& accepted) const = 0;
};

class RemoteRenderExecutor final : public IRemoteRenderExecutor
{
public:
    // 목적: manual Worker endpoint에서 root-relative single-RAW render를 동기 실행
    // 입력: endpoint: TCP 주소와 timeout, request: Worker root 기준 경로와 처리 값, cancellationToken: cooperative 중단
    // 상태 출력: remote artifact/stats 또는 connection, protocol, busy, render failure
    [[nodiscard]] RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                             const RemoteRenderRequest& request,
                                             const core::types::CancellationToken& cancellationToken) const override;

    // 목적: JobAccepted observer를 포함해 manual Worker endpoint에서 동기 render 실행
    // 입력: endpoint/request/token: 실행 값, accepted: 같은 execute thread에서 0~1회 호출하고 저장하지 않는 callback
    // 출력: callback 완료와 session 종료 뒤 remote artifact/stats 또는 typed transport/worker 오류
    [[nodiscard]] RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                             const RemoteRenderRequest& request,
                                             const core::types::CancellationToken& cancellationToken,
                                             const RemoteRenderAcceptedCallback& accepted) const override;
};

}  // namespace flexraw::worker::client
