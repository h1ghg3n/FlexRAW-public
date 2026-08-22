#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "source_fingerprint.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: fingerprint test에 사용할 고정 content file 생성
// 입력: directory: file을 생성할 temporary directory, content: 저장할 byte
// 출력: 생성된 file path 또는 실패 시 빈 path
[[nodiscard]] QString writeSourceFile(const QTemporaryDir& directory, const QByteArray& content)
{
    const QString path = directory.filePath(QStringLiteral("source.raw"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
    {
        return {};
    }
    return path;
}

TEST(SourceFingerprintTest, InspectsMetadataWithoutReadingContentHash)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeSourceFile(directory, QByteArrayLiteral("abc"));
    ASSERT_FALSE(path.isEmpty());

    const SourceFingerprintResult result = inspectSourceMetadata(types::SourceLocator{path});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(3, result.value().sizeBytes);
    EXPECT_TRUE(result.value().sha256.isEmpty());
}

TEST(SourceFingerprintTest, CalculatesStableSha256WithMetadata)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeSourceFile(directory, QByteArrayLiteral("abc"));
    ASSERT_FALSE(path.isEmpty());
    const types::CancellationSource cancellation;

    const SourceFingerprintResult result = calculateSourceFingerprint(types::SourceLocator{path}, cancellation.token());

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(3, result.value().sizeBytes);
    EXPECT_EQ(QByteArray::fromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
              result.value().sha256);
}

TEST(SourceFingerprintTest, StopsBeforeReadingWhenCancellationWasRequested)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeSourceFile(directory, QByteArrayLiteral("abc"));
    ASSERT_FALSE(path.isEmpty());
    const types::CancellationSource cancellation;
    cancellation.requestCancellation();

    const SourceFingerprintResult result = calculateSourceFingerprint(types::SourceLocator{path}, cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::Cancelled, result.error().code);
}

TEST(SourceFingerprintTest, RejectsMissingSource)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const SourceFingerprintResult result =
        inspectSourceMetadata(types::SourceLocator{directory.filePath(QStringLiteral("missing.raw"))});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::catalog
