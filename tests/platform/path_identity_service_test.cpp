#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "current_path_identity_service.h"

namespace flexraw::platform
{
namespace
{

class TemporaryDirectory final
{
public:
    // 목적: current filesystem path identity test용 고유 임시 directory 생성
    // 입력: 없음
    // 출력: 생성 결과는 isValid로 확인하는 scope-bound directory
    TemporaryDirectory()
    {
        static std::atomic_uint64_t nextId{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("flexraw-path-identity-" + std::to_string(timestamp) + "-" + std::to_string(nextId++));
        std::error_code error;
        m_valid = std::filesystem::create_directories(m_path, error) && !error;
    }

    // 목적: test가 만든 임시 directory tree를 scope 종료 시 제거
    // 입력: 없음
    // 출력: 생성된 file과 directory의 best-effort 삭제
    ~TemporaryDirectory()
    {
        std::error_code error;
        (void)std::filesystem::remove_all(m_path, error);
    }

    // 목적: 임시 directory 생성 성공 여부 조회
    // 입력: 없음
    // 출력: path를 test fixture로 사용할 수 있으면 true
    [[nodiscard]] bool isValid() const noexcept
    {
        return m_valid;
    }

    // 목적: 생성된 임시 directory 절대 경로 조회
    // 입력: 없음
    // 출력: 이 object lifetime 동안 유효한 path reference
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
    bool m_valid{false};
};

TEST(PathIdentityServiceTest, MatchesCurrentFilesystemCasePolicyForProspectiveSiblingNames)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path firstPath = directory.path() / "CaseProbe.flexraw";
    const std::filesystem::path alternatePath = directory.path() / "caseprobe.flexraw";
    std::ofstream(firstPath).put('x');
    ASSERT_TRUE(std::filesystem::exists(firstPath));
    std::error_code error;
    const bool alternateResolvesToExistingFile = std::filesystem::exists(alternatePath, error) && !error;
    const std::unique_ptr<IPathIdentityService> service = createCurrentPathIdentityService();
    ASSERT_NE(nullptr, service);

    const std::optional<TransientPathKey> firstKey = service->comparisonKey(firstPath);
    const std::optional<TransientPathKey> alternateKey = service->comparisonKey(alternatePath);

    ASSERT_TRUE(firstKey.has_value());
    ASSERT_TRUE(alternateKey.has_value());
    EXPECT_EQ(alternateResolvesToExistingFile, *firstKey == *alternateKey);
}

TEST(PathIdentityServiceTest, DistinguishesParentIdentityAndRejectsUnresolvablePaths)
{
    TemporaryDirectory directory;
    ASSERT_TRUE(directory.isValid());
    const std::filesystem::path otherParent = directory.path() / "other";
    ASSERT_TRUE(std::filesystem::create_directory(otherParent));
    const std::unique_ptr<IPathIdentityService> service = createCurrentPathIdentityService();
    ASSERT_NE(nullptr, service);

    const std::optional<TransientPathKey> firstKey = service->comparisonKey(directory.path() / "result.jpg");
    const std::optional<TransientPathKey> secondKey = service->comparisonKey(otherParent / "result.jpg");

    ASSERT_TRUE(firstKey.has_value());
    ASSERT_TRUE(secondKey.has_value());
    EXPECT_NE(*firstKey, *secondKey);
    EXPECT_FALSE(service->comparisonKey(std::filesystem::path("relative.jpg")).has_value());
    EXPECT_FALSE(service->comparisonKey(directory.path() / "missing" / "result.jpg").has_value());
}

}  // namespace
}  // namespace flexraw::platform
