#pragma once

#include <QFileInfo>
#include <QString>

namespace flexraw::core::types {

enum class SupportedFileKind {
    Unknown,
    Raw,
    RasterImage,
};

enum class FileScanStatus {
    Pending,
    Ready,
    Unsupported,
    Failed,
};

struct FileDescriptor {
    QString path;
    QString extension;
    QString displayName;
    SupportedFileKind kind{SupportedFileKind::Unknown};
};

// 목적: QFileInfo 에서 catalog 와 preview 가 공유할 파일 식별 정보 생성
// 입력: fileInfo: 파일 경로와 이름을 포함한 QFileInfo 값, kind: 지원 파일 종류
// 출력: FileDescriptor 값
[[nodiscard]] inline FileDescriptor makeFileDescriptor(const QFileInfo& fileInfo, SupportedFileKind kind)
{
    return FileDescriptor{
        fileInfo.absoluteFilePath(),
        fileInfo.suffix().toLower(),
        fileInfo.fileName(),
        kind,
    };
}

} // namespace flexraw::core::types
