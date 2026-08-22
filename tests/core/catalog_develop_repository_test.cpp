#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
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

// 목적: develop repository test에서 사용할 저장 가능한 catalog 사진 항목 생성
// 입력: path: 사진 절대 경로
// 출력: Ready 상태의 RAW CatalogEntry 값
[[nodiscard]] CatalogEntry makeEntry(const QString& path)
{
    return {
        types::FileDescriptor{
            path,
            QFileInfo(path).suffix().toLower(),
            QFileInfo(path).fileName(),
            types::SupportedFileKind::Raw,
        },
        types::FileScanStatus::Ready,
    };
}

// 목적: develop repository test에 사용할 photo를 저장하고 안정적 identity 조회
// 입력: repository: photo storage, path: 등록할 source path
// 출력: 저장된 PhotoId 또는 실패 시 invalid PhotoId
[[nodiscard]] types::PhotoId storePhoto(CatalogPhotoRepository& repository, const QString& path)
{
    if (repository.upsert({makeEntry(path)}).hasError())
    {
        return {};
    }

    const CatalogPhotoRecordResult record = repository.findBySourcePath(path);
    return record.hasValue() && record.value().has_value() ? record.value()->id : types::PhotoId{};
}

class CatalogDevelopRepositoryTest : public testing::Test
{
protected:
    // 목적: CatalogDevelopRepository test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        ensureCoreApplication();
    }
};

TEST_F(CatalogDevelopRepositoryTest, StoresAndLoadsCurrentDevelopParams)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString photoPath = QStringLiteral("C:/photos/one.raw");
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photoRepository(*database.value());
    const types::PhotoId photoId = storePhoto(photoRepository, photoPath);
    ASSERT_TRUE(types::isValidPhotoId(photoId));
    CatalogDevelopRepository repository(*database.value());
    types::DevelopParams params;
    params.exposureEv = 1.25F;
    params.contrast = -0.3F;
    params.vibrance = 0.6F;
    params.whiteBalanceMode = types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 4800.0F;
    params.whiteBalanceTint = -0.2F;
    params.clarity = 0.4F;
    params.dehaze = 0.5F;
    params.sharpeningAmount = 0.3F;
    params.sharpeningRadius = 2.0F;
    params.sharpeningDetail = -0.4F;
    params.sharpeningMasking = 0.6F;
    params.luminanceNoiseReduction = 0.3F;
    params.colorNoiseReduction = 0.4F;
    params.toneCurveShadows = 0.2F;
    params.toneCurveDarks = -0.1F;
    params.toneCurveLights = 0.3F;
    params.toneCurveHighlights = -0.4F;
    params.pointCurveBlack = -0.2F;
    params.pointCurveShadows = 0.1F;
    params.pointCurveMidtones = 0.3F;
    params.pointCurveHighlights = -0.1F;
    params.pointCurveWhite = 0.2F;

    ASSERT_TRUE(repository.saveParams(photoId, params).hasValue());
    const CatalogDevelopParamsResult loaded = repository.loadParams(photoId);

    ASSERT_TRUE(loaded.hasValue());
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(*loaded.value(), params);
}

TEST_F(CatalogDevelopRepositoryTest, SavesDevelopStateWithOptimisticRevision)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photoRepository(*database.value());
    const types::PhotoId photoId = storePhoto(photoRepository, QStringLiteral("C:/photos/revision.raw"));
    ASSERT_TRUE(types::isValidPhotoId(photoId));
    CatalogDevelopRepository repository(*database.value());
    types::DevelopParams firstParams;
    firstParams.exposureEv = 0.5F;
    types::DevelopParams secondParams = firstParams;
    secondParams.contrast = 0.25F;

    const CatalogDevelopSaveResult firstSave = repository.saveState(photoId, firstParams, 0);
    ASSERT_TRUE(firstSave.hasValue());
    EXPECT_EQ(1U, firstSave.value().revision);
    const CatalogDevelopSaveResult secondSave = repository.saveState(photoId, secondParams, firstSave.value().revision);
    ASSERT_TRUE(secondSave.hasValue());
    EXPECT_EQ(2U, secondSave.value().revision);

    const CatalogDevelopStateResult loaded = repository.loadState(photoId);
    ASSERT_TRUE(loaded.hasValue());
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(secondParams, loaded.value()->params);
    EXPECT_EQ(2U, loaded.value()->revision);
}

TEST_F(CatalogDevelopRepositoryTest, RejectsStaleDevelopRevisionWithoutOverwritingState)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photoRepository(*database.value());
    const types::PhotoId photoId = storePhoto(photoRepository, QStringLiteral("C:/photos/conflict.raw"));
    ASSERT_TRUE(types::isValidPhotoId(photoId));
    CatalogDevelopRepository repository(*database.value());
    types::DevelopParams persistedParams;
    persistedParams.exposureEv = 0.75F;
    ASSERT_TRUE(repository.saveState(photoId, persistedParams, 0).hasValue());
    types::DevelopParams staleParams;
    staleParams.exposureEv = -0.75F;

    const CatalogDevelopSaveResult staleSave = repository.saveState(photoId, staleParams, 0);

    ASSERT_TRUE(staleSave.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, staleSave.error().code);
    const CatalogDevelopStateResult loaded = repository.loadState(photoId);
    ASSERT_TRUE(loaded.hasValue());
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(persistedParams, loaded.value()->params);
    EXPECT_EQ(1U, loaded.value()->revision);
}

TEST_F(CatalogDevelopRepositoryTest, LoadsLegacyDevelopParamsAsAsShotWhiteBalance)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString photoPath = QStringLiteral("C:/photos/legacy.raw");
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photoRepository(*database.value());
    const types::PhotoId photoId = storePhoto(photoRepository, photoPath);
    ASSERT_TRUE(types::isValidPhotoId(photoId));

    const QString connectionName = QStringLiteral("catalog_develop_legacy_test");
    {
        QSqlDatabase legacyConnection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        legacyConnection.setDatabaseName(catalogPath);
        ASSERT_TRUE(legacyConnection.open());
        QSqlQuery query(legacyConnection);
        query.prepare(QStringLiteral(
            "INSERT INTO develop_params (photo_id, params_json, updated_at_ms) VALUES (:photoId, :params, 0)"));
        query.bindValue(QStringLiteral(":photoId"), photoId.value);
        query.bindValue(
            QStringLiteral(":params"),
            QStringLiteral(
                R"({"exposureEv":1.0,"contrast":0.0,"highlights":0.0,"shadows":0.0,"whites":0.0,"blacks":0.0,"saturation":0.0,"vibrance":0.0})"));
        ASSERT_TRUE(query.exec());
        legacyConnection.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    CatalogDevelopRepository repository(*database.value());
    const CatalogDevelopParamsResult loaded = repository.loadParams(photoId);

    ASSERT_TRUE(loaded.hasValue());
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(types::WhiteBalanceMode::AsShot, loaded.value()->whiteBalanceMode);
    EXPECT_FLOAT_EQ(6500.0F, loaded.value()->whiteBalanceTemperatureKelvin);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->whiteBalanceTint);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->clarity);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->dehaze);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->sharpeningAmount);
    EXPECT_FLOAT_EQ(1.0F, loaded.value()->sharpeningRadius);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->sharpeningDetail);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->sharpeningMasking);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->luminanceNoiseReduction);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->colorNoiseReduction);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->toneCurveShadows);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->toneCurveDarks);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->toneCurveLights);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->toneCurveHighlights);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->pointCurveBlack);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->pointCurveShadows);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->pointCurveMidtones);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->pointCurveHighlights);
    EXPECT_FLOAT_EQ(0.0F, loaded.value()->pointCurveWhite);
}

TEST_F(CatalogDevelopRepositoryTest, AppendsAndListsPhotoHistoryInStepOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString photoPath = QStringLiteral("C:/photos/one.raw");
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photoRepository(*database.value());
    const types::PhotoId photoId = storePhoto(photoRepository, photoPath);
    ASSERT_TRUE(types::isValidPhotoId(photoId));
    CatalogDevelopRepository repository(*database.value());
    types::DevelopParams firstParams;
    firstParams.exposureEv = -0.5F;
    types::DevelopParams secondParams;
    secondParams.saturation = 0.7F;

    const CatalogDevelopHistoryStoreResult firstStep = repository.appendHistory(photoId, firstParams);
    const CatalogDevelopHistoryStoreResult secondStep = repository.appendHistory(photoId, secondParams);
    const CatalogDevelopHistoryResult history = repository.listHistory(photoId);

    ASSERT_TRUE(firstStep.hasValue());
    EXPECT_EQ(1, firstStep.value());
    ASSERT_TRUE(secondStep.hasValue());
    EXPECT_EQ(2, secondStep.value());
    ASSERT_TRUE(history.hasValue());
    ASSERT_EQ(2, history.value().size());
    EXPECT_EQ(1, history.value()[0].stepId);
    EXPECT_EQ(firstParams, history.value()[0].params);
    EXPECT_EQ(2, history.value()[1].stepId);
    EXPECT_EQ(secondParams, history.value()[1].params);
}

TEST_F(CatalogDevelopRepositoryTest, RejectsParamsForUnknownPhoto)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogDevelopRepository repository(*database.value());

    const CatalogDevelopStoreResult result = repository.saveParams(types::PhotoId{999}, {});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

TEST_F(CatalogDevelopRepositoryTest, KeepsIdentityLoadAvailableButBlocksPathProcessingDuringReplacement)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString photoPath = QStringLiteral("C:/photos/replaced.raw");
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photoRepository(*database.value());
    const types::PhotoId photoId = storePhoto(photoRepository, photoPath);
    ASSERT_TRUE(types::isValidPhotoId(photoId));
    CatalogDevelopRepository repository(*database.value());
    types::DevelopParams params;
    params.exposureEv = 1.0F;
    ASSERT_TRUE(repository.saveParams(photoId, params).hasValue());
    ASSERT_TRUE(photoRepository.recordSourceState(photoId, SourceBindingState::ReplacementDetected).hasValue());

    const CatalogDevelopParamsResult identityLoad = repository.loadParams(photoId);
    const CatalogDevelopParamsResult pathLoad = repository.loadParams(photoPath);

    ASSERT_TRUE(identityLoad.hasValue());
    ASSERT_TRUE(identityLoad.value().has_value());
    EXPECT_EQ(params, *identityLoad.value());
    ASSERT_TRUE(pathLoad.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, pathLoad.error().code);
}

}  // namespace
}  // namespace flexraw::core::catalog
