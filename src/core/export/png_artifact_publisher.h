#pragma once

#include <cstdint>
#include <filesystem>

#include "png_encoder.h"
#include "result.h"

namespace flexraw::core::export_
{

enum class PngArtifactPublishErrorCode : std::uint8_t
{
    InvalidPath,
    ParentNotFound,
    ParentNotDirectory,
    OutputExists,
    TemporaryDirectoryCreationFailed,
    TemporaryFileOpenFailed,
    TemporaryFileWriteFailed,
    PublicationFailed,
    TemporaryCleanupFailed,
};

struct PngArtifactPublishError
{
    PngArtifactPublishErrorCode code{PngArtifactPublishErrorCode::PublicationFailed};

    [[nodiscard]] bool operator==(const PngArtifactPublishError& other) const = default;
};

struct PngArtifact
{
    std::filesystem::path path;
    std::uint64_t byteSize{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

using PngArtifactPublishResult = types::Result<PngArtifact, PngArtifactPublishError>;

// 목적: complete PNG payload를 existing output을 덮어쓰지 않고 local artifact로 publish
// 입력: encoded: 소비할 PNG owner, outputPath: final local file path
// 출력: published artifact 정보 또는 path/write/publication 오류
[[nodiscard]] PngArtifactPublishResult publishPngArtifactNoReplace(EncodedPng8 encoded,
                                                                   const std::filesystem::path& outputPath);

}  // namespace flexraw::core::export_
