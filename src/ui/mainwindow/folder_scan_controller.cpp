#include "folder_scan_controller.h"

#include "log.h"

#include <QtConcurrentRun>

namespace flexraw::ui::mainwindow
{

// 목적: UI thread에서 folder scan 작업을 관리하는 controller 생성
// 입력: parent: Qt 부모 객체
// 출력: 초기화된 FolderScanController 객체
FolderScanController::FolderScanController(QObject* parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<core::catalog::CatalogScanResult>::finished, this, [this] {
        const core::catalog::CatalogScanResult scanResult = m_watcher.result();

        if (scanResult.hasError()) {
            LOG_WARN("catalog", "Folder scan failed: {}", scanResult.error().message.toStdString());
        }

        emit scanFinished(scanResult);
    });
}

// 목적: worker thread에서 지정 folder의 catalog scan 시작
// 입력: folderPath: scan할 folder 경로
// 출력: 이미 scan 중이면 무시, 그 외 비동기 작업 시작
void FolderScanController::scanFolder(const QString& folderPath)
{
    if (m_watcher.isRunning())
    {
        LOG_WARN("catalog", "Ignored folder scan request because another scan is running");
        return;
    }

    emit scanStarted(folderPath);
    m_watcher.setFuture(QtConcurrent::run([folderPath] { return core::catalog::scanFolder(folderPath); }));
}

// 목적: folder scan worker가 현재 실행 중인지 확인
// 입력: 없음
// 출력: 비동기 scan 실행 여부
bool FolderScanController::isScanning() const
{
    return m_watcher.isRunning();
}

}  // namespace flexraw::ui::mainwindow
