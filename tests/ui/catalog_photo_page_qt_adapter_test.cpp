#include <cstdint>

#include <gtest/gtest.h>

#include "catalog_photo_page_qt_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

TEST(CatalogPhotoPageQtAdapterTest, PreservesUtf8SourceIdentityAndFingerprintState)
{
    core::client::CatalogPhotoSnapshot snapshot;
    snapshot.id = {41};
    snapshot.sourcePath = "D:/사진/선택.raw";
    snapshot.lastKnownPath = "D:/사진/선택.raw";
    snapshot.extension = "raw";
    snapshot.displayName = "선택.raw";
    snapshot.kind = core::client::CatalogFileKind::Raw;
    snapshot.scanStatus = core::client::CatalogScanStatus::Ready;
    snapshot.fingerprint = {1234, 5678, {0, 127, 128, 255}};
    snapshot.sourceState = core::client::CatalogSourceState::ReplacementDetected;

    const core::catalog::CatalogPhotoRecord photo = toCatalogPhotoRecord(snapshot);

    EXPECT_EQ(41, photo.id.value);
    ASSERT_TRUE(photo.source.has_value());
    EXPECT_EQ(QStringLiteral("D:/사진/선택.raw"), photo.source->path);
    EXPECT_EQ(QStringLiteral("선택.raw"), photo.displayName);
    EXPECT_EQ(core::types::SupportedFileKind::Raw, photo.kind);
    EXPECT_EQ(core::types::FileScanStatus::Ready, photo.scanStatus);
    EXPECT_EQ(1234, photo.fingerprint.sizeBytes);
    EXPECT_EQ(5678, photo.fingerprint.modifiedAtMs);
    ASSERT_EQ(4, photo.fingerprint.sha256.size());
    EXPECT_EQ(128, static_cast<std::uint8_t>(photo.fingerprint.sha256[2]));
    EXPECT_EQ(255, static_cast<std::uint8_t>(photo.fingerprint.sha256[3]));
    EXPECT_EQ(core::catalog::SourceBindingState::ReplacementDetected, photo.sourceState);
}

TEST(CatalogPhotoPageQtAdapterTest, PreservesStableOrderAndAbsentSource)
{
    core::client::CatalogPhotoSnapshot first;
    first.id = {11};
    first.displayName = "first.jpg";
    core::client::CatalogPhotoSnapshot second;
    second.id = {17};
    second.displayName = "second.jpg";

    const QVector<core::catalog::CatalogPhotoRecord> photos = toCatalogPhotoRecords({first, second});

    ASSERT_EQ(2, photos.size());
    EXPECT_EQ(11, photos[0].id.value);
    EXPECT_EQ(QStringLiteral("first.jpg"), photos[0].displayName);
    EXPECT_FALSE(photos[0].source.has_value());
    EXPECT_EQ(17, photos[1].id.value);
}

}  // namespace
}  // namespace flexraw::ui::facade
