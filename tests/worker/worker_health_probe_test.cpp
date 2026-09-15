#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <gtest/gtest.h>

#include "frame_codec.h"
#include "frame_parser.h"
#include "health_payload_codec.h"
#include "worker_health_probe.h"

namespace flexraw::worker::client
{
namespace
{

using namespace std::chrono_literals;
using HealthRequestHandler = std::function<void(QTcpSocket&, const protocol::ProtocolFrame&)>;

// 목적: Qt server event를 처리하며 asynchronous health probe 완료 대기
// 입력: future: probe 결과, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 future가 ready이면 true
[[nodiscard]] bool waitForFuture(std::future<WorkerHealthTransportResult>& future, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (future.wait_for(0ms) != std::future_status::ready && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return future.wait_for(0ms) == std::future_status::ready;
}

class ScriptedHealthServer final
{
public:
    // 목적: one-shot health client test용 ephemeral localhost server 시작
    // 입력: handler: 완성된 request frame에 대한 response script
    // 출력: 연결을 받을 수 있는 scripted server
    explicit ScriptedHealthServer(HealthRequestHandler handler) : m_handler(std::move(handler))
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() { acceptConnection(); });
        if (!m_server.listen(QHostAddress::LocalHost, 0))
        {
            throw std::runtime_error("Unable to start scripted Worker health server.");
        }
    }

    // 목적: client가 연결할 ephemeral localhost port 반환
    // 입력: 없음
    // 출력: bound server port
    [[nodiscard]] quint16 port() const noexcept
    {
        return m_server.serverPort();
    }

private:
    // 목적: pending socket을 server child로 소유하고 request parser 연결
    // 입력: 없음
    // 출력: readyRead event를 처리하는 connection
    void acceptConnection()
    {
        QTcpSocket* const socket = m_server.nextPendingConnection();
        if (socket != nullptr)
        {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() { readAvailable(*socket); });
        }
    }

    // 목적: received bytes를 frame parser에 공급하고 response script 실행
    // 입력: socket: request/response connection
    // 출력: 완성 frame별 handler 호출 또는 protocol error 시 abort
    void readAvailable(QTcpSocket& socket)
    {
        protocol::ParseFramesOutcome outcome = m_parser.append(socket.readAll());
        if (outcome.terminalError.has_value())
        {
            socket.abort();
            return;
        }
        for (const protocol::ProtocolFrame& frame : outcome.frames)
        {
            m_handler(socket, frame);
        }
    }

    QTcpServer m_server;
    protocol::FrameParser m_parser;
    HealthRequestHandler m_handler;
};

// 목적: health response frame을 scripted socket에 전송
// 입력: socket: 연결 channel, frame: response value
// 출력: encode와 전체 write가 성공하면 true
[[nodiscard]] bool sendFrame(QTcpSocket& socket, const protocol::ProtocolFrame& frame)
{
    const protocol::EncodeFrameResult encoded = protocol::encodeFrame(frame);
    return encoded.hasValue() && socket.write(encoded.value()) == encoded.value().size();
}

// 목적: test server용 짧은 timeout의 valid health endpoint 생성
// 입력: port: ephemeral localhost port
// 출력: 1초 connect/response limit endpoint
[[nodiscard]] WorkerHealthEndpoint makeEndpoint(const quint16 port)
{
    return {QStringLiteral("127.0.0.1"), port, 1000ms, 1000ms};
}

// 목적: blocking health probe를 별도 thread에서 실행해 server event loop 유지
// 입력: probe/endpoint: 실행할 one-shot operation
// 출력: asynchronous transport result future
[[nodiscard]] std::future<WorkerHealthTransportResult> probeAsync(const TcpWorkerHealthProbe& probe,
                                                                  const WorkerHealthEndpoint& endpoint)
{
    return std::async(std::launch::async, [&probe, endpoint]() { return probe.probe(endpoint); });
}

TEST(WorkerHealthProbeTest, ReturnsCompatibleRuntimeObservation)
{
    ScriptedHealthServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& request) {
        ASSERT_EQ(protocol::MessageType::HealthRequest, request.messageType);
        ASSERT_TRUE(request.payload.isEmpty());
        const protocol::EncodeHealthPayloadResult payload =
            protocol::encodeHealthResponsePayload({protocol::HealthServiceState::Draining, 2, 1, 4, 8});
        ASSERT_TRUE(payload.hasValue());
        ASSERT_TRUE(sendFrame(socket, {protocol::MessageType::HealthResponse, request.jobId, payload.value()}));
    });
    const TcpWorkerHealthProbe probe;
    std::future<WorkerHealthTransportResult> future = probeAsync(probe, makeEndpoint(server.port()));

    ASSERT_TRUE(waitForFuture(future));
    const WorkerHealthTransportResult result = future.get();

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(WorkerHealthTransportServiceState::Draining, result.value().serviceState);
    EXPECT_EQ(1U, result.value().runningJobs);
    EXPECT_EQ(2U, result.value().queuedJobs);
    EXPECT_EQ(4U, result.value().maximumConcurrentJobs);
    EXPECT_EQ(8U, result.value().queueCapacity);
    EXPECT_GE(result.value().roundTrip.count(), 0);
}

TEST(WorkerHealthProbeTest, RejectsUnexpectedResponseType)
{
    ScriptedHealthServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& request) {
        ASSERT_TRUE(sendFrame(socket, {protocol::MessageType::JobAccepted, request.jobId, {}}));
    });
    const TcpWorkerHealthProbe probe;
    std::future<WorkerHealthTransportResult> future = probeAsync(probe, makeEndpoint(server.port()));

    ASSERT_TRUE(waitForFuture(future));
    const WorkerHealthTransportResult result = future.get();

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(WorkerHealthTransportErrorCode::ProtocolViolation, result.error().code);
}

TEST(WorkerHealthProbeTest, ReportsClosedEndpointAsConnectionFailure)
{
    QTcpServer portProbe;
    ASSERT_TRUE(portProbe.listen(QHostAddress::LocalHost, 0));
    const quint16 closedPort = portProbe.serverPort();
    portProbe.close();
    const TcpWorkerHealthProbe probe;

    const WorkerHealthTransportResult result = probe.probe(makeEndpoint(closedPort));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(WorkerHealthTransportErrorCode::ConnectionFailed, result.error().code);
}

}  // namespace
}  // namespace flexraw::worker::client
