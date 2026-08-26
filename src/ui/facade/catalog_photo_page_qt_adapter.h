#pragma once

#include <vector>

#include <QVector>

#include "catalog_entry.h"
#include "catalog_photo_client.h"

namespace flexraw::ui::facade
{

// 목적: Qt-free photo snapshot을 기존 Qt Catalog presentation record로 변환
// 입력: snapshot: identity/source/display/fingerprint를 보유한 client state
// 출력: 같은 의미의 CatalogPhotoRecord
[[nodiscard]] core::catalog::CatalogPhotoRecord toCatalogPhotoRecord(
    const core::client::CatalogPhotoSnapshot& snapshot);

// 목적: Qt-free bounded photo page rows를 Catalog widget 입력으로 변환
// 입력: snapshots: stable order client photo snapshot 목록
// 출력: 순서를 보존한 Qt record vector
[[nodiscard]] QVector<core::catalog::CatalogPhotoRecord> toCatalogPhotoRecords(
    const std::vector<core::client::CatalogPhotoSnapshot>& snapshots);

}  // namespace flexraw::ui::facade
