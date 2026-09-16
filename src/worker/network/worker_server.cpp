#include "worker_server.h"

#include <utility>

#include <QTcpSocket>

namespace flexraw::worker::network
{

// 목적: shared Worker runtime 위에 TCP acceptor 구성
// 입력: runtime: 모든 session이 공유할 Runtime port, configuration: connection/session 한도
// 출력: 아직 listen하지 않는 server
WorkerServer::WorkerServer(runtime::IRenderWorkerRuntime& runtime,
                           WorkerServerConfiguration configuration,
                           QObject* const parent)
    : QObject(parent), m_runtime(runtime), m_configuration(std::move(configuration)), m_server(this)
{
    connect(&m_server, &QTcpServer::newConnection, this, &WorkerServer::acceptPendingConnections);
}

// 목적: accept 중단과 active session close를 보장
// 입력: 없음
// 출력: socket/session이 신규 작업을 접수하지 않는 상태
WorkerServer::~WorkerServer()
{
    close();
}

// 목적: 지정 address/port에서 Worker TCP protocol listen 시작
// 입력: address: bind address, port: 0이면 OS-assigned ephemeral port
// 출력: listen 성공 여부
bool WorkerServer::listen(const QHostAddress& address, const quint16 port)
{
    if (m_server.isListening() || m_configuration.maximumConnections <= 0 ||
        m_configuration.session.maximumPendingWriteBytes <= 0)
    {
        return false;
    }
    m_server.setMaxPendingConnections(m_configuration.maximumConnections);
    return m_server.listen(address, port);
}

// 목적: 신규 연결 접수를 중단하고 모든 active session 종료
// 입력: 없음
// 출력: server가 listen하지 않고 session이 closing인 상태
void WorkerServer::close()
{
    m_server.close();
    const QSet<ProtocolSession*> sessions = m_sessions;
    for (ProtocolSession* const session : sessions)
    {
        session->close();
    }
}

// 목적: 현재 server listen 여부 확인
// 입력: 없음
// 출력: QTcpServer listen state
bool WorkerServer::isListening() const noexcept
{
    return m_server.isListening();
}

// 목적: 실제 bind된 TCP port 조회
// 입력: 없음
// 출력: listen 전 0 또는 bound port
quint16 WorkerServer::serverPort() const noexcept
{
    return m_server.serverPort();
}

// 목적: 마지막 QTcpServer 오류 text 조회
// 입력: 없음
// 출력: listen/accept 진단 text
QString WorkerServer::errorString() const
{
    return m_server.errorString();
}

// 목적: 현재 소유한 active/closing session 수 조회
// 입력: 없음
// 출력: connection count
qsizetype WorkerServer::sessionCount() const noexcept
{
    return m_sessions.size();
}

// 목적: pending socket을 connection 상한 안에서 ProtocolSession으로 승격
// 입력: 없음
// 출력: session 생성 또는 초과 socket 즉시 종료
void WorkerServer::acceptPendingConnections()
{
    while (m_server.hasPendingConnections())
    {
        QTcpSocket* const socket = m_server.nextPendingConnection();
        if (socket == nullptr)
        {
            continue;
        }
        if (m_sessions.size() >= m_configuration.maximumConnections)
        {
            socket->abort();
            socket->deleteLater();
            continue;
        }

        auto* const session = new ProtocolSession(nextSessionId(), socket, m_runtime, m_configuration.session, this);
        m_sessions.insert(session);
        connect(session, &ProtocolSession::finished, this, [this, session]() {
            m_sessions.remove(session);
            session->deleteLater();
        });
    }
}

// 목적: 다음 non-zero process-local session identity 발급
// 입력: 없음
// 출력: wrap 시 0을 건너뛴 WorkerSessionId
runtime::WorkerSessionId WorkerServer::nextSessionId() noexcept
{
    const runtime::WorkerSessionId result = m_nextSessionId;
    ++m_nextSessionId;
    if (m_nextSessionId == 0)
    {
        m_nextSessionId = 1;
    }
    return result;
}

}  // namespace flexraw::worker::network
