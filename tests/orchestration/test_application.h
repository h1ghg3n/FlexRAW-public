#pragma once

#include <QCoreApplication>

namespace flexraw::core::orchestration::test
{

// 목적: async orchestration test에 필요한 process-wide Qt event dispatcher 보장
// 입력: 없음
// 출력: test process lifetime 동안 유지되는 QCoreApplication 참조
inline QCoreApplication& application()
{
    static int argumentCount = 1;
    static char applicationName[] = "flexraw_orchestration_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
    return application;
}

}  // namespace flexraw::core::orchestration::test
