#include <QCoreApplication>

#include <gtest/gtest.h>

// 목적: MCP test process의 Qt application lifetime을 test와 static cleanup 전체에 명시적으로 제공
// 입력: argc, argv — GoogleTest와 Qt에 전달할 process arguments
// 출력: 전체 MCP test suite의 성공 또는 실패 exit code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
