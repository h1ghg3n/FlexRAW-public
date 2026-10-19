#pragma once

#include <optional>

#include <QString>

#include "catalog_entry.h"
#include "editor_contracts.h"
#include "export_dialog.h"

namespace flexraw::ui::mainwindow
{

// 목적: 등록된 Catalog Photo를 format 중립적인 Export dialog source로 투영
// 입력: photo: Catalog record, editorState: current in-memory state, catalogPath: active Catalog
// 출력: 처리 가능한 source의 Catalog-backed projection 또는 빈 값
[[nodiscard]] std::optional<export_::ExportDialogSource> makeCatalogExportSource(
    const core::catalog::CatalogPhotoRecord& photo,
    const core::orchestration::EditorState& editorState,
    const QString& catalogPath);

// 목적: 아직 등록되지 않은 Folder entry를 format 중립적인 Export dialog source로 투영
// 입력: entry: Folder scan record, editorState: current in-memory state, catalogPath: active Catalog
// 출력: Ready source의 explicit current/default-Develop projection 또는 빈 값
[[nodiscard]] std::optional<export_::ExportDialogSource> makeFolderExportSource(
    const core::catalog::CatalogEntry& entry,
    const core::orchestration::EditorState& editorState,
    const QString& catalogPath);

}  // namespace flexraw::ui::mainwindow
