#pragma once

#include <QWidget>

class QLineEdit;
class QPlainTextEdit;

namespace flexraw::core::client
{
class IFolderImportClient;
class IFolderImportEventSource;
class IExportClient;
class IExportDefaultsClient;
class IExportEventSource;
}  // namespace flexraw::core::client

namespace flexraw::ui::cli
{

class ConsoleCommandController;

class ConsoleModeWidget : public QWidget
{
    Q_OBJECT

public:
    // 목적: 전체 화면 console mode surface 초기화
    // 입력: Folder/Export command/event와 Export default contract, parent: Qt 부모 widget
    // 출력: 초기화된 ConsoleModeWidget 객체
    ConsoleModeWidget(core::client::IFolderImportClient& folderImportClient,
                      core::client::IFolderImportEventSource& folderImportEventSource,
                      core::client::IExportClient& exportClient,
                      core::client::IExportEventSource& exportEventSource,
                      core::client::IExportDefaultsClient& exportDefaultsClient,
                      QWidget* parent = nullptr);

    // 목적: command 입력 필드에 keyboard focus 부여
    // 입력: 없음
    // 출력: 없음
    void focusInput();

signals:
    // 목적: console command로 graphical UI 복귀 요청 전달
    // 입력: 없음
    // 출력: 없음
    void exitRequested();

private:
    // 목적: 입력된 최소 console command 처리
    // 입력: 없음
    // 출력: 없음
    void executeCommand();

    // 목적: console output 영역에 한 줄 추가
    // 입력: line: 추가할 output 문자열
    // 출력: 없음
    void appendOutput(const QString& line);

    QPlainTextEdit* m_output{nullptr};
    QLineEdit* m_input{nullptr};
    ConsoleCommandController* m_commandController{nullptr};
};

}  // namespace flexraw::ui::cli
