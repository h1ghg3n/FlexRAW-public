#include <QFontDatabase>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include "console_command_controller.h"
#include "console_mode_widget.h"

namespace flexraw::ui::cli
{

// 목적: 전체 화면 console mode surface 초기화
// 입력: catalogOrchestrator: active catalog use case, exportOrchestrator: shared export use case, parent: Qt 부모
// widget 출력: 초기화된 ConsoleModeWidget 객체
ConsoleModeWidget::ConsoleModeWidget(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                     core::orchestration::ExportOrchestrator& exportOrchestrator,
                                     QWidget* parent)
    : QWidget(parent),
      m_output(new QPlainTextEdit(this)),
      m_input(new QLineEdit(this)),
      m_commandController(new ConsoleCommandController(catalogOrchestrator, exportOrchestrator, this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);
    layout->addWidget(m_output, 1);
    layout->addWidget(m_input);

    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_output->setFont(fixedFont);
    m_output->setReadOnly(true);
    m_input->setFont(fixedFont);
    m_input->setPlaceholderText(tr("Enter command"));
    connect(m_input, &QLineEdit::returnPressed, this, &ConsoleModeWidget::executeCommand);
    connect(m_commandController, &ConsoleCommandController::outputReady, this, &ConsoleModeWidget::appendOutput);
    connect(m_commandController, &ConsoleCommandController::busyChanged, this, [this](const bool busy) {
        m_input->setEnabled(!busy);
        if (!busy)
        {
            focusInput();
        }
    });

    appendOutput(tr("Flexraw console mode"));
    appendOutput(tr("Type 'help' for available commands."));
}

// 목적: command 입력 필드에 keyboard focus 부여
// 입력: 없음
// 출력: 없음
void ConsoleModeWidget::focusInput()
{
    m_input->setFocus();
}

// 목적: 입력된 최소 console command 처리
// 입력: 없음
// 출력: 없음
void ConsoleModeWidget::executeCommand()
{
    const QString command = m_input->text().trimmed();
    m_input->clear();

    if (command.isEmpty())
    {
        return;
    }

    appendOutput(QStringLiteral("> %1").arg(command));

    if (command.compare(QStringLiteral("help"), Qt::CaseInsensitive) == 0)
    {
        appendOutput(
            tr("Commands: help, clear, gui, diagnose raw, catalog import, export raster, export raw, export batch"));
        return;
    }

    if (command.compare(QStringLiteral("clear"), Qt::CaseInsensitive) == 0)
    {
        m_output->clear();
        return;
    }

    if (command.compare(QStringLiteral("gui"), Qt::CaseInsensitive) == 0)
    {
        emit exitRequested();
        return;
    }

    if (m_commandController->tryExecute(command))
    {
        return;
    }

    appendOutput(tr("Command execution is unavailable until backend workflows are implemented."));
}

// 목적: console output 영역에 한 줄 추가
// 입력: line: 추가할 output 문자열
// 출력: 없음
void ConsoleModeWidget::appendOutput(const QString& line)
{
    m_output->appendPlainText(line);
}

}  // namespace flexraw::ui::cli
