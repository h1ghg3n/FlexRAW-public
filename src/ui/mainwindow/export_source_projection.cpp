#include "export_source_projection.h"

#include "source_binding.h"

namespace flexraw::ui::mainwindow
{

// 목적: 등록된 Catalog Photo를 format 중립적인 Export dialog source로 투영
// 입력: photo: Catalog record, editorState: current in-memory state, catalogPath: active Catalog
// 출력: 처리 가능한 source의 Catalog-backed projection 또는 빈 값
std::optional<export_::ExportDialogSource> makeCatalogExportSource(const core::catalog::CatalogPhotoRecord& photo,
                                                                   const core::orchestration::EditorState& editorState,
                                                                   const QString& catalogPath)
{
    if (!photo.source.has_value() || !core::catalog::allowsSourceProcessing(photo.sourceState))
    {
        return std::nullopt;
    }

    const core::types::FileDescriptor source{
        photo.source->path,
        photo.extension,
        photo.displayName,
        photo.kind,
    };
    const bool isCurrentPhoto = editorState.photo.photoId.value == photo.id.value;
    return export_::ExportDialogSource{
        source,
        catalogPath,
        isCurrentPhoto ? std::optional<core::types::DevelopParams>{editorState.params} : std::nullopt,
    };
}

// 목적: 아직 등록되지 않은 Folder entry를 format 중립적인 Export dialog source로 투영
// 입력: entry: Folder scan record, editorState: current in-memory state, catalogPath: active Catalog
// 출력: Ready source의 explicit current/default-Develop projection 또는 빈 값
std::optional<export_::ExportDialogSource> makeFolderExportSource(const core::catalog::CatalogEntry& entry,
                                                                  const core::orchestration::EditorState& editorState,
                                                                  const QString& catalogPath)
{
    if (entry.status != core::types::FileScanStatus::Ready)
    {
        return std::nullopt;
    }

    const bool isCurrentPhoto = entry.file.path == editorState.source.path;
    return export_::ExportDialogSource{
        entry.file,
        catalogPath,
        isCurrentPhoto ? std::optional<core::types::DevelopParams>{editorState.params} : std::nullopt,
        !isCurrentPhoto,
    };
}

}  // namespace flexraw::ui::mainwindow
