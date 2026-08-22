#include "catalog_develop_repository.h"

#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

#include "catalog_preset_repository.h"
#include "source_binding.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: develop persistence 실패를 설명하는 CoreError 값 생성
// 입력: message: 호출자에게 전달할 database 또는 data 오류 설명
// 출력: DatabaseError로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeDatabaseError(QString message)
{
    return {types::ErrorCode::DatabaseError, std::move(message)};
}

// 목적: optimistic develop save 충돌을 일관된 CoreError로 생성
// 입력: message: caller baseline이 stale임을 설명하는 technical message
// 출력: Conflict로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeConflictError(QString message)
{
    return {types::ErrorCode::Conflict, std::move(message)};
}

// 목적: 저장 가능한 develop parameter 범위와 유한값 여부 확인
// 입력: params: 검증할 develop parameter 값
// 출력: 모든 parameter가 처리 가능한 범위면 true
[[nodiscard]] bool isValidDevelopParams(const types::DevelopParams& params)
{
    return std::isfinite(params.exposureEv) && std::isfinite(params.contrast) && std::isfinite(params.highlights) &&
           std::isfinite(params.shadows) && std::isfinite(params.whites) && std::isfinite(params.blacks) &&
           std::isfinite(params.saturation) && std::isfinite(params.vibrance) &&
           std::isfinite(params.whiteBalanceTemperatureKelvin) && std::isfinite(params.whiteBalanceTint) &&
           std::isfinite(params.clarity) && std::isfinite(params.dehaze) && std::isfinite(params.sharpeningAmount) &&
           std::isfinite(params.sharpeningRadius) && std::isfinite(params.sharpeningDetail) &&
           std::isfinite(params.sharpeningMasking) && std::isfinite(params.luminanceNoiseReduction) &&
           std::isfinite(params.colorNoiseReduction) && std::isfinite(params.toneCurveShadows) &&
           std::isfinite(params.toneCurveDarks) && std::isfinite(params.toneCurveLights) &&
           std::isfinite(params.toneCurveHighlights) && std::isfinite(params.pointCurveBlack) &&
           std::isfinite(params.pointCurveShadows) && std::isfinite(params.pointCurveMidtones) &&
           std::isfinite(params.pointCurveHighlights) && std::isfinite(params.pointCurveWhite) &&
           (params.whiteBalanceMode == types::WhiteBalanceMode::AsShot ||
            params.whiteBalanceMode == types::WhiteBalanceMode::Custom) &&
           params.contrast >= -1.0F && params.contrast <= 1.0F && params.highlights >= -1.0F &&
           params.highlights <= 1.0F && params.shadows >= -1.0F && params.shadows <= 1.0F && params.whites >= -1.0F &&
           params.whites <= 1.0F && params.blacks >= -1.0F && params.blacks <= 1.0F && params.saturation >= -1.0F &&
           params.saturation <= 1.0F && params.vibrance >= -1.0F && params.vibrance <= 1.0F &&
           params.whiteBalanceTemperatureKelvin >= 2000.0F && params.whiteBalanceTemperatureKelvin <= 50000.0F &&
           params.whiteBalanceTint >= -1.0F && params.whiteBalanceTint <= 1.0F && params.clarity >= -1.0F &&
           params.clarity <= 1.0F && params.sharpeningAmount >= -1.0F && params.sharpeningAmount <= 1.0F &&
           params.dehaze >= -1.0F && params.dehaze <= 1.0F && params.sharpeningRadius >= 1.0F &&
           params.sharpeningRadius <= 3.0F && params.sharpeningDetail >= -1.0F && params.sharpeningDetail <= 1.0F &&
           params.sharpeningMasking >= 0.0F && params.sharpeningMasking <= 1.0F &&
           params.luminanceNoiseReduction >= 0.0F && params.luminanceNoiseReduction <= 1.0F &&
           params.colorNoiseReduction >= 0.0F && params.colorNoiseReduction <= 1.0F &&
           params.toneCurveShadows >= -1.0F && params.toneCurveShadows <= 1.0F && params.toneCurveDarks >= -1.0F &&
           params.toneCurveDarks <= 1.0F && params.toneCurveLights >= -1.0F && params.toneCurveLights <= 1.0F &&
           params.toneCurveHighlights >= -1.0F && params.toneCurveHighlights <= 1.0F &&
           params.pointCurveBlack >= -1.0F && params.pointCurveBlack <= 1.0F && params.pointCurveShadows >= -1.0F &&
           params.pointCurveShadows <= 1.0F && params.pointCurveMidtones >= -1.0F &&
           params.pointCurveMidtones <= 1.0F && params.pointCurveHighlights >= -1.0F &&
           params.pointCurveHighlights <= 1.0F && params.pointCurveWhite >= -1.0F && params.pointCurveWhite <= 1.0F;
}

// 목적: DevelopParams를 catalog JSON column에 저장할 compact text로 직렬화
// 입력: params: 직렬화할 유효한 develop parameter 값
// 출력: 모든 parameter를 포함하는 UTF-8 JSON text
[[nodiscard]] QString serializeDevelopParams(const types::DevelopParams& params)
{
    const QJsonObject object{
        {QStringLiteral("exposureEv"), params.exposureEv},
        {QStringLiteral("contrast"), params.contrast},
        {QStringLiteral("highlights"), params.highlights},
        {QStringLiteral("shadows"), params.shadows},
        {QStringLiteral("whites"), params.whites},
        {QStringLiteral("blacks"), params.blacks},
        {QStringLiteral("saturation"), params.saturation},
        {QStringLiteral("vibrance"), params.vibrance},
        {QStringLiteral("whiteBalanceMode"),
         params.whiteBalanceMode == types::WhiteBalanceMode::AsShot ? QStringLiteral("asShot")
                                                                    : QStringLiteral("custom")},
        {QStringLiteral("whiteBalanceTemperatureKelvin"), params.whiteBalanceTemperatureKelvin},
        {QStringLiteral("whiteBalanceTint"), params.whiteBalanceTint},
        {QStringLiteral("clarity"), params.clarity},
        {QStringLiteral("dehaze"), params.dehaze},
        {QStringLiteral("sharpeningAmount"), params.sharpeningAmount},
        {QStringLiteral("sharpeningRadius"), params.sharpeningRadius},
        {QStringLiteral("sharpeningDetail"), params.sharpeningDetail},
        {QStringLiteral("sharpeningMasking"), params.sharpeningMasking},
        {QStringLiteral("luminanceNoiseReduction"), params.luminanceNoiseReduction},
        {QStringLiteral("colorNoiseReduction"), params.colorNoiseReduction},
        {QStringLiteral("toneCurveShadows"), params.toneCurveShadows},
        {QStringLiteral("toneCurveDarks"), params.toneCurveDarks},
        {QStringLiteral("toneCurveLights"), params.toneCurveLights},
        {QStringLiteral("toneCurveHighlights"), params.toneCurveHighlights},
        {QStringLiteral("pointCurveBlack"), params.pointCurveBlack},
        {QStringLiteral("pointCurveShadows"), params.pointCurveShadows},
        {QStringLiteral("pointCurveMidtones"), params.pointCurveMidtones},
        {QStringLiteral("pointCurveHighlights"), params.pointCurveHighlights},
        {QStringLiteral("pointCurveWhite"), params.pointCurveWhite},
    };
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// 목적: catalog JSON text를 검증된 DevelopParams 값으로 역직렬화
// 입력: json: database에서 읽은 parameter JSON text
// 출력: DevelopParams 또는 구조화된 database 오류
[[nodiscard]] types::Result<types::DevelopParams, types::CoreError> deserializeDevelopParams(const QString& json)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return types::Result<types::DevelopParams, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog develop parameter JSON is invalid.")));
    }

    const QJsonObject object = document.object();
    constexpr std::array<const char*, 8> ParameterNames{
        "exposureEv",
        "contrast",
        "highlights",
        "shadows",
        "whites",
        "blacks",
        "saturation",
        "vibrance",
    };

    for (const char* name : ParameterNames)
    {
        if (!object.value(QString::fromLatin1(name)).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog develop parameter JSON is incomplete.")));
        }
    }

    types::DevelopParams params;
    params.exposureEv = static_cast<float>(object.value(QStringLiteral("exposureEv")).toDouble());
    params.contrast = static_cast<float>(object.value(QStringLiteral("contrast")).toDouble());
    params.highlights = static_cast<float>(object.value(QStringLiteral("highlights")).toDouble());
    params.shadows = static_cast<float>(object.value(QStringLiteral("shadows")).toDouble());
    params.whites = static_cast<float>(object.value(QStringLiteral("whites")).toDouble());
    params.blacks = static_cast<float>(object.value(QStringLiteral("blacks")).toDouble());
    params.saturation = static_cast<float>(object.value(QStringLiteral("saturation")).toDouble());
    params.vibrance = static_cast<float>(object.value(QStringLiteral("vibrance")).toDouble());

    const bool hasWhiteBalanceValues = object.contains(QStringLiteral("whiteBalanceMode")) ||
                                       object.contains(QStringLiteral("whiteBalanceTemperatureKelvin")) ||
                                       object.contains(QStringLiteral("whiteBalanceTint"));
    if (hasWhiteBalanceValues)
    {
        const QString mode = object.value(QStringLiteral("whiteBalanceMode")).toString();
        if ((mode != QStringLiteral("asShot") && mode != QStringLiteral("custom")) ||
            !object.value(QStringLiteral("whiteBalanceTemperatureKelvin")).isDouble() ||
            !object.value(QStringLiteral("whiteBalanceTint")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog white balance parameter JSON is invalid.")));
        }

        params.whiteBalanceMode =
            mode == QStringLiteral("asShot") ? types::WhiteBalanceMode::AsShot : types::WhiteBalanceMode::Custom;
        params.whiteBalanceTemperatureKelvin =
            static_cast<float>(object.value(QStringLiteral("whiteBalanceTemperatureKelvin")).toDouble());
        params.whiteBalanceTint = static_cast<float>(object.value(QStringLiteral("whiteBalanceTint")).toDouble());
    }

    if (object.contains(QStringLiteral("clarity")))
    {
        if (!object.value(QStringLiteral("clarity")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog clarity parameter JSON is invalid.")));
        }

        params.clarity = static_cast<float>(object.value(QStringLiteral("clarity")).toDouble());
    }

    if (object.contains(QStringLiteral("dehaze")))
    {
        if (!object.value(QStringLiteral("dehaze")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog dehaze parameter JSON is invalid.")));
        }
        params.dehaze = static_cast<float>(object.value(QStringLiteral("dehaze")).toDouble());
    }

    if (object.contains(QStringLiteral("sharpeningAmount")))
    {
        if (!object.value(QStringLiteral("sharpeningAmount")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog sharpening parameter JSON is invalid.")));
        }
        params.sharpeningAmount = static_cast<float>(object.value(QStringLiteral("sharpeningAmount")).toDouble());
    }

    if (object.contains(QStringLiteral("sharpeningRadius")))
    {
        if (!object.value(QStringLiteral("sharpeningRadius")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog sharpening radius JSON is invalid.")));
        }
        params.sharpeningRadius = static_cast<float>(object.value(QStringLiteral("sharpeningRadius")).toDouble());
    }

    if (object.contains(QStringLiteral("sharpeningDetail")))
    {
        if (!object.value(QStringLiteral("sharpeningDetail")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog sharpening detail JSON is invalid.")));
        }
        params.sharpeningDetail = static_cast<float>(object.value(QStringLiteral("sharpeningDetail")).toDouble());
    }

    if (object.contains(QStringLiteral("sharpeningMasking")))
    {
        if (!object.value(QStringLiteral("sharpeningMasking")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog sharpening masking JSON is invalid.")));
        }
        params.sharpeningMasking = static_cast<float>(object.value(QStringLiteral("sharpeningMasking")).toDouble());
    }

    if (object.contains(QStringLiteral("luminanceNoiseReduction")))
    {
        if (!object.value(QStringLiteral("luminanceNoiseReduction")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog luminance noise reduction JSON is invalid.")));
        }
        params.luminanceNoiseReduction =
            static_cast<float>(object.value(QStringLiteral("luminanceNoiseReduction")).toDouble());
    }

    if (object.contains(QStringLiteral("colorNoiseReduction")))
    {
        if (!object.value(QStringLiteral("colorNoiseReduction")).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog color noise reduction JSON is invalid.")));
        }
        params.colorNoiseReduction = static_cast<float>(object.value(QStringLiteral("colorNoiseReduction")).toDouble());
    }

    constexpr std::array<const char*, 4> ToneCurveNames{
        "toneCurveShadows", "toneCurveDarks", "toneCurveLights", "toneCurveHighlights"};
    for (const char* name : ToneCurveNames)
    {
        if (object.contains(QString::fromLatin1(name)) && !object.value(QString::fromLatin1(name)).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog tone curve parameter JSON is invalid.")));
        }
    }
    params.toneCurveShadows = static_cast<float>(object.value(QStringLiteral("toneCurveShadows")).toDouble());
    params.toneCurveDarks = static_cast<float>(object.value(QStringLiteral("toneCurveDarks")).toDouble());
    params.toneCurveLights = static_cast<float>(object.value(QStringLiteral("toneCurveLights")).toDouble());
    params.toneCurveHighlights = static_cast<float>(object.value(QStringLiteral("toneCurveHighlights")).toDouble());

    constexpr std::array<const char*, 5> PointCurveNames{
        "pointCurveBlack", "pointCurveShadows", "pointCurveMidtones", "pointCurveHighlights", "pointCurveWhite"};
    for (const char* name : PointCurveNames)
    {
        if (object.contains(QString::fromLatin1(name)) && !object.value(QString::fromLatin1(name)).isDouble())
        {
            return types::Result<types::DevelopParams, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog point curve parameter JSON is invalid.")));
        }
    }
    params.pointCurveBlack = static_cast<float>(object.value(QStringLiteral("pointCurveBlack")).toDouble());
    params.pointCurveShadows = static_cast<float>(object.value(QStringLiteral("pointCurveShadows")).toDouble());
    params.pointCurveMidtones = static_cast<float>(object.value(QStringLiteral("pointCurveMidtones")).toDouble());
    params.pointCurveHighlights = static_cast<float>(object.value(QStringLiteral("pointCurveHighlights")).toDouble());
    params.pointCurveWhite = static_cast<float>(object.value(QStringLiteral("pointCurveWhite")).toDouble());

    if (!isValidDevelopParams(params))
    {
        return types::Result<types::DevelopParams, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog develop parameter values are invalid.")));
    }

    return types::Result<types::DevelopParams, types::CoreError>::success(params);
}

// 목적: source path compatibility API가 processing 가능한 photo만 resolve하도록 binding state 확인
// 입력: database: 조회할 SQLite connection, photoId: source path에서 찾은 identity
// 출력: processing 가능한 PhotoId 또는 invalid state·resolution conflict·database 오류
[[nodiscard]] types::Result<types::PhotoId, types::CoreError> requireProcessableSource(QSqlDatabase& database,
                                                                                       types::PhotoId photoId)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT source_binding_state FROM photos WHERE id = :photoId"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec() || !query.next())
    {
        return types::Result<types::PhotoId, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to read catalog source state: %1").arg(query.lastError().text())));
    }

    const int stateValue = query.value(0).toInt();
    if (!isSourceBindingState(stateValue))
    {
        return types::Result<types::PhotoId, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog source state is invalid.")));
    }
    if (!allowsSourceProcessing(static_cast<SourceBindingState>(stateValue)))
    {
        return types::Result<types::PhotoId, types::CoreError>::failure(
            {types::ErrorCode::Conflict, QStringLiteral("Catalog source requires resolution before processing.")});
    }

    return types::Result<types::PhotoId, types::CoreError>::success(photoId);
}

// 목적: photos.source_path에 binding된 processing 가능 catalog-local PhotoId 조회
// 입력: database: 조회할 열린 SQLite connection, sourcePath: 현재 source locator
// 출력: PhotoId 또는 NotFound·Conflict·DatabaseError
[[nodiscard]] types::Result<types::PhotoId, types::CoreError> findPhotoId(QSqlDatabase& database,
                                                                          const QString& sourcePath)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id FROM photos WHERE source_path = :sourcePath"));
    query.bindValue(QStringLiteral(":sourcePath"), sourcePath);

    if (!query.exec())
    {
        return types::Result<types::PhotoId, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to find catalog photo: %1").arg(query.lastError().text())));
    }

    if (!query.next())
    {
        return types::Result<types::PhotoId, types::CoreError>::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Catalog photo does not exist.")});
    }

    return requireProcessableSource(database, {query.value(0).toLongLong()});
}

// 목적: PhotoId가 develop state를 저장할 수 있는 catalog photo인지 확인
// 입력: database: 조회할 SQLite connection, photoId: 확인할 catalog-local identity
// 출력: 존재하는 photo면 성공 표식, 그 외 InvalidArgument·NotFound·DatabaseError
[[nodiscard]] types::Result<std::monostate, types::CoreError> requirePhoto(QSqlDatabase& database,
                                                                           types::PhotoId photoId)
{
    if (!types::isValidPhotoId(photoId))
    {
        return types::Result<std::monostate, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Photo ID must be positive.")});
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT 1 FROM photos WHERE id = :photoId"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec())
    {
        return types::Result<std::monostate, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to validate catalog photo: %1").arg(query.lastError().text())));
    }
    if (!query.next())
    {
        return types::Result<std::monostate, types::CoreError>::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Catalog photo does not exist.")});
    }

    return types::Result<std::monostate, types::CoreError>::success({});
}

}  // namespace

// 목적: 열린 catalog database에 연결된 develop state repository 생성
// 입력: database: develop state table을 보유한 열린 CatalogDatabase
// 출력: 초기화된 CatalogDevelopRepository 객체
CatalogDevelopRepository::CatalogDevelopRepository(CatalogDatabase& database) : m_database(database) {}

// 목적: 안정적 photo identity의 저장된 현재 develop parameter를 조회
// 입력: photoId: 조회할 catalog-local PhotoId
// 출력: 저장값 또는 아직 저장되지 않았으면 빈 값
CatalogDevelopParamsResult CatalogDevelopRepository::loadParams(types::PhotoId photoId) const
{
    const types::Result<std::monostate, types::CoreError> photo = requirePhoto(m_database.m_database, photoId);

    if (photo.hasError())
    {
        return CatalogDevelopParamsResult::failure(photo.error());
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("SELECT params_json FROM develop_params WHERE photo_id = :photoId"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);

    if (!query.exec())
    {
        return CatalogDevelopParamsResult::failure(makeDatabaseError(
            QStringLiteral("Unable to load catalog develop parameters: %1").arg(query.lastError().text())));
    }

    if (!query.next())
    {
        return CatalogDevelopParamsResult::success(std::nullopt);
    }

    const types::Result<types::DevelopParams, types::CoreError> params =
        deserializeDevelopParams(query.value(0).toString());

    if (params.hasError())
    {
        return CatalogDevelopParamsResult::failure(params.error());
    }

    return CatalogDevelopParamsResult::success(params.value());
}

// 목적: 안정적 photo identity의 저장된 develop parameter와 persisted revision 조회
// 입력: photoId: 조회할 catalog-local PhotoId
// 출력: 저장된 state 또는 아직 저장되지 않았으면 빈 값
CatalogDevelopStateResult CatalogDevelopRepository::loadState(types::PhotoId photoId) const
{
    const types::Result<std::monostate, types::CoreError> photo = requirePhoto(m_database.m_database, photoId);
    if (photo.hasError())
    {
        return CatalogDevelopStateResult::failure(photo.error());
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("SELECT params_json, revision FROM develop_params WHERE photo_id = :photoId"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec())
    {
        return CatalogDevelopStateResult::failure(makeDatabaseError(
            QStringLiteral("Unable to load catalog develop state: %1").arg(query.lastError().text())));
    }
    if (!query.next())
    {
        return CatalogDevelopStateResult::success(std::nullopt);
    }

    const types::Result<types::DevelopParams, types::CoreError> params =
        deserializeDevelopParams(query.value(0).toString());
    const qint64 revision = query.value(1).toLongLong();
    if (params.hasError())
    {
        return CatalogDevelopStateResult::failure(params.error());
    }
    if (revision <= 0)
    {
        return CatalogDevelopStateResult::failure(
            makeDatabaseError(QStringLiteral("Catalog develop revision is invalid.")));
    }

    return CatalogDevelopStateResult::success(
        CatalogDevelopState{params.value(), static_cast<types::DevelopRevision>(revision)});
}

// 목적: caller가 읽은 revision과 일치할 때만 develop state를 원자적으로 저장
// 입력: photoId: 저장할 PhotoId, params: 새 parameter, expectedRevision: caller의 persisted baseline revision
// 출력: 증가한 revision을 포함한 state 또는 revision conflict·database 오류
CatalogDevelopSaveResult CatalogDevelopRepository::saveState(types::PhotoId photoId,
                                                             const types::DevelopParams& params,
                                                             types::DevelopRevision expectedRevision)
{
    constexpr auto MaximumPersistedRevision = static_cast<types::DevelopRevision>(std::numeric_limits<qint64>::max());
    if (!isValidDevelopParams(params) || expectedRevision >= MaximumPersistedRevision)
    {
        return CatalogDevelopSaveResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Develop state or expected revision is invalid.")});
    }

    const types::Result<std::monostate, types::CoreError> photo = requirePhoto(m_database.m_database, photoId);
    if (photo.hasError())
    {
        return CatalogDevelopSaveResult::failure(photo.error());
    }

    QSqlQuery query(m_database.m_database);
    if (expectedRevision == 0)
    {
        query.prepare(QStringLiteral("INSERT INTO develop_params "
                                     "(photo_id, params_json, updated_at_ms, revision) "
                                     "VALUES (:photoId, :params, :updatedAtMs, 1) "
                                     "ON CONFLICT(photo_id) DO NOTHING"));
    }
    else
    {
        query.prepare(QStringLiteral("UPDATE develop_params SET params_json = :params, "
                                     "updated_at_ms = :updatedAtMs, revision = revision + 1 "
                                     "WHERE photo_id = :photoId AND revision = :expectedRevision"));
        query.bindValue(QStringLiteral(":expectedRevision"), static_cast<qint64>(expectedRevision));
    }
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    query.bindValue(QStringLiteral(":params"), serializeDevelopParams(params));
    query.bindValue(QStringLiteral(":updatedAtMs"), QDateTime::currentMSecsSinceEpoch());

    if (!query.exec())
    {
        return CatalogDevelopSaveResult::failure(makeDatabaseError(
            QStringLiteral("Unable to save catalog develop state: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogDevelopSaveResult::failure(
            makeConflictError(QStringLiteral("Catalog develop state changed after the caller loaded it.")));
    }

    return CatalogDevelopSaveResult::success({params, expectedRevision + 1});
}

// 목적: 안정적 photo identity의 현재 develop parameter를 추가 또는 갱신
// 입력: photoId: 저장할 catalog-local PhotoId, params: 저장할 develop parameter 값
// 출력: 성공 표식 또는 구조화된 오류
CatalogDevelopStoreResult CatalogDevelopRepository::saveParams(types::PhotoId photoId,
                                                               const types::DevelopParams& params)
{
    if (!isValidDevelopParams(params))
    {
        return CatalogDevelopStoreResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Develop parameters are invalid.")});
    }

    const types::Result<std::monostate, types::CoreError> photo = requirePhoto(m_database.m_database, photoId);

    if (photo.hasError())
    {
        return CatalogDevelopStoreResult::failure(photo.error());
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("INSERT INTO develop_params (photo_id, params_json, updated_at_ms, revision) "
                                 "VALUES (:photoId, :params, :updatedAtMs, 1) "
                                 "ON CONFLICT(photo_id) DO UPDATE SET "
                                 "params_json = excluded.params_json, updated_at_ms = excluded.updated_at_ms, "
                                 "revision = develop_params.revision + 1"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    query.bindValue(QStringLiteral(":params"), serializeDevelopParams(params));
    query.bindValue(QStringLiteral(":updatedAtMs"), QDateTime::currentMSecsSinceEpoch());

    if (!query.exec())
    {
        return CatalogDevelopStoreResult::failure(makeDatabaseError(
            QStringLiteral("Unable to save catalog develop parameters: %1").arg(query.lastError().text())));
    }

    return CatalogDevelopStoreResult::success({});
}

// 목적: 안정적 photo identity의 develop history 끝에 현재 parameter snapshot을 추가
// 입력: photoId: snapshot을 추가할 PhotoId, params: 저장할 develop parameter 값
// 출력: 사진별 단조 증가 step ID 또는 구조화된 오류
CatalogDevelopHistoryStoreResult CatalogDevelopRepository::appendHistory(types::PhotoId photoId,
                                                                         const types::DevelopParams& params)
{
    if (!isValidDevelopParams(params))
    {
        return CatalogDevelopHistoryStoreResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Develop parameters are invalid.")});
    }

    QSqlDatabase& database = m_database.m_database;

    if (!database.transaction())
    {
        return CatalogDevelopHistoryStoreResult::failure(
            makeDatabaseError(QStringLiteral("Unable to begin catalog develop history transaction: %1")
                                  .arg(database.lastError().text())));
    }

    const types::Result<std::monostate, types::CoreError> photo = requirePhoto(database, photoId);

    if (photo.hasError())
    {
        database.rollback();
        return CatalogDevelopHistoryStoreResult::failure(photo.error());
    }

    QSqlQuery nextStepQuery(database);
    nextStepQuery.prepare(
        QStringLiteral("SELECT COALESCE(MAX(step_id), 0) + 1 FROM develop_history WHERE photo_id = :photoId"));
    nextStepQuery.bindValue(QStringLiteral(":photoId"), photoId.value);

    if (!nextStepQuery.exec() || !nextStepQuery.next())
    {
        database.rollback();
        return CatalogDevelopHistoryStoreResult::failure(
            makeDatabaseError(QStringLiteral("Unable to determine catalog develop history step: %1")
                                  .arg(nextStepQuery.lastError().text())));
    }

    const qint64 stepId = nextStepQuery.value(0).toLongLong();
    QSqlQuery insertQuery(database);
    insertQuery.prepare(QStringLiteral("INSERT INTO develop_history (photo_id, step_id, params_json, created_at_ms) "
                                       "VALUES (:photoId, :stepId, :params, :createdAtMs)"));
    insertQuery.bindValue(QStringLiteral(":photoId"), photoId.value);
    insertQuery.bindValue(QStringLiteral(":stepId"), stepId);
    insertQuery.bindValue(QStringLiteral(":params"), serializeDevelopParams(params));
    insertQuery.bindValue(QStringLiteral(":createdAtMs"), QDateTime::currentMSecsSinceEpoch());

    if (!insertQuery.exec() || !database.commit())
    {
        database.rollback();
        return CatalogDevelopHistoryStoreResult::failure(makeDatabaseError(
            QStringLiteral("Unable to save catalog develop history: %1").arg(insertQuery.lastError().text())));
    }

    return CatalogDevelopHistoryStoreResult::success(stepId);
}

// 목적: 안정적 photo identity의 develop history snapshot을 생성 순서대로 조회
// 입력: photoId: 조회할 catalog-local PhotoId
// 출력: 오래된 순서의 history entry 목록 또는 구조화된 오류
CatalogDevelopHistoryResult CatalogDevelopRepository::listHistory(types::PhotoId photoId) const
{
    const types::Result<std::monostate, types::CoreError> photo = requirePhoto(m_database.m_database, photoId);

    if (photo.hasError())
    {
        return CatalogDevelopHistoryResult::failure(photo.error());
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("SELECT step_id, params_json, created_at_ms FROM develop_history "
                                 "WHERE photo_id = :photoId ORDER BY step_id"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);

    if (!query.exec())
    {
        return CatalogDevelopHistoryResult::failure(makeDatabaseError(
            QStringLiteral("Unable to list catalog develop history: %1").arg(query.lastError().text())));
    }

    QVector<DevelopHistoryEntry> entries;

    while (query.next())
    {
        const types::Result<types::DevelopParams, types::CoreError> params =
            deserializeDevelopParams(query.value(1).toString());

        if (params.hasError())
        {
            return CatalogDevelopHistoryResult::failure(params.error());
        }

        entries.push_back({query.value(0).toLongLong(), params.value(), query.value(2).toLongLong()});
    }

    return CatalogDevelopHistoryResult::success(std::move(entries));
}

// 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 develop parameter 조회
// 입력: sourcePath: photos.source_path에 binding된 현재 locator
// 출력: 저장값 또는 아직 저장되지 않았으면 빈 값
CatalogDevelopParamsResult CatalogDevelopRepository::loadParams(const QString& sourcePath) const
{
    const types::Result<types::PhotoId, types::CoreError> photoId = findPhotoId(m_database.m_database, sourcePath);
    return photoId.hasError() ? CatalogDevelopParamsResult::failure(photoId.error()) : loadParams(photoId.value());
}

// 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 develop parameter 저장
// 입력: sourcePath: photos.source_path locator, params: 저장할 develop parameter 값
// 출력: 성공 표식 또는 구조화된 오류
CatalogDevelopStoreResult CatalogDevelopRepository::saveParams(const QString& sourcePath,
                                                               const types::DevelopParams& params)
{
    const types::Result<types::PhotoId, types::CoreError> photoId = findPhotoId(m_database.m_database, sourcePath);
    return photoId.hasError() ? CatalogDevelopStoreResult::failure(photoId.error())
                              : saveParams(photoId.value(), params);
}

// 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 history snapshot 추가
// 입력: sourcePath: photos.source_path locator, params: 저장할 develop parameter 값
// 출력: 사진별 단조 증가 step ID 또는 구조화된 오류
CatalogDevelopHistoryStoreResult CatalogDevelopRepository::appendHistory(const QString& sourcePath,
                                                                         const types::DevelopParams& params)
{
    const types::Result<types::PhotoId, types::CoreError> photoId = findPhotoId(m_database.m_database, sourcePath);
    return photoId.hasError() ? CatalogDevelopHistoryStoreResult::failure(photoId.error())
                              : appendHistory(photoId.value(), params);
}

// 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 history 조회
// 입력: sourcePath: photos.source_path에 binding된 현재 locator
// 출력: 오래된 순서의 history entry 목록 또는 구조화된 오류
CatalogDevelopHistoryResult CatalogDevelopRepository::listHistory(const QString& sourcePath) const
{
    const types::Result<types::PhotoId, types::CoreError> photoId = findPhotoId(m_database.m_database, sourcePath);
    return photoId.hasError() ? CatalogDevelopHistoryResult::failure(photoId.error()) : listHistory(photoId.value());
}

// 목적: 저장 요청의 preset 이름과 category를 정규화하고 parameter를 검증
// 입력: definition: 저장할 사용자 preset 정의
// 출력: 정규화된 preset 정의 또는 입력 검증 오류
[[nodiscard]] types::Result<preset::PresetDefinition, types::CoreError> normalizePresetDefinition(
    const preset::PresetDefinition& definition)
{
    preset::PresetDefinition normalized{definition.name.trimmed(), definition.category.trimmed(), definition.params};

    if (normalized.name.isEmpty())
    {
        return types::Result<preset::PresetDefinition, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Preset name is empty.")});
    }

    if (!isValidDevelopParams(normalized.params))
    {
        return types::Result<preset::PresetDefinition, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Preset develop parameters are invalid.")});
    }

    return types::Result<preset::PresetDefinition, types::CoreError>::success(std::move(normalized));
}

// 목적: 현재 SQL query 행을 CatalogPreset 값으로 변환
// 입력: query: id, name, category, params_json, created_at_ms, updated_at_ms를 가진 현재 행
// 출력: 변환된 preset 또는 parameter JSON 오류
[[nodiscard]] CatalogPresetLoadResult readCatalogPreset(const QSqlQuery& query)
{
    const types::Result<types::DevelopParams, types::CoreError> params =
        deserializeDevelopParams(query.value(3).toString());

    if (params.hasError())
    {
        return CatalogPresetLoadResult::failure(params.error());
    }

    return CatalogPresetLoadResult::success(CatalogPreset{
        query.value(0).toLongLong(),
        preset::PresetDefinition{query.value(1).toString(), query.value(2).toString(), params.value()},
        query.value(4).toLongLong(),
        query.value(5).toLongLong(),
    });
}

// 목적: 열린 catalog database에 연결된 preset repository 생성
// 입력: database: presets table을 보유한 열린 CatalogDatabase
// 출력: 초기화된 CatalogPresetRepository 객체
CatalogPresetRepository::CatalogPresetRepository(CatalogDatabase& database) : m_database(database) {}

// 목적: 이름과 category 기준으로 preset을 추가 또는 갱신
// 입력: definition: 저장할 preset 이름, category, develop parameter
// 출력: 추가 또는 갱신된 preset ID 또는 구조화된 오류
CatalogPresetStoreResult CatalogPresetRepository::save(const preset::PresetDefinition& definition)
{
    const types::Result<preset::PresetDefinition, types::CoreError> normalized = normalizePresetDefinition(definition);

    if (normalized.hasError())
    {
        return CatalogPresetStoreResult::failure(normalized.error());
    }

    const qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery saveQuery(m_database.m_database);
    saveQuery.prepare(QStringLiteral("INSERT INTO presets (name, category, params_json, created_at_ms, updated_at_ms) "
                                     "VALUES (:name, :category, :params, :createdAtMs, :updatedAtMs) "
                                     "ON CONFLICT(category, name) DO UPDATE SET "
                                     "params_json = excluded.params_json, updated_at_ms = excluded.updated_at_ms"));
    saveQuery.bindValue(QStringLiteral(":name"), normalized.value().name);
    saveQuery.bindValue(QStringLiteral(":category"), normalized.value().category);
    saveQuery.bindValue(QStringLiteral(":params"), serializeDevelopParams(normalized.value().params));
    saveQuery.bindValue(QStringLiteral(":createdAtMs"), timestamp);
    saveQuery.bindValue(QStringLiteral(":updatedAtMs"), timestamp);

    if (!saveQuery.exec())
    {
        return CatalogPresetStoreResult::failure(
            makeDatabaseError(QStringLiteral("Unable to save catalog preset: %1").arg(saveQuery.lastError().text())));
    }

    QSqlQuery idQuery(m_database.m_database);
    idQuery.prepare(QStringLiteral("SELECT id FROM presets WHERE category = :category AND name = :name"));
    idQuery.bindValue(QStringLiteral(":category"), normalized.value().category);
    idQuery.bindValue(QStringLiteral(":name"), normalized.value().name);

    if (!idQuery.exec() || !idQuery.next())
    {
        return CatalogPresetStoreResult::failure(makeDatabaseError(
            QStringLiteral("Unable to read saved catalog preset: %1").arg(idQuery.lastError().text())));
    }

    return CatalogPresetStoreResult::success(idQuery.value(0).toLongLong());
}

// 목적: preset ID로 저장된 preset을 조회
// 입력: presetId: 조회할 presets.id 값
// 출력: 저장된 preset 또는 NotFound/DatabaseError
CatalogPresetLoadResult CatalogPresetRepository::load(const qint64 presetId) const
{
    if (presetId <= 0)
    {
        return CatalogPresetLoadResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Preset ID must be positive.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("SELECT id, name, category, params_json, created_at_ms, updated_at_ms "
                                 "FROM presets WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), presetId);

    if (!query.exec())
    {
        return CatalogPresetLoadResult::failure(
            makeDatabaseError(QStringLiteral("Unable to load catalog preset: %1").arg(query.lastError().text())));
    }

    if (!query.next())
    {
        return CatalogPresetLoadResult::failure({types::ErrorCode::NotFound, QStringLiteral("Preset does not exist.")});
    }

    return readCatalogPreset(query);
}

// 목적: 저장된 preset을 category와 name 순서로 조회
// 입력: 없음
// 출력: 정렬된 preset 목록 또는 구조화된 database 오류
CatalogPresetListResult CatalogPresetRepository::list() const
{
    QSqlQuery query(m_database.m_database);

    if (!query.exec(QStringLiteral("SELECT id, name, category, params_json, created_at_ms, updated_at_ms FROM presets "
                                   "ORDER BY category COLLATE NOCASE, name COLLATE NOCASE, id")))
    {
        return CatalogPresetListResult::failure(
            makeDatabaseError(QStringLiteral("Unable to list catalog presets: %1").arg(query.lastError().text())));
    }

    QVector<CatalogPreset> presets;

    while (query.next())
    {
        const CatalogPresetLoadResult preset = readCatalogPreset(query);

        if (preset.hasError())
        {
            return CatalogPresetListResult::failure(preset.error());
        }

        presets.push_back(preset.value());
    }

    return CatalogPresetListResult::success(std::move(presets));
}

// 목적: preset ID에 해당하는 사용자 preset 삭제
// 입력: presetId: 삭제할 presets.id 값
// 출력: 성공 표식 또는 NotFound/DatabaseError
CatalogPresetRemoveResult CatalogPresetRepository::remove(const qint64 presetId)
{
    if (presetId <= 0)
    {
        return CatalogPresetRemoveResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Preset ID must be positive.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("DELETE FROM presets WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), presetId);

    if (!query.exec())
    {
        return CatalogPresetRemoveResult::failure(
            makeDatabaseError(QStringLiteral("Unable to remove catalog preset: %1").arg(query.lastError().text())));
    }

    if (query.numRowsAffected() == 0)
    {
        return CatalogPresetRemoveResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Preset does not exist.")});
    }

    return CatalogPresetRemoveResult::success({});
}

}  // namespace flexraw::core::catalog
