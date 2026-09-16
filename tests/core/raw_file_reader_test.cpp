#include "raw_file_reader.h"

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace flexraw::core::raw {
namespace {

// 목적: RAW reader 형식 검증 test 에 사용할 빈 파일 생성
// 입력: path: 생성할 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createEmptyFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

TEST(RawFileReader, ReturnsInvalidArgumentForEmptyPath)
{
    const RawMetadataResult result = readRawMetadata(QStringLiteral(" "));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(RawFileReader, RejectsOuterWhitespaceBeforeFilesystemLookup)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const QString sourcePath = QDir(folder.path()).filePath(QStringLiteral("input.dng"));
    ASSERT_TRUE(createEmptyFile(sourcePath));

    const RawMetadataResult result = readRawMetadata(sourcePath + QLatin1Char(' '));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(RawFileReader, ReturnsNotFoundForMissingFile)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const RawMetadataResult result = readRawMetadata(QDir(folder.path()).filePath(QStringLiteral("missing.dng")));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

TEST(RawFileReader, RejectsNonRawThumbnailSource)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString imagePath = QDir(folder.path()).filePath(QStringLiteral("image.jpg"));
    ASSERT_TRUE(createEmptyFile(imagePath));

    const RawThumbnailResult result = extractEmbeddedThumbnail(imagePath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::UnsupportedFormat, result.error().code);
}

TEST(RawFileReader, RejectsNonRawFullPreviewSource)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString imagePath = QDir(folder.path()).filePath(QStringLiteral("image.jpg"));
    ASSERT_TRUE(createEmptyFile(imagePath));

    const RawPreviewImageResult result = decodeRawPreviewImage(imagePath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::UnsupportedFormat, result.error().code);
}

} // namespace
} // namespace flexraw::core::raw
