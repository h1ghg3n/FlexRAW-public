#include "png_artifact_publisher.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace flexraw::core::export_
{
namespace
{

constexpr int MaximumTemporaryDirectoryAttempts = 16;

class ScopedTemporaryDirectory final
{
public:
    // 목적: caller가 독점 생성한 temporary directory의 cleanup ownership 획득
    // 입력: path: 현재 operation만 소유하는 existing directory
    // 출력: scope 종료 시 best-effort cleanup을 수행하는 guard
    explicit ScopedTemporaryDirectory(std::filesystem::path path) : m_path(std::move(path)) {}

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;

    // 목적: owned temporary directory와 남은 내용을 best-effort로 제거
    // 입력: 없음
    // 출력: 없음
    ~ScopedTemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    // 목적: owned temporary directory path 반환
    // 입력: 없음
    // 출력: guard lifetime 동안 유효한 path 참조
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

    // 목적: owned temporary directory와 남은 내용을 즉시 제거
    // 입력: 없음
    // 출력: directory가 없어진 경우 true
    [[nodiscard]] bool cleanup() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        return !error;
    }

private:
    std::filesystem::path m_path;
};

// 목적: publication failure result 생성
// 입력: code: path, write, link 또는 cleanup 오류 분류
// 출력: 지정 오류를 보유한 실패 결과
[[nodiscard]] PngArtifactPublishResult makeFailure(const PngArtifactPublishErrorCode code)
{
    return PngArtifactPublishResult::failure(PngArtifactPublishError{code});
}

// 목적: output entry가 regular file, directory 또는 symlink로 이미 존재하는지 확인
// 입력: path: 검사할 final locator, queryError: filesystem 조회 오류 output
// 출력: directory entry가 존재하면 true
[[nodiscard]] bool pathEntryExists(const std::filesystem::path& path, std::error_code& queryError) noexcept
{
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, queryError);
    if (queryError == std::errc::no_such_file_or_directory)
    {
        queryError.clear();
        return false;
    }
    return !queryError && status.type() != std::filesystem::file_type::not_found;
}

// 목적: cross-process collision 가능성이 낮은 temporary directory suffix 생성
// 입력: 없음
// 출력: 128-bit random 값을 lowercase hexadecimal text로 표현한 suffix
[[nodiscard]] std::string randomSuffix()
{
    std::random_device random;
    std::array<std::uint32_t, 4> values{};
    for (std::uint32_t& value : values)
    {
        value = random();
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::uint32_t value : values)
    {
        stream << std::setw(8) << value;
    }
    return stream.str();
}

// 목적: output parent 안에 current operation 전용 temporary directory 생성
// 입력: parent: existing output parent directory
// 출력: 독점 생성에 성공한 sibling directory 또는 빈 값
[[nodiscard]] std::optional<std::filesystem::path> createTemporaryDirectory(const std::filesystem::path& parent)
{
    for (int attempt = 0; attempt < MaximumTemporaryDirectoryAttempts; ++attempt)
    {
        const std::filesystem::path candidate = parent / (".flexraw-cli-" + randomSuffix());
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error))
        {
            return candidate;
        }
        if (error && error != std::errc::file_exists)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

PngArtifactPublishResult publishPngArtifactNoReplace(EncodedPng8 encoded, const std::filesystem::path& outputPath)
{
    if (outputPath.empty() || outputPath.filename().empty() || encoded.bytes().empty())
    {
        return makeFailure(PngArtifactPublishErrorCode::InvalidPath);
    }

    std::error_code error;
    const std::filesystem::path absoluteOutput = std::filesystem::absolute(outputPath, error).lexically_normal();
    if (error || absoluteOutput.filename().empty())
    {
        return makeFailure(PngArtifactPublishErrorCode::InvalidPath);
    }

    const std::filesystem::path parent = absoluteOutput.parent_path();
    const std::filesystem::file_status parentStatus = std::filesystem::status(parent, error);
    if (error || parentStatus.type() == std::filesystem::file_type::not_found)
    {
        return makeFailure(PngArtifactPublishErrorCode::ParentNotFound);
    }
    if (!std::filesystem::is_directory(parentStatus))
    {
        return makeFailure(PngArtifactPublishErrorCode::ParentNotDirectory);
    }

    if (pathEntryExists(absoluteOutput, error))
    {
        return makeFailure(PngArtifactPublishErrorCode::OutputExists);
    }
    if (error)
    {
        return makeFailure(PngArtifactPublishErrorCode::PublicationFailed);
    }

    std::optional<std::filesystem::path> temporaryDirectory;
    try
    {
        temporaryDirectory = createTemporaryDirectory(parent);
    }
    catch (const std::system_error&)
    {
        return makeFailure(PngArtifactPublishErrorCode::TemporaryDirectoryCreationFailed);
    }
    if (!temporaryDirectory.has_value())
    {
        return makeFailure(PngArtifactPublishErrorCode::TemporaryDirectoryCreationFailed);
    }

    ScopedTemporaryDirectory temporaryGuard(*temporaryDirectory);
    const std::filesystem::path temporaryFile = temporaryGuard.path() / "artifact.png";
    std::ofstream stream(temporaryFile, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
    {
        return makeFailure(PngArtifactPublishErrorCode::TemporaryFileOpenFailed);
    }
    if (encoded.bytes().size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        return makeFailure(PngArtifactPublishErrorCode::TemporaryFileWriteFailed);
    }

    stream.write(reinterpret_cast<const char*>(encoded.bytes().data()),
                 static_cast<std::streamsize>(encoded.bytes().size()));
    stream.flush();
    stream.close();
    if (stream.fail())
    {
        return makeFailure(PngArtifactPublishErrorCode::TemporaryFileWriteFailed);
    }

    std::filesystem::create_hard_link(temporaryFile, absoluteOutput, error);
    if (error)
    {
        std::error_code existenceError;
        if (pathEntryExists(absoluteOutput, existenceError))
        {
            return makeFailure(PngArtifactPublishErrorCode::OutputExists);
        }
        return makeFailure(PngArtifactPublishErrorCode::PublicationFailed);
    }

    const std::uint64_t byteSize = static_cast<std::uint64_t>(encoded.bytes().size());
    const std::uint32_t width = encoded.width();
    const std::uint32_t height = encoded.height();
    if (!temporaryGuard.cleanup())
    {
        std::error_code rollbackError;
        std::filesystem::remove(absoluteOutput, rollbackError);
        return makeFailure(PngArtifactPublishErrorCode::TemporaryCleanupFailed);
    }
    return PngArtifactPublishResult::success(PngArtifact{absoluteOutput, byteSize, width, height});
}

}  // namespace flexraw::core::export_
