#include "progressive_preview_controller.h"

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

TEST(ProgressivePreviewController, EmitsAndCachesRasterThumbnail)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString imagePath = QDir(folder.path()).filePath(QStringLiteral("sample.bmp"));
    const QString cacheRoot = QDir(folder.path()).filePath(QStringLiteral("cache"));
    ASSERT_TRUE(createBitmapFile(imagePath));
    ProgressivePreviewController controller(cacheRoot);
    QImage emittedImage;
    QString emittedPath;

    QObject::connect(
        &controller,
        &ProgressivePreviewController::thumbnailPreviewReady,
        [&emittedImage, &emittedPath](const QString& filePath, const QString&, const QImage& image) {
            emittedPath = filePath;
            emittedImage = image;
        });
    const types::FileDescriptor file{
        imagePath,
        QStringLiteral("bmp"),
        QStringLiteral("sample.bmp"),
        types::SupportedFileKind::RasterImage,
    };
    controller.requestPreview(file, QSize{2, 2});

    EXPECT_EQ(imagePath, emittedPath);
    EXPECT_EQ(QColor(Qt::blue), emittedImage.pixelColor(0, 0));
    const PreviewCache cache(cacheRoot);
    const PreviewCacheLookupResult cached = cache.load(imagePath, PreviewCacheTier::Thumbnail, QSize{2, 2});
    ASSERT_TRUE(cached.hasValue());
    EXPECT_TRUE(cached.value().hit);
}

} // namespace
} // namespace flexraw::core::preview
