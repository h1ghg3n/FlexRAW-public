#pragma once

#include <optional>

#include <QString>

#include "file_types.h"
#include "photo_identity.h"
#include "source_binding.h"

namespace flexraw::core::catalog
{

struct CatalogEntry
{
    types::FileDescriptor file;
    types::FileScanStatus status{types::FileScanStatus::Pending};
};

struct CatalogPhotoRecord
{
    types::PhotoId id;
    std::optional<types::SourceLocator> source;
    QString lastKnownPath;
    QString extension;
    QString displayName;
    types::SupportedFileKind kind{types::SupportedFileKind::Unknown};
    types::FileScanStatus scanStatus{types::FileScanStatus::Pending};
    types::SourceFingerprint fingerprint;
    SourceBindingState sourceState{SourceBindingState::FingerprintPending};
};

}  // namespace flexraw::core::catalog
