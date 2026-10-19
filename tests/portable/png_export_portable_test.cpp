#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <png.h>

#include <gtest/gtest.h>

#include "owned_rgba8_image.h"
#include "png_artifact_publisher.h"
#include "png_encoder.h"

namespace flexraw::core::export_
{
namespace
{

class TemporaryDirectory final
{
public:
    // 목적: portable publisher test가 단독 소유할 temporary directory 생성
    // 입력: 없음
    // 출력: test 종료 시 recursive cleanup되는 directory owner
    TemporaryDirectory()
    {
        std::random_device random;
        for (int attempt = 0; attempt < 16 && m_path.empty(); ++attempt)
        {
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path() / ("flexraw-png-test-" + std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error))
            {
                m_path = candidate;
            }
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    // 목적: test-owned temporary directory 제거
    // 입력: 없음
    // 출력: 없음
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    // 목적: test-owned directory path 반환
    // 입력: 없음
    // 출력: fixture lifetime 동안 유효한 path
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

// 목적: test RGBA storage를 complete PNG bytes로 encode
// 입력: bytes/width/height/rowStride: source fixture layout
// 출력: encode 성공을 assert한 move-only PNG owner
[[nodiscard]] EncodedPng8 encodeFixture(std::vector<std::uint8_t> bytes,
                                        const std::uint32_t width,
                                        const std::uint32_t height,
                                        const std::size_t rowStride)
{
    std::optional<image::OwnedRgba8Image> source =
        image::OwnedRgba8Image::create(width, height, rowStride, std::move(bytes));
    EXPECT_TRUE(source.has_value());
    PngEncodingResult encoded = encodePng8Srgb(source->constView());
    EXPECT_TRUE(encoded.hasValue());
    return std::move(encoded.value());
}

// 목적: libpng simplified API로 encoded fixture를 RGBA8 pixel로 decode
// 입력: encoded: complete PNG byte owner
// 출력: decode 성공을 assert한 tight RGBA8 bytes
[[nodiscard]] std::vector<std::uint8_t> decodeFixture(const EncodedPng8& encoded)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    EXPECT_NE(0, png_image_begin_read_from_memory(&image, encoded.bytes().data(), encoded.bytes().size()));
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    EXPECT_NE(0, png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr));
    png_image_free(&image);
    return pixels;
}

// 목적: directory 안의 Flexraw CLI temporary entry 수 계산
// 입력: directory: publisher output parent
// 출력: `.flexraw-cli-` prefix를 가진 entry 수
[[nodiscard]] std::size_t temporaryEntryCount(const std::filesystem::path& directory)
{
    return static_cast<std::size_t>(std::count_if(
        std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator(), [](const auto& entry) {
            return entry.path().filename().string().starts_with(".flexraw-cli-");
        }));
}

TEST(PngExportPortableTest, EncodesPaddedRgba8WithSrgbChunkAndAlpha)
{
    const std::vector<std::uint8_t> source{255, 0, 0,   64, 0,   255, 0,   192, 9, 9, 9, 9,
                                           0,   0, 255, 32, 255, 255, 255, 255, 8, 8, 8, 8};
    EncodedPng8 encoded = encodeFixture(source, 2, 2, 12);

    constexpr std::array<std::uint8_t, 8> Signature{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    ASSERT_GE(encoded.bytes().size(), Signature.size());
    EXPECT_TRUE(std::equal(Signature.begin(), Signature.end(), encoded.bytes().begin()));
    const std::array<std::uint8_t, 4> SrgbChunk{'s', 'R', 'G', 'B'};
    EXPECT_NE(encoded.bytes().end(),
              std::search(encoded.bytes().begin(), encoded.bytes().end(), SrgbChunk.begin(), SrgbChunk.end()));
    EXPECT_EQ((std::vector<std::uint8_t>{255, 0, 0, 64, 0, 255, 0, 192, 0, 0, 255, 32, 255, 255, 255, 255}),
              decodeFixture(encoded));
}

TEST(PngExportPortableTest, PublishesCompleteArtifactAndRemovesTemporaryDirectory)
{
    TemporaryDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    const std::filesystem::path output = directory.path() / "result.png";
    EncodedPng8 encoded = encodeFixture({255, 0, 0, 255}, 1, 1, 4);
    const std::vector<std::uint8_t> expected(encoded.bytes().begin(), encoded.bytes().end());

    PngArtifactPublishResult result = publishPngArtifactNoReplace(std::move(encoded), output);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(std::filesystem::absolute(output).lexically_normal(), result.value().path);
    EXPECT_EQ(expected.size(), result.value().byteSize);
    std::ifstream stream(output, std::ios::binary);
    const std::vector<std::uint8_t> actual{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(0U, temporaryEntryCount(directory.path()));
}

TEST(PngExportPortableTest, PreservesExistingOutputAndCleansTemporaryState)
{
    TemporaryDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    const std::filesystem::path output = directory.path() / "sentinel.png";
    {
        std::ofstream stream(output, std::ios::binary);
        stream << "sentinel";
    }
    EncodedPng8 encoded = encodeFixture({0, 0, 255, 255}, 1, 1, 4);

    const PngArtifactPublishResult result = publishPngArtifactNoReplace(std::move(encoded), output);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(PngArtifactPublishErrorCode::OutputExists, result.error().code);
    std::ifstream stream(output, std::ios::binary);
    EXPECT_EQ("sentinel", std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
    EXPECT_EQ(0U, temporaryEntryCount(directory.path()));
}

TEST(PngExportPortableTest, RejectsMissingParentWithoutCreatingFinalArtifact)
{
    TemporaryDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    const std::filesystem::path output = directory.path() / "missing" / "result.png";
    EncodedPng8 encoded = encodeFixture({0, 0, 0, 255}, 1, 1, 4);

    const PngArtifactPublishResult result = publishPngArtifactNoReplace(std::move(encoded), output);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(PngArtifactPublishErrorCode::ParentNotFound, result.error().code);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(0U, temporaryEntryCount(directory.path()));
}

}  // namespace
}  // namespace flexraw::core::export_
