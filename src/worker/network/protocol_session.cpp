#include "protocol_session.h"

#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QTcpSocket>

#include "frame_codec.h"
#include "health_payload_codec.h"
#include "render_payload_codec.h"

namespace flexraw::worker::network
{
namespace
{

constexpr qint64 MaximumReadBytesPerTurn = protocol::ProtocolHeaderBytes * 64;

// 목적: Worker path 오류를 client-facing CoreError 분류로 축소
// 입력: error: root/path resolver 상세 오류
// 출력: 경로 정보를 노출하지 않는 render failure
[[nodiscard]] core::types::CoreError makePathCoreError(const runtime::WorkerPathError& error)
{
    const core::types::ErrorCode code = error.code == runtime::WorkerPathErrorCode::SourceNotFound ||
                                                error.code == runtime::WorkerPathErrorCode::OutputParentNotFound
                                            ? core::types::ErrorCode::NotFound
                                            : core::types::ErrorCode::InvalidArgument;
    return {code, error.message};
}

// 목적: payload decode 오류를 client-facing invalid argument로 변환
// 입력: error: schema/value codec 오류
// 출력: RenderFailed에 넣을 CoreError
[[nodiscard]] core::types::CoreError makePayloadCoreError(const runtime::PayloadError& error)
{
    return {core::types::ErrorCode::InvalidArgument, error.message};
}

// 목적: wire correlation identity를 Runtime-owned job identity로 명시적으로 변환
// 입력: jobId: protocol frame의 fixed-width JobId
// 출력: 같은 numeric identity를 보존한 Runtime 값
[[nodiscard]] runtime::RenderJobId toRuntimeJobId(const protocol::JobId jobId) noexcept
{
    return {jobId};
}

// 목적: Runtime terminal identity를 wire response correlation 값으로 명시적으로 변환
// 입력: jobId: Runtime-owned fixed-width job identity
// 출력: 같은 numeric identity를 보존한 protocol JobId
[[nodiscard]] protocol::JobId toProtocolJobId(const runtime::RenderJobId jobId) noexcept
{
    return jobId.value;
}

}  // namespace

// 목적: 연결된 socket을 session-scoped render protocol adapter로 구성
// 입력: sessionId/socket: 연결 identity와 소유 대상, runtime: shared Runtime port, configuration: I/O 한도
// 출력: readyRead/disconnect/timeout을 처리하는 session
ProtocolSession::ProtocolSession(const runtime::WorkerSessionId sessionId,
                                 QTcpSocket* const socket,
                                 runtime::IRenderWorkerRuntime& runtime,
                                 ProtocolSessionConfiguration configuration,
                                 QObject* const parent)
    : QObject(parent),
      m_sessionId(sessionId),
      m_socket(socket),
      m_runtime(runtime),
      m_configuration(std::move(configuration))
{
    Q_ASSERT(m_sessionId != 0);
    Q_ASSERT(m_socket != nullptr);
    m_socket->setParent(this);
    m_inactivityTimer.setSingleShot(true);
    connect(m_socket, &QTcpSocket::readyRead, this, &ProtocolSession::handleReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ProtocolSession::handleDisconnected);
    connect(&m_inactivityTimer, &QTimer::timeout, this, &ProtocolSession::handleInactivityTimeout);
    if (m_configuration.inactivityTimeoutMilliseconds > 0)
    {
        m_inactivityTimer.start(m_configuration.inactivityTimeoutMilliseconds);
    }
}

// 목적: session의 active job을 취소하고 socket 종료
// 입력: 없음
// 출력: disconnect 또는 즉시 finished signal로 이어지는 closing 상태
void ProtocolSession::close()
{
    if (m_closing)
    {
        return;
    }
    m_closing = true;
    m_inactivityTimer.stop();
    cancelActiveJobs();
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
    {
        finishOnce();
        return;
    }
    m_socket->disconnectFromHost();
}

// 목적: server가 부여한 process-local session identity 조회
// 입력: 없음
// 출력: non-zero WorkerSessionId
runtime::WorkerSessionId ProtocolSession::sessionId() const noexcept
{
    return m_sessionId;
}

// 목적: bounded socket chunk를 parser에 공급하고 완성 frame 처리
// 입력: 없음
// 출력: frame response, 후속 queued read 또는 terminal disconnect
void ProtocolSession::handleReadyRead()
{
    m_readContinuationQueued = false;
    if (m_closing)
    {
        return;
    }
    if (m_configuration.inactivityTimeoutMilliseconds > 0)
    {
        m_inactivityTimer.start(m_configuration.inactivityTimeoutMilliseconds);
    }

    const QByteArray bytes = m_socket->read(MaximumReadBytesPerTurn);
    const protocol::ParseFramesOutcome outcome = m_parser.append(bytes);
    for (const protocol::ProtocolFrame& frame : outcome.frames)
    {
        if (!handleFrame(frame))
        {
            close();
            return;
        }
    }
    if (outcome.terminalError.has_value())
    {
        close();
        return;
    }

    if (m_socket->bytesAvailable() > 0 && !m_readContinuationQueued)
    {
        m_readContinuationQueued = true;
        QMetaObject::invokeMethod(
            this,
            [this]() {
                m_readContinuationQueued = false;
                handleReadyRead();
            },
            Qt::QueuedConnection);
    }
}

// 목적: peer/socket disconnect를 active cancellation과 session 완료로 변환
// 입력: 없음
// 출력: 정확히 한 번의 finished signal
void ProtocolSession::handleDisconnected()
{
    m_closing = true;
    m_inactivityTimer.stop();
    cancelActiveJobs();
    finishOnce();
}

// 목적: partial frame 또는 idle connection timeout 처리
// 입력: 없음
// 출력: active render가 없으면 close, 있으면 timeout 재시작
void ProtocolSession::handleInactivityTimeout()
{
    if (!m_activeJobIds.empty() && m_parser.bufferedByteCount() == 0)
    {
        m_inactivityTimer.start(m_configuration.inactivityTimeoutMilliseconds);
        return;
    }
    close();
}

// 목적: client 방향에서 허용된 request/cancel frame dispatch
// 입력: frame: envelope 검증이 끝난 wire frame
// 출력: session을 계속 유지할 수 있으면 true
bool ProtocolSession::handleFrame(const protocol::ProtocolFrame& frame)
{
    switch (frame.messageType)
    {
    case protocol::MessageType::RenderRequest:
        return handleRenderRequest(frame);
    case protocol::MessageType::CancelRequest:
        return handleCancelRequest(frame);
    case protocol::MessageType::HealthRequest:
        return handleHealthRequest(frame);
    case protocol::MessageType::JobAccepted:
    case protocol::MessageType::RenderSucceeded:
    case protocol::MessageType::RenderFailed:
    case protocol::MessageType::ServerBusy:
    case protocol::MessageType::ResourceBusy:
    case protocol::MessageType::HealthResponse:
        return false;
    }
    return false;
}

// 목적: render payload decode/Runtime submit과 accepted/busy 응답 수행
// 입력: frame: RenderRequest message와 session JobId
// 출력: semantic 처리를 계속할 수 있으면 true
bool ProtocolSession::handleRenderRequest(const protocol::ProtocolFrame& frame)
{
    if (m_activeJobIds.contains(frame.jobId))
    {
        return false;
    }

    const runtime::DecodeRenderRequestResult decoded = runtime::decodeRenderRequestPayload(frame.payload);
    if (decoded.hasError())
    {
        return sendFailure(frame.jobId, makePayloadCoreError(decoded.error()));
    }
    QPointer<ProtocolSession> session(this);
    QCoreApplication* const dispatchContext = QCoreApplication::instance();
    if (dispatchContext == nullptr)
    {
        return false;
    }
    const runtime::RenderRequestPayload& payload = decoded.value();
    runtime::RenderWorkerCommand command{
        {m_sessionId, toRuntimeJobId(frame.jobId)},
        {payload.sourceRelativePath, payload.outputRelativePath, payload.developParams, payload.outputOptions}};
    const runtime::RenderWorkerSubmitResult submitted =
        m_runtime.submit(std::move(command), [session, dispatchContext](runtime::RenderJobOutcome outcome) mutable {
            QMetaObject::invokeMethod(
                dispatchContext,
                [session, outcome = std::move(outcome)]() mutable {
                    if (session)
                    {
                        session->handleOutcome(std::move(outcome));
                    }
                },
                Qt::QueuedConnection);
        });
    if (submitted.hasError())
    {
        return sendFailure(frame.jobId, makePathCoreError(submitted.error()));
    }

    switch (submitted.value())
    {
    case runtime::SubmitStatus::Accepted:
        m_activeJobIds.insert(frame.jobId);
        return sendFrame({protocol::MessageType::JobAccepted, frame.jobId, {}});
    case runtime::SubmitStatus::QueueFull:
        return sendBusy(frame.jobId, QStringLiteral("Worker render queue is full."));
    case runtime::SubmitStatus::ShuttingDown:
        return sendBusy(frame.jobId, QStringLiteral("Worker is shutting down."));
    case runtime::SubmitStatus::InvalidJobId:
    case runtime::SubmitStatus::DuplicateJobId:
        return false;
    }
    return false;
}

// 목적: active render에 cooperative cancellation 요청 전달
// 입력: frame: empty payload CancelRequest
// 출력: payload 방향 계약이 유효하면 true
bool ProtocolSession::handleCancelRequest(const protocol::ProtocolFrame& frame)
{
    if (!frame.payload.isEmpty())
    {
        return false;
    }
    if (!m_activeJobIds.contains(frame.jobId))
    {
        return true;
    }
    // Runtime terminal이 Qt delivery queue를 먼저 이긴 경우에도 이미 예약된 terminal을 보존한다.
    static_cast<void>(m_runtime.cancel({m_sessionId, toRuntimeJobId(frame.jobId)}));
    return true;
}

// 목적: Runtime read-only snapshot을 bounded health response로 투영
// 입력: frame: empty payload HealthRequest와 correlation identity
// 출력: payload 방향과 response write가 유효하면 true
bool ProtocolSession::handleHealthRequest(const protocol::ProtocolFrame& frame)
{
    if (!frame.payload.isEmpty())
    {
        return false;
    }
    const runtime::WorkerRuntimeSnapshot snapshot = m_runtime.snapshot();
    const protocol::HealthResponsePayload payload{
        snapshot.accepting ? protocol::HealthServiceState::Ready : protocol::HealthServiceState::ShuttingDown,
        snapshot.queued,
        snapshot.running,
        snapshot.maximumConcurrency,
        snapshot.queueCapacity,
    };
    const protocol::EncodeHealthPayloadResult encoded = protocol::encodeHealthResponsePayload(payload);
    return encoded.hasValue() && sendFrame({protocol::MessageType::HealthResponse, frame.jobId, encoded.value()});
}

// 목적: session event-loop에서 active identity를 제거하고 terminal response 전송
// 입력: outcome: Runtime이 완료한 job 결과
// 출력: RenderSucceeded, RenderFailed 또는 ResourceBusy frame
void ProtocolSession::handleOutcome(runtime::RenderJobOutcome outcome)
{
    const protocol::JobId jobId = toProtocolJobId(outcome.key.jobId);
    if (outcome.key.sessionId != m_sessionId || m_activeJobIds.erase(jobId) == 0 || m_closing)
    {
        return;
    }

    if (outcome.result.hasError())
    {
        if (!sendResourceBusy(jobId, outcome.result.error()))
        {
            close();
            return;
        }
    }
    else if (const core::render::ResolvedRenderPipelineResult& renderResult = outcome.result.value();
             renderResult.hasValue())
    {
        const runtime::RenderSucceededPayload payload{
            outcome.outputRelativePath, renderResult.value().artifact.byteSize, renderResult.value().stats};
        const runtime::EncodePayloadResult encoded = runtime::encodeRenderSucceededPayload(payload);
        if (encoded.hasError() || !sendFrame({protocol::MessageType::RenderSucceeded, jobId, encoded.value()}))
        {
            close();
            return;
        }
    }
    else if (!sendFailure(jobId, renderResult.error().cause, renderResult.error().stats))
    {
        close();
        return;
    }

    if (m_activeJobIds.empty() && m_configuration.inactivityTimeoutMilliseconds > 0)
    {
        m_inactivityTimer.start(m_configuration.inactivityTimeoutMilliseconds);
    }
}

// 목적: bounded socket write queue에 protocol frame 직렬화 및 추가
// 입력: frame: response message, JobId와 payload
// 출력: write를 접수했으면 true
bool ProtocolSession::sendFrame(const protocol::ProtocolFrame& frame)
{
    const protocol::EncodeFrameResult encoded = protocol::encodeFrame(frame);
    if (encoded.hasError() || m_socket->state() == QAbstractSocket::UnconnectedState ||
        m_socket->bytesToWrite() + encoded.value().size() > m_configuration.maximumPendingWriteBytes)
    {
        m_socket->abort();
        return false;
    }

    if (m_socket->write(encoded.value()) != encoded.value().size())
    {
        m_socket->abort();
        return false;
    }
    return true;
}

// 목적: CoreError와 RenderStats를 RenderFailed frame으로 전달
// 입력: jobId/cause/stats: correlation과 terminal failure 값
// 출력: response write 성공 여부
bool ProtocolSession::sendFailure(const protocol::JobId jobId,
                                  const core::types::CoreError& cause,
                                  const core::measurement::RenderStats& stats)
{
    const runtime::EncodePayloadResult encoded = runtime::encodeRenderFailedPayload({cause, stats});
    return encoded.hasValue() && sendFrame({protocol::MessageType::RenderFailed, jobId, encoded.value()});
}

// 목적: 접수하지 못한 request를 ServerBusy terminal response로 전달
// 입력: jobId: correlation identity, message: bounded 진단
// 출력: response write 성공 여부
bool ProtocolSession::sendBusy(const protocol::JobId jobId, const QString& message)
{
    const runtime::EncodePayloadResult encoded = runtime::encodeServerBusyPayload({message});
    return encoded.hasValue() && sendFrame({protocol::MessageType::ServerBusy, jobId, encoded.value()});
}

// 목적: accepted job의 resource admission 거절을 retry advice와 함께 전달
// 입력: jobId: correlation identity, busy: resource 진단과 optional retry delay
// 출력: response write 성공 여부
bool ProtocolSession::sendResourceBusy(const protocol::JobId jobId, const runtime::RenderResourceBusy& busy)
{
    const runtime::EncodePayloadResult encoded = runtime::encodeResourceBusyPayload({busy.message, busy.retryAfter});
    return encoded.hasValue() && sendFrame({protocol::MessageType::ResourceBusy, jobId, encoded.value()});
}

// 목적: 모든 active Runtime job에 cancellation 요청
// 입력: 없음
// 출력: 없음
void ProtocolSession::cancelActiveJobs()
{
    for (const protocol::JobId jobId : m_activeJobIds)
    {
        static_cast<void>(m_runtime.cancel({m_sessionId, toRuntimeJobId(jobId)}));
    }
    m_activeJobIds.clear();
}

// 목적: socket 상태와 무관하게 finished signal을 정확히 한 번 발생
// 입력: 없음
// 출력: server가 session을 제거할 수 있는 완료 상태
void ProtocolSession::finishOnce()
{
    if (m_finished)
    {
        return;
    }
    m_finished = true;
    emit finished();
}

}  // namespace flexraw::worker::network
