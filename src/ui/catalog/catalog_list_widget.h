#pragma once

#include <optional>

#include <QImage>
#include <QListWidget>
#include <QSet>
#include <QSize>
#include <QVector>

#include "catalog_entry.h"
#include "catalog_thumbnail_client.h"

class QResizeEvent;
class QTimer;

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

    // 목적: current generation과 tagged identity에 해당하는 decoded image 적용
    // 입력: generation: accepted window identity, identity: Catalog PhotoId 또는 transient locator, image: Qt frame
    // 출력: item이 여전히 adjacent window에 있을 때만 row icon 갱신
    void applyThumbnail(core::client::CatalogThumbnailWindowGeneration generation,
                        const core::client::CatalogThumbnailItemIdentity& identity,
                        const QImage& image);

    // 목적: current generation item의 terminal thumbnail 실패 기록
    // 입력: generation: accepted window identity, identity: 재요청 반복을 막을 tagged item identity
    // 출력: source가 window를 벗어나기 전까지 같은 decode 요청 생략
    void markThumbnailFailed(core::client::CatalogThumbnailWindowGeneration generation,
                             const core::client::CatalogThumbnailItemIdentity& identity);

    // 목적: owner가 accepted한 generation을 이후 frame/issue filtering 기준으로 설정
    // 입력: generation: replace command receipt의 nonzero generation
    // 출력: 이전 generation event가 widget에 적용되지 않음
    void acceptThumbnailWindow(core::client::CatalogThumbnailWindowGeneration generation);

    // 목적: rejected/cleared thumbnail command 뒤 event 적용 기준 제거
    // 입력: 없음
    // 출력: 새 generation accept 전까지 frame/issue가 적용되지 않음
    void clearThumbnailWindowGeneration();

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

    // 목적: viewport와 앞뒤 인접 범위에 필요한 Qt-free bounded thumbnail window 전달
    // 입력: command: tagged identity, normalized locator와 target extent
    // 출력: 없음
    void thumbnailWindowChanged(const core::client::ReplaceCatalogThumbnailWindowCommand& command);

    // 목적: 표시할 thumbnail item이 없을 때 active owner window 정리 요청
    // 입력: 없음
    // 출력: 없음
    void thumbnailWindowCleared();

protected:
    // 목적: viewport 크기 변경을 adjacent thumbnail window 재계산으로 변환
    // 입력: event: Qt resize event
    // 출력: base resize 처리 후 coalesced window refresh 예약
    void resizeEvent(QResizeEvent* event) override;

private:
    // 목적: 현재 row 변경을 CatalogEntry 선택 signal로 변환
    // 입력: row: 새로 선택된 목록 row
    // 출력: 없음
    void handleCurrentRowChanged(int row);

    // 목적: 연속 scroll/resize event를 한 번의 thumbnail window 갱신으로 coalesce
    // 입력: 없음
    // 출력: 다음 event-loop turn에 refreshThumbnailWindow 실행 예약
    void scheduleThumbnailWindowRefresh();

    // 목적: visible row와 앞뒤 한 viewport만 thumbnail materialization 대상으로 교체
    // 입력: 없음
    // 출력: 범위 밖 QPixmap 해제 및 아직 terminal 결과가 없는 source 요청
    void refreshThumbnailWindow();

    // 목적: row를 Catalog PhotoId 또는 transient normalized locator thumbnail item으로 투영
    // 입력: row: 현재 list row index
    // 출력: source가 processing 가능하면 Qt-free item, 아니면 빈 값
    [[nodiscard]] std::optional<core::client::CatalogThumbnailItem> thumbnailItemAt(int row) const;

    QVector<core::catalog::CatalogEntry> m_entries;
    QVector<core::catalog::CatalogPhotoRecord> m_photos;
    QSet<QString> m_thumbnailWindowKeys;
    QSet<QString> m_thumbnailTerminalKeys;
    std::optional<core::client::CatalogThumbnailWindowGeneration> m_thumbnailWindowGeneration;
    QTimer* m_thumbnailRefreshTimer{nullptr};
    bool m_showingCatalogPhotos{false};
};

}  // namespace flexraw::ui::catalog

Q_DECLARE_METATYPE(flexraw::core::client::ReplaceCatalogThumbnailWindowCommand)
