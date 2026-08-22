#include "remote_render_executor.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <utility>

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QTcpSocket>

#include "frame_codec.h"
#include "frame_parser.h"
#include "render_payload_codec.h"

namespace flexraw::worker::client
{
namespace
{

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds SocketPollInterval{25};
constexpr std::chrono::milliseconds WriteTimeout{5000};

// 목적: session-scoped protocol request에 사용할 non-zero JobId 생성
// 입력: 없음
// 출력: process 안에서 단조 증가하는 non-zero JobId
[[nodiscard]] protocol::JobId nextJobId() noexcept
{
    static std::atomic<protocol::JobId> next{1};
    protocol::JobId value = next.fetch_add(1, std::memory_order_relaxed);
    while (value == 0)
    {
        value = next.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

// 목적: client-side 진단을 typed remote render failure로 조립
// 입력: code: transport 분류, coreCode/message: 공통 오류 값, stats/retryAfter: optional remote evidence
// 출력: RemoteRenderResult failure
[[nodiscard]] RemoteRenderResult makeFailure(const RemoteRenderErrorCode code,
                                             const core::types::ErrorCode coreCode,
                                             QString message,
                                             const core::measurement::RenderStats& stats = {},
                                             std::optional<std::chrono::milliseconds> retryAfter = std::nullopt)
{
    return RemoteRenderResult::failure({code, {coreCode, std::move(message)}, stats, retryAfter});
}

// 목적: 현재 cancellation state를 일관된 remote failure로 변환
// 입력: stats: remote terminal response가 제공한 optional stage timing
// 출력: Cancelled RemoteRenderResult
[[nodiscard]] RemoteRenderResult makeCancelled(const core::measurement::RenderStats& stats = {})
{
    return makeFailure(RemoteRenderErrorCode::Cancelled,
                       core::types::ErrorCode::Cancelled,
                       QStringLiteral("Remote render cancellation requested."),
                       stats);
}

// 목적: Qt blocking wait에 사용할 bounded millisecond 구간 계산
// 입력: timer: 시작된 elapsed timer, timeout: 전체 제한
// 출력: 남은 시간과 poll interval 중 작은 positive int
[[nodiscard]] int nextWaitMilliseconds(const QElapsedTimer& timer, const std::chrono::milliseconds timeout)
{
    const qint64 remaining = timeout.count() - timer.elapsed();
    const qint64 bounded = std::clamp<qint64>(remaining, 1, SocketPollInterval.count());
    return static_cast<int>(bounded);
}

// 목적: endpoint와 timeout 값이 blocking socket contract에 유효한지 확인
// 입력: endpoint: 검증할 manual TCP endpoint
// 출력: 유효하면 true
[[nodiscard]] bool isValidEndpoint(const RemoteRenderEndpoint& endpoint)
{
    constexpr auto MaximumQtTimeout = std::chrono::milliseconds(std::numeric_limits<int>::max());
    return !endpoint.host.trimmed().isEmpty() && endpoint.port != 0 && endpoint.connectTimeout > 0ms &&
           endpoint.connectTimeout <= MaximumQtTimeout && endpoint.renderTimeout > 0ms &&
           endpoint.cancellationTimeout > 0ms && endpoint.cancellationTimeout <= MaximumQtTimeout;
}

class RemoteRenderSession final
{
public:
    // 목적: 한 execute call 전용 socket/session state 초기화
    // 입력: endpoint/request/token/jobId: immutable remote operation 값
    // 출력: 독립적으로 연결하고 종료할 session
    RemoteRenderSession(const RemoteRenderEndpoint& endpoint,
                        const RemoteRenderRequest& request,
                        const core::types::CancellationToken& cancellationToken,
                        const protocol::JobId jobId,
                        const RemoteRenderAcceptedCallback& accepted)
        : m_endpoint(endpoint),
          m_request(request),
          m_cancellationToken(cancellationToken),
          m_jobId(jobId),
          m_acceptedCallback(accepted)
    {}

    // 목적: request encode, TCP connect, submit과 terminal response 처리를 순서대로 실행
    // 입력: 없음
    // 출력: remote render 성공 또는 typed terminal failure
    [[nodiscard]] RemoteRenderResult execute()
    {
        const runtime::EncodePayloadResult payload = runtime::encodeRenderRequestPayload({m_request.sourceRelativePath,
                                                                                          m_request.outputRelativePath,
                                                                                          m_request.developParams,
                                                                                          m_request.outputOptions});
        if (payload.hasError())
        {
            return makeFailure(RemoteRenderErrorCode::InvalidRequest,
                               core::types::ErrorCode::InvalidArgument,
                               payload.error().message);
        }
        if (m_cancellationToken.isCancellationRequested())
        {
            return makeCancelled();
        }
        if (const std::optional<RemoteRenderResult> connectionFailure = connectToWorker();
            connectionFailure.has_value())
        {
            return *connectionFailure;
        }
        if (const std::optional<RemoteRenderResult> writeFailure =
                sendFrame({protocol::MessageType::RenderRequest, m_jobId, payload.value()}, true);
            writeFailure.has_value())
        {
            return *writeFailure;
        }
        return waitForTerminal();
    }

private:
    // 목적: configured timeout 안에서 Worker endpoint에 연결하고 cancellation 우선순위 보존
    // 입력: 없음
    // 출력: 성공 시 empty, 실패 또는 취소 시 terminal result
    [[nodiscard]] std::optional<RemoteRenderResult> connectToWorker()
    {
        m_socket.connectToHost(m_endpoint.host.trimmed(), m_endpoint.port);
        QElapsedTimer timer;
        timer.start();
        while (m_socket.state() != QAbstractSocket::ConnectedState &&
               timer.elapsed() < m_endpoint.connectTimeout.count())
        {
            if (m_cancellationToken.isCancellationRequested())
            {
                m_socket.abort();
                return makeCancelled();
            }
            const qint64 remaining = m_endpoint.connectTimeout.count() - timer.elapsed();
            const bool connected = m_socket.waitForConnected(static_cast<int>(std::max<qint64>(remaining, 1)));
            if (m_cancellationToken.isCancellationRequested())
            {
                m_socket.abort();
                return makeCancelled();
            }
            if (connected)
            {
                return std::nullopt;
            }
            if (m_socket.state() == QAbstractSocket::UnconnectedState &&
                m_socket.error() != QAbstractSocket::SocketTimeoutError &&
                m_socket.error() != QAbstractSocket::UnknownSocketError)
            {
                return makeFailure(
                    RemoteRenderErrorCode::ConnectionFailed, core::types::ErrorCode::Unknown, m_socket.errorString());
            }
        }
        if (m_socket.state() == QAbstractSocket::ConnectedState)
        {
            return std::nullopt;
        }
        m_socket.abort();
        return makeFailure(RemoteRenderErrorCode::ConnectionFailed,
                           core::types::ErrorCode::Unknown,
                           QStringLiteral("Timed out connecting to the Render Worker."));
    }

    // 목적: frame 전체를 socket write queue에 넣고 bounded flush
    // 입력: frame: 전송할 protocol frame, observeCancellation: 일반 request 전송 중 cancellation 반영 여부
    // 출력: 성공 시 empty, encode/write/connection/cancellation failure
    [[nodiscard]] std::optional<RemoteRenderResult> sendFrame(const protocol::ProtocolFrame& frame,
                                                              const bool observeCancellation)
    {
        const protocol::EncodeFrameResult encoded = protocol::encodeFrame(frame);
        if (encoded.hasError())
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::InvalidArgument,
                               encoded.error().message);
        }

        qint64 offset = 0;
        while (offset < encoded.value().size())
        {
            if (observeCancellation && m_cancellationToken.isCancellationRequested())
            {
                return makeCancelled();
            }
            const qint64 written =
                m_socket.write(encoded.value().constData() + offset, encoded.value().size() - offset);
            if (written < 0)
            {
                return makeFailure(
                    RemoteRenderErrorCode::ConnectionLost, core::types::ErrorCode::Unknown, m_socket.errorString());
            }
            if (written == 0)
            {
                if (!m_socket.waitForBytesWritten(static_cast<int>(SocketPollInterval.count())) &&
                    m_socket.state() == QAbstractSocket::UnconnectedState)
                {
                    return makeFailure(
                        RemoteRenderErrorCode::ConnectionLost, core::types::ErrorCode::Unknown, m_socket.errorString());
                }
                continue;
            }
            offset += written;
        }

        QElapsedTimer timer;
        timer.start();
        while (m_socket.bytesToWrite() > 0 && timer.elapsed() < WriteTimeout.count())
        {
            if (observeCancellation && m_cancellationToken.isCancellationRequested())
            {
                return makeCancelled();
            }
            static_cast<void>(m_socket.waitForBytesWritten(nextWaitMilliseconds(timer, WriteTimeout)));
            if (m_socket.state() == QAbstractSocket::UnconnectedState)
            {
                return makeFailure(
                    RemoteRenderErrorCode::ConnectionLost, core::types::ErrorCode::Unknown, m_socket.errorString());
            }
        }
        if (m_socket.bytesToWrite() > 0)
        {
            return makeFailure(RemoteRenderErrorCode::TimedOut,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Timed out writing a Render Worker frame."));
        }
        return std::nullopt;
    }

    // 목적: cancellation 시 CancelRequest를 정확히 한 번 전송
    // 입력: 없음
    // 출력: 전송 성공 시 empty, connection/protocol failure
    [[nodiscard]] std::optional<RemoteRenderResult> sendCancellation()
    {
        if (m_cancelSent)
        {
            return std::nullopt;
        }
        m_cancelSent = true;
        m_cancellationTimer.start();
        return sendFrame({protocol::MessageType::CancelRequest, m_jobId, {}}, false);
    }

    // 목적: incremental frame을 읽어 accepted와 terminal response contract 해석
    // 입력: 없음
    // 출력: success/failure terminal response 또는 timeout/connection loss
    [[nodiscard]] RemoteRenderResult waitForTerminal()
    {
        QElapsedTimer renderTimer;
        renderTimer.start();
        while (renderTimer.elapsed() < m_endpoint.renderTimeout.count())
        {
            if (m_cancellationToken.isCancellationRequested())
            {
                if (const std::optional<RemoteRenderResult> cancelFailure = sendCancellation();
                    cancelFailure.has_value())
                {
                    m_socket.abort();
                    return makeCancelled();
                }
                if (m_cancellationTimer.elapsed() >= m_endpoint.cancellationTimeout.count())
                {
                    m_socket.abort();
                    return makeCancelled();
                }
            }

            if (m_socket.bytesAvailable() == 0)
            {
                static_cast<void>(m_socket.waitForReadyRead(static_cast<int>(SocketPollInterval.count())));
            }
            if (const std::optional<RemoteRenderResult> terminal = readAvailableFrames(); terminal.has_value())
            {
                return *terminal;
            }
            if (m_socket.state() == QAbstractSocket::UnconnectedState && m_socket.bytesAvailable() == 0)
            {
                return m_cancellationToken.isCancellationRequested()
                           ? makeCancelled()
                           : makeFailure(RemoteRenderErrorCode::ConnectionLost,
                                         core::types::ErrorCode::Unknown,
                                         QStringLiteral("Render Worker disconnected before a terminal response."));
            }
        }
        m_socket.abort();
        return m_cancellationToken.isCancellationRequested()
                   ? makeCancelled()
                   : makeFailure(RemoteRenderErrorCode::TimedOut,
                                 core::types::ErrorCode::Unknown,
                                 QStringLiteral("Timed out waiting for the Render Worker result."));
    }

    // 목적: 현재 socket bytes를 parser에 공급하고 완성 frame 처리
    // 입력: 없음
    // 출력: terminal frame이 있으면 result, 더 기다려야 하면 empty
    [[nodiscard]] std::optional<RemoteRenderResult> readAvailableFrames()
    {
        const QByteArray bytes = m_socket.readAll();
        if (bytes.isEmpty())
        {
            return std::nullopt;
        }
        protocol::ParseFramesOutcome outcome = m_parser.append(bytes);
        for (const protocol::ProtocolFrame& frame : outcome.frames)
        {
            if (const std::optional<RemoteRenderResult> terminal = handleFrame(frame); terminal.has_value())
            {
                return terminal;
            }
        }
        if (outcome.terminalError.has_value())
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               outcome.terminalError->message);
        }
        return std::nullopt;
    }

    // 목적: 현재 JobId의 accepted 또는 terminal frame을 typed result로 변환
    // 입력: frame: envelope와 payload 검증을 마친 server response
    // 출력: accepted이면 empty, terminal 또는 protocol violation이면 result
    [[nodiscard]] std::optional<RemoteRenderResult> handleFrame(const protocol::ProtocolFrame& frame)
    {
        if (frame.jobId != m_jobId)
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Render Worker response JobId did not match the request."));
        }
        switch (frame.messageType)
        {
        case protocol::MessageType::JobAccepted:
            if (m_accepted || !frame.payload.isEmpty())
            {
                return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                                   core::types::ErrorCode::Unknown,
                                   QStringLiteral("Render Worker sent an invalid JobAccepted response."));
            }
            m_accepted = true;
            if (m_acceptedCallback)
            {
                m_acceptedCallback();
            }
            return std::nullopt;
        case protocol::MessageType::RenderSucceeded:
            return handleSuccess(frame);
        case protocol::MessageType::RenderFailed:
            return handleRenderFailure(frame);
        case protocol::MessageType::ServerBusy:
            return handleServerBusy(frame);
        case protocol::MessageType::ResourceBusy:
            return handleResourceBusy(frame);
        case protocol::MessageType::RenderRequest:
        case protocol::MessageType::CancelRequest:
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Render Worker sent a client-only message type."));
        }
        return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                           core::types::ErrorCode::Unknown,
                           QStringLiteral("Render Worker sent an unknown response."));
    }

    // 목적: accepted RenderSucceeded payload를 공통 render success로 변환
    // 입력: frame: 현재 JobId의 success frame
    // 출력: artifact/stats 또는 ordering/payload failure
    [[nodiscard]] RemoteRenderResult handleSuccess(const protocol::ProtocolFrame& frame) const
    {
        if (!m_accepted)
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Render Worker success arrived before JobAccepted."));
        }
        const runtime::DecodeRenderSucceededResult decoded = runtime::decodeRenderSucceededPayload(frame.payload);
        if (decoded.hasError())
        {
            return makeFailure(
                RemoteRenderErrorCode::ProtocolViolation, core::types::ErrorCode::Unknown, decoded.error().message);
        }
        if (decoded.value().outputRelativePath != m_request.outputRelativePath)
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Render Worker returned an unexpected artifact path."));
        }
        if (m_cancellationToken.isCancellationRequested())
        {
            return makeCancelled(decoded.value().stats);
        }
        return RemoteRenderResult::success(
            {{decoded.value().outputRelativePath, decoded.value().byteSize}, decoded.value().stats});
    }

    // 목적: RenderFailed payload의 CoreError와 stage timing 보존
    // 입력: frame: accepted 또는 pre-accept validation failure frame
    // 출력: Cancelled 또는 RenderFailed typed result
    [[nodiscard]] RemoteRenderResult handleRenderFailure(const protocol::ProtocolFrame& frame) const
    {
        const runtime::DecodeRenderFailedResult decoded = runtime::decodeRenderFailedPayload(frame.payload);
        if (decoded.hasError())
        {
            return makeFailure(
                RemoteRenderErrorCode::ProtocolViolation, core::types::ErrorCode::Unknown, decoded.error().message);
        }
        if (m_cancellationToken.isCancellationRequested() ||
            decoded.value().cause.code == core::types::ErrorCode::Cancelled)
        {
            return makeCancelled(decoded.value().stats);
        }
        return RemoteRenderResult::failure(
            {RemoteRenderErrorCode::RenderFailed, decoded.value().cause, decoded.value().stats, std::nullopt});
    }

    // 목적: pre-accept scheduler rejection을 구조화된 busy failure로 변환
    // 입력: frame: ServerBusy payload
    // 출력: ServerBusy 또는 ordering/payload failure
    [[nodiscard]] RemoteRenderResult handleServerBusy(const protocol::ProtocolFrame& frame) const
    {
        if (m_accepted)
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Render Worker sent ServerBusy after JobAccepted."));
        }
        const runtime::DecodeServerBusyResult decoded = runtime::decodeServerBusyPayload(frame.payload);
        if (decoded.hasError())
        {
            return makeFailure(
                RemoteRenderErrorCode::ProtocolViolation, core::types::ErrorCode::Unknown, decoded.error().message);
        }
        return makeFailure(
            RemoteRenderErrorCode::ServerBusy, core::types::ErrorCode::Conflict, decoded.value().message);
    }

    // 목적: accepted resource admission rejection과 retry advice 보존
    // 입력: frame: ResourceBusy payload
    // 출력: ResourceBusy 또는 ordering/payload failure
    [[nodiscard]] RemoteRenderResult handleResourceBusy(const protocol::ProtocolFrame& frame) const
    {
        if (!m_accepted)
        {
            return makeFailure(RemoteRenderErrorCode::ProtocolViolation,
                               core::types::ErrorCode::Unknown,
                               QStringLiteral("Render Worker sent ResourceBusy before JobAccepted."));
        }
        const runtime::DecodeResourceBusyResult decoded = runtime::decodeResourceBusyPayload(frame.payload);
        if (decoded.hasError())
        {
            return makeFailure(
                RemoteRenderErrorCode::ProtocolViolation, core::types::ErrorCode::Unknown, decoded.error().message);
        }
        return makeFailure(RemoteRenderErrorCode::ResourceBusy,
                           core::types::ErrorCode::Conflict,
                           decoded.value().message,
                           {},
                           decoded.value().retryAfter);
    }

    const RemoteRenderEndpoint& m_endpoint;
    const RemoteRenderRequest& m_request;
    const core::types::CancellationToken& m_cancellationToken;
    protocol::JobId m_jobId{0};
    QTcpSocket m_socket;
    protocol::FrameParser m_parser;
    QElapsedTimer m_cancellationTimer;
    RemoteRenderAcceptedCallback m_acceptedCallback;
    bool m_accepted{false};
    bool m_cancelSent{false};
};

}  // namespace

// 목적: accepted observer를 지원하지 않는 기존 executor implementation의 호환 실행
// 입력: endpoint/request/token: 실행 값, accepted: 기본 implementation에서는 사용하지 않음
// 출력: 기존 3-argument execute 결과
RemoteRenderResult IRemoteRenderExecutor::execute(const RemoteRenderEndpoint& endpoint,
                                                  const RemoteRenderRequest& request,
                                                  const core::types::CancellationToken& cancellationToken,
                                                  const RemoteRenderAcceptedCallback& accepted) const
{
    static_cast<void>(accepted);
    return execute(endpoint, request, cancellationToken);
}

// 목적: manual Worker endpoint에서 root-relative single-RAW render를 동기 실행
// 입력: endpoint: TCP 주소와 timeout, request: Worker root 기준 경로와 처리 값, cancellationToken: cooperative 중단
// 상태 출력: remote artifact/stats 또는 connection, protocol, busy, render failure
RemoteRenderResult RemoteRenderExecutor::execute(const RemoteRenderEndpoint& endpoint,
                                                 const RemoteRenderRequest& request,
                                                 const core::types::CancellationToken& cancellationToken) const
{
    return execute(endpoint, request, cancellationToken, {});
}

// 목적: JobAccepted observer를 포함해 manual Worker endpoint에서 동기 render 실행
// 입력: endpoint/request/token: 실행 값, accepted: 유효한 JobAccepted 수신 callback
// 출력: remote artifact/stats 또는 typed transport/worker 오류
RemoteRenderResult RemoteRenderExecutor::execute(const RemoteRenderEndpoint& endpoint,
                                                 const RemoteRenderRequest& request,
                                                 const core::types::CancellationToken& cancellationToken,
                                                 const RemoteRenderAcceptedCallback& accepted) const
{
    if (!isValidEndpoint(endpoint))
    {
        return makeFailure(RemoteRenderErrorCode::InvalidEndpoint,
                           core::types::ErrorCode::InvalidArgument,
                           QStringLiteral("Remote Render Worker endpoint is invalid."));
    }
    RemoteRenderSession session(endpoint, request, cancellationToken, nextJobId(), accepted);
    return session.execute();
}

}  // namespace flexraw::worker::client
