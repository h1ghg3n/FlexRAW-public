#include <exception>

#include <QApplication>
#include <QCoreApplication>

#include "application_context.h"
#include "log.h"

namespace
{

// 목적: Flexraw runtime, Composition Root와 Qt event loop 실행
// 입력: argc: process argument 수, argv: process argument 값
// 출력: Qt application exit code
int runDesktopProcess(int argc, char* argv[])
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

}  // namespace

// 목적: Desktop Composition 생성 예외를 process diagnostic과 실패 code로 정규화
// 입력: argc: process argument 수, argv: process argument 값
// 출력: 정상 Qt exit code 또는 예상하지 못한 startup 실패 1
int main(int argc, char* argv[])
{
    try
    {
        return runDesktopProcess(argc, argv);
    }
    catch (const std::exception& exception)
    {
        flexraw::core::util::initializeLogging();
        LOG_ERROR("app", "Unable to start Flexraw: {}", exception.what());
    }
    catch (...)
    {
        flexraw::core::util::initializeLogging();
        LOG_ERROR("app", "Unable to start Flexraw: unknown exception");
    }

    flexraw::core::util::shutdownLogging();
    return 1;
}
