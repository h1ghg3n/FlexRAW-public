#include "preview_cache.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace flexraw::core::preview {
namespace {

constexpr int CacheFormatVersion = 3;

struct CacheEntryPaths {
    QString imagePath;
    QString metadataPath;
    QString sourcePath;
    qint64 sourceModifiedMs{0};
    QString tierName;
};

// 목적: preview cache 처리 실패를 설명하는 CoreError 값 생성
// 입력: code: 오류 분류, message: 오류 설명
// 출력: 구조화된 CoreError 값
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return types::CoreError{
        code,
        std::move(message),
    };
}

// 목적: cache에 사용할 target 크기가 유효한지 확인
// 입력: targetSize: 요청된 최대 preview 크기
// 출력: 유효 여부
[[nodiscard]] bool isValidTargetSize(const QSize& targetSize)
{
    return targetSize.width() > 0 && targetSize.height() > 0;
}

// 목적: cache key 생성에 사용할 원본 파일 정보를 검증
// 입력: sourcePath: 확인할 원본 파일 경로
// 출력: 검증된 QFileInfo 또는 구조화된 오류
[[nodiscard]] types::Result<QFileInfo, types::CoreError> validateSourceFile(const QString& sourcePath)
{
    if (sourcePath.isEmpty()) {
        return types::Result<QFileInfo, types::CoreError>::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview cache source path is empty.")));
    }

    const QFileInfo sourceInfo(QDir::cleanPath(sourcePath));

    if (!sourceInfo.exists()) {
        return types::Result<QFileInfo, types::CoreError>::failure(
            makeError(types::ErrorCode::NotFound, QStringLiteral("Preview cache source file does not exist.")));
    }

    if (!sourceInfo.isFile()) {
        return types::Result<QFileInfo, types::CoreError>::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview cache source path is not a file.")));
    }

    return types::Result<QFileInfo, types::CoreError>::success(sourceInfo);
}

// 목적: preview 품질 단계를 cache directory 이름으로 변환
// 입력: tier: 변환할 preview 품질 단계
// 출력: cache directory 이름
[[nodiscard]] QString cacheTierName(PreviewCacheTier tier)
{
    switch (tier) {
    case PreviewCacheTier::Thumbnail:
        return QStringLiteral("thumbnail");
    case PreviewCacheTier::Standard:
        return QStringLiteral("standard");
    }

    return QStringLiteral("unknown");
}

// 목적: 원본 파일과 preview 요청을 고유한 cache entry 경로로 변환
// 입력: rootPath: cache root, sourceInfo: 검증된 원본 파일 정보, tier: preview 품질 단계, targetSize: 최대 크기
// 출력: image와 metadata의 cache 경로 및 검증 metadata
[[nodiscard]] CacheEntryPaths makeCacheEntry(
    const QString& rootPath,
    const QFileInfo& sourceInfo,
    PreviewCacheTier tier,
    const QSize& targetSize)
{
    const QString canonicalPath = sourceInfo.canonicalFilePath().isEmpty()
        ? sourceInfo.absoluteFilePath()
        : sourceInfo.canonicalFilePath();
    const QString tierName = cacheTierName(tier);
    const QByteArray keyData = QStringLiteral("%1\n%2\n%3x%4")
                                   .arg(canonicalPath, tierName)
                                   .arg(targetSize.width())
                                   .arg(targetSize.height())
                                   .toUtf8();
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(keyData, QCryptographicHash::Sha256).toHex());
    const QString entryDirectory = QDir(rootPath).filePath(tierName);

    return CacheEntryPaths{
        QDir(entryDirectory).filePath(key + QStringLiteral(".png")),
        QDir(entryDirectory).filePath(key + QStringLiteral(".json")),
        canonicalPath,
        sourceInfo.lastModified().toMSecsSinceEpoch(),
        tierName,
    };
}

// 목적: image와 metadata로 구성된 cache entry를 함께 삭제
// 입력: entry: 삭제할 cache entry 경로
// 출력: 없음
void removeCacheEntry(const CacheEntryPaths& entry)
{
    QFile::remove(entry.imagePath);
    QFile::remove(entry.metadataPath);
}

// 목적: metadata가 현재 원본 파일과 preview 요청에 일치하는지 확인
// 입력: metadata: 읽어온 cache metadata, entry: 현재 요청의 cache entry 정보, targetSize: 최대 preview 크기
// 출력: metadata가 현재 cache 항목을 설명하면 true
[[nodiscard]] bool isCurrentMetadata(
    const QJsonObject& metadata,
    const CacheEntryPaths& entry,
    const QSize& targetSize)
{
    return metadata.value(QStringLiteral("formatVersion")).toInt() == CacheFormatVersion
        && metadata.value(QStringLiteral("sourcePath")).toString() == entry.sourcePath
        && metadata.value(QStringLiteral("sourceModifiedMs")).toString()
            == QString::number(entry.sourceModifiedMs)
        && metadata.value(QStringLiteral("tier")).toString() == entry.tierName
        && metadata.value(QStringLiteral("targetWidth")).toInt() == targetSize.width()
        && metadata.value(QStringLiteral("targetHeight")).toInt() == targetSize.height();
}

// 목적: cache entry를 설명하는 metadata JSON 생성
// 입력: entry: 저장할 cache entry 정보, targetSize: 최대 preview 크기
// 출력: JSON object 형태의 cache metadata
[[nodiscard]] QJsonObject makeMetadata(const CacheEntryPaths& entry, const QSize& targetSize)
{
    return QJsonObject{
        {QStringLiteral("formatVersion"), CacheFormatVersion},
        {QStringLiteral("sourcePath"), entry.sourcePath},
        {QStringLiteral("sourceModifiedMs"), QString::number(entry.sourceModifiedMs)},
        {QStringLiteral("tier"), entry.tierName},
        {QStringLiteral("targetWidth"), targetSize.width()},
        {QStringLiteral("targetHeight"), targetSize.height()},
    };
}

} // namespace

// 목적: 설정 UI가 없을 때 사용할 기본 preview cache root 경로 반환
// 입력: 없음
// 출력: 사용자 writable directory 아래 preview cache root 경로
QString defaultPreviewCacheRoot()
{
    QString rootPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    if (rootPath.isEmpty()) {
        rootPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }

    if (rootPath.isEmpty()) {
        rootPath = QDir::tempPath();
    }

    return QDir(rootPath).filePath(QStringLiteral("Flexraw/preview-cache"));
}

// 목적: 명시된 root directory를 사용하는 preview cache 초기화
// 입력: rootPath: thumbnail과 standard preview를 저장할 root directory
// 출력: 초기화된 preview cache 객체
PreviewCache::PreviewCache(QString rootPath)
    : m_rootPath(QDir::cleanPath(std::move(rootPath)))
{
}

// 목적: 원본 파일과 target 크기에 일치하는 유효한 preview cache 항목 조회
// 입력: sourcePath: 원본 파일 경로, tier: preview 품질 단계, targetSize: 최대 preview 크기
// 출력: cache hit 여부와 QImage 또는 구조화된 오류
PreviewCacheLookupResult PreviewCache::load(
    const QString& sourcePath,
    PreviewCacheTier tier,
    const QSize& targetSize) const
{
    if (m_rootPath.isEmpty() || !isValidTargetSize(targetSize)) {
        return PreviewCacheLookupResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview cache arguments are invalid.")));
    }

    const types::Result<QFileInfo, types::CoreError> sourceInfo = validateSourceFile(sourcePath);

    if (sourceInfo.hasError()) {
        return PreviewCacheLookupResult::failure(sourceInfo.error());
    }

    const CacheEntryPaths entry = makeCacheEntry(m_rootPath, sourceInfo.value(), tier, targetSize);

    if (!QFile::exists(entry.imagePath) && !QFile::exists(entry.metadataPath)) {
        return PreviewCacheLookupResult::success({});
    }

    QFile metadataFile(entry.metadataPath);

    if (!metadataFile.open(QIODevice::ReadOnly)) {
        removeCacheEntry(entry);
        return PreviewCacheLookupResult::success({});
    }

    const QJsonDocument metadata = QJsonDocument::fromJson(metadataFile.readAll());

    if (!metadata.isObject() || !isCurrentMetadata(metadata.object(), entry, targetSize)) {
        removeCacheEntry(entry);
        return PreviewCacheLookupResult::success({});
    }

    QImageReader imageReader(entry.imagePath);
    const QImage image = imageReader.read();

    if (image.isNull()) {
        removeCacheEntry(entry);
        return PreviewCacheLookupResult::success({});
    }

    return PreviewCacheLookupResult::success({true, image});
}

// 목적: 원본 파일의 현재 mtime에 연결된 preview image를 cache에 원자적으로 저장
// 입력: sourcePath: 원본 파일 경로, tier: preview 품질 단계, targetSize: 최대 preview 크기, image: 저장할 preview
// 출력: 성공 표식 또는 구조화된 오류
PreviewCacheStoreResult PreviewCache::store(
    const QString& sourcePath,
    PreviewCacheTier tier,
    const QSize& targetSize,
    const QImage& image) const
{
    if (m_rootPath.isEmpty() || !isValidTargetSize(targetSize) || image.isNull()) {
        return PreviewCacheStoreResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview cache arguments are invalid.")));
    }

    const types::Result<QFileInfo, types::CoreError> sourceInfo = validateSourceFile(sourcePath);

    if (sourceInfo.hasError()) {
        return PreviewCacheStoreResult::failure(sourceInfo.error());
    }

    const CacheEntryPaths entry = makeCacheEntry(m_rootPath, sourceInfo.value(), tier, targetSize);
    const QString entryDirectory = QFileInfo(entry.imagePath).absolutePath();

    if (!QDir().mkpath(entryDirectory)) {
        return PreviewCacheStoreResult::failure(
            makeError(types::ErrorCode::PermissionDenied, QStringLiteral("Unable to create preview cache directory.")));
    }

    QByteArray imageData;
    QBuffer imageBuffer(&imageData);
    QImageWriter imageWriter(&imageBuffer, "PNG");

    if (!imageBuffer.open(QIODevice::WriteOnly) || !imageWriter.write(image)) {
        return PreviewCacheStoreResult::failure(
            makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Unable to encode preview cache image: %1").arg(imageWriter.errorString())));
    }

    QSaveFile imageFile(entry.imagePath);

    if (!imageFile.open(QIODevice::WriteOnly) || imageFile.write(imageData) != imageData.size()
        || !imageFile.commit()) {
        return PreviewCacheStoreResult::failure(
            makeError(types::ErrorCode::PermissionDenied, QStringLiteral("Unable to write preview cache image.")));
    }

    QSaveFile metadataFile(entry.metadataPath);
    const QByteArray metadata = QJsonDocument(makeMetadata(entry, targetSize)).toJson(QJsonDocument::Compact);

    if (!metadataFile.open(QIODevice::WriteOnly) || metadataFile.write(metadata) != metadata.size()
        || !metadataFile.commit()) {
        QFile::remove(entry.imagePath);
        return PreviewCacheStoreResult::failure(
            makeError(types::ErrorCode::PermissionDenied, QStringLiteral("Unable to write preview cache metadata.")));
    }

    return PreviewCacheStoreResult::success({});
}

} // namespace flexraw::core::preview
