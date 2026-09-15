#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QListWidgetItem>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>

#include "catalog_list_widget.h"

namespace flexraw::ui::catalog
{
namespace
{

constexpr QSize CatalogThumbnailSize{96, 72};
constexpr int CatalogThumbnailRowHeight = 80;

// 목적: QString을 길이를 보존한 Qt-free UTF-8 string으로 변환
// 입력: value: source/display metadata text
// 출력: 같은 Unicode text의 UTF-8 byte string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// 목적: transient thumbnail identity에 사용할 absolute lexical source locator 생성
// 입력: sourcePath: scanner 또는 Catalog record의 local path
// 출력: separator와 dot segment를 정리한 UTF-8 absolute locator, 실패하면 빈 문자열
[[nodiscard]] std::string normalizedSourceLocator(const QString& sourcePath)
{
    if (sourcePath.isEmpty() || sourcePath != sourcePath.trimmed())
    {
        return {};
    }
    const QString absolute = QFileInfo(QDir::fromNativeSeparators(sourcePath)).absoluteFilePath();
    const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(absolute));
    return normalized.isEmpty() || normalized == QStringLiteral(".") ? std::string{} : toClientString(normalized);
}

// 목적: internal file kind를 Qt-free Catalog thumbnail kind로 변환
// 입력: kind: scanner/Catalog record file 분류
// 출력: 같은 의미의 client enum
[[nodiscard]] core::client::CatalogFileKind toClientFileKind(core::types::SupportedFileKind kind) noexcept
{
    switch (kind)
    {
    case core::types::SupportedFileKind::Raw:
        return core::client::CatalogFileKind::Raw;
    case core::types::SupportedFileKind::RasterImage:
        return core::client::CatalogFileKind::RasterImage;
    case core::types::SupportedFileKind::Unknown:
        return core::client::CatalogFileKind::Unknown;
    }
    return core::client::CatalogFileKind::Unknown;
}

// 목적: tagged identity를 widget window/terminal set의 deterministic QString key로 변환
// 입력: identity: stable PhotoId 또는 normalized transient locator
// 출력: kind prefix를 포함한 presentation key
[[nodiscard]] QString thumbnailIdentityKey(const core::client::CatalogThumbnailItemIdentity& identity)
{
    if (identity.kind == core::client::CatalogThumbnailIdentityKind::CatalogPhoto)
    {
        return QStringLiteral("photo:%1").arg(identity.photoId.value);
    }
    return QStringLiteral("source:") +
           QString::fromUtf8(identity.transientSourceLocator.data(),
                             static_cast<qsizetype>(identity.transientSourceLocator.size()));
}

}  // namespace

// 목적: catalog 목록 표시를 위한 widget 초기화
// 입력: parent: Qt 부모 widget
// 출력: 초기화된 CatalogListWidget 객체
CatalogListWidget::CatalogListWidget(QWidget* parent) : QListWidget(parent)
{
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformItemSizes(true);
    setIconSize(CatalogThumbnailSize);
    m_thumbnailRefreshTimer = new QTimer(this);
    m_thumbnailRefreshTimer->setSingleShot(true);

    connect(this, &QListWidget::currentRowChanged, this, &CatalogListWidget::handleCurrentRowChanged);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { scheduleThumbnailWindowRefresh(); });
    connect(m_thumbnailRefreshTimer, &QTimer::timeout, this, &CatalogListWidget::refreshThumbnailWindow);
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
        auto* item = new QListWidgetItem(entry.file.displayName, this);
        item->setSizeHint(QSize{0, CatalogThumbnailRowHeight});
    }
    scheduleThumbnailWindowRefresh();
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
        auto* item = new QListWidgetItem(photo.displayName, this);
        item->setSizeHint(QSize{0, CatalogThumbnailRowHeight});
    }

    if (!m_photos.isEmpty())
    {
        setCurrentRow(0);
    }
    scheduleThumbnailWindowRefresh();
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
            item(row)->setIcon({});
            m_thumbnailTerminalKeys.remove(
                thumbnailIdentityKey({core::client::CatalogThumbnailIdentityKind::CatalogPhoto, {photo.id.value}, {}}));
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
                item(row)->setIcon({});
                m_thumbnailTerminalKeys.remove(thumbnailIdentityKey(
                    {core::client::CatalogThumbnailIdentityKind::CatalogPhoto, {createdPhoto->id.value}, {}}));
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed)
        {
            m_photos.push_back(*createdPhoto);
            auto* createdItem = new QListWidgetItem(createdPhoto->displayName, this);
            createdItem->setSizeHint(QSize{0, CatalogThumbnailRowHeight});
        }
    }

    scheduleThumbnailWindowRefresh();
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

// 목적: current generation과 tagged identity에 해당하는 decoded image 적용
// 입력: generation: accepted window identity, identity: Catalog PhotoId 또는 transient locator, image: Qt frame
// 출력: item이 여전히 adjacent window에 있을 때만 row icon 갱신
void CatalogListWidget::applyThumbnail(core::client::CatalogThumbnailWindowGeneration generation,
                                       const core::client::CatalogThumbnailItemIdentity& identity,
                                       const QImage& image)
{
    const QString key = thumbnailIdentityKey(identity);
    if (!m_thumbnailWindowGeneration.has_value() || generation != *m_thumbnailWindowGeneration ||
        !m_thumbnailWindowKeys.contains(key) || image.isNull())
    {
        return;
    }

    for (int row = 0; row < count(); ++row)
    {
        const std::optional<core::client::CatalogThumbnailItem> thumbnail = thumbnailItemAt(row);
        if (thumbnail.has_value() && thumbnail->identity == identity)
        {
            item(row)->setIcon(QPixmap::fromImage(image));
            m_thumbnailTerminalKeys.insert(key);
            return;
        }
    }
}

// 목적: current generation item의 terminal thumbnail 실패 기록
// 입력: generation: accepted window identity, identity: 재요청 반복을 막을 tagged item identity
// 출력: source가 window를 벗어나기 전까지 같은 decode 요청 생략
void CatalogListWidget::markThumbnailFailed(core::client::CatalogThumbnailWindowGeneration generation,
                                            const core::client::CatalogThumbnailItemIdentity& identity)
{
    const QString key = thumbnailIdentityKey(identity);
    if (m_thumbnailWindowGeneration.has_value() && generation == *m_thumbnailWindowGeneration &&
        m_thumbnailWindowKeys.contains(key))
    {
        m_thumbnailTerminalKeys.insert(key);
    }
}

// 목적: owner가 accepted한 generation을 이후 frame/issue filtering 기준으로 설정
// 입력: generation: replace command receipt의 nonzero generation
// 출력: 이전 generation event가 widget에 적용되지 않음
void CatalogListWidget::acceptThumbnailWindow(core::client::CatalogThumbnailWindowGeneration generation)
{
    m_thumbnailWindowGeneration = generation.value == 0
                                      ? std::nullopt
                                      : std::optional<core::client::CatalogThumbnailWindowGeneration>{generation};
}

// 목적: rejected/cleared thumbnail command 뒤 event 적용 기준 제거
// 입력: 없음
// 출력: 새 generation accept 전까지 frame/issue가 적용되지 않음
void CatalogListWidget::clearThumbnailWindowGeneration()
{
    m_thumbnailWindowGeneration.reset();
}

// 목적: 현재 catalog 목록과 선택 상태 초기화
// 입력: 없음
// 출력: 없음
void CatalogListWidget::clearEntries()
{
    m_thumbnailRefreshTimer->stop();
    m_thumbnailWindowKeys.clear();
    m_thumbnailTerminalKeys.clear();
    m_thumbnailWindowGeneration.reset();
    emit thumbnailWindowCleared();
    clear();
    m_entries.clear();
    m_photos.clear();
    m_showingCatalogPhotos = false;
}

// 목적: viewport 크기 변경을 adjacent thumbnail window 재계산으로 변환
// 입력: event: Qt resize event
// 출력: base resize 처리 후 coalesced window refresh 예약
void CatalogListWidget::resizeEvent(QResizeEvent* event)
{
    QListWidget::resizeEvent(event);
    scheduleThumbnailWindowRefresh();
}

// 목적: 연속 scroll/resize event를 한 번의 thumbnail window 갱신으로 coalesce
// 입력: 없음
// 출력: 다음 event-loop turn에 refreshThumbnailWindow 실행 예약
void CatalogListWidget::scheduleThumbnailWindowRefresh()
{
    m_thumbnailRefreshTimer->start(0);
}

// 목적: visible row와 앞뒤 한 viewport만 thumbnail materialization 대상으로 교체
// 입력: 없음
// 출력: 범위 밖 QPixmap 해제 및 아직 terminal 결과가 없는 source 요청
void CatalogListWidget::refreshThumbnailWindow()
{
    if (count() <= 0 || viewport()->height() <= 0)
    {
        m_thumbnailWindowKeys.clear();
        m_thumbnailWindowGeneration.reset();
        emit thumbnailWindowCleared();
        return;
    }

    QModelIndex firstIndex = indexAt(QPoint{1, 1});
    QModelIndex lastIndex = indexAt(QPoint{1, std::max(1, viewport()->height() - 2)});
    const int firstVisibleRow = firstIndex.isValid() ? firstIndex.row() : 0;
    const int lastVisibleRow = lastIndex.isValid() ? lastIndex.row() : count() - 1;
    const int visibleRowCount = std::max(1, lastVisibleRow - firstVisibleRow + 1);
    const int firstWindowRow = std::max(0, firstVisibleRow - visibleRowCount);
    const int lastWindowRow = std::min(count() - 1, lastVisibleRow + visibleRowCount);

    QSet<QString> nextWindowKeys;
    std::vector<core::client::CatalogThumbnailItem> requestedItems;
    for (int row = firstWindowRow; row <= lastWindowRow; ++row)
    {
        const std::optional<core::client::CatalogThumbnailItem> thumbnail = thumbnailItemAt(row);
        if (!thumbnail.has_value())
        {
            continue;
        }
        const QString key = thumbnailIdentityKey(thumbnail->identity);
        nextWindowKeys.insert(key);
        if (item(row)->icon().isNull() && !m_thumbnailTerminalKeys.contains(key))
        {
            requestedItems.push_back(*thumbnail);
        }
    }

    for (int row = 0; row < count(); ++row)
    {
        const std::optional<core::client::CatalogThumbnailItem> thumbnail = thumbnailItemAt(row);
        if (thumbnail.has_value() && !nextWindowKeys.contains(thumbnailIdentityKey(thumbnail->identity)))
        {
            item(row)->setIcon({});
            m_thumbnailTerminalKeys.remove(thumbnailIdentityKey(thumbnail->identity));
        }
    }
    m_thumbnailWindowKeys = std::move(nextWindowKeys);
    if (requestedItems.empty())
    {
        m_thumbnailWindowGeneration.reset();
        emit thumbnailWindowCleared();
        return;
    }
    emit thumbnailWindowChanged({std::move(requestedItems),
                                 {static_cast<std::uint32_t>(CatalogThumbnailSize.width()),
                                  static_cast<std::uint32_t>(CatalogThumbnailSize.height())}});
}

// 목적: row를 Catalog PhotoId 또는 transient normalized locator thumbnail item으로 투영
// 입력: row: 현재 list row index
// 출력: source가 processing 가능하면 Qt-free item, 아니면 빈 값
std::optional<core::client::CatalogThumbnailItem> CatalogListWidget::thumbnailItemAt(int row) const
{
    if (row < 0)
    {
        return std::nullopt;
    }
    if (m_showingCatalogPhotos && row < m_photos.size() && m_photos.at(row).source.has_value())
    {
        const core::catalog::CatalogPhotoRecord& photo = m_photos.at(row);
        const std::string locator = normalizedSourceLocator(photo.source->path);
        if (locator.empty() || photo.id.value <= 0)
        {
            return std::nullopt;
        }
        return core::client::CatalogThumbnailItem{
            {core::client::CatalogThumbnailIdentityKind::CatalogPhoto, {photo.id.value}, {}},
            locator,
            toClientString(photo.extension),
            toClientString(photo.displayName),
            toClientFileKind(photo.kind)};
    }
    if (!m_showingCatalogPhotos && row < m_entries.size())
    {
        const core::types::FileDescriptor& file = m_entries.at(row).file;
        const std::string locator = normalizedSourceLocator(file.path);
        if (locator.empty())
        {
            return std::nullopt;
        }
        return core::client::CatalogThumbnailItem{
            {core::client::CatalogThumbnailIdentityKind::TransientSource, {}, locator},
            locator,
            toClientString(file.extension),
            toClientString(file.displayName),
            toClientFileKind(file.kind)};
    }
    return std::nullopt;
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
