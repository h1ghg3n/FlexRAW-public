#include "catalog_photo_page_qt_adapter.h"

#include <cstddef>
#include <utility>

#include <QByteArray>
#include <QString>

namespace flexraw::ui::facade
{
namespace
{

// 목적: UTF-8 client string을 Qt presentation 문자열로 변환
// 입력: value: byte length가 명시된 UTF-8 string
// 출력: 같은 Unicode text를 보유한 QString
[[nodiscard]] QString toQString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Qt-free file kind를 기존 Catalog record enum으로 변환
// 입력: kind: client photo file 분류
// 출력: 같은 의미의 internal enum
[[nodiscard]] core::types::SupportedFileKind toFileKind(core::client::CatalogFileKind kind) noexcept
{
    switch (kind)
    {
    case core::client::CatalogFileKind::Raw:
        return core::types::SupportedFileKind::Raw;
    case core::client::CatalogFileKind::RasterImage:
        return core::types::SupportedFileKind::RasterImage;
    case core::client::CatalogFileKind::Unknown:
        return core::types::SupportedFileKind::Unknown;
    }
    return core::types::SupportedFileKind::Unknown;
}

// 목적: Qt-free scan status를 기존 Catalog record enum으로 변환
// 입력: status: client photo scan 상태
// 출력: 같은 의미의 internal enum
[[nodiscard]] core::types::FileScanStatus toScanStatus(core::client::CatalogScanStatus status) noexcept
{
    switch (status)
    {
    case core::client::CatalogScanStatus::Pending:
        return core::types::FileScanStatus::Pending;
    case core::client::CatalogScanStatus::Ready:
        return core::types::FileScanStatus::Ready;
    case core::client::CatalogScanStatus::Unsupported:
        return core::types::FileScanStatus::Unsupported;
    case core::client::CatalogScanStatus::Failed:
        return core::types::FileScanStatus::Failed;
    }
    return core::types::FileScanStatus::Pending;
}

// 목적: Qt-free source state를 기존 Catalog source binding enum으로 변환
// 입력: state: client source identity 상태
// 출력: 같은 의미의 internal enum
[[nodiscard]] core::catalog::SourceBindingState toSourceState(core::client::CatalogSourceState state) noexcept
{
    switch (state)
    {
    case core::client::CatalogSourceState::FingerprintPending:
        return core::catalog::SourceBindingState::FingerprintPending;
    case core::client::CatalogSourceState::Available:
        return core::catalog::SourceBindingState::Available;
    case core::client::CatalogSourceState::Missing:
        return core::catalog::SourceBindingState::Missing;
    case core::client::CatalogSourceState::VerificationRequired:
        return core::catalog::SourceBindingState::VerificationRequired;
    case core::client::CatalogSourceState::IdentityUnverified:
        return core::catalog::SourceBindingState::IdentityUnverified;
    case core::client::CatalogSourceState::ReplacementDetected:
        return core::catalog::SourceBindingState::ReplacementDetected;
    case core::client::CatalogSourceState::Unreadable:
        return core::catalog::SourceBindingState::Unreadable;
    case core::client::CatalogSourceState::Unlinked:
        return core::catalog::SourceBindingState::Unlinked;
    }
    return core::catalog::SourceBindingState::FingerprintPending;
}

// 목적: Qt-free fingerprint byte vector를 기존 source identity value로 변환
// 입력: fingerprint: fixed-width metadata와 optional SHA-256 bytes
// 출력: 같은 metadata와 owned QByteArray
[[nodiscard]] core::types::SourceFingerprint toFingerprint(const core::client::CatalogSourceFingerprint& fingerprint)
{
    QByteArray sha256;
    sha256.resize(static_cast<qsizetype>(fingerprint.sha256.size()));
    for (std::size_t index = 0; index < fingerprint.sha256.size(); ++index)
    {
        sha256[static_cast<qsizetype>(index)] = static_cast<char>(fingerprint.sha256[index]);
    }
    return {fingerprint.sizeBytes, fingerprint.modifiedAtMs, std::move(sha256)};
}

}  // namespace

// 목적: Qt-free photo snapshot을 기존 Qt Catalog presentation record로 변환
// 입력: snapshot: identity/source/display/fingerprint를 보유한 client state
// 출력: 같은 의미의 CatalogPhotoRecord
core::catalog::CatalogPhotoRecord toCatalogPhotoRecord(const core::client::CatalogPhotoSnapshot& snapshot)
{
    std::optional<core::types::SourceLocator> source;
    if (snapshot.sourcePath.has_value())
    {
        source = core::types::SourceLocator{toQString(*snapshot.sourcePath)};
    }
    return {
        core::types::PhotoId{snapshot.id.value},
        std::move(source),
        toQString(snapshot.lastKnownPath),
        toQString(snapshot.extension),
        toQString(snapshot.displayName),
        toFileKind(snapshot.kind),
        toScanStatus(snapshot.scanStatus),
        toFingerprint(snapshot.fingerprint),
        toSourceState(snapshot.sourceState),
    };
}

// 목적: Qt-free bounded photo page rows를 Catalog widget 입력으로 변환
// 입력: snapshots: stable order client photo snapshot 목록
// 출력: 순서를 보존한 Qt record vector
QVector<core::catalog::CatalogPhotoRecord> toCatalogPhotoRecords(
    const std::vector<core::client::CatalogPhotoSnapshot>& snapshots)
{
    QVector<core::catalog::CatalogPhotoRecord> photos;
    photos.reserve(static_cast<qsizetype>(snapshots.size()));
    for (const core::client::CatalogPhotoSnapshot& snapshot : snapshots)
    {
        photos.push_back(toCatalogPhotoRecord(snapshot));
    }
    return photos;
}

}  // namespace flexraw::ui::facade
