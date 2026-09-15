#include "worker_health_probe.h"

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
#include "health_payload_codec.h"

namespace flexraw::worker::client
{
namespace
{

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds SocketPollInterval{25};

// 목적: one-shot health request에 사용할 non-zero wire correlation identity 생성
// 입력: 없음
// 출력: process 안에서 단조 증가하는 non-zero 값
[[nodiscard]] protocol::JobId nextCorrelationId() noexcept
{
    static std::atomic<protocol::JobId> next{1};
    protocol::JobId value = next.fetch_add(1, std::memory_order_relaxed);
    while (value == 0)
    {
        value = next.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

// 목적: typed Worker health transport failure 생성
// 입력: code: failure 분류, message: bounded 진단 text
// 출력: failure 상태의 WorkerHealthTransportResult
[[nodiscard]] WorkerHealthTransportResult makeFailure(const WorkerHealthTransportErrorCode code, QString message)
{
    return WorkerHealthTransportResult::failure({code, std::move(message)});
}

// 목적: endpoint timeout을 Qt blocking API가 받을 수 있는 범위로 검증
// 입력: value: 검증할 duration
// 출력: positive int millisecond 범위이면 true
[[nodiscard]] bool isValidTimeout(const std::chrono::milliseconds value) noexcept
{
    return value > 0ms && value <= std::chrono::milliseconds(std::numeric_limits<int>::max());
}

// 목적: elapsed timeout 안에서 다음 socket wait 구간 계산
// 입력: timer: 시작된 timer, timeout: 전체 제한
// 출력: 남은 시간과 poll interval 중 작은 positive int
[[nodiscard]] int nextWaitMilliseconds(const QElapsedTimer& timer, const std::chrono::milliseconds timeout)
{
    const qint64 remaining = timeout.count() - timer.elapsed();
    return static_cast<int>(std::clamp<qint64>(remaining, 1, SocketPollInterval.count()));
}

// 목적: wire health service state를 transport-local 값으로 변환
// 입력: state: 검증된 protocol payload enum
// 출력: 의미가 같은 transport snapshot enum
[[nodiscard]] WorkerHealthTransportServiceState toTransportServiceState(
    const protocol::HealthServiceState state) noexcept
{
    switch (state)
    {
    case protocol::HealthServiceState::Ready:
        return WorkerHealthTransportServiceState::Ready;
    case protocol::HealthServiceState::Draining:
        return WorkerHealthTransportServiceState::Draining;
    case protocol::HealthServiceState::ShuttingDown:
        return WorkerHealthTransportServiceState::ShuttingDown;
    }
    return WorkerHealthTransportServiceState::ShuttingDown;
}

// 목적: 작은 health request frame을 bounded socket write로 완전히 전송
// 입력: socket: connected channel, frame: encoded request, timeout: write 제한
// 출력: 성공 시 empty, 실패 시 typed transport error
[[nodiscard]] std::optional<WorkerHealthTransportError> writeFrame(QTcpSocket& socket,
                                                                   const QByteArray& frame,
                                                                   const std::chrono::milliseconds timeout)
{
    if (socket.write(frame) != frame.size())
    {
        return WorkerHealthTransportError{WorkerHealthTransportErrorCode::ConnectionLost, socket.errorString()};
    }
    QElapsedTimer timer;
    timer.start();
    while (socket.bytesToWrite() > 0 && timer.elapsed() < timeout.count())
    {
        static_cast<void>(socket.waitForBytesWritten(nextWaitMilliseconds(timer, timeout)));
        if (socket.state() == QAbstractSocket::UnconnectedState)
        {
            return WorkerHealthTransportError{WorkerHealthTransportErrorCode::ConnectionLost, socket.errorString()};
        }
    }
    if (socket.bytesToWrite() > 0)
    {
        return WorkerHealthTransportError{WorkerHealthTransportErrorCode::TimedOut,
                                          QStringLiteral("Timed out writing a Worker health request.")};
    }
    return std::nullopt;
}

}  // namespace

// 목적: manual Worker endpoint에 동기 bounded health request 실행
// 입력: endpoint: TCP 주소와 connect/response timeout
// 출력: decoded runtime observation 또는 typed transport/protocol 오류
WorkerHealthTransportResult TcpWorkerHealthProbe::probe(const WorkerHealthEndpoint& endpoint) const
{
    if (endpoint.host.trimmed().isEmpty() || endpoint.port == 0 || !isValidTimeout(endpoint.connectTimeout) ||
        !isValidTimeout(endpoint.responseTimeout))
    {
        return makeFailure(WorkerHealthTransportErrorCode::InvalidEndpoint,
                           QStringLiteral("Worker health endpoint is invalid."));
    }

    QTcpSocket socket;
    QElapsedTimer roundTripTimer;
    roundTripTimer.start();
    socket.connectToHost(endpoint.host.trimmed(), endpoint.port);
    if (!socket.waitForConnected(static_cast<int>(endpoint.connectTimeout.count())))
    {
        socket.abort();
        return makeFailure(WorkerHealthTransportErrorCode::ConnectionFailed, socket.errorString());
    }

    const protocol::JobId correlationId = nextCorrelationId();
    const protocol::EncodeFrameResult encoded =
        protocol::encodeFrame({protocol::MessageType::HealthRequest, correlationId, {}});
    if (encoded.hasError())
    {
        return makeFailure(WorkerHealthTransportErrorCode::ProtocolViolation, encoded.error().message);
    }
    if (const std::optional<WorkerHealthTransportError> writeError =
            writeFrame(socket, encoded.value(), endpoint.responseTimeout);
        writeError.has_value())
    {
        return WorkerHealthTransportResult::failure(*writeError);
    }

    protocol::FrameParser parser;
    QElapsedTimer responseTimer;
    responseTimer.start();
    while (responseTimer.elapsed() < endpoint.responseTimeout.count())
    {
        if (socket.bytesAvailable() == 0)
        {
            static_cast<void>(socket.waitForReadyRead(nextWaitMilliseconds(responseTimer, endpoint.responseTimeout)));
        }
        const QByteArray bytes = socket.readAll();
        if (!bytes.isEmpty())
        {
            protocol::ParseFramesOutcome outcome = parser.append(bytes);
            if (outcome.terminalError.has_value())
            {
                return makeFailure(WorkerHealthTransportErrorCode::ProtocolViolation, outcome.terminalError->message);
            }
            for (const protocol::ProtocolFrame& frame : outcome.frames)
            {
                if (frame.jobId != correlationId || frame.messageType != protocol::MessageType::HealthResponse)
                {
                    return makeFailure(WorkerHealthTransportErrorCode::ProtocolViolation,
                                       QStringLiteral("Worker health response correlation or type is invalid."));
                }
                const protocol::DecodeHealthPayloadResult decoded =
                    protocol::decodeHealthResponsePayload(frame.payload);
                if (decoded.hasError())
                {
                    return makeFailure(WorkerHealthTransportErrorCode::ProtocolViolation, decoded.error().message);
                }
                const protocol::HealthResponsePayload& payload = decoded.value();
                return WorkerHealthTransportResult::success({toTransportServiceState(payload.serviceState),
                                                             payload.queuedJobs,
                                                             payload.runningJobs,
                                                             payload.maximumConcurrentJobs,
                                                             payload.queueCapacity,
                                                             std::chrono::milliseconds(roundTripTimer.elapsed())});
            }
        }
        if (socket.state() == QAbstractSocket::UnconnectedState)
        {
            return makeFailure(WorkerHealthTransportErrorCode::ConnectionLost,
                               QStringLiteral("Worker closed the health probe connection."));
        }
    }
    socket.abort();
    return makeFailure(WorkerHealthTransportErrorCode::TimedOut,
                       QStringLiteral("Timed out waiting for a Worker health response."));
}

}  // namespace flexraw::worker::client
