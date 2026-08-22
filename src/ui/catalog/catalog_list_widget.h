#pragma once

#include <optional>

#include <QListWidget>
#include <QVector>

#include "catalog_entry.h"

namespace flexraw::ui::catalog
{

class CatalogListWidget : public QListWidget
{
    Q_OBJECT

public:
    // 목적: catalog 목록 표시를 위한 widget 초기화
    // 입력: parent: Qt 부모 widget
    // 출력: 초기화된 CatalogListWidget 객체
    explicit CatalogListWidget(QWidget* parent = nullptr);

    // 목적: scan된 transient folder 항목을 등록 없이 목록에 표시
    // 입력: entries: 표시할 CatalogEntry 목록
    // 출력: 없음
    void setEntries(const QVector<core::catalog::CatalogEntry>& entries);

    // 목적: stable PhotoId를 포함한 catalog photo record를 목록에 표시하고 첫 항목 선택
    // 입력: photos: 열린 SQLite catalog에서 조회한 photo record 목록
    // 출력: 없음
    void setPhotos(const QVector<core::catalog::CatalogPhotoRecord>& photos);

    // 목적: 현재 multi-selection에 포함된 transient Folder 항목을 화면 row 순서로 반환
    // 입력: 없음
    // 출력: Catalog photo mode이면 빈 목록, 아니면 선택된 CatalogEntry 목록
    [[nodiscard]] QVector<core::catalog::CatalogEntry> selectedEntries() const;

    // 목적: 현재 multi-selection에 포함된 catalog-backed photo를 화면 row 순서로 반환
    // 입력: 없음
    // 출력: transient Folder mode이면 빈 목록, 아니면 선택된 CatalogPhotoRecord 목록
    [[nodiscard]] QVector<core::catalog::CatalogPhotoRecord> selectedPhotos() const;

    // 목적: adapter가 거부한 navigation을 stable PhotoId row로 signal 없이 복원
    // 입력: photoId: 다시 표시할 catalog-local identity, 유효하지 않으면 선택 해제
    // 출력: 없음
    void restorePhotoSelection(core::types::PhotoId photoId);

    // 목적: adapter가 거부한 transient navigation을 source path row로 signal 없이 복원
    // 입력: sourcePath: 다시 표시할 folder entry path, 일치하지 않으면 선택 해제
    // 출력: 없음
    void restoreEntrySelection(const QString& sourcePath);

    // 목적: source binding transition을 현재 catalog-backed list에 국소 반영
    // 입력: photo: 갱신된 기존 record, createdPhoto: optional 신규 identity, selectedPhotoId: 유지할 선택
    // 출력: transient folder list는 변경하지 않음
    void applyPhotoUpdate(const core::catalog::CatalogPhotoRecord& photo,
                          const std::optional<core::catalog::CatalogPhotoRecord>& createdPhoto,
                          core::types::PhotoId selectedPhotoId);

    // 목적: 현재 catalog 목록과 선택 상태 초기화
    // 입력: 없음
    // 출력: 없음
    void clearEntries();

signals:
    // 목적: 사용자가 목록에서 catalog 항목을 선택했음을 전달
    // 입력: entry: 선택된 CatalogEntry 값
    // 출력: 없음
    void entrySelected(const core::catalog::CatalogEntry& entry);

    // 목적: 사용자가 catalog-backed photo를 선택했음을 전달
    // 입력: photo: stable PhotoId와 source binding을 포함한 선택 record
    // 출력: 없음
    void photoSelected(const core::catalog::CatalogPhotoRecord& photo);

private:
    // 목적: 현재 row 변경을 CatalogEntry 선택 signal로 변환
    // 입력: row: 새로 선택된 목록 row
    // 출력: 없음
    void handleCurrentRowChanged(int row);

    QVector<core::catalog::CatalogEntry> m_entries;
    QVector<core::catalog::CatalogPhotoRecord> m_photos;
    bool m_showingCatalogPhotos{false};
};

}  // namespace flexraw::ui::catalog
