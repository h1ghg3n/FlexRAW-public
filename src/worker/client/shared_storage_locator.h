#pragma once

#include <cstdint>

#include <QString>
#include <QUuid>

#include "result.h"

namespace flexraw::worker::client
{

inline constexpr auto SharedStorageMarkerFileName = ".flexraw-storage.json";

struct LocatedStorage
{
    QString localRoot;
    QUuid storageId;
    QString relativePath;
};

struct SharedStorageRoot
{
    QString localRoot;
    QUuid storageId;
};

enum class SharedStorageLocatorErrorCode : std::uint8_t
{
    InvalidPath,
    SourceNotFound,
    OutputParentNotFound,
    NoStorageMarker,
    MarkerUnreadable,
    MalformedMarker,
    UnsupportedSchema,
    InvalidStorageId,
    NonPortablePath,
    MarkerWriteFailed,
};

struct SharedStorageLocatorError
{
    SharedStorageLocatorErrorCode code{SharedStorageLocatorErrorCode::InvalidPath};
    QString message;
};

using LocateSharedStorageResult = core::types::Result<LocatedStorage, SharedStorageLocatorError>;
using EnsureSharedStorageMarkerResult = core::types::Result<SharedStorageRoot, SharedStorageLocatorError>;

class SharedStorageLocator final
{
public:
    // 목적: 기존 source file이 속한 가장 가까운 shared storage marker namespace 탐색
    // 입력: sourcePath: Desktop process가 접근 가능한 absolute regular file path
    // 출력: canonical local root, normalized storage UUID와 portable relative path 또는 typed 오류
    [[nodiscard]] static LocateSharedStorageResult locateSource(const QString& sourcePath);

    // 목적: 아직 존재하지 않을 수 있는 output file이 속한 가장 가까운 shared storage marker namespace 탐색
    // 입력: outputPath: 기존 parent directory 아래의 absolute output file path
    // 출력: canonical local root, normalized storage UUID와 portable relative path 또는 typed 오류
    [[nodiscard]] static LocateSharedStorageResult locateOutput(const QString& outputPath);

    // 목적: directory에서 root boundary까지 기존 marker를 찾고 없으면 boundary root에 marker를 한 번 생성
    // 입력: directoryPath: Open Folder absolute directory, storageRootPath: 비어 있으면 filesystem/share root
    // 출력: 기존 또는 생성된 canonical marker root와 UUID, path/write/schema typed 오류
    [[nodiscard]] static EnsureSharedStorageMarkerResult ensureMarkerForDirectory(const QString& directoryPath,
                                                                                  const QString& storageRootPath = {});
};

}  // namespace flexraw::worker::client
