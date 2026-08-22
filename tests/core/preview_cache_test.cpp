#include "preview_cache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace flexraw::core::preview {
namespace {

// 목적: preview cache test용 원본 파일 생성
// 입력: path: 생성할 원본 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createSourceFile(const QString& path)
{
    QFile sourceFile(path);

    if (!sourceFile.open(QIODevice::WriteOnly)) {
        return false;
    }

    return sourceFile.write("preview-cache-source") > 0;
}

TEST(PreviewCache, ReturnsMissForNewEntry)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString sourcePath = QDir(folder.path()).filePath(QStringLiteral("source.dng"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    const PreviewCache cache(QDir(folder.path()).filePath(QStringLiteral("cache")));

    const PreviewCacheLookupResult result = cache.load(sourcePath, PreviewCacheTier::Thumbnail, QSize{128, 128});

    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(result.value().hit);
    EXPECT_TRUE(result.value().image.isNull());
}

TEST(PreviewCache, StoresAndLoadsCurrentPreview)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString sourcePath = QDir(folder.path()).filePath(QStringLiteral("source.dng"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    const PreviewCache cache(QDir(folder.path()).filePath(QStringLiteral("cache")));
    QImage image(2, 1, QImage::Format_RGBA8888);
    image.fill(Qt::red);

    const PreviewCacheStoreResult storeResult = cache.store(sourcePath, PreviewCacheTier::Standard, QSize{2, 1}, image);
    ASSERT_TRUE(storeResult.hasValue()) << storeResult.error().message.toStdString();

    const PreviewCacheLookupResult loadResult = cache.load(sourcePath, PreviewCacheTier::Standard, QSize{2, 1});

    ASSERT_TRUE(loadResult.hasValue());
    ASSERT_TRUE(loadResult.value().hit);
    EXPECT_EQ(QColor(Qt::red), loadResult.value().image.pixelColor(0, 0));
}

TEST(PreviewCache, InvalidatesEntryWhenSourceMtimeChanges)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());

    const QString sourcePath = QDir(folder.path()).filePath(QStringLiteral("source.dng"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    const PreviewCache cache(QDir(folder.path()).filePath(QStringLiteral("cache")));
    QImage image(2, 1, QImage::Format_RGBA8888);
    image.fill(Qt::blue);

    const PreviewCacheStoreResult storeResult =
        cache.store(sourcePath, PreviewCacheTier::Thumbnail, QSize{2, 1}, image);
    ASSERT_TRUE(storeResult.hasValue()) << storeResult.error().message.toStdString();

    const QFileInfo sourceInfo(sourcePath);
    QFile sourceFile(sourcePath);
    ASSERT_TRUE(sourceFile.open(QIODevice::ReadWrite));
    ASSERT_TRUE(sourceFile.setFileTime(sourceInfo.lastModified().addSecs(2), QFileDevice::FileModificationTime));
    sourceFile.close();

    const PreviewCacheLookupResult loadResult =
        cache.load(sourcePath, PreviewCacheTier::Thumbnail, QSize{2, 1});

    ASSERT_TRUE(loadResult.hasValue());
    EXPECT_FALSE(loadResult.value().hit);
}

} // namespace
} // namespace flexraw::core::preview
