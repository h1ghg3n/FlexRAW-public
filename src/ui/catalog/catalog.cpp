#include <algorithm>

#include <QItemSelectionModel>
#include <QListWidgetItem>
#include <QSignalBlocker>

#include "catalog_list_widget.h"

namespace flexraw::ui::catalog
{

// 목적: catalog 목록 표시를 위한 widget 초기화
// 입력: parent: Qt 부모 widget
// 출력: 초기화된 CatalogListWidget 객체
CatalogListWidget::CatalogListWidget(QWidget* parent) : QListWidget(parent)
{
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformItemSizes(true);

    connect(this, &QListWidget::currentRowChanged, this, &CatalogListWidget::handleCurrentRowChanged);
}

// 목적: scan된 transient folder 항목을 등록 없이 목록에 표시
// 입력: entries: 표시할 CatalogEntry 목록
// 출력: 없음
void CatalogListWidget::setEntries(const QVector<core::catalog::CatalogEntry>& entries)
{
    clearEntries();
    m_entries = entries;
    m_showingCatalogPhotos = false;

    for (const core::catalog::CatalogEntry& entry : m_entries)
    {
        addItem(entry.file.displayName);
    }
}

// 목적: stable PhotoId를 포함한 catalog photo record를 목록에 표시하고 첫 항목 선택
// 입력: photos: 열린 SQLite catalog에서 조회한 photo record 목록
// 출력: 없음
void CatalogListWidget::setPhotos(const QVector<core::catalog::CatalogPhotoRecord>& photos)
{
    clearEntries();
    m_photos = photos;
    m_showingCatalogPhotos = true;

    for (const core::catalog::CatalogPhotoRecord& photo : m_photos)
    {
        addItem(photo.displayName);
    }

    if (!m_photos.isEmpty())
    {
        setCurrentRow(0);
    }
}

// 목적: 현재 multi-selection에 포함된 transient Folder 항목을 화면 row 순서로 반환
// 입력: 없음
// 출력: Catalog photo mode이면 빈 목록, 아니면 선택된 CatalogEntry 목록
QVector<core::catalog::CatalogEntry> CatalogListWidget::selectedEntries() const
{
    QVector<core::catalog::CatalogEntry> entries;
    if (m_showingCatalogPhotos || selectionModel() == nullptr)
    {
        return entries;
    }

    QModelIndexList rows = selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& left, const QModelIndex& right) {
        return left.row() < right.row();
    });
    entries.reserve(rows.size());
    for (const QModelIndex& index : rows)
    {
        if (index.row() >= 0 && index.row() < m_entries.size())
        {
            entries.push_back(m_entries.at(index.row()));
        }
    }
    return entries;
}

// 목적: 현재 multi-selection에 포함된 catalog-backed photo를 화면 row 순서로 반환
// 입력: 없음
// 출력: transient Folder mode이면 빈 목록, 아니면 선택된 CatalogPhotoRecord 목록
QVector<core::catalog::CatalogPhotoRecord> CatalogListWidget::selectedPhotos() const
{
    QVector<core::catalog::CatalogPhotoRecord> photos;
    if (!m_showingCatalogPhotos || selectionModel() == nullptr)
    {
        return photos;
    }

    QModelIndexList rows = selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& left, const QModelIndex& right) {
        return left.row() < right.row();
    });
    photos.reserve(rows.size());
    for (const QModelIndex& index : rows)
    {
        if (index.row() >= 0 && index.row() < m_photos.size())
        {
            photos.push_back(m_photos.at(index.row()));
        }
    }
    return photos;
}

// 목적: adapter가 거부한 navigation을 stable PhotoId row로 signal 없이 복원
// 입력: photoId: 다시 표시할 catalog-local identity, 유효하지 않으면 선택 해제
// 출력: 없음
void CatalogListWidget::restorePhotoSelection(core::types::PhotoId photoId)
{
    const QSignalBlocker blocker(this);
    for (qsizetype row = 0; row < m_photos.size(); ++row)
    {
        if (m_photos.at(row).id.value == photoId.value)
        {
            setCurrentRow(row);
            return;
        }
    }

    setCurrentRow(-1);
}

// 목적: adapter가 거부한 transient navigation을 source path row로 signal 없이 복원
// 입력: sourcePath: 다시 표시할 folder entry path, 일치하지 않으면 선택 해제
// 출력: 없음
void CatalogListWidget::restoreEntrySelection(const QString& sourcePath)
{
    const QSignalBlocker blocker(this);
    for (qsizetype row = 0; row < m_entries.size(); ++row)
    {
        if (m_entries.at(row).file.path == sourcePath)
        {
            setCurrentRow(row);
            return;
        }
    }

    setCurrentRow(-1);
}

// 목적: source binding transition을 현재 catalog-backed list에 국소 반영
// 입력: photo: 갱신된 기존 record, createdPhoto: optional 신규 identity, selectedPhotoId: 유지할 선택
// 출력: transient folder list는 변경하지 않음
void CatalogListWidget::applyPhotoUpdate(const core::catalog::CatalogPhotoRecord& photo,
                                         const std::optional<core::catalog::CatalogPhotoRecord>& createdPhoto,
                                         core::types::PhotoId selectedPhotoId)
{
    if (!m_showingCatalogPhotos)
    {
        return;
    }

    const QSignalBlocker blocker(this);
    for (qsizetype row = 0; row < m_photos.size(); ++row)
    {
        if (m_photos.at(row).id.value == photo.id.value)
        {
            m_photos[row] = photo;
            item(row)->setText(photo.displayName);
            break;
        }
    }

    if (createdPhoto.has_value())
    {
        bool alreadyListed = false;
        for (qsizetype row = 0; row < m_photos.size(); ++row)
        {
            if (m_photos.at(row).id.value == createdPhoto->id.value)
            {
                m_photos[row] = *createdPhoto;
                item(row)->setText(createdPhoto->displayName);
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed)
        {
            m_photos.push_back(*createdPhoto);
            addItem(createdPhoto->displayName);
        }
    }

    for (qsizetype row = 0; row < m_photos.size(); ++row)
    {
        if (m_photos.at(row).id.value == selectedPhotoId.value)
        {
            setCurrentRow(row);
            return;
        }
    }
    setCurrentRow(-1);
}

// 목적: 현재 catalog 목록과 선택 상태 초기화
// 입력: 없음
// 출력: 없음
void CatalogListWidget::clearEntries()
{
    clear();
    m_entries.clear();
    m_photos.clear();
    m_showingCatalogPhotos = false;
}

// 목적: 현재 row 변경을 CatalogEntry 선택 signal로 변환
// 입력: row: 새로 선택된 목록 row
// 출력: 없음
void CatalogListWidget::handleCurrentRowChanged(int row)
{
    if (row < 0)
    {
        return;
    }

    if (row < m_photos.size())
    {
        emit photoSelected(m_photos.at(row));
        return;
    }

    if (row < m_entries.size())
    {
        emit entrySelected(m_entries.at(row));
    }
}

}  // namespace flexraw::ui::catalog
