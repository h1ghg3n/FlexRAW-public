#include <QCoreApplication>

#include <gtest/gtest.h>

// 목적: Worker network test용 Qt event loop와 Google Test runtime 초기화
// 입력: argc/argv: CTest와 Google Test command-line arguments
// 출력: 전체 Worker test 결과 process code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Worker Tests"));
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
