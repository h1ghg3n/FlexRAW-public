#pragma once

#include <QWidget>

class QLineEdit;
class QPlainTextEdit;

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class ExportOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::cli
{

class ConsoleCommandController;

class ConsoleModeWidget : public QWidget
{
    Q_OBJECT

public:
    // 목적: 전체 화면 console mode surface 초기화
    // 입력: catalogOrchestrator: active catalog use case, exportOrchestrator: shared export use case, parent: Qt 부모
    // widget 출력: 초기화된 ConsoleModeWidget 객체
    ConsoleModeWidget(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                      core::orchestration::ExportOrchestrator& exportOrchestrator,
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
