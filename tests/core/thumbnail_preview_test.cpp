#include "thumbnail_preview.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace flexraw::core::preview {
namespace {

// 목적: image plugin 없이 raster preview test용 BMP 파일 생성
// 입력: path: 생성할 BMP 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createBitmapFile(const QString& path)
{
    const QByteArray bitmap = QByteArray::fromHex(
        "424d3e0000000000000036000000"
        "28000000020000000100000001001800000000000800000000000000000000000000000000000000"
        "ff0000ff00000000");
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    return file.write(bitmap) == bitmap.size();
}

TEST(ThumbnailPreview, RejectsInvalidTargetSize)
{
    const raw::RawThumbnail thumbnail{
        QByteArray::fromHex("ff000000ff00"),
        raw::ThumbnailFormat::Bitmap,
        2,
        1,
        3,
        8,
    };

    const ThumbnailPreviewResult result = createThumbnailPreview(thumbnail, QSize{});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(ThumbnailPreview, CopiesBitmapThumbnailIntoOwnedImage)
{
    const raw::RawThumbnail thumbnail{
        QByteArray::fromHex("ff000000ff00"),
        raw::ThumbnailFormat::Bitmap,
        2,
        1,
        3,
        8,
    };

    const ThumbnailPreviewResult result = createThumbnailPreview(thumbnail, QSize{2, 1});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(2, result.value().width());
    EXPECT_EQ(1, result.value().height());
    EXPECT_EQ(QColor(Qt::red), result.value().pixelColor(0, 0));
    EXPECT_EQ(QColor(Qt::green), result.value().pixelColor(1, 0));
}

TEST(ThumbnailPreview, AppliesEmbeddedOrientationBeforeDownscale)
{
    const raw::RawThumbnail thumbnail{
        QByteArray::fromHex("ff000000ff00"),
        raw::ThumbnailFormat::Bitmap,
        2,
        1,
        3,
        8,
        raw::RawPreviewOrientation::Rotate90Clockwise,
    };

    const ThumbnailPreviewResult result = createThumbnailPreview(thumbnail, QSize{1, 2});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(1, result.value().width());
    EXPECT_EQ(2, result.value().height());
    EXPECT_EQ(QColor(Qt::red), result.value().pixelColor(0, 0));
    EXPECT_EQ(QColor(Qt::green), result.value().pixelColor(0, 1));
}

TEST(ThumbnailPreview, ConvertsStandardRawPreviewIntoOwnedImage)
{
    const raw::RawPreviewImage image{
        QByteArray::fromHex("ff000000ff00"),
        2,
        1,
        3,
        8,
    };

    const ThumbnailPreviewResult result = createStandardRawPreview(image, QSize{2, 1});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(2, result.value().width());
    EXPECT_EQ(1, result.value().height());
    EXPECT_EQ(QColor(Qt::red), result.value().pixelColor(0, 0));
    EXPECT_EQ(QColor(Qt::green), result.value().pixelColor(1, 0));
}

TEST(ThumbnailPreview, AppliesStandardRawPreviewOrientation)
{
    const raw::RawPreviewImage image{
        QByteArray::fromHex("ff000000ff00"),
        2,
        1,
        3,
        8,
        raw::RawPreviewOrientation::Rotate90Clockwise,
    };

    const ThumbnailPreviewResult result = createStandardRawPreview(image, QSize{2, 2});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(1, result.value().width());
    EXPECT_EQ(2, result.value().height());
    EXPECT_EQ(QColor(Qt::red), result.value().pixelColor(0, 0));
    EXPECT_EQ(QColor(Qt::green), result.value().pixelColor(0, 1));
}

TEST(ThumbnailPreview, AppliesStandardRawPreviewRotation180)
{
    const raw::RawPreviewImage image{
        QByteArray::fromHex("ff000000ff00"),
        2,
        1,
        3,
        8,
        raw::RawPreviewOrientation::Rotate180,
    };

    const ThumbnailPreviewResult result = createStandardRawPreview(image, QSize{2, 1});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QColor(Qt::green), result.value().pixelColor(0, 0));
    EXPECT_EQ(QColor(Qt::red), result.value().pixelColor(1, 0));
}

TEST(ThumbnailPreview, RejectsIncompleteBitmapData)
{
    const raw::RawThumbnail thumbnail{
        QByteArray::fromHex("ff0000"),
        raw::ThumbnailFormat::Bitmap,
        2,
        1,
        3,
        8,
    };

    const ThumbnailPreviewResult result = createThumbnailPreview(thumbnail, QSize{2, 1});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DecodeFailed, result.error().code);
}

TEST(ThumbnailPreview, PropagatesRawReaderFailure)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const ThumbnailPreviewResult result =
        loadRawThumbnailPreview(QDir(folder.path()).filePath(QStringLiteral("missing.dng")), QSize{128, 128});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

TEST(ThumbnailPreview, LoadsRasterImagePreview)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString imagePath = QDir(folder.path()).filePath(QStringLiteral("sample.bmp"));
    ASSERT_TRUE(createBitmapFile(imagePath));

    const types::FileDescriptor file{
        imagePath,
        QStringLiteral("bmp"),
        QStringLiteral("sample.bmp"),
        types::SupportedFileKind::RasterImage,
    };
    const ThumbnailPreviewResult result = loadFilePreview(file, QSize{2, 2});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(2, result.value().width());
    EXPECT_EQ(1, result.value().height());
    EXPECT_EQ(QColor(Qt::blue), result.value().pixelColor(0, 0));
}

} // namespace
} // namespace flexraw::core::preview
