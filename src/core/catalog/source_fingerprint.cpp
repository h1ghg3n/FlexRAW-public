#include "source_fingerprint.h"

#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

namespace flexraw::core::catalog
{
namespace
{

constexpr qint64 HashChunkSize = 1024 * 1024;

// 목적: QFileInfo의 optional 수정 시각을 millisecond epoch로 변환
// 입력: fileInfo: source file metadata snapshot
// 출력: 유효한 수정 시각 또는 없으면 0
[[nodiscard]] qint64 modifiedAtMs(const QFileInfo& fileInfo)
{
    const QDateTime modifiedAt = fileInfo.lastModified();
    return modifiedAt.isValid() ? modifiedAt.toMSecsSinceEpoch() : 0;
}

// 목적: source locator가 fingerprint 생성 가능한 readable file을 가리키는지 검증
// 입력: locator: 확인할 source path
// 출력: 검증된 QFileInfo 또는 argument·not-found·permission 오류
[[nodiscard]] types::Result<QFileInfo, types::CoreError> validateSource(const types::SourceLocator& locator)
{
    if (locator.path.isEmpty() || locator.path != locator.path.trimmed())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Source path is empty or contains outer whitespace.")});
    }

    const QFileInfo fileInfo(locator.path);
    if (!fileInfo.exists())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Source file does not exist.")});
    }
    if (!fileInfo.isFile())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Source path is not a file.")});
    }
    if (!fileInfo.isReadable())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            {types::ErrorCode::PermissionDenied, QStringLiteral("Source file is not readable.")});
    }

    return types::Result<QFileInfo, types::CoreError>::success(fileInfo);
}

}  // namespace

// 목적: source identity fast path에 사용할 file size와 수정 시각 조회
// 입력: locator: 조회할 현재 source path
// 출력: content hash가 없는 metadata fingerprint 또는 file 접근 오류
SourceFingerprintResult inspectSourceMetadata(const types::SourceLocator& locator)
{
    const types::Result<QFileInfo, types::CoreError> source = validateSource(locator);
    if (source.hasError())
    {
        return SourceFingerprintResult::failure(source.error());
    }

    return SourceFingerprintResult::success({source.value().size(), modifiedAtMs(source.value()), {}});
}

// 목적: background source identity baseline·verification에 사용할 SHA-256 fingerprint 생성
// 입력: locator: hash할 source path, cancellationToken: chunk 사이에 확인할 cancellation state
// 출력: metadata와 SHA-256를 포함한 fingerprint 또는 file 변경·취소·IO 오류
SourceFingerprintResult calculateSourceFingerprint(const types::SourceLocator& locator,
                                                   const types::CancellationToken& cancellationToken)
{
    const SourceFingerprintResult initial = inspectSourceMetadata(locator);
    if (initial.hasError())
    {
        return initial;
    }
    if (cancellationToken.isCancellationRequested())
    {
        return SourceFingerprintResult::failure(
            {types::ErrorCode::Cancelled, QStringLiteral("Source fingerprint cancellation requested.")});
    }

    QFile sourceFile(locator.path);
    if (!sourceFile.open(QIODevice::ReadOnly))
    {
        return SourceFingerprintResult::failure(
            {types::ErrorCode::PermissionDenied, QStringLiteral("Unable to open source file for fingerprinting.")});
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!sourceFile.atEnd())
    {
        if (cancellationToken.isCancellationRequested())
        {
            return SourceFingerprintResult::failure(
                {types::ErrorCode::Cancelled, QStringLiteral("Source fingerprint cancellation requested.")});
        }

        const QByteArray chunk = sourceFile.read(HashChunkSize);
        if (chunk.isEmpty() && sourceFile.error() != QFileDevice::NoError)
        {
            return SourceFingerprintResult::failure(
                {types::ErrorCode::PermissionDenied, QStringLiteral("Unable to read source file for fingerprinting.")});
        }
        hash.addData(chunk);
    }
    sourceFile.close();

    const SourceFingerprintResult final = inspectSourceMetadata(locator);
    if (final.hasError())
    {
        return final;
    }
    if (initial.value().sizeBytes != final.value().sizeBytes ||
        initial.value().modifiedAtMs != final.value().modifiedAtMs)
    {
        return SourceFingerprintResult::failure(
            {types::ErrorCode::Conflict, QStringLiteral("Source file changed while its fingerprint was generated.")});
    }

    types::SourceFingerprint fingerprint = final.value();
    fingerprint.sha256 = hash.result();
    return SourceFingerprintResult::success(std::move(fingerprint));
}

}  // namespace flexraw::core::catalog
