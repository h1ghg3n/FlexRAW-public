#include <QApplication>
#include <QCoreApplication>

#include "application_context.h"
#include "log.h"

// 목적: Flexraw runtime, Composition Root와 Qt event loop 실행
// 입력: argc: process argument 수, argv: process argument 값
// 출력: Qt application exit code
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Flexraw"));
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw"));
    flexraw::core::util::initializeLogging();
    LOG_INFO("app", "Flexraw process started");

    int exitCode = 0;

    {
        flexraw::app::ApplicationContext applicationContext;
        applicationContext.mainWindow().show();
        exitCode = app.exec();
    }

    LOG_INFO("app", "Flexraw process stopped with exit code {}", exitCode);
    flexraw::core::util::shutdownLogging();
    return exitCode;
}
