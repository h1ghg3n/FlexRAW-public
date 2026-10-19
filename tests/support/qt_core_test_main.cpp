#include <QCoreApplication>

#include <gtest/gtest.h>

// 목적: Qt Core test에서 application을 test 실행 전 생성하고 process 전역 객체보다 먼저 소멸
// 입력: argc/argv: Google Test filter와 process 인자
// 출력: 전체 test 결과 code와 정상 application teardown
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
