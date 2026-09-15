#include "shared_storage_locator.h"

#include <cmath>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStorageInfo>
#include <QStringList>

namespace flexraw::worker::client
{
namespace
{

constexpr qint64 MaximumMarkerBytes = 16 * 1024;
constexpr int SupportedMarkerSchema = 1;
constexpr qsizetype MaximumMarkerNameCharacters = 256;

struct ParsedStorageMarker
{
    QUuid storageId;
};

using ParseMarkerResult = core::types::Result<ParsedStorageMarker, SharedStorageLocatorError>;

// 목적: shared storage marker 탐색 오류를 지정 code와 bounded message로 구성
// 입력: code: locator 오류 분류, message: local preflight 진단
// 출력: 구성된 SharedStorageLocatorError
[[nodiscard]] SharedStorageLocatorError makeError(const SharedStorageLocatorErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: marker JSON의 schema와 storage UUID를 strict하게 검증
// 입력: markerPath: 발견한 marker file의 local absolute path
// 출력: normalized non-null UUID 또는 typed read/schema 오류
[[nodiscard]] ParseMarkerResult parseMarker(const QString& markerPath)
{
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly))
    {
        return ParseMarkerResult::failure(makeError(SharedStorageLocatorErrorCode::MarkerUnreadable,
                                                    QStringLiteral("Shared storage marker cannot be read.")));
    }
    if (marker.size() <= 0 || marker.size() > MaximumMarkerBytes)
    {
        return ParseMarkerResult::failure(makeError(SharedStorageLocatorErrorCode::MalformedMarker,
                                                    QStringLiteral("Shared storage marker size is invalid.")));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(marker.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return ParseMarkerResult::failure(makeError(SharedStorageLocatorErrorCode::MalformedMarker,
                                                    QStringLiteral("Shared storage marker is not valid JSON.")));
    }

    const QJsonObject object = document.object();
    const QJsonValue schemaValue = object.value(QStringLiteral("schema"));
    if (!schemaValue.isDouble() || !std::isfinite(schemaValue.toDouble()) ||
        std::floor(schemaValue.toDouble()) != schemaValue.toDouble())
    {
        return ParseMarkerResult::failure(
            makeError(SharedStorageLocatorErrorCode::MalformedMarker,
                      QStringLiteral("Shared storage marker schema must be an integer.")));
    }
    if (schemaValue.toInt() != SupportedMarkerSchema)
    {
        return ParseMarkerResult::failure(makeError(SharedStorageLocatorErrorCode::UnsupportedSchema,
                                                    QStringLiteral("Shared storage marker schema is unsupported.")));
    }

    const QJsonValue storageIdValue = object.value(QStringLiteral("storage_id"));
    if (!storageIdValue.isString())
    {
        return ParseMarkerResult::failure(
            makeError(SharedStorageLocatorErrorCode::InvalidStorageId,
                      QStringLiteral("Shared storage marker storage_id must be a UUID.")));
    }
    const QUuid storageId = QUuid::fromString(storageIdValue.toString().trimmed());
    if (storageId.isNull())
    {
        return ParseMarkerResult::failure(makeError(SharedStorageLocatorErrorCode::InvalidStorageId,
                                                    QStringLiteral("Shared storage marker storage_id is invalid.")));
    }

    const QJsonValue nameValue = object.value(QStringLiteral("name"));
    if (!nameValue.isUndefined() &&
        (!nameValue.isString() || nameValue.toString().size() > MaximumMarkerNameCharacters))
    {
        return ParseMarkerResult::failure(makeError(SharedStorageLocatorErrorCode::MalformedMarker,
                                                    QStringLiteral("Shared storage marker name is invalid.")));
    }
    return ParseMarkerResult::success({storageId});
}

// 목적: canonical marker root와 candidate를 Worker protocol용 portable relative path로 변환
// 입력: rootPath: marker directory, candidatePath: root 아래 canonical source/output candidate
// 출력: slash-separated relative path 또는 portability/root escape 오류
[[nodiscard]] core::types::Result<QString, SharedStorageLocatorError> makePortableRelativePath(
    const QString& rootPath, const QString& candidatePath)
{
    const QString relative = QDir::fromNativeSeparators(QDir(rootPath).relativeFilePath(candidatePath));
    if (relative.isEmpty() || relative.contains(QChar::Null) || relative.contains(QLatin1Char('\\')) ||
        relative.contains(QLatin1Char(':')) || QDir::isAbsolutePath(relative) || relative == QStringLiteral("..") ||
        relative.startsWith(QStringLiteral("../")))
    {
        return core::types::Result<QString, SharedStorageLocatorError>::failure(
            makeError(SharedStorageLocatorErrorCode::NonPortablePath,
                      QStringLiteral("Shared storage path is not a portable relative path.")));
    }

    for (const QChar character : relative)
    {
        if (character.unicode() < 0x20U)
        {
            return core::types::Result<QString, SharedStorageLocatorError>::failure(
                makeError(SharedStorageLocatorErrorCode::NonPortablePath,
                          QStringLiteral("Shared storage path contains a control character.")));
        }
    }

    const QStringList segments = relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& segment : segments)
    {
        if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral(".."))
        {
            return core::types::Result<QString, SharedStorageLocatorError>::failure(
                makeError(SharedStorageLocatorErrorCode::NonPortablePath,
                          QStringLiteral("Shared storage path contains an invalid segment.")));
        }
    }
    if (QDir::cleanPath(relative) != relative)
    {
        return core::types::Result<QString, SharedStorageLocatorError>::failure(
            makeError(SharedStorageLocatorErrorCode::NonPortablePath,
                      QStringLiteral("Shared storage path is not in canonical relative form.")));
    }
    return core::types::Result<QString, SharedStorageLocatorError>::success(relative);
}

// 목적: canonical candidate parent부터 filesystem root까지 가장 가까운 marker 탐색
// 입력: startDirectory: 기존 canonical directory, candidatePath: 반환할 relative path의 canonical candidate
// 출력: nearest marker namespace 또는 marker 없음/schema/path 오류
[[nodiscard]] LocateSharedStorageResult locateFromDirectory(const QString& startDirectory,
                                                            const QString& candidatePath,
                                                            const QString& stopDirectory = {})
{
    QString currentDirectory = startDirectory;
    while (!currentDirectory.isEmpty())
    {
        const QFileInfo markerInfo(QDir(currentDirectory).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
        if (markerInfo.exists())
        {
            if (!markerInfo.isFile())
            {
                return LocateSharedStorageResult::failure(
                    makeError(SharedStorageLocatorErrorCode::MalformedMarker,
                              QStringLiteral("Shared storage marker is not a regular file.")));
            }
            const ParseMarkerResult marker = parseMarker(markerInfo.absoluteFilePath());
            if (marker.hasError())
            {
                return LocateSharedStorageResult::failure(marker.error());
            }
            const core::types::Result<QString, SharedStorageLocatorError> relative =
                makePortableRelativePath(currentDirectory, candidatePath);
            if (relative.hasError())
            {
                return LocateSharedStorageResult::failure(relative.error());
            }
            return LocateSharedStorageResult::success({currentDirectory, marker.value().storageId, relative.value()});
        }

        if (!stopDirectory.isEmpty() && currentDirectory == stopDirectory)
        {
            break;
        }

        QDir parentDirectory(currentDirectory);
        if (!parentDirectory.cdUp())
        {
            break;
        }
        const QString parentPath = QDir::cleanPath(parentDirectory.absolutePath());
        if (parentPath == currentDirectory)
        {
            break;
        }
        currentDirectory = parentPath;
    }
    return LocateSharedStorageResult::failure(makeError(SharedStorageLocatorErrorCode::NoStorageMarker,
                                                        QStringLiteral("No shared storage marker was found.")));
}

}  // namespace

// 목적: 기존 source file이 속한 가장 가까운 shared storage marker namespace 탐색
// 입력: sourcePath: Desktop process가 접근 가능한 absolute regular file path
// 출력: canonical local root, normalized storage UUID와 portable relative path 또는 typed 오류
LocateSharedStorageResult SharedStorageLocator::locateSource(const QString& sourcePath)
{
    const QString cleanedPath = QDir::cleanPath(sourcePath);
    if (sourcePath.isEmpty() || sourcePath != sourcePath.trimmed() || !QDir::isAbsolutePath(cleanedPath))
    {
        return LocateSharedStorageResult::failure(makeError(SharedStorageLocatorErrorCode::InvalidPath,
                                                            QStringLiteral("Remote source path must be absolute.")));
    }

    const QFileInfo sourceInfo(cleanedPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile())
    {
        return LocateSharedStorageResult::failure(makeError(SharedStorageLocatorErrorCode::SourceNotFound,
                                                            QStringLiteral("Remote source file does not exist.")));
    }
    const QString canonicalPath = sourceInfo.canonicalFilePath();
    if (canonicalPath.isEmpty())
    {
        return LocateSharedStorageResult::failure(makeError(
            SharedStorageLocatorErrorCode::InvalidPath, QStringLiteral("Remote source path cannot be canonicalized.")));
    }
    return locateFromDirectory(QFileInfo(canonicalPath).absolutePath(), canonicalPath);
}

// 목적: 아직 존재하지 않을 수 있는 output file이 속한 가장 가까운 shared storage marker namespace 탐색
// 입력: outputPath: 기존 parent directory 아래의 absolute output file path
// 출력: canonical local root, normalized storage UUID와 portable relative path 또는 typed 오류
LocateSharedStorageResult SharedStorageLocator::locateOutput(const QString& outputPath)
{
    const QString cleanedPath = QDir::cleanPath(outputPath);
    if (outputPath.isEmpty() || outputPath != outputPath.trimmed() || !QDir::isAbsolutePath(cleanedPath))
    {
        return LocateSharedStorageResult::failure(makeError(SharedStorageLocatorErrorCode::InvalidPath,
                                                            QStringLiteral("Remote output path must be absolute.")));
    }

    const QFileInfo outputInfo(cleanedPath);
    if (outputInfo.fileName().isEmpty())
    {
        return LocateSharedStorageResult::failure(makeError(SharedStorageLocatorErrorCode::InvalidPath,
                                                            QStringLiteral("Remote output path must name a file.")));
    }
    const QFileInfo parentInfo(outputInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir())
    {
        return LocateSharedStorageResult::failure(
            makeError(SharedStorageLocatorErrorCode::OutputParentNotFound,
                      QStringLiteral("Remote output parent directory does not exist.")));
    }
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty())
    {
        return LocateSharedStorageResult::failure(
            makeError(SharedStorageLocatorErrorCode::InvalidPath,
                      QStringLiteral("Remote output parent cannot be canonicalized.")));
    }
    return locateFromDirectory(canonicalParent, QDir(canonicalParent).filePath(outputInfo.fileName()));
}

// 목적: directory에서 root boundary까지 기존 marker를 찾고 없으면 boundary root에 marker를 한 번 생성
// 입력: directoryPath: Open Folder absolute directory, storageRootPath: 비어 있으면 filesystem/share root
// 출력: 기존 또는 생성된 canonical marker root와 UUID, path/write/schema typed 오류
EnsureSharedStorageMarkerResult SharedStorageLocator::ensureMarkerForDirectory(const QString& directoryPath,
                                                                               const QString& storageRootPath)
{
    const QString cleanedDirectory = QDir::cleanPath(directoryPath);
    if (directoryPath.isEmpty() || directoryPath != directoryPath.trimmed() ||
        !QDir::isAbsolutePath(cleanedDirectory) || storageRootPath != storageRootPath.trimmed())
    {
        return EnsureSharedStorageMarkerResult::failure(makeError(
            SharedStorageLocatorErrorCode::InvalidPath, QStringLiteral("Shared storage directory must be absolute.")));
    }

    const QFileInfo directoryInfo(cleanedDirectory);
    const QString canonicalDirectory = directoryInfo.canonicalFilePath();
    if (!directoryInfo.exists() || !directoryInfo.isDir() || canonicalDirectory.isEmpty())
    {
        return EnsureSharedStorageMarkerResult::failure(makeError(
            SharedStorageLocatorErrorCode::InvalidPath, QStringLiteral("Shared storage directory does not exist.")));
    }

    const QString requestedRoot =
        storageRootPath.isEmpty() ? QStorageInfo(canonicalDirectory).rootPath() : QDir::cleanPath(storageRootPath);
    const QFileInfo rootInfo(requestedRoot);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (!rootInfo.exists() || !rootInfo.isDir() || canonicalRoot.isEmpty())
    {
        return EnsureSharedStorageMarkerResult::failure(makeError(
            SharedStorageLocatorErrorCode::InvalidPath, QStringLiteral("Shared storage root does not exist.")));
    }

    const QString directoryFromRoot =
        QDir::fromNativeSeparators(QDir(canonicalRoot).relativeFilePath(canonicalDirectory));
    if (directoryFromRoot == QStringLiteral("..") || directoryFromRoot.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(directoryFromRoot))
    {
        return EnsureSharedStorageMarkerResult::failure(
            makeError(SharedStorageLocatorErrorCode::InvalidPath,
                      QStringLiteral("Shared storage directory is outside its root.")));
    }

    const QString probePath = QDir(canonicalDirectory).filePath(QStringLiteral(".flexraw-storage-probe"));
    const LocateSharedStorageResult existing = locateFromDirectory(canonicalDirectory, probePath, canonicalRoot);
    if (!existing.hasError())
    {
        return EnsureSharedStorageMarkerResult::success({existing.value().localRoot, existing.value().storageId});
    }
    if (existing.error().code != SharedStorageLocatorErrorCode::NoStorageMarker)
    {
        return EnsureSharedStorageMarkerResult::failure(existing.error());
    }

    const QUuid storageId = QUuid::createUuid();
    const QJsonObject markerObject{{QStringLiteral("schema"), SupportedMarkerSchema},
                                   {QStringLiteral("storage_id"), storageId.toString(QUuid::WithoutBraces)}};
    const QByteArray markerBytes = QJsonDocument(markerObject).toJson(QJsonDocument::Compact);
    const QString markerPath = QDir(canonicalRoot).filePath(QString::fromLatin1(SharedStorageMarkerFileName));
    QFile marker(markerPath);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        const LocateSharedStorageResult raced = locateFromDirectory(canonicalDirectory, probePath, canonicalRoot);
        if (!raced.hasError())
        {
            return EnsureSharedStorageMarkerResult::success({raced.value().localRoot, raced.value().storageId});
        }
        return EnsureSharedStorageMarkerResult::failure(
            makeError(SharedStorageLocatorErrorCode::MarkerWriteFailed,
                      QStringLiteral("Shared storage marker cannot be created at the storage root.")));
    }

    const bool wroteMarker = marker.write(markerBytes) == markerBytes.size() && marker.flush();
    marker.close();
    if (!wroteMarker)
    {
        (void)QFile::remove(markerPath);
        return EnsureSharedStorageMarkerResult::failure(
            makeError(SharedStorageLocatorErrorCode::MarkerWriteFailed,
                      QStringLiteral("Shared storage marker could not be written completely.")));
    }
    return EnsureSharedStorageMarkerResult::success({canonicalRoot, storageId});
}

}  // namespace flexraw::worker::client
