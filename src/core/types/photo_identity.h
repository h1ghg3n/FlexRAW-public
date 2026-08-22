#pragma once

#include <QByteArray>
#include <QString>
#include <QtTypes>

namespace flexraw::core::types
{

constexpr qsizetype Sha256DigestSize = 32;

struct PhotoId
{
    qint64 value{0};
};

struct SourceLocator
{
    QString path;
};

struct SourceFingerprint
{
    qint64 sizeBytes{0};
    qint64 modifiedAtMs{0};
    QByteArray sha256;
};

// 목적: PhotoId가 catalog에서 발급된 양의 SQLite identity인지 확인
// 입력: photoId: 확인할 catalog-local photo identity
// 출력: 0보다 큰 identity면 true
[[nodiscard]] bool isValidPhotoId(PhotoId photoId) noexcept;

// 목적: SourceFingerprint가 완전한 SHA-256 content hash를 보유하는지 확인
// 입력: fingerprint: 확인할 source fingerprint
// 출력: SHA-256 digest가 정확히 32 byte면 true
[[nodiscard]] bool hasSourceContentHash(const SourceFingerprint& fingerprint) noexcept;

// 목적: SourceFingerprint가 catalog에 저장 가능한 형태인지 확인
// 입력: fingerprint: file metadata와 optional SHA-256를 포함한 fingerprint
// 출력: 음수가 아닌 metadata와 없거나 32 byte인 hash면 true
[[nodiscard]] bool isValidSourceFingerprint(const SourceFingerprint& fingerprint) noexcept;

}  // namespace flexraw::core::types
