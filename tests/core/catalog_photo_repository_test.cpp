#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "catalog_photo_repository.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: Qt SQL test에 필요한 단일 QCoreApplication instance 보장
// 입력: 없음
// 출력: 없음
void ensureCoreApplication()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }

    static int argumentCount = 1;
    static char applicationName[] = "flexraw_core_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
}

// 목적: repository 저장 test에 사용할 CatalogEntry 생성
// 입력: path: 사진 절대 경로, displayName: UI 표시 파일명, kind: 지원 파일 종류
// 출력: Ready 상태의 CatalogEntry 값
[[nodiscard]] CatalogEntry makeEntry(const QString& path, const QString& displayName, types::SupportedFileKind kind)
{
    return {
        types::FileDescriptor{
            path,
            QFileInfo(path).suffix().toLower(),
            displayName,
            kind,
        },
        types::FileScanStatus::Ready,
    };
}

// 목적: source binding repository test에서 사용할 완전한 fingerprint 생성
// 입력: marker: SHA-256 digest를 구분할 byte, sizeBytes: source size, modifiedAtMs: source 수정 시각
// 출력: 32 byte content hash를 포함한 SourceFingerprint
[[nodiscard]] types::SourceFingerprint makeFingerprint(char marker, qint64 sizeBytes = 0, qint64 modifiedAtMs = 0)
{
    return {sizeBytes, modifiedAtMs, QByteArray(types::Sha256DigestSize, marker)};
}

class CatalogPhotoRepositoryTest : public testing::Test
{
protected:
    // 목적: CatalogPhotoRepository test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        ensureCoreApplication();
    }
};

TEST_F(CatalogPhotoRepositoryTest, StoresAndQueriesPhotosInDisplayOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QVector<CatalogEntry> entries{
        makeEntry(QStringLiteral("C:/photos/b.CR3"), QStringLiteral("b.CR3"), types::SupportedFileKind::Raw),
        makeEntry(QStringLiteral("C:/photos/a.jpg"), QStringLiteral("a.jpg"), types::SupportedFileKind::RasterImage),
    };

    const CatalogPhotoStoreResult stored = repository.upsert(entries);
    const CatalogPhotoQueryResult listed = repository.queryPage(CatalogPhotoPageRequest{});

    ASSERT_TRUE(stored.hasValue());
    EXPECT_EQ(2, stored.value());
    ASSERT_TRUE(listed.hasValue());
    ASSERT_EQ(2, listed.value().photos.size());
    EXPECT_EQ(QStringLiteral("a.jpg"), listed.value().photos[0].displayName);
    EXPECT_EQ(types::SupportedFileKind::RasterImage, listed.value().photos[0].kind);
    EXPECT_EQ(QStringLiteral("b.CR3"), listed.value().photos[1].displayName);
    EXPECT_EQ(types::SupportedFileKind::Raw, listed.value().photos[1].kind);
}

TEST_F(CatalogPhotoRepositoryTest, QueriesDistinctSourceFoldersWithPhotoCounts)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QVector<CatalogEntry> entries{
        makeEntry(QStringLiteral("C:/photos/first/one.jpg"),
                  QStringLiteral("one.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/first/two.jpg"),
                  QStringLiteral("two.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/second/three.jpg"),
                  QStringLiteral("three.jpg"),
                  types::SupportedFileKind::RasterImage),
    };
    ASSERT_TRUE(repository.upsert(entries).hasValue());

    const CatalogFolderQueryResult folders = repository.queryFolders();

    ASSERT_TRUE(folders.hasValue());
    ASSERT_EQ(2, folders.value().size());
    EXPECT_EQ(QStringLiteral("C:/photos/first"), folders.value()[0].path);
    EXPECT_EQ(2, folders.value()[0].photoCount);
    EXPECT_EQ(QStringLiteral("C:/photos/second"), folders.value()[1].path);
    EXPECT_EQ(1, folders.value()[1].photoCount);
}

TEST_F(CatalogPhotoRepositoryTest, NavigatesDuplicateDisplayNamesWithStableBidirectionalCursor)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QVector<CatalogEntry> entries{
        makeEntry(QStringLiteral("C:/photos/alpha-1.jpg"),
                  QStringLiteral("alpha.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/alpha-2.jpg"),
                  QStringLiteral("Alpha.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/alpha-3.jpg"),
                  QStringLiteral("alpha.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(
            QStringLiteral("C:/photos/beta.jpg"), QStringLiteral("beta.jpg"), types::SupportedFileKind::RasterImage),
        makeEntry(
            QStringLiteral("C:/photos/gamma.jpg"), QStringLiteral("gamma.jpg"), types::SupportedFileKind::RasterImage),
    };
    ASSERT_TRUE(repository.upsert(entries).hasValue());
    CatalogPhotoPageRequest request;
    request.pageSize = 2;

    const CatalogPhotoQueryResult first = repository.queryPage(request);
    ASSERT_TRUE(first.hasValue());
    ASSERT_EQ(2, first.value().photos.size());
    EXPECT_FALSE(first.value().previousCursor.has_value());
    ASSERT_TRUE(first.value().nextCursor.has_value());
    EXPECT_EQ(QStringLiteral("alpha.jpg"), first.value().photos[0].displayName);
    EXPECT_EQ(QStringLiteral("Alpha.jpg"), first.value().photos[1].displayName);
    EXPECT_LT(first.value().photos[0].id.value, first.value().photos[1].id.value);

    request.cursor = first.value().nextCursor;
    const CatalogPhotoQueryResult second = repository.queryPage(request);
    ASSERT_TRUE(second.hasValue());
    ASSERT_EQ(2, second.value().photos.size());
    ASSERT_TRUE(second.value().previousCursor.has_value());
    ASSERT_TRUE(second.value().nextCursor.has_value());
    EXPECT_EQ(QStringLiteral("alpha.jpg"), second.value().photos[0].displayName);
    EXPECT_EQ(QStringLiteral("beta.jpg"), second.value().photos[1].displayName);
    EXPECT_GT(second.value().photos[0].id.value, first.value().photos[1].id.value);

    request.cursor = second.value().nextCursor;
    const CatalogPhotoQueryResult third = repository.queryPage(request);
    ASSERT_TRUE(third.hasValue());
    ASSERT_EQ(1, third.value().photos.size());
    ASSERT_TRUE(third.value().previousCursor.has_value());
    EXPECT_FALSE(third.value().nextCursor.has_value());
    EXPECT_EQ(QStringLiteral("gamma.jpg"), third.value().photos.front().displayName);

    request.direction = CatalogPhotoPageDirection::Backward;
    request.cursor = third.value().previousCursor;
    const CatalogPhotoQueryResult previous = repository.queryPage(request);
    ASSERT_TRUE(previous.hasValue());
    ASSERT_EQ(2, previous.value().photos.size());
    EXPECT_EQ(second.value().photos[0].id.value, previous.value().photos[0].id.value);
    EXPECT_EQ(second.value().photos[1].id.value, previous.value().photos[1].id.value);

    request.cursor = previous.value().previousCursor;
    const CatalogPhotoQueryResult firstAgain = repository.queryPage(request);
    ASSERT_TRUE(firstAgain.hasValue());
    ASSERT_EQ(2, firstAgain.value().photos.size());
    EXPECT_FALSE(firstAgain.value().previousCursor.has_value());
    EXPECT_EQ(first.value().photos[0].id.value, firstAgain.value().photos[0].id.value);
    EXPECT_EQ(first.value().photos[1].id.value, firstAgain.value().photos[1].id.value);
}

TEST_F(CatalogPhotoRepositoryTest, QueriesOnlyExactFolderWithScopedBidirectionalCursor)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QVector<CatalogEntry> entries{
        makeEntry(QStringLiteral("C:/photos/session/alpha.jpg"),
                  QStringLiteral("alpha.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/session/beta.jpg"),
                  QStringLiteral("beta.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/session/gamma.jpg"),
                  QStringLiteral("gamma.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/session/nested/hidden.jpg"),
                  QStringLiteral("hidden.jpg"),
                  types::SupportedFileKind::RasterImage),
        makeEntry(QStringLiteral("C:/photos/session-copy/sibling.jpg"),
                  QStringLiteral("sibling.jpg"),
                  types::SupportedFileKind::RasterImage),
    };
    ASSERT_TRUE(repository.upsert(entries).hasValue());
    CatalogPhotoPageRequest request;
    request.pageSize = 2;
    request.exactFolderPath = QStringLiteral("C:\\photos\\session\\");

    const CatalogPhotoQueryResult first = repository.queryPage(request);
    ASSERT_TRUE(first.hasValue());
    ASSERT_EQ(2, first.value().photos.size());
    EXPECT_EQ(QStringLiteral("alpha.jpg"), first.value().photos[0].displayName);
    EXPECT_EQ(QStringLiteral("beta.jpg"), first.value().photos[1].displayName);
    ASSERT_TRUE(first.value().nextCursor.has_value());
    ASSERT_TRUE(first.value().nextCursor->exactFolderPath.has_value());
    EXPECT_EQ(QStringLiteral("C:/photos/session"), *first.value().nextCursor->exactFolderPath);

    request.cursor = first.value().nextCursor;
    const CatalogPhotoQueryResult second = repository.queryPage(request);
    ASSERT_TRUE(second.hasValue());
    ASSERT_EQ(1, second.value().photos.size());
    EXPECT_EQ(QStringLiteral("gamma.jpg"), second.value().photos.front().displayName);
    ASSERT_TRUE(second.value().previousCursor.has_value());
    EXPECT_FALSE(second.value().nextCursor.has_value());

    request.direction = CatalogPhotoPageDirection::Backward;
    request.cursor = second.value().previousCursor;
    const CatalogPhotoQueryResult firstAgain = repository.queryPage(request);
    ASSERT_TRUE(firstAgain.hasValue());
    ASSERT_EQ(2, firstAgain.value().photos.size());
    EXPECT_EQ(QStringLiteral("alpha.jpg"), firstAgain.value().photos[0].displayName);
    EXPECT_EQ(QStringLiteral("beta.jpg"), firstAgain.value().photos[1].displayName);
}

TEST_F(CatalogPhotoRepositoryTest, RejectsUnboundedOrInvalidPageRequests)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    CatalogPhotoPageRequest request;

    request.pageSize = 0;
    ASSERT_TRUE(repository.queryPage(request).hasError());
    request.pageSize = MaximumCatalogPhotoPageSize + 1;
    ASSERT_TRUE(repository.queryPage(request).hasError());
    request.pageSize = DefaultCatalogPhotoPageSize;
    request.direction = CatalogPhotoPageDirection::Backward;
    ASSERT_TRUE(repository.queryPage(request).hasError());
    request.direction = CatalogPhotoPageDirection::Forward;
    request.cursor = CatalogPhotoPageCursor{QStringLiteral("photo.jpg"), types::PhotoId{}};
    const CatalogPhotoQueryResult invalidCursor = repository.queryPage(request);

    ASSERT_TRUE(invalidCursor.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidCursor.error().code);

    request.cursor.reset();
    request.exactFolderPath = QStringLiteral(" ");
    EXPECT_TRUE(repository.queryPage(request).hasError());
    request.exactFolderPath = QStringLiteral("C:/photos/one");
    request.cursor =
        CatalogPhotoPageCursor{QStringLiteral("photo.jpg"), types::PhotoId{1}, QStringLiteral("C:/photos/two")};
    const CatalogPhotoQueryResult mismatchedScope = repository.queryPage(request);
    ASSERT_TRUE(mismatchedScope.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, mismatchedScope.error().code);
}

TEST_F(CatalogPhotoRepositoryTest, UpdatesExistingPhotoByPath)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QString path = QStringLiteral("C:/photos/renamed.jpg");

    ASSERT_TRUE(repository.upsert({makeEntry(path, QStringLiteral("old.jpg"), types::SupportedFileKind::RasterImage)})
                    .hasValue());
    const CatalogPhotoStoreResult updated =
        repository.upsert({makeEntry(path, QStringLiteral("renamed.jpg"), types::SupportedFileKind::RasterImage)});
    const CatalogPhotoQueryResult listed = repository.queryPage(CatalogPhotoPageRequest{});

    ASSERT_TRUE(updated.hasValue());
    EXPECT_EQ(1, updated.value());
    ASSERT_TRUE(listed.hasValue());
    ASSERT_EQ(1, listed.value().photos.size());
    EXPECT_EQ(QStringLiteral("renamed.jpg"), listed.value().photos[0].displayName);

    const CatalogPhotoRecordResult record = repository.findBySourcePath(path);
    ASSERT_TRUE(record.hasValue());
    ASSERT_TRUE(record.value().has_value());
    EXPECT_TRUE(types::isValidPhotoId(record.value()->id));
}

TEST_F(CatalogPhotoRepositoryTest, RejectsIncompleteEntry)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());

    const CatalogPhotoStoreResult result = repository.upsert({CatalogEntry{}});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST_F(CatalogPhotoRepositoryTest, KeepsDeferredReplacementStateAndBaselineAcrossReopen)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString sourcePath = QStringLiteral("C:/photos/replaced.CR3");
    types::PhotoId photoId;
    const types::SourceFingerprint baseline = makeFingerprint('a');

    {
        CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        CatalogPhotoRepository repository(*database.value());
        ASSERT_TRUE(
            repository.upsert({makeEntry(sourcePath, QStringLiteral("replaced.CR3"), types::SupportedFileKind::Raw)})
                .hasValue());
        CatalogPhotoRecordResult record = repository.findBySourcePath(sourcePath);
        ASSERT_TRUE(record.hasValue());
        ASSERT_TRUE(record.value().has_value());
        photoId = record.value()->id;
        ASSERT_TRUE(repository.establishSourceFingerprint(photoId, baseline).hasValue());
        ASSERT_TRUE(repository.recordSourceState(photoId, SourceBindingState::ReplacementDetected).hasValue());
    }

    CatalogDatabaseOpenResult reopened = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(reopened.hasValue());
    CatalogPhotoRepository repository(*reopened.value());
    const CatalogPhotoRecordResult deferred = repository.findById(photoId);

    ASSERT_TRUE(deferred.hasValue());
    ASSERT_TRUE(deferred.value().has_value());
    EXPECT_EQ(SourceBindingState::ReplacementDetected, deferred.value()->sourceState);
    EXPECT_EQ(baseline.sha256, deferred.value()->fingerprint.sha256);
    ASSERT_TRUE(deferred.value()->source.has_value());
    EXPECT_EQ(sourcePath, deferred.value()->source->path);
}

TEST_F(CatalogPhotoRepositoryTest, AcceptsReplacementWithoutChangingPhotoId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QString sourcePath = QStringLiteral("C:/photos/replaced.CR3");
    ASSERT_TRUE(
        repository.upsert({makeEntry(sourcePath, QStringLiteral("replaced.CR3"), types::SupportedFileKind::Raw)})
            .hasValue());
    const CatalogPhotoRecordResult initial = repository.findBySourcePath(sourcePath);
    ASSERT_TRUE(initial.hasValue());
    ASSERT_TRUE(initial.value().has_value());
    const types::PhotoId photoId = initial.value()->id;
    ASSERT_TRUE(repository.establishSourceFingerprint(photoId, makeFingerprint('a')).hasValue());
    ASSERT_TRUE(repository.recordSourceState(photoId, SourceBindingState::ReplacementDetected).hasValue());
    const types::SourceFingerprint replacement = makeFingerprint('b', 2048, 5678);

    const CatalogPhotoMutationResult accepted = repository.acceptReplacement(photoId, replacement);
    const CatalogPhotoRecordResult record = repository.findById(photoId);

    ASSERT_TRUE(accepted.hasValue());
    ASSERT_TRUE(record.hasValue());
    ASSERT_TRUE(record.value().has_value());
    EXPECT_EQ(photoId.value, record.value()->id.value);
    EXPECT_EQ(SourceBindingState::Available, record.value()->sourceState);
    EXPECT_EQ(replacement.sha256, record.value()->fingerprint.sha256);
}

TEST_F(CatalogPhotoRepositoryTest, ConfirmsMetadataChangeOnlyWhenContentHashMatchesBaseline)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QString sourcePath = QStringLiteral("C:/photos/metadata-change.CR3");
    ASSERT_TRUE(
        repository.upsert({makeEntry(sourcePath, QStringLiteral("metadata-change.CR3"), types::SupportedFileKind::Raw)})
            .hasValue());
    const CatalogPhotoRecordResult initial = repository.findBySourcePath(sourcePath);
    ASSERT_TRUE(initial.hasValue());
    ASSERT_TRUE(initial.value().has_value());
    const types::PhotoId photoId = initial.value()->id;
    const types::SourceFingerprint baseline = makeFingerprint('a');
    ASSERT_TRUE(repository.establishSourceFingerprint(photoId, baseline).hasValue());
    ASSERT_TRUE(repository.recordSourceState(photoId, SourceBindingState::VerificationRequired).hasValue());
    const types::SourceFingerprint mismatched = makeFingerprint('b', 2048, 5678);
    const CatalogPhotoMutationResult rejected = repository.confirmSourceMatch(photoId, mismatched);
    ASSERT_TRUE(rejected.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, rejected.error().code);
    const types::SourceFingerprint matched = makeFingerprint('a', 2048, 5678);

    const CatalogPhotoMutationResult confirmed = repository.confirmSourceMatch(photoId, matched);
    const CatalogPhotoRecordResult record = repository.findById(photoId);

    ASSERT_TRUE(confirmed.hasValue());
    ASSERT_TRUE(record.hasValue());
    ASSERT_TRUE(record.value().has_value());
    EXPECT_EQ(SourceBindingState::Available, record.value()->sourceState);
    EXPECT_EQ(2048, record.value()->fingerprint.sizeBytes);
    EXPECT_EQ(5678, record.value()->fingerprint.modifiedAtMs);
    EXPECT_EQ(baseline.sha256, record.value()->fingerprint.sha256);
}

TEST_F(CatalogPhotoRepositoryTest, RegistersReplacementWithNewIdAndLeavesOriginalUnlinked)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QString sourcePath = QStringLiteral("C:/photos/replaced.CR3");
    const CatalogEntry entry = makeEntry(sourcePath, QStringLiteral("replaced.CR3"), types::SupportedFileKind::Raw);
    ASSERT_TRUE(repository.upsert({entry}).hasValue());
    const CatalogPhotoRecordResult initial = repository.findBySourcePath(sourcePath);
    ASSERT_TRUE(initial.hasValue());
    ASSERT_TRUE(initial.value().has_value());
    const types::PhotoId originalId = initial.value()->id;
    const types::SourceFingerprint originalFingerprint = makeFingerprint('a');
    ASSERT_TRUE(repository.establishSourceFingerprint(originalId, originalFingerprint).hasValue());
    CatalogDevelopRepository developRepository(*database.value());
    types::DevelopParams originalParams;
    originalParams.exposureEv = 1.0F;
    ASSERT_TRUE(developRepository.saveParams(originalId, originalParams).hasValue());
    ASSERT_TRUE(developRepository.appendHistory(originalId, originalParams).hasValue());
    ASSERT_TRUE(repository.recordSourceState(originalId, SourceBindingState::ReplacementDetected).hasValue());

    const CatalogPhotoCreateResult registered =
        repository.registerReplacementAsNew(originalId, entry, makeFingerprint('b', 2048, 5678));
    const CatalogPhotoRecordResult original = repository.findById(originalId);
    const CatalogPhotoRecordResult replacement = repository.findBySourcePath(sourcePath);

    ASSERT_TRUE(registered.hasValue());
    EXPECT_NE(originalId.value, registered.value().value);
    ASSERT_TRUE(original.hasValue());
    ASSERT_TRUE(original.value().has_value());
    EXPECT_FALSE(original.value()->source.has_value());
    EXPECT_EQ(SourceBindingState::Unlinked, original.value()->sourceState);
    EXPECT_EQ(originalFingerprint.sha256, original.value()->fingerprint.sha256);
    ASSERT_TRUE(replacement.hasValue());
    ASSERT_TRUE(replacement.value().has_value());
    EXPECT_EQ(registered.value().value, replacement.value()->id.value);
    EXPECT_EQ(SourceBindingState::Available, replacement.value()->sourceState);
    const CatalogDevelopParamsResult preservedParams = developRepository.loadParams(originalId);
    const CatalogDevelopHistoryResult preservedHistory = developRepository.listHistory(originalId);
    const CatalogDevelopParamsResult newParams = developRepository.loadParams(registered.value());
    ASSERT_TRUE(preservedParams.hasValue());
    ASSERT_TRUE(preservedParams.value().has_value());
    EXPECT_EQ(originalParams, *preservedParams.value());
    ASSERT_TRUE(preservedHistory.hasValue());
    ASSERT_EQ(1, preservedHistory.value().size());
    ASSERT_TRUE(newParams.hasValue());
    EXPECT_FALSE(newParams.value().has_value());
}

TEST_F(CatalogPhotoRepositoryTest, RelinksOriginalPhotoWithoutChangingIdentity)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository repository(*database.value());
    const QString oldPath = QStringLiteral("C:/photos/missing.CR3");
    ASSERT_TRUE(repository.upsert({makeEntry(oldPath, QStringLiteral("missing.CR3"), types::SupportedFileKind::Raw)})
                    .hasValue());
    const CatalogPhotoRecordResult initial = repository.findBySourcePath(oldPath);
    ASSERT_TRUE(initial.hasValue());
    ASSERT_TRUE(initial.value().has_value());
    const types::PhotoId photoId = initial.value()->id;
    const types::SourceFingerprint fingerprint = makeFingerprint('a');
    ASSERT_TRUE(repository.establishSourceFingerprint(photoId, fingerprint).hasValue());
    ASSERT_TRUE(repository.recordSourceState(photoId, SourceBindingState::Missing).hasValue());
    const QString newPath = QStringLiteral("D:/archive/missing.CR3");
    const CatalogPhotoMutationResult rejected =
        repository.relinkSource(photoId, types::SourceLocator{newPath}, makeFingerprint('b'));
    ASSERT_TRUE(rejected.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, rejected.error().code);

    const CatalogPhotoMutationResult relinked =
        repository.relinkSource(photoId, types::SourceLocator{newPath}, fingerprint);
    const CatalogPhotoRecordResult record = repository.findById(photoId);

    ASSERT_TRUE(relinked.hasValue());
    ASSERT_TRUE(record.hasValue());
    ASSERT_TRUE(record.value().has_value());
    EXPECT_EQ(photoId.value, record.value()->id.value);
    ASSERT_TRUE(record.value()->source.has_value());
    EXPECT_EQ(newPath, record.value()->source->path);
    EXPECT_EQ(SourceBindingState::Available, record.value()->sourceState);
    CatalogPhotoPageRequest oldFolderRequest;
    oldFolderRequest.exactFolderPath = QStringLiteral("C:/photos");
    const CatalogPhotoQueryResult oldFolder = repository.queryPage(oldFolderRequest);
    ASSERT_TRUE(oldFolder.hasValue());
    EXPECT_TRUE(oldFolder.value().photos.isEmpty());
    CatalogPhotoPageRequest newFolderRequest;
    newFolderRequest.exactFolderPath = QStringLiteral("D:/archive");
    const CatalogPhotoQueryResult newFolder = repository.queryPage(newFolderRequest);
    ASSERT_TRUE(newFolder.hasValue());
    ASSERT_EQ(1, newFolder.value().photos.size());
    EXPECT_EQ(photoId.value, newFolder.value().photos.front().id.value);
}

}  // namespace
}  // namespace flexraw::core::catalog
