#pragma once

#include <QFutureWatcher>
#include <QObject>

#include "folder_scanner.h"

namespace flexraw::ui::mainwindow
{

class FolderScanController final : public QObject
{
    Q_OBJECT

public:
    // 목적: UI thread에서 folder scan 작업을 관리하는 controller 생성
    // 입력: parent: Qt 부모 객체
    // 출력: 초기화된 FolderScanController 객체
    explicit FolderScanController(QObject* parent = nullptr);

    // 목적: worker thread에서 지정 folder의 catalog scan 시작
    // 입력: folderPath: scan할 folder 경로
    // 출력: 이미 scan 중이면 무시, 그 외 비동기 작업 시작
    void scanFolder(const QString& folderPath);

    // 목적: folder scan worker가 현재 실행 중인지 확인
    // 입력: 없음
    // 출력: 비동기 scan 실행 여부
    [[nodiscard]] bool isScanning() const;

signals:
    // 목적: 새 folder scan 작업 시작을 UI에 알림
    // 입력: folderPath: scan을 시작한 folder 경로
    // 출력: 없음
    void scanStarted(const QString& folderPath);

    // 목적: worker thread의 folder scan 결과를 UI thread에 전달
    // 입력: result: 성공 목록 또는 구조화된 scan 오류
    // 출력: 없음
    void scanFinished(const core::catalog::CatalogScanResult& result);

private:
    QFutureWatcher<core::catalog::CatalogScanResult> m_watcher;
};

}  // namespace flexraw::ui::mainwindow
