#pragma once

#include <optional>
#include <variant>

#include <QVector>

#include "catalog_database.h"
#include "preset.h"

namespace flexraw::core::catalog
{

struct CatalogPreset
{
    qint64 id{0};
    preset::PresetDefinition definition;
    qint64 createdAtMs{0};
    qint64 updatedAtMs{0};
};

using CatalogPresetStoreResult = types::Result<qint64, types::CoreError>;
using CatalogPresetLoadResult = types::Result<CatalogPreset, types::CoreError>;
using CatalogPresetListResult = types::Result<QVector<CatalogPreset>, types::CoreError>;
using CatalogPresetRemoveResult = types::Result<std::monostate, types::CoreError>;

class CatalogPresetRepository final
{
public:
    // 목적: 열린 catalog database에 연결된 preset repository 생성
    // 입력: database: presets table을 보유한 열린 CatalogDatabase
    // 출력: 초기화된 CatalogPresetRepository 객체
    explicit CatalogPresetRepository(CatalogDatabase& database);

    // 목적: 이름과 category 기준으로 preset을 추가 또는 갱신
    // 입력: definition: 저장할 preset 이름, category, develop parameter
    // 출력: 추가 또는 갱신된 preset ID 또는 구조화된 오류
    [[nodiscard]] CatalogPresetStoreResult save(const preset::PresetDefinition& definition);

    // 목적: preset ID로 저장된 preset을 조회
    // 입력: presetId: 조회할 presets.id 값
    // 출력: 저장된 preset 또는 NotFound/DatabaseError
    [[nodiscard]] CatalogPresetLoadResult load(qint64 presetId) const;

    // 목적: 저장된 preset을 category와 name 순서로 조회
    // 입력: 없음
    // 출력: 정렬된 preset 목록 또는 구조화된 database 오류
    [[nodiscard]] CatalogPresetListResult list() const;

    // 목적: preset ID에 해당하는 사용자 preset 삭제
    // 입력: presetId: 삭제할 presets.id 값
    // 출력: 성공 표식 또는 NotFound/DatabaseError
    [[nodiscard]] CatalogPresetRemoveResult remove(qint64 presetId);

private:
    CatalogDatabase& m_database;
};

}  // namespace flexraw::core::catalog
