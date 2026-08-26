#include <QApplication>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QListWidgetItem>
#include <QScrollBar>

#include <gtest/gtest.h>

#include "catalog_list_widget.h"

namespace flexraw::ui::catalog
{
namespace
{

// 목적: CatalogListWidget test용 stable PhotoId record 생성
// 입력: id: catalog-local identity, displayName: 목록 표시 이름
// 출력: processing 가능한 최소 CatalogPhotoRecord
[[nodiscard]] core::catalog::CatalogPhotoRecord makePhoto(qint64 id, const QString& displayName)
{
    return {
        core::types::PhotoId{id},
        core::types::SourceLocator{QStringLiteral("D:/Photos/") + displayName},
        QStringLiteral("D:/Photos/") + displayName,
        QStringLiteral("jpg"),
        displayName,
        core::types::SupportedFileKind::RasterImage,
        core::types::FileScanStatus::Ready,
        {},
        core::catalog::SourceBindingState::Available,
    };
}

TEST(CatalogListWidgetTest, EmitsStablePhotoRecordSelection)
{
    CatalogListWidget widget;
    core::types::PhotoId selectedPhotoId;
    QObject::connect(&widget, &CatalogListWidget::photoSelected, &widget, [&selectedPhotoId](const auto& photo) {
        selectedPhotoId = photo.id;
    });

    widget.setPhotos({makePhoto(17, QStringLiteral("first.jpg")), makePhoto(23, QStringLiteral("second.jpg"))});

    ASSERT_EQ(2, widget.count());
    EXPECT_EQ(QStringLiteral("first.jpg"), widget.item(0)->text());
    EXPECT_EQ(17, selectedPhotoId.value);

    widget.setCurrentRow(1);

    EXPECT_EQ(23, selectedPhotoId.value);
}

TEST(CatalogListWidgetTest, SupportsRangeAndToggleSelection)
{
    CatalogListWidget widget;
    widget.setPhotos({makePhoto(11, QStringLiteral("first.jpg")),
                      makePhoto(13, QStringLiteral("second.jpg")),
                      makePhoto(17, QStringLiteral("third.jpg")),
                      makePhoto(19, QStringLiteral("fourth.jpg"))});

    ASSERT_EQ(QAbstractItemView::ExtendedSelection, widget.selectionMode());
    QItemSelectionModel* selectionModel = widget.selectionModel();
    ASSERT_NE(nullptr, selectionModel);
    const QModelIndex first = widget.model()->index(0, 0);
    const QModelIndex third = widget.model()->index(2, 0);
    selectionModel->select(QItemSelection(first, third), QItemSelectionModel::ClearAndSelect);

    EXPECT_EQ(3, selectionModel->selectedRows().size());

    const QModelIndex second = widget.model()->index(1, 0);
    selectionModel->select(second, QItemSelectionModel::Toggle);
    ASSERT_EQ(2, selectionModel->selectedRows().size());
    EXPECT_TRUE(selectionModel->isRowSelected(0, QModelIndex{}));
    EXPECT_FALSE(selectionModel->isRowSelected(1, QModelIndex{}));
    EXPECT_TRUE(selectionModel->isRowSelected(2, QModelIndex{}));
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = widget.selectedPhotos();
    ASSERT_EQ(2, selectedPhotos.size());
    EXPECT_EQ(11, selectedPhotos.at(0).id.value);
    EXPECT_EQ(17, selectedPhotos.at(1).id.value);
}

TEST(CatalogListWidgetTest, ReplacesTransientEntriesWhenCatalogPhotosAreLoaded)
{
    CatalogListWidget widget;
    int transientSelectionCount = 0;
    int catalogSelectionCount = 0;
    QObject::connect(&widget, &CatalogListWidget::entrySelected, &widget, [&transientSelectionCount](const auto&) {
        ++transientSelectionCount;
    });
    QObject::connect(&widget, &CatalogListWidget::photoSelected, &widget, [&catalogSelectionCount](const auto&) {
        ++catalogSelectionCount;
    });
    const core::catalog::CatalogEntry transientEntry{
        {QStringLiteral("D:/Photos/transient.jpg"),
         QStringLiteral("jpg"),
         QStringLiteral("transient.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    widget.setEntries({transientEntry});
    widget.setPhotos({makePhoto(31, QStringLiteral("catalog.jpg"))});

    EXPECT_EQ(0, transientSelectionCount);
    EXPECT_EQ(1, catalogSelectionCount);
    ASSERT_EQ(1, widget.count());
    EXPECT_EQ(QStringLiteral("catalog.jpg"), widget.item(0)->text());
}

TEST(CatalogListWidgetTest, ActivatesTransientEntryOnlyAfterExplicitSelection)
{
    CatalogListWidget widget;
    int transientSelectionCount = 0;
    QObject::connect(&widget, &CatalogListWidget::entrySelected, &widget, [&transientSelectionCount](const auto&) {
        ++transientSelectionCount;
    });
    const core::catalog::CatalogEntry entry{
        {QStringLiteral("D:/Photos/transient.jpg"),
         QStringLiteral("jpg"),
         QStringLiteral("transient.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    widget.setEntries({entry});

    EXPECT_EQ(-1, widget.currentRow());
    EXPECT_EQ(0, transientSelectionCount);
    widget.setCurrentRow(0);
    EXPECT_EQ(1, transientSelectionCount);
}

TEST(CatalogListWidgetTest, RestoresPhotoSelectionWithoutPublishingAnotherRequest)
{
    CatalogListWidget widget;
    int catalogSelectionCount = 0;
    QObject::connect(&widget, &CatalogListWidget::photoSelected, &widget, [&catalogSelectionCount](const auto&) {
        ++catalogSelectionCount;
    });
    widget.setPhotos({makePhoto(41, QStringLiteral("first.jpg")), makePhoto(43, QStringLiteral("second.jpg"))});
    widget.setCurrentRow(1);

    widget.restorePhotoSelection(core::types::PhotoId{41});

    EXPECT_EQ(0, widget.currentRow());
    EXPECT_EQ(2, catalogSelectionCount);
}

TEST(CatalogListWidgetTest, RestoresTransientSelectionWithoutPublishingAnotherRequest)
{
    CatalogListWidget widget;
    int transientSelectionCount = 0;
    QObject::connect(&widget, &CatalogListWidget::entrySelected, &widget, [&transientSelectionCount](const auto&) {
        ++transientSelectionCount;
    });
    const core::catalog::CatalogEntry first{
        {QStringLiteral("D:/Photos/first.jpg"),
         QStringLiteral("jpg"),
         QStringLiteral("first.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };
    core::catalog::CatalogEntry second = first;
    second.file.path = QStringLiteral("D:/Photos/second.jpg");
    second.file.displayName = QStringLiteral("second.jpg");
    widget.setEntries({first, second});
    widget.setCurrentRow(1);

    widget.restoreEntrySelection(first.file.path);

    EXPECT_EQ(0, widget.currentRow());
    EXPECT_EQ(1, transientSelectionCount);
}

TEST(CatalogListWidgetTest, AppliesSourceUpdateAndSelectsCreatedIdentityWithoutSignal)
{
    CatalogListWidget widget;
    int catalogSelectionCount = 0;
    QObject::connect(&widget, &CatalogListWidget::photoSelected, &widget, [&catalogSelectionCount](const auto&) {
        ++catalogSelectionCount;
    });
    core::catalog::CatalogPhotoRecord updated = makePhoto(51, QStringLiteral("old.jpg"));
    core::catalog::CatalogPhotoRecord created = makePhoto(59, QStringLiteral("replacement.jpg"));
    widget.setPhotos({updated});
    updated.source.reset();
    updated.sourceState = core::catalog::SourceBindingState::Unlinked;

    widget.applyPhotoUpdate(updated, created, created.id);

    ASSERT_EQ(2, widget.count());
    EXPECT_EQ(1, widget.currentRow());
    EXPECT_EQ(QStringLiteral("replacement.jpg"), widget.item(1)->text());
    EXPECT_EQ(1, catalogSelectionCount);
}

TEST(CatalogListWidgetTest, MaterializesOnlyVisibleAndAdjacentThumbnailRows)
{
    CatalogListWidget widget;
    widget.resize(280, 240);
    widget.show();
    QVector<QVector<core::types::FileDescriptor>> requestedWindows;
    QObject::connect(&widget,
                     &CatalogListWidget::thumbnailWindowChanged,
                     &widget,
                     [&requestedWindows](const QVector<core::types::FileDescriptor>& sources, const QSize&) {
                         if (!sources.isEmpty())
                         {
                             requestedWindows.push_back(sources);
                         }
                     });
    QVector<core::catalog::CatalogPhotoRecord> photos;
    for (int index = 0; index < 20; ++index)
    {
        photos.push_back(makePhoto(index + 1, QStringLiteral("photo-%1.jpg").arg(index)));
    }

    widget.setPhotos(photos);
    QApplication::processEvents();
    QApplication::processEvents();

    ASSERT_FALSE(requestedWindows.isEmpty());
    const QVector<core::types::FileDescriptor> firstWindow = requestedWindows.back();
    EXPECT_GT(firstWindow.size(), 0);
    EXPECT_LT(firstWindow.size(), photos.size());
    QImage thumbnail(2, 1, QImage::Format_RGB32);
    thumbnail.fill(Qt::blue);
    widget.applyThumbnail(firstWindow.front().path, thumbnail);
    EXPECT_FALSE(widget.item(0)->icon().isNull());

    widget.verticalScrollBar()->setValue(widget.verticalScrollBar()->maximum());
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_TRUE(widget.item(0)->icon().isNull());
    ASSERT_GE(requestedWindows.size(), 2);
    EXPECT_LT(requestedWindows.back().size(), photos.size());
    EXPECT_NE(firstWindow.front().path, requestedWindows.back().front().path);
}

}  // namespace
}  // namespace flexraw::ui::catalog
