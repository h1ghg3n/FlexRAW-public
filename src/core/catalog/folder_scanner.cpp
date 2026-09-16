#include "folder_scanner.h"

#include "supported_extensions.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace flexraw::core::catalog {
namespace {

// 목적: code 와 message 를 담은 CoreError 값 생성
// 입력: code: 오류 분류, message: 오류 설명
// 출력: CoreError 값
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return types::CoreError{
        code,
        std::move(message),
    };
}

// 목적: scan 가능한 folder 경로인지 확인
// 입력: folderPath: scan 대상 folder 경로
// 출력: 검증된 QFileInfo 또는 구조화된 오류
[[nodiscard]] types::Result<QFileInfo, types::CoreError> validateFolderPath(const QString& folderPath)
{
    if (folderPath.isEmpty() || folderPath != folderPath.trimmed())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(makeError(
            types::ErrorCode::InvalidArgument, QStringLiteral("Folder path is empty or contains outer whitespace.")));
    }

    const QFileInfo folderInfo(folderPath);

    if (!folderInfo.exists()) {
        return types::Result<QFileInfo, types::CoreError>::failure(
            makeError(types::ErrorCode::NotFound, QStringLiteral("Folder path does not exist.")));
    }

    if (!folderInfo.isDir()) {
        return types::Result<QFileInfo, types::CoreError>::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Folder path is not a directory.")));
    }

    if (!folderInfo.isReadable()) {
        return types::Result<QFileInfo, types::CoreError>::failure(
            makeError(types::ErrorCode::PermissionDenied, QStringLiteral("Folder path is not readable.")));
    }

    return types::Result<QFileInfo, types::CoreError>::success(folderInfo);
}

// 목적: 지원 확장자를 가진 파일에서 CatalogEntry 값 생성
// 입력: fileInfo: scan 중 발견한 파일 정보, kind: 분류된 지원 파일 종류
// 출력: Ready 상태의 CatalogEntry 값
[[nodiscard]] CatalogEntry makeCatalogEntry(const QFileInfo& fileInfo, types::SupportedFileKind kind)
{
    return CatalogEntry{
        types::makeFileDescriptor(fileInfo, kind),
        types::FileScanStatus::Ready,
    };
}

// 목적: folder 내부의 지원 파일을 CatalogEntry 목록으로 수집
// 입력: folder: scan 대상 folder
// 출력: 지원 파일만 포함한 CatalogEntry 목록
[[nodiscard]] QVector<CatalogEntry> collectCatalogEntries(const QDir& folder)
{
    QVector<CatalogEntry> entries;

    const QFileInfoList fileInfos =
        folder.entryInfoList(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::NoDotAndDotDot);

    entries.reserve(fileInfos.size());

    for (const QFileInfo& fileInfo : fileInfos) {
        const types::SupportedFileKind kind = util::classifyExtension(fileInfo.suffix());

        if (kind == types::SupportedFileKind::Unknown) {
            continue;
        }

        entries.push_back(makeCatalogEntry(fileInfo, kind));
    }

    return entries;
}

// 목적: CatalogEntry 목록을 UI 표시가 안정적인 순서로 정렬
// 입력: entries: 정렬할 CatalogEntry 목록
// 출력: 없음
void sortCatalogEntries(QVector<CatalogEntry>& entries)
{
    std::sort(entries.begin(), entries.end(), [](const CatalogEntry& left, const CatalogEntry& right) {
        const int nameOrder = QString::compare(left.file.displayName, right.file.displayName, Qt::CaseInsensitive);

        if (nameOrder != 0) {
            return nameOrder < 0;
        }

        return QString::compare(left.file.path, right.file.path, Qt::CaseInsensitive) < 0;
    });
}

} // namespace

// 목적: 지정된 folder 에서 지원되는 RAW/image 파일 목록 검색
// 입력: folderPath: scan 대상 folder 경로
// 출력: CatalogEntry 목록 또는 구조화된 오류
CatalogScanResult scanFolder(const QString& folderPath)
{
    const types::Result<QFileInfo, types::CoreError> validatedFolder = validateFolderPath(folderPath);

    if (validatedFolder.hasError()) {
        return CatalogScanResult::failure(validatedFolder.error());
    }

    QDir folder(validatedFolder.value().absoluteFilePath());
    QVector<CatalogEntry> entries = collectCatalogEntries(folder);
    sortCatalogEntries(entries);

    return CatalogScanResult::success(std::move(entries));
}

} // namespace flexraw::core::catalog
