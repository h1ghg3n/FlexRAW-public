#pragma once

#include <cstdint>
#include <unordered_set>

#include <QObject>
#include <QTimer>

#include "frame_parser.h"
#include "render_worker_runtime_port.h"

class QTcpSocket;

namespace flexraw::worker::network
{

struct ProtocolSessionConfiguration
{
    int inactivityTimeoutMilliseconds{30000};
    qint64 maximumPendingWriteBytes{2 * 1024 * 1024};
};

class ProtocolSession final : public QObject
{
    Q_OBJECT

public:
    // 목적: 연결된 socket을 session-scoped render protocol adapter로 구성
    // 입력: sessionId/socket: 연결 identity와 소유 대상, runtime: shared Runtime port, configuration: I/O 한도
    // 출력: readyRead/disconnect/timeout을 처리하는 session
    ProtocolSession(runtime::WorkerSessionId sessionId,
                    QTcpSocket* socket,
                    runtime::IRenderWorkerRuntime& runtime,
                    ProtocolSessionConfiguration configuration = {},
                    QObject* parent = nullptr);

    // 목적: session의 active job을 취소하고 socket 종료
    // 입력: 없음
    // 출력: disconnect 또는 즉시 finished signal로 이어지는 closing 상태
    void close();

    // 목적: server가 부여한 process-local session identity 조회
    // 입력: 없음
    // 출력: non-zero WorkerSessionId
    [[nodiscard]] runtime::WorkerSessionId sessionId() const noexcept;

signals:
    void finished();

private slots:
    // 목적: bounded socket chunk를 parser에 공급하고 완성 frame 처리
    // 입력: 없음
    // 출력: frame response, 후속 queued read 또는 terminal disconnect
    void handleReadyRead();

    // 목적: peer/socket disconnect를 active cancellation과 session 완료로 변환
    // 입력: 없음
    // 출력: 정확히 한 번의 finished signal
    void handleDisconnected();

    // 목적: partial frame 또는 idle connection timeout 처리
    // 입력: 없음
    // 출력: active render가 없으면 close, 있으면 timeout 재시작
    void handleInactivityTimeout();

private:
    // 목적: client 방향에서 허용된 request/cancel frame dispatch
    // 입력: frame: envelope 검증이 끝난 wire frame
    // 출력: session을 계속 유지할 수 있으면 true
    [[nodiscard]] bool handleFrame(const protocol::ProtocolFrame& frame);

    // 목적: render payload decode/path resolve/scheduler submit과 accepted/busy 응답 수행
    // 입력: frame: RenderRequest message와 session JobId
    // 출력: semantic 처리를 계속할 수 있으면 true
    [[nodiscard]] bool handleRenderRequest(const protocol::ProtocolFrame& frame);

    // 목적: active render에 cooperative cancellation 요청 전달
    // 입력: frame: empty payload CancelRequest
    // 출력: payload 방향 계약이 유효하면 true
    [[nodiscard]] bool handleCancelRequest(const protocol::ProtocolFrame& frame);

    // 목적: Runtime read-only snapshot을 bounded health response로 투영
    // 입력: frame: empty payload HealthRequest와 correlation identity
    // 출력: payload 방향과 response write가 유효하면 true
    [[nodiscard]] bool handleHealthRequest(const protocol::ProtocolFrame& frame);

    // 목적: session event-loop에서 active identity를 제거하고 terminal response 전송
    // 입력: outcome: scheduler가 완료한 job 결과
    // 출력: RenderSucceeded, RenderFailed 또는 ResourceBusy frame
    void handleOutcome(runtime::RenderJobOutcome outcome);

    // 목적: bounded socket write queue에 protocol frame 직렬화 및 추가
    // 입력: frame: response message, JobId와 payload
    // 출력: write를 접수했으면 true
    [[nodiscard]] bool sendFrame(const protocol::ProtocolFrame& frame);

    // 목적: CoreError와 RenderStats를 RenderFailed frame으로 전달
    // 입력: jobId/cause/stats: correlation과 terminal failure 값
    // 출력: response write 성공 여부
    [[nodiscard]] bool sendFailure(protocol::JobId jobId,
                                   const core::types::CoreError& cause,
                                   const core::measurement::RenderStats& stats = {});

    // 목적: 접수하지 못한 request를 ServerBusy terminal response로 전달
    // 입력: jobId: correlation identity, message: bounded 진단
    // 출력: response write 성공 여부
    [[nodiscard]] bool sendBusy(protocol::JobId jobId, const QString& message);

    // 목적: accepted job의 resource admission 거절을 retry advice와 함께 전달
    // 입력: jobId: correlation identity, busy: resource 진단과 optional retry delay
    // 출력: response write 성공 여부
    [[nodiscard]] bool sendResourceBusy(protocol::JobId jobId, const runtime::RenderResourceBusy& busy);

    // 목적: 모든 active scheduler job에 cancellation 요청
    // 입력: 없음
    // 출력: 없음
    void cancelActiveJobs();

    // 목적: socket 상태와 무관하게 finished signal을 정확히 한 번 발생
    // 입력: 없음
    // 출력: server가 session을 제거할 수 있는 완료 상태
    void finishOnce();

    runtime::WorkerSessionId m_sessionId{0};
    QTcpSocket* m_socket{nullptr};
    runtime::IRenderWorkerRuntime& m_runtime;
    ProtocolSessionConfiguration m_configuration;
    protocol::FrameParser m_parser;
    QTimer m_inactivityTimer;
    std::unordered_set<protocol::JobId> m_activeJobIds;
    bool m_readContinuationQueued{false};
    bool m_closing{false};
    bool m_finished{false};
};

}  // namespace flexraw::worker::network
