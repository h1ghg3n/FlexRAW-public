#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>
#include <QTimer>

#include <gtest/gtest.h>

#include "folder_scan_controller.h"

namespace flexraw::ui::mainwindow
{
namespace
{

// 목적: folder scan controller test에 사용할 빈 지원 파일 생성
// 입력: path: 생성할 지원 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createEmptyFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

TEST(FolderScanControllerTest, ReturnsSupportedEntriesFromWorker)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(createEmptyFile(QDir(directory.path()).filePath(QStringLiteral("photo.jpg"))));
    ASSERT_TRUE(createEmptyFile(QDir(directory.path()).filePath(QStringLiteral("ignored.txt"))));
    FolderScanController controller;
    QEventLoop eventLoop;
    int startedCount = 0;
    core::catalog::CatalogScanResult result =
        core::catalog::CatalogScanResult::failure({core::types::ErrorCode::Unknown, {}});
    QObject::connect(&controller, &FolderScanController::scanStarted, &controller, [&startedCount](const QString&) {
        ++startedCount;
    });
    QObject::connect(&controller,
                     &FolderScanController::scanFinished,
                     &controller,
                     [&result, &eventLoop](const core::catalog::CatalogScanResult& scanResult) {
                         result = scanResult;
                         eventLoop.quit();
                     });

    controller.scanFolder(directory.path());
    QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();

    EXPECT_EQ(1, startedCount);
    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(1, result.value().size());
    EXPECT_EQ(QStringLiteral("photo.jpg"), result.value().front().file.displayName);
}

}  // namespace
}  // namespace flexraw::ui::mainwindow
