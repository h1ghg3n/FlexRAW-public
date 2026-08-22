#pragma once

#include <optional>
#include <variant>

#include <QVector>

#include "catalog_database.h"
#include "catalog_entry.h"
#include "catalog_photo_page.h"
#include "result.h"

namespace flexraw::core::catalog
{

using CatalogPhotoStoreResult = types::Result<int, types::CoreError>;
using CatalogPhotoQueryResult = types::Result<CatalogPhotoPage, types::CoreError>;
using CatalogPhotoRecordResult = types::Result<std::optional<CatalogPhotoRecord>, types::CoreError>;
using CatalogPhotoMutationResult = types::Result<std::monostate, types::CoreError>;
using CatalogPhotoCreateResult = types::Result<types::PhotoId, types::CoreError>;

class CatalogPhotoRepository final
{
public:
    // 목적: 열린 catalog database에 연결된 사진 repository 생성
    // 입력: database: photos table을 보유한 열린 CatalogDatabase
    // 출력: 초기화된 CatalogPhotoRepository 객체
    explicit CatalogPhotoRepository(CatalogDatabase& database);

    // 목적: folder scan 항목을 새 PhotoId로 binding하거나 기존 locator의 표시 metadata 갱신
    // 입력: entries: 저장할 CatalogEntry 목록
    // 출력: 기존 fingerprint를 변경하지 않고 처리한 항목 수 또는 database 오류
    [[nodiscard]] CatalogPhotoStoreResult upsert(const QVector<CatalogEntry>& entries);

    // 목적: optional exact-folder scope와 stable cursor로 bounded catalog photo page 조회
    // 입력: request: page 크기, 이동 방향, optional exclusive cursor와 folder scope
    // 출력: 오름차순 record와 이전·다음 cursor 또는 validation·database 오류
    [[nodiscard]] CatalogPhotoQueryResult queryPage(const CatalogPhotoPageRequest& request) const;

    // 목적: catalog-local PhotoId로 photo record 조회
    // 입력: photoId: 조회할 안정적 photo identity
    // 출력: 일치하는 record 또는 없으면 빈 값
    [[nodiscard]] CatalogPhotoRecordResult findById(types::PhotoId photoId) const;

    // 목적: 현재 source locator path에 binding된 photo record 조회
    // 입력: sourcePath: catalog에 저장된 현재 source path
    // 출력: path에 binding된 record 또는 없으면 빈 값
    [[nodiscard]] CatalogPhotoRecordResult findBySourcePath(const QString& sourcePath) const;

    // 목적: identity 해결 전에 관찰된 source 상태만 기록
    // 입력: photoId: 대상 identity, state: 해결되지 않은 관찰 상태
    // 출력: fingerprint와 develop data를 변경하지 않은 성공 표식 또는 오류
    [[nodiscard]] CatalogPhotoMutationResult recordSourceState(types::PhotoId photoId, SourceBindingState state);

    // 목적: import 직후 pending source의 baseline SHA-256를 최초 확정
    // 입력: photoId: 대상 identity, fingerprint: metadata와 완전한 SHA-256 baseline
    // 출력: 성공 표식 또는 stale state·입력·database 오류
    [[nodiscard]] CatalogPhotoMutationResult establishSourceFingerprint(types::PhotoId photoId,
                                                                        const types::SourceFingerprint& fingerprint);

    // 목적: metadata는 변경됐지만 SHA-256가 일치하는 source를 동일 photo로 재확인
    // 입력: photoId: 대상 identity, observed: 현재 metadata와 baseline과 일치하는 SHA-256
    // 출력: baseline hash를 변경하지 않은 성공 표식 또는 hash·state conflict 오류
    [[nodiscard]] CatalogPhotoMutationResult confirmSourceMatch(types::PhotoId photoId,
                                                                const types::SourceFingerprint& observed);

    // 목적: 사용자가 교체된 source에 기존 보정값 적용을 선택한 결과 반영
    // 입력: photoId: 유지할 identity, replacement: 새로 수용할 source fingerprint
    // 출력: PhotoId와 develop data를 유지한 성공 표식 또는 오류
    [[nodiscard]] CatalogPhotoMutationResult acceptReplacement(types::PhotoId photoId,
                                                               const types::SourceFingerprint& replacement);

    // 목적: 교체된 현재 file을 새 PhotoId로 등록하고 기존 photo는 unlinked로 보존
    // 입력: replacedPhotoId: 기존 identity, replacement: 현재 file 정보, fingerprint: 새 source baseline
    // 출력: develop data가 없는 새 PhotoId 또는 transaction 오류
    [[nodiscard]] CatalogPhotoCreateResult registerReplacementAsNew(types::PhotoId replacedPhotoId,
                                                                    const CatalogEntry& replacement,
                                                                    const types::SourceFingerprint& fingerprint);

    // 목적: 기존 PhotoId를 검증된 다른 source locator에 재연결
    // 입력: photoId: 유지할 identity, locator: 새 path, fingerprint: 동일 source로 검증된 fingerprint
    // 출력: develop data를 유지한 relink 성공 표식 또는 오류
    [[nodiscard]] CatalogPhotoMutationResult relinkSource(types::PhotoId photoId,
                                                          const types::SourceLocator& locator,
                                                          const types::SourceFingerprint& fingerprint);

private:
    CatalogDatabase& m_database;
};

}  // namespace flexraw::core::catalog
