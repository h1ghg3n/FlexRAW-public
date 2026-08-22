#pragma once

#include <cstdint>

#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QTcpServer>

#include "job_scheduler.h"
#include "protocol_session.h"
#include "worker_path_resolver.h"

namespace flexraw::worker::network
{

struct WorkerServerConfiguration
{
    int maximumConnections{16};
    ProtocolSessionConfiguration session;
};

class WorkerServer final : public QObject
{
    Q_OBJECT

public:
    // 목적: shared Worker runtime 위에 TCP acceptor 구성
    // 입력: resolver/scheduler: 모든 session이 공유할 runtime, configuration: connection/session 한도
    // 출력: 아직 listen하지 않는 server
    WorkerServer(const runtime::WorkerPathResolver& resolver,
                 runtime::JobScheduler& scheduler,
                 WorkerServerConfiguration configuration = {},
                 QObject* parent = nullptr);

    // 목적: accept 중단과 active session close를 보장
    // 입력: 없음
    // 출력: socket/session이 신규 작업을 접수하지 않는 상태
    ~WorkerServer() override;

    // 목적: 지정 address/port에서 Worker TCP protocol listen 시작
    // 입력: address: bind address, port: 0이면 OS-assigned ephemeral port
    // 출력: listen 성공 여부
    [[nodiscard]] bool listen(const QHostAddress& address, quint16 port);

    // 목적: 신규 연결 접수를 중단하고 모든 active session 종료
    // 입력: 없음
    // 출력: server가 listen하지 않고 session이 closing인 상태
    void close();

    // 목적: 현재 server listen 여부 확인
    // 입력: 없음
    // 출력: QTcpServer listen state
    [[nodiscard]] bool isListening() const noexcept;

    // 목적: 실제 bind된 TCP port 조회
    // 입력: 없음
    // 출력: listen 전 0 또는 bound port
    [[nodiscard]] quint16 serverPort() const noexcept;

    // 목적: 마지막 QTcpServer 오류 text 조회
    // 입력: 없음
    // 출력: listen/accept 진단 text
    [[nodiscard]] QString errorString() const;

    // 목적: 현재 소유한 active/closing session 수 조회
    // 입력: 없음
    // 출력: connection count
    [[nodiscard]] qsizetype sessionCount() const noexcept;

private slots:
    // 목적: pending socket을 connection 상한 안에서 ProtocolSession으로 승격
    // 입력: 없음
    // 출력: session 생성 또는 초과 socket 즉시 종료
    void acceptPendingConnections();

private:
    // 목적: 다음 non-zero process-local session identity 발급
    // 입력: 없음
    // 출력: wrap 시 0을 건너뛴 WorkerSessionId
    [[nodiscard]] runtime::WorkerSessionId nextSessionId() noexcept;

    const runtime::WorkerPathResolver& m_resolver;
    runtime::JobScheduler& m_scheduler;
    WorkerServerConfiguration m_configuration;
    QTcpServer m_server;
    QSet<ProtocolSession*> m_sessions;
    runtime::WorkerSessionId m_nextSessionId{1};
};

}  // namespace flexraw::worker::network
