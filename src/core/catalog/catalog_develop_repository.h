#pragma once

#include <optional>
#include <variant>

#include <QVector>

#include "catalog_database.h"
#include "develop_params.h"
#include "operation_types.h"
#include "photo_identity.h"

namespace flexraw::core::catalog
{

struct DevelopHistoryEntry
{
    qint64 stepId{0};
    types::DevelopParams params;
    qint64 createdAtMs{0};
};

struct CatalogDevelopState
{
    types::DevelopParams params;
    types::DevelopRevision revision{0};
};

using CatalogDevelopParamsResult = types::Result<std::optional<types::DevelopParams>, types::CoreError>;
using CatalogDevelopStateResult = types::Result<std::optional<CatalogDevelopState>, types::CoreError>;
using CatalogDevelopSaveResult = types::Result<CatalogDevelopState, types::CoreError>;
using CatalogDevelopStoreResult = types::Result<std::monostate, types::CoreError>;
using CatalogDevelopHistoryStoreResult = types::Result<qint64, types::CoreError>;
using CatalogDevelopHistoryResult = types::Result<QVector<DevelopHistoryEntry>, types::CoreError>;

class CatalogDevelopRepository final
{
public:
    // 목적: 열린 catalog database에 연결된 develop state repository 생성
    // 입력: database: develop state table을 보유한 열린 CatalogDatabase
    // 출력: 초기화된 CatalogDevelopRepository 객체
    explicit CatalogDevelopRepository(CatalogDatabase& database);

    // 목적: 안정적 photo identity의 저장된 현재 develop parameter를 조회
    // 입력: photoId: 조회할 catalog-local PhotoId
    // 출력: 저장값 또는 아직 저장되지 않았으면 빈 값
    [[nodiscard]] CatalogDevelopParamsResult loadParams(types::PhotoId photoId) const;

    // 목적: 안정적 photo identity의 저장된 develop parameter와 persisted revision 조회
    // 입력: photoId: 조회할 catalog-local PhotoId
    // 출력: 저장된 state 또는 아직 저장되지 않았으면 빈 값
    [[nodiscard]] CatalogDevelopStateResult loadState(types::PhotoId photoId) const;

    // 목적: caller가 읽은 revision과 일치할 때만 develop state를 원자적으로 저장
    // 입력: photoId: 저장할 PhotoId, params: 새 parameter, expectedRevision: caller의 persisted baseline revision
    // 출력: 증가한 revision을 포함한 state 또는 revision conflict·database 오류
    [[nodiscard]] CatalogDevelopSaveResult saveState(types::PhotoId photoId,
                                                     const types::DevelopParams& params,
                                                     types::DevelopRevision expectedRevision);

    // 목적: 안정적 photo identity의 현재 develop parameter를 추가 또는 갱신
    // 입력: photoId: 저장할 catalog-local PhotoId, params: 저장할 develop parameter 값
    // 출력: 성공 표식 또는 구조화된 오류
    [[nodiscard]] CatalogDevelopStoreResult saveParams(types::PhotoId photoId, const types::DevelopParams& params);

    // 목적: 안정적 photo identity의 develop history 끝에 현재 parameter snapshot을 추가
    // 입력: photoId: snapshot을 추가할 PhotoId, params: 저장할 develop parameter 값
    // 출력: 사진별 단조 증가 step ID 또는 구조화된 오류
    [[nodiscard]] CatalogDevelopHistoryStoreResult appendHistory(types::PhotoId photoId,
                                                                 const types::DevelopParams& params);

    // 목적: 안정적 photo identity의 develop history snapshot을 생성 순서대로 조회
    // 입력: photoId: 조회할 catalog-local PhotoId
    // 출력: 오래된 순서의 history entry 목록 또는 구조화된 오류
    [[nodiscard]] CatalogDevelopHistoryResult listHistory(types::PhotoId photoId) const;

    // 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 develop parameter 조회
    // 입력: sourcePath: photos.source_path에 binding된 현재 locator
    // 출력: 저장값 또는 아직 저장되지 않았으면 빈 값
    [[nodiscard]] CatalogDevelopParamsResult loadParams(const QString& sourcePath) const;

    // 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 develop parameter 저장
    // 입력: sourcePath: photos.source_path locator, params: 저장할 develop parameter 값
    // 출력: 성공 표식 또는 구조화된 오류
    [[nodiscard]] CatalogDevelopStoreResult saveParams(const QString& sourcePath, const types::DevelopParams& params);

    // 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 history snapshot 추가
    // 입력: sourcePath: photos.source_path locator, params: 저장할 develop parameter 값
    // 출력: 사진별 단조 증가 step ID 또는 구조화된 오류
    [[nodiscard]] CatalogDevelopHistoryStoreResult appendHistory(const QString& sourcePath,
                                                                 const types::DevelopParams& params);

    // 목적: CatalogOrchestrator migration 전 source path를 PhotoId로 resolve해 history 조회
    // 입력: sourcePath: photos.source_path에 binding된 현재 locator
    // 출력: 오래된 순서의 history entry 목록 또는 구조화된 오류
    [[nodiscard]] CatalogDevelopHistoryResult listHistory(const QString& sourcePath) const;

private:
    CatalogDatabase& m_database;
};

}  // namespace flexraw::core::catalog
