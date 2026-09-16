#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

#include "shared_storage_locator.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: temporary storage root에 schema 1 marker JSON 생성
// 입력: rootPath/storageId: marker directory와 UUID, name: optional label
// 출력: marker file write 성공 여부
[[nodiscard]] bool writeMarker(const QString& rootPath, const QUuid& storageId, const QString& name = {})
{
    QJsonObject object{{QStringLiteral("schema"), 1},
                       {QStringLiteral("storage_id"), storageId.toString(QUuid::WithoutBraces)}};
    if (!name.isEmpty())
    {
        object.insert(QStringLiteral("name"), name);
    }
    QFile marker(QDir(rootPath).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    return marker.open(QIODevice::WriteOnly) && marker.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
}

// 목적: locator source fixture에 사용할 regular file 생성
// 입력: path: 기존 parent 아래 absolute file path
// 출력: file 생성 성공 여부
[[nodiscard]] bool createFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("raw") == 3;
}

TEST(SharedStorageLocatorTest, UsesNearestAncestorMarkerForNestedUnicodeSource)
{
    QTemporaryDir storageRoot;
    ASSERT_TRUE(storageRoot.isValid());
    const QUuid outerId = QUuid::createUuid();
    const QUuid nearestId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(storageRoot.path(), outerId));
    ASSERT_TRUE(QDir(storageRoot.path()).mkpath(QStringLiteral("사진/작업/2026/여행")));
    const QString nearestRoot = QDir(storageRoot.path()).filePath(QStringLiteral("사진/작업"));
    ASSERT_TRUE(writeMarker(nearestRoot, nearestId, QStringLiteral("작업 저장소")));
    const QString sourcePath = QDir(nearestRoot).filePath(QStringLiteral("2026/여행/사진.ARW"));
    ASSERT_TRUE(createFile(sourcePath));

    const LocateSharedStorageResult located = SharedStorageLocator::locateSource(sourcePath);

    ASSERT_TRUE(located.hasValue());
    EXPECT_EQ(QFileInfo(nearestRoot).canonicalFilePath(), located.value().localRoot);
    EXPECT_EQ(nearestId, located.value().storageId);
    EXPECT_EQ(QStringLiteral("2026/여행/사진.ARW"), located.value().relativePath);
}

TEST(SharedStorageLocatorTest, LocatesMissingOutputFromExistingParent)
{
    QTemporaryDir storageRoot;
    ASSERT_TRUE(storageRoot.isValid());
    const QUuid storageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(storageRoot.path(), storageId));
    ASSERT_TRUE(QDir(storageRoot.path()).mkpath(QStringLiteral("exports/결과")));
    const QString outputPath = QDir(storageRoot.path()).filePath(QStringLiteral("exports/결과/output.jpg"));

    const LocateSharedStorageResult located = SharedStorageLocator::locateOutput(outputPath);

    ASSERT_TRUE(located.hasValue());
    EXPECT_EQ(QFileInfo(storageRoot.path()).canonicalFilePath(), located.value().localRoot);
    EXPECT_EQ(storageId, located.value().storageId);
    EXPECT_EQ(QStringLiteral("exports/결과/output.jpg"), located.value().relativePath);
}

TEST(SharedStorageLocatorTest, RejectsPathWithoutMarker)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createFile(sourcePath));

    const LocateSharedStorageResult located = SharedStorageLocator::locateSource(sourcePath);

    ASSERT_TRUE(located.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::NoStorageMarker, located.error().code);
}

TEST(SharedStorageLocatorTest, RejectsOuterWhitespaceWithoutRemappingTheSource)
{
    QTemporaryDir storageRoot;
    ASSERT_TRUE(storageRoot.isValid());
    ASSERT_TRUE(writeMarker(storageRoot.path(), QUuid::createUuid()));
    const QString sourcePath = QDir(storageRoot.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createFile(sourcePath));

    const LocateSharedStorageResult located = SharedStorageLocator::locateSource(sourcePath + QLatin1Char(' '));

    ASSERT_TRUE(located.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::InvalidPath, located.error().code);
}

TEST(SharedStorageLocatorTest, RejectsMalformedAndUnsupportedMarkers)
{
    QTemporaryDir malformedRoot;
    QTemporaryDir unsupportedRoot;
    ASSERT_TRUE(malformedRoot.isValid());
    ASSERT_TRUE(unsupportedRoot.isValid());
    const QString malformedSource = QDir(malformedRoot.path()).filePath(QStringLiteral("input.ARW"));
    const QString unsupportedSource = QDir(unsupportedRoot.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createFile(malformedSource));
    ASSERT_TRUE(createFile(unsupportedSource));

    QFile malformedMarker(QDir(malformedRoot.path()).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    ASSERT_TRUE(malformedMarker.open(QIODevice::WriteOnly));
    ASSERT_GT(malformedMarker.write("{broken"), 0);
    malformedMarker.close();
    QFile unsupportedMarker(QDir(unsupportedRoot.path()).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    ASSERT_TRUE(unsupportedMarker.open(QIODevice::WriteOnly));
    ASSERT_GT(unsupportedMarker.write("{\"schema\":2,\"storage_id\":\"00000000-0000-0000-0000-000000000001\"}"), 0);
    unsupportedMarker.close();

    const LocateSharedStorageResult malformed = SharedStorageLocator::locateSource(malformedSource);
    const LocateSharedStorageResult unsupported = SharedStorageLocator::locateSource(unsupportedSource);

    ASSERT_TRUE(malformed.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::MalformedMarker, malformed.error().code);
    ASSERT_TRUE(unsupported.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::UnsupportedSchema, unsupported.error().code);
}

TEST(SharedStorageLocatorTest, RejectsEmptyOrInvalidStorageId)
{
    QTemporaryDir emptyRoot;
    QTemporaryDir invalidRoot;
    ASSERT_TRUE(emptyRoot.isValid());
    ASSERT_TRUE(invalidRoot.isValid());
    const QString emptySource = QDir(emptyRoot.path()).filePath(QStringLiteral("input.ARW"));
    const QString invalidSource = QDir(invalidRoot.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createFile(emptySource));
    ASSERT_TRUE(createFile(invalidSource));

    QFile emptyMarker(QDir(emptyRoot.path()).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    ASSERT_TRUE(emptyMarker.open(QIODevice::WriteOnly));
    ASSERT_GT(emptyMarker.write("{\"schema\":1,\"storage_id\":\"\"}"), 0);
    emptyMarker.close();
    QFile invalidMarker(QDir(invalidRoot.path()).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    ASSERT_TRUE(invalidMarker.open(QIODevice::WriteOnly));
    ASSERT_GT(invalidMarker.write("{\"schema\":1,\"storage_id\":\"not-a-uuid\"}"), 0);
    invalidMarker.close();

    const LocateSharedStorageResult empty = SharedStorageLocator::locateSource(emptySource);
    const LocateSharedStorageResult invalid = SharedStorageLocator::locateSource(invalidSource);

    ASSERT_TRUE(empty.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::InvalidStorageId, empty.error().code);
    ASSERT_TRUE(invalid.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::InvalidStorageId, invalid.error().code);
}

TEST(SharedStorageLocatorTest, RejectsNonPortableOutputName)
{
    QTemporaryDir storageRoot;
    ASSERT_TRUE(storageRoot.isValid());
    ASSERT_TRUE(writeMarker(storageRoot.path(), QUuid::createUuid()));

    const LocateSharedStorageResult located =
        SharedStorageLocator::locateOutput(QDir(storageRoot.path()).filePath(QStringLiteral("bad:name.jpg")));

    ASSERT_TRUE(located.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::NonPortablePath, located.error().code);
}

TEST(SharedStorageLocatorTest, CreatesOneMarkerAtConfiguredRootAndReusesIt)
{
    QTemporaryDir storageRoot;
    ASSERT_TRUE(storageRoot.isValid());
    const QString nestedDirectory = QDir(storageRoot.path()).filePath(QStringLiteral("촬영/원본"));
    ASSERT_TRUE(QDir().mkpath(nestedDirectory));

    const EnsureSharedStorageMarkerResult created =
        SharedStorageLocator::ensureMarkerForDirectory(nestedDirectory, storageRoot.path());
    const EnsureSharedStorageMarkerResult reused =
        SharedStorageLocator::ensureMarkerForDirectory(nestedDirectory, storageRoot.path());

    ASSERT_FALSE(created.hasError()) << created.error().message.toStdString();
    ASSERT_FALSE(reused.hasError()) << reused.error().message.toStdString();
    EXPECT_EQ(QFileInfo(storageRoot.path()).canonicalFilePath(), created.value().localRoot);
    EXPECT_FALSE(created.value().storageId.isNull());
    EXPECT_EQ(created.value().storageId, reused.value().storageId);
    EXPECT_TRUE(QFileInfo::exists(QDir(storageRoot.path()).filePath(QString::fromLatin1(SharedStorageMarkerFileName))));
    EXPECT_FALSE(QFileInfo::exists(QDir(nestedDirectory).filePath(QString::fromLatin1(SharedStorageMarkerFileName))));
}

TEST(SharedStorageLocatorTest, DoesNotReplaceMalformedMarkerDuringEnsure)
{
    QTemporaryDir storageRoot;
    ASSERT_TRUE(storageRoot.isValid());
    QFile marker(QDir(storageRoot.path()).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
    ASSERT_EQ(7, marker.write("{broken"));
    marker.close();

    const EnsureSharedStorageMarkerResult ensured =
        SharedStorageLocator::ensureMarkerForDirectory(storageRoot.path(), storageRoot.path());

    ASSERT_TRUE(ensured.hasError());
    EXPECT_EQ(SharedStorageLocatorErrorCode::MalformedMarker, ensured.error().code);
    ASSERT_TRUE(marker.open(QIODevice::ReadOnly));
    EXPECT_EQ(QByteArray("{broken"), marker.readAll());
}

}  // namespace
}  // namespace flexraw::worker::client
