#include <QApplication>
#include <QByteArray>

#include <gtest/gtest.h>

// 목적: UI test용 QApplication을 생성하고 Google Test suite 실행
// 입력: argc: argument 개수, argv: argument 문자열 배열
// 출력: Google Test 종료 code
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("minimal"));
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
