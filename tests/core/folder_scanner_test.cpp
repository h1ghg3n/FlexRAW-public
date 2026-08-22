#include "folder_scanner.h"

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace flexraw::core::catalog {
namespace {

// 목적: folder scanner test 에 사용할 빈 파일 생성
// 입력: path: 생성할 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createEmptyFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

TEST(FolderScanner, ReturnsInvalidArgumentForEmptyPath)
{
    const CatalogScanResult result = scanFolder(QStringLiteral(" "));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(FolderScanner, ReturnsNotFoundForMissingFolder)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString missingPath = QDir(folder.path()).filePath(QStringLiteral("missing"));
    const CatalogScanResult result = scanFolder(missingPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

TEST(FolderScanner, IgnoresUnsupportedFiles)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    ASSERT_TRUE(createEmptyFile(QDir(folder.path()).filePath(QStringLiteral("notes.txt"))));

    const CatalogScanResult result = scanFolder(folder.path());

    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().isEmpty());
}

TEST(FolderScanner, ReturnsSupportedFilesSortedByDisplayName)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    ASSERT_TRUE(createEmptyFile(QDir(folder.path()).filePath(QStringLiteral("b.CR3"))));
    ASSERT_TRUE(createEmptyFile(QDir(folder.path()).filePath(QStringLiteral("a.jpg"))));
    ASSERT_TRUE(createEmptyFile(QDir(folder.path()).filePath(QStringLiteral("ignored.txt"))));

    const CatalogScanResult result = scanFolder(folder.path());

    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(2, result.value().size());
    EXPECT_EQ(QStringLiteral("a.jpg"), result.value()[0].file.displayName);
    EXPECT_EQ(types::SupportedFileKind::RasterImage, result.value()[0].file.kind);
    EXPECT_EQ(QStringLiteral("b.CR3"), result.value()[1].file.displayName);
    EXPECT_EQ(types::SupportedFileKind::Raw, result.value()[1].file.kind);
}

} // namespace
} // namespace flexraw::core::catalog
