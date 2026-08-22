#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: catalog orchestration test용 source file 생성 또는 내용 교체
// 입력: path: 생성할 file 경로, contents: 저장할 byte 내용
// 출력: file을 완전히 기록했으면 true
[[nodiscard]] bool writeSourceFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: Qt event를 처리하면서 source binding update 수가 목표에 도달할 때까지 대기
// 입력: updates: signal 기록 container, expectedCount: 목표 event 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 수에 도달하면 true
[[nodiscard]] bool waitForUpdateCount(const std::vector<CatalogSourceUpdate>& updates,
                                      std::size_t expectedCount,
                                      int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs)
    {
        if (updates.size() >= expectedCount)
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return updates.size() >= expectedCount;
}

TEST(CatalogOrchestratorTest, OpensImportsAndPersistsBackgroundFingerprint)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("sample.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("catalog-source")));
    CatalogOrchestrator orchestrator;
    std::vector<CatalogSourceUpdate> updates;
    QObject::connect(&orchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&](const CatalogSourceUpdate& update) {
        updates.push_back(update);
    });

    const CatalogSessionResult opened = orchestrator.openCatalog(catalogPath);
    ASSERT_TRUE(opened.hasValue());
    EXPECT_TRUE(opened.value().isOpen);
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().scannedCount);
    ASSERT_EQ(1, imported.value().storedCount);
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_EQ(1, imported.value().fingerprintRequestIds.size());
    ASSERT_TRUE(waitForUpdateCount(updates, 1));
    EXPECT_EQ(catalog::SourceBindingState::Available, updates.front().photo.sourceState);
    EXPECT_TRUE(types::hasSourceContentHash(updates.front().photo.fingerprint));
    const types::PhotoId photoId = imported.value().photoIds.front();

    EXPECT_FALSE(orchestrator.closeCatalog().isOpen);
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogPhotoStateResult restored = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(photoId.value, restored.value().photo.id.value);
    EXPECT_TRUE(restored.value().sourceProcessingAllowed);
    EXPECT_FALSE(restored.value().sourceVerificationRequestId.has_value());
    EXPECT_EQ(0U, restored.value().persistedRevision);
}

TEST(CatalogOrchestratorTest, ImportsPreScannedEntriesWithoutScanningOnOwnerThread)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("prescanned.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("prescanned-source")));
    CatalogOrchestrator orchestrator;
    std::vector<CatalogSourceUpdate> updates;
    QObject::connect(&orchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&](const CatalogSourceUpdate& update) {
        updates.push_back(update);
    });
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const catalog::CatalogEntry entry{
        types::makeFileDescriptor(QFileInfo(sourcePath), types::SupportedFileKind::RasterImage),
        types::FileScanStatus::Ready,
    };

    const CatalogImportResult imported = orchestrator.importScannedEntries({entry});

    ASSERT_TRUE(imported.hasValue());
    EXPECT_EQ(1, imported.value().scannedCount);
    EXPECT_EQ(1, imported.value().storedCount);
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_EQ(1, imported.value().fingerprintRequestIds.size());
    ASSERT_TRUE(waitForUpdateCount(updates, 1));
    EXPECT_EQ(catalog::SourceBindingState::Available, updates.front().photo.sourceState);
}

TEST(CatalogOrchestratorTest, RegistersEditorActivationOnceAndReturnsStableIdentity)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("activation.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("activation-source")));
    CatalogOrchestrator orchestrator;
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const catalog::CatalogEntry entry{
        types::makeFileDescriptor(QFileInfo(sourcePath), types::SupportedFileKind::RasterImage),
        types::FileScanStatus::Ready,
    };

    const CatalogPhotoRegistrationResult first = orchestrator.registerPhoto(entry);
    const CatalogPhotoRegistrationResult second = orchestrator.registerPhoto(entry);

    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(first.value().value, second.value().value);
    const CatalogPhotoPageResult photos = orchestrator.queryPhotos(catalog::CatalogPhotoPageRequest{});
    ASSERT_TRUE(photos.hasValue());
    ASSERT_EQ(1, photos.value().photos.size());
    EXPECT_EQ(first.value().value, photos.value().photos.front().id.value);
}

TEST(CatalogOrchestratorTest, SavesDevelopStateAndRejectsStaleRevision)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("revision.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("revision-source")));
    CatalogOrchestrator orchestrator;
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    const types::PhotoId photoId = imported.value().photoIds.front();
    types::DevelopParams persistedParams;
    persistedParams.exposureEv = 0.8F;

    const CatalogPhotoStateResult saved = orchestrator.saveDevelopState(photoId, persistedParams, 0);
    ASSERT_TRUE(saved.hasValue());
    EXPECT_EQ(persistedParams, saved.value().developParams);
    EXPECT_EQ(1U, saved.value().persistedRevision);
    types::DevelopParams staleParams;
    staleParams.exposureEv = -0.8F;
    const CatalogPhotoStateResult stale = orchestrator.saveDevelopState(photoId, staleParams, 0);
    ASSERT_TRUE(stale.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, stale.error().code);

    const CatalogPhotoStateResult loaded = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(persistedParams, loaded.value().developParams);
    EXPECT_EQ(1U, loaded.value().persistedRevision);
}

TEST(CatalogOrchestratorTest, DetectsAndExplicitlyAcceptsReplacement)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("replacement.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("original-source")));
    CatalogOrchestrator orchestrator;
    std::vector<CatalogSourceUpdate> updates;
    QObject::connect(&orchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&](const CatalogSourceUpdate& update) {
        updates.push_back(update);
    });
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_TRUE(waitForUpdateCount(updates, 1));
    const types::PhotoId photoId = imported.value().photoIds.front();
    types::DevelopParams params;
    params.contrast = 0.2F;
    ASSERT_TRUE(orchestrator.saveDevelopState(photoId, params, 0).hasValue());

    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("different-replacement-source")));
    const CatalogPhotoStateResult verifying = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(verifying.hasValue());
    ASSERT_TRUE(verifying.value().sourceVerificationRequestId.has_value());
    EXPECT_FALSE(verifying.value().sourceProcessingAllowed);
    ASSERT_TRUE(waitForUpdateCount(updates, 2));
    EXPECT_EQ(catalog::SourceBindingState::ReplacementDetected, updates[1].photo.sourceState);

    const CatalogSourceSubmissionResult accepted = orchestrator.acceptReplacement(photoId);
    ASSERT_TRUE(accepted.hasValue());
    ASSERT_TRUE(waitForUpdateCount(updates, 3));
    EXPECT_EQ(photoId.value, updates[2].photo.id.value);
    EXPECT_EQ(catalog::SourceBindingState::Available, updates[2].photo.sourceState);
    const CatalogPhotoStateResult resolved = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(resolved.hasValue());
    EXPECT_TRUE(resolved.value().sourceProcessingAllowed);
    EXPECT_EQ(params, resolved.value().developParams);
    EXPECT_EQ(1U, resolved.value().persistedRevision);
}

TEST(CatalogOrchestratorTest, RegistersReplacementAsNewIdentityWithoutMovingDevelopState)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("new-identity.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("original-identity")));
    CatalogOrchestrator orchestrator;
    std::vector<CatalogSourceUpdate> updates;
    QObject::connect(&orchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&](const CatalogSourceUpdate& update) {
        updates.push_back(update);
    });
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_TRUE(waitForUpdateCount(updates, 1));
    const types::PhotoId originalId = imported.value().photoIds.front();
    types::DevelopParams params;
    params.exposureEv = 1.0F;
    ASSERT_TRUE(orchestrator.saveDevelopState(originalId, params, 0).hasValue());

    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("replacement-new-identity")));
    ASSERT_TRUE(orchestrator.resolvePhoto(originalId).hasValue());
    ASSERT_TRUE(waitForUpdateCount(updates, 2));
    ASSERT_EQ(catalog::SourceBindingState::ReplacementDetected, updates[1].photo.sourceState);
    const CatalogSourceSubmissionResult registered = orchestrator.registerReplacementAsNew(originalId);
    ASSERT_TRUE(registered.hasValue());
    ASSERT_TRUE(waitForUpdateCount(updates, 3));
    ASSERT_TRUE(updates[2].createdPhoto.has_value());
    const types::PhotoId newId = updates[2].createdPhoto->id;
    EXPECT_NE(originalId.value, newId.value);
    EXPECT_EQ(catalog::SourceBindingState::Unlinked, updates[2].photo.sourceState);
    EXPECT_FALSE(updates[2].photo.source.has_value());
    EXPECT_EQ(catalog::SourceBindingState::Available, updates[2].createdPhoto->sourceState);

    const CatalogPhotoStateResult original = orchestrator.resolvePhoto(originalId);
    const CatalogPhotoStateResult replacement = orchestrator.resolvePhoto(newId);
    ASSERT_TRUE(original.hasValue());
    ASSERT_TRUE(replacement.hasValue());
    EXPECT_EQ(params, original.value().developParams);
    EXPECT_EQ(1U, original.value().persistedRevision);
    EXPECT_EQ(types::DevelopParams{}, replacement.value().developParams);
    EXPECT_EQ(0U, replacement.value().persistedRevision);
}

TEST(CatalogOrchestratorTest, RelinksMissingPhotoOnlyAfterContentMatch)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("original.jpg"));
    const QString relinkPath = QDir(directory.path()).filePath(QStringLiteral("moved.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("same-content-after-move")));
    CatalogOrchestrator orchestrator;
    std::vector<CatalogSourceUpdate> updates;
    QObject::connect(&orchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&](const CatalogSourceUpdate& update) {
        updates.push_back(update);
    });
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_TRUE(waitForUpdateCount(updates, 1));
    const types::PhotoId photoId = imported.value().photoIds.front();
    ASSERT_TRUE(QFile::rename(sourcePath, relinkPath));
    const CatalogPhotoStateResult missing = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(missing.hasValue());
    EXPECT_EQ(catalog::SourceBindingState::Missing, missing.value().photo.sourceState);
    EXPECT_FALSE(missing.value().sourceProcessingAllowed);

    const CatalogSourceSubmissionResult relinked = orchestrator.relinkSource(photoId, {relinkPath});
    ASSERT_TRUE(relinked.hasValue());
    ASSERT_TRUE(waitForUpdateCount(updates, 2));
    EXPECT_EQ(catalog::SourceBindingState::Available, updates[1].photo.sourceState);
    ASSERT_TRUE(updates[1].photo.source.has_value());
    EXPECT_EQ(QFileInfo(relinkPath).absoluteFilePath(), updates[1].photo.source->path);
    EXPECT_EQ(photoId.value, updates[1].photo.id.value);
}

TEST(CatalogOrchestratorTest, RejectsRelinkWhenLegacyIdentityHasNoHashBaseline)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("pending.jpg"));
    const QString relinkPath = QDir(directory.path()).filePath(QStringLiteral("unknown.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("pending-baseline")));
    CatalogOrchestrator orchestrator;
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_EQ(1, imported.value().fingerprintRequestIds.size());
    ASSERT_TRUE(orchestrator.cancelSourceRequest(imported.value().fingerprintRequestIds.front()));
    const types::PhotoId photoId = imported.value().photoIds.front();
    ASSERT_TRUE(QFile::rename(sourcePath, relinkPath));
    const CatalogPhotoStateResult missing = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(missing.hasValue());
    EXPECT_EQ(catalog::SourceBindingState::Missing, missing.value().photo.sourceState);

    const CatalogSourceSubmissionResult relinked = orchestrator.relinkSource(photoId, {relinkPath});

    ASSERT_TRUE(relinked.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, relinked.error().code);
}

TEST(CatalogOrchestratorTest, ClosingCatalogCancelsPendingFingerprintRequest)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("large.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    QFile source(sourcePath);
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_TRUE(source.resize(64 * 1024 * 1024));
    source.close();
    CatalogOrchestrator orchestrator;
    std::vector<types::RequestId> cancellations;
    QObject::connect(&orchestrator, &CatalogOrchestrator::sourceBindingCancelled, [&](types::RequestId requestId) {
        cancellations.push_back(requestId);
    });
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = orchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().fingerprintRequestIds.size());

    const CatalogSessionState closed = orchestrator.closeCatalog();

    EXPECT_FALSE(closed.isOpen);
    ASSERT_EQ(1U, cancellations.size());
    EXPECT_EQ(imported.value().fingerprintRequestIds.front(), cancellations.front());
    EXPECT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
}

}  // namespace
}  // namespace flexraw::core::orchestration
