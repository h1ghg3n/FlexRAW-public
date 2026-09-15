#include "linux_path_identity_service.h"

#include <cerrno>
#include <cstddef>
#include <memory>
#include <string>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flexraw::platform
{
namespace
{

// 목적: trivially-copyable native identity 값을 process-local key에 결합
// 입력: destination: key byte buffer, value: 결합할 고정 크기 값
// 출력: destination 뒤에 value의 현재 process byte 표현 추가
template<typename Value> void appendValue(std::string& destination, const Value& value)
{
    const auto* const begin = reinterpret_cast<const char*>(std::addressof(value));
    destination.append(begin, sizeof(Value));
}

// 목적: 감지 가능한 Linux directory casefold policy를 fail-closed 상태로 조회
// 입력: parentPath: 기존 parent directory 절대 경로
// 출력: casefold 여부, directory open 또는 ioctl 오류면 nullopt
[[nodiscard]] std::optional<bool> directoryUsesCaseFold(const std::filesystem::path& parentPath)
{
#if defined(FS_CASEFOLD_FL)
    const int descriptor = open(parentPath.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0)
    {
        return std::nullopt;
    }

    int flags = 0;
    const int queryResult = ioctl(descriptor, FS_IOC_GETFLAGS, &flags);
    const int queryError = errno;
    (void)close(descriptor);
    if (queryResult == 0)
    {
        return (flags & FS_CASEFOLD_FL) != 0;
    }
    if (queryError == ENOTTY || queryError == EOPNOTSUPP || queryError == EINVAL)
    {
        return false;
    }
    return std::nullopt;
#else
    (void)parentPath;
    return false;
#endif
}

}  // namespace

// 목적: Linux parent identity와 exact filename을 결합한 임시 path key 생성
// 입력: absolutePath: 기존 parent directory와 prospective leaf를 가진 절대 경로
// 출력: 같은 parent와 exact leaf를 비교할 key, native 조회 실패나 casefold parent면 nullopt
std::optional<TransientPathKey> LinuxPathIdentityService::comparisonKey(const std::filesystem::path& absolutePath) const
{
    const std::filesystem::path normalizedPath = absolutePath.lexically_normal();
    const std::filesystem::path parentPath = normalizedPath.parent_path();
    const std::string leaf = normalizedPath.filename().native();
    if (!normalizedPath.is_absolute() || parentPath.empty() || leaf.empty())
    {
        return std::nullopt;
    }

    struct stat parentIdentity{};
    if (stat(parentPath.c_str(), &parentIdentity) != 0 || !S_ISDIR(parentIdentity.st_mode))
    {
        return std::nullopt;
    }

    const std::optional<bool> caseFold = directoryUsesCaseFold(parentPath);
    if (!caseFold.has_value() || *caseFold)
    {
        return std::nullopt;
    }

    TransientPathKey key;
    key.bytes.reserve(sizeof(parentIdentity.st_dev) + sizeof(parentIdentity.st_ino) + leaf.size());
    appendValue(key.bytes, parentIdentity.st_dev);
    appendValue(key.bytes, parentIdentity.st_ino);
    key.bytes.append(leaf);
    return key;
}

}  // namespace flexraw::platform
