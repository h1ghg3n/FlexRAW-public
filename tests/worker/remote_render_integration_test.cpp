#include <functional>
#include <iterator>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "frame_codec.h"
#include "frame_parser.h"
#include "render_payload_codec.h"
#include "worker_application_context.h"

namespace flexraw::worker::app
{
namespace
{

constexpr int RenderTimeoutMilliseconds = 120000;

// 목적: 실제 render 중 Qt event를 처리하며 predicate가 참이 될 때까지 bounded wait
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 조건을 만족하면 true
[[nodiscard]] bool waitUntil(const std::function<bool()>& predicate,
                             const int timeoutMilliseconds = RenderTimeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

class RealRenderProtocolClient final
{
public:
    // 목적: 실제 Worker context의 localhost server에 연결
    // 입력: port: Worker가 bind한 TCP port
    // 출력: timeout 전에 연결되면 true
    [[nodiscard]] bool connectToServer(const quint16 port)
    {
        m_socket.connectToHost(QHostAddress::LocalHost, port);
        return waitUntil([this]() { return m_socket.state() == QAbstractSocket::ConnectedState; });
    }

    // 목적: RenderRequest frame을 encode해 Worker socket에 제출
    // 입력: frame: payload와 non-zero JobId가 있는 request
    // 출력: frame 전체가 Qt write queue에 들어가면 true
    [[nodiscard]] bool sendFrame(const protocol::ProtocolFrame& frame)
    {
        const protocol::EncodeFrameResult encoded = protocol::encodeFrame(frame);
        return encoded.hasValue() && m_socket.write(encoded.value()) == encoded.value().size();
    }

    // 목적: accepted와 terminal response가 모두 도착할 때까지 기다림
    // 입력: count: 필요한 response frame 수
    // 출력: protocol error 없이 지정 수를 받으면 true
    [[nodiscard]] bool waitForFrames(const std::size_t count)
    {
        const bool completed = waitUntil([this, count]() {
            readAvailable();
            return m_protocolError || m_frames.size() >= count;
        });
        return completed && !m_protocolError && m_frames.size() >= count;
    }

    // 목적: 누적 response frame을 wire 순서대로 반환하고 내부 목록 비우기
    // 입력: 없음
    // 출력: 현재까지 수신한 frame 목록
    [[nodiscard]] std::vector<protocol::ProtocolFrame> takeFrames()
    {
        readAvailable();
        return std::exchange(m_frames, {});
    }

private:
    // 목적: socket receive buffer를 incremental parser에 공급
    // 입력: 없음
    // 출력: 완성 frame 누적 또는 terminal protocol error 기록
    void readAvailable()
    {
        const QByteArray bytes = m_socket.readAll();
        if (bytes.isEmpty())
        {
            return;
        }
        protocol::ParseFramesOutcome outcome = m_parser.append(bytes);
        m_frames.insert(m_frames.end(),
                        std::make_move_iterator(outcome.frames.begin()),
                        std::make_move_iterator(outcome.frames.end()));
        m_protocolError = outcome.terminalError.has_value();
    }

    QTcpSocket m_socket;
    protocol::FrameParser m_parser;
    std::vector<protocol::ProtocolFrame> m_frames;
    bool m_protocolError{false};
};

TEST(RemoteRenderIntegrationTest, RendersConfiguredRawAcrossLoopbackTcpWhenFixtureIsProvided)
{
    const QString rawPath = qEnvironmentVariable("FLEXRAW_TEST_RAW_PATH");
    const QFileInfo rawInfo(rawPath);
    if (rawPath.isEmpty() || !rawInfo.exists() || !rawInfo.isFile())
    {
        GTEST_SKIP() << "Set FLEXRAW_TEST_RAW_PATH to an existing camera RAW fixture.";
    }

    QTemporaryDir outputRoot;
    ASSERT_TRUE(outputRoot.isValid());
    runtime::WorkerPathResolver::CreateResult resolver =
        runtime::WorkerPathResolver::create({rawInfo.absolutePath(), outputRoot.path()});
    ASSERT_TRUE(resolver.hasValue()) << resolver.error().message.toStdString();

    WorkerApplicationConfiguration configuration;
    configuration.maximumConcurrency = 1;
    configuration.queueCapacity = 1;
    configuration.server.session.inactivityTimeoutMilliseconds = RenderTimeoutMilliseconds;
    WorkerApplicationContext context(std::move(resolver.value()), configuration);
    ASSERT_TRUE(context.listen(QHostAddress::LocalHost, 0)) << context.errorString().toStdString();

    RealRenderProtocolClient client;
    ASSERT_TRUE(client.connectToServer(context.serverPort()));

    runtime::RenderRequestPayload payload;
    payload.sourceRelativePath = QDir::fromNativeSeparators(rawInfo.fileName());
    payload.outputRelativePath = QStringLiteral("한글-결과.jpg");
    payload.outputOptions.includeMetadata = false;
    const runtime::EncodePayloadResult encoded = runtime::encodeRenderRequestPayload(payload);
    ASSERT_TRUE(encoded.hasValue()) << encoded.error().message.toStdString();
    ASSERT_TRUE(client.sendFrame({protocol::MessageType::RenderRequest, 101, encoded.value()}));

    ASSERT_TRUE(client.waitForFrames(2));
    const std::vector<protocol::ProtocolFrame> frames = client.takeFrames();
    ASSERT_EQ(2U, frames.size());
    EXPECT_EQ(protocol::MessageType::JobAccepted, frames[0].messageType);
    EXPECT_EQ(101U, frames[0].jobId);
    EXPECT_EQ(101U, frames[1].jobId);
    if (frames[1].messageType == protocol::MessageType::RenderFailed)
    {
        const runtime::DecodeRenderFailedResult failure = runtime::decodeRenderFailedPayload(frames[1].payload);
        ASSERT_TRUE(failure.hasValue()) << failure.error().message.toStdString();
        FAIL() << failure.value().cause.message.toStdString();
    }
    ASSERT_EQ(protocol::MessageType::RenderSucceeded, frames[1].messageType);

    const runtime::DecodeRenderSucceededResult result = runtime::decodeRenderSucceededPayload(frames[1].payload);
    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    EXPECT_EQ(payload.outputRelativePath, result.value().outputRelativePath);
    EXPECT_GT(result.value().byteSize, 0U);
    EXPECT_GT(result.value().stats.decodeNanoseconds, 0U);
    EXPECT_GE(result.value().stats.totalNanoseconds, result.value().stats.decodeNanoseconds);

    const QFileInfo artifact(QDir(outputRoot.path()).filePath(payload.outputRelativePath));
    EXPECT_TRUE(artifact.exists());
    EXPECT_TRUE(artifact.isFile());
    EXPECT_EQ(static_cast<quint64>(artifact.size()), result.value().byteSize);

    context.shutdown();
    EXPECT_FALSE(context.runtimeSnapshot().accepting);
}

}  // namespace
}  // namespace flexraw::worker::app
