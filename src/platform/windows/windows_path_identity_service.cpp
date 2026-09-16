#include "windows_path_identity_service.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

namespace flexraw::platform
{
namespace
{

constexpr ULONG CaseSensitiveDirectoryFlag = 0x00000001;

struct HandleCloser
{
    // 목적: 유효한 Windows native handle을 scope 종료 시 해제
    // 입력: handle: CreateFileW가 반환한 directory handle
    // 출력: 유효한 handle이면 CloseHandle 호출
    void operator()(void* handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// 목적: trivially-copyable native identity 값을 process-local key에 결합
// 입력: destination: key byte buffer, value: 결합할 고정 크기 값
// 출력: destination 뒤에 value의 현재 process byte 표현 추가
template<typename Value> void appendValue(std::string& destination, const Value& value)
{
    const auto* const begin = reinterpret_cast<const char*>(std::addressof(value));
    destination.append(begin, sizeof(Value));
}

// 목적: Windows case-insensitive filename 비교용 invariant uppercase 문자열 생성
// 입력: leaf: prospective output file의 native leaf 이름
// 출력: 변환 성공 시 uppercase leaf, Win32 변환 실패 시 nullopt
[[nodiscard]] std::optional<std::wstring> uppercaseLeaf(const std::wstring_view leaf)
{
    if (leaf.empty() || leaf.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }

    const int sourceLength = static_cast<int>(leaf.size());
    const int requiredLength = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, leaf.data(), sourceLength, nullptr, 0, nullptr, nullptr, 0);
    if (requiredLength <= 0)
    {
        return std::nullopt;
    }

    std::wstring uppercase(static_cast<std::size_t>(requiredLength), L'\0');
    const int written = LCMapStringEx(LOCALE_NAME_INVARIANT,
                                      LCMAP_UPPERCASE,
                                      leaf.data(),
                                      sourceLength,
                                      uppercase.data(),
                                      requiredLength,
                                      nullptr,
                                      nullptr,
                                      0);
    return written == requiredLength ? std::optional<std::wstring>{std::move(uppercase)} : std::nullopt;
}

}  // namespace

// 목적: Windows parent identity와 directory case policy를 반영한 임시 path key 생성
// 입력: absolutePath: 기존 parent directory와 prospective leaf를 가진 절대 경로
// 출력: 같은 parent와 Windows filename 의미를 비교할 key, native 조회 실패 시 nullopt
std::optional<TransientPathKey> WindowsPathIdentityService::comparisonKey(
    const std::filesystem::path& absolutePath) const
{
    const std::filesystem::path normalizedPath = absolutePath.lexically_normal();
    const std::filesystem::path parentPath = normalizedPath.parent_path();
    const std::wstring leaf = normalizedPath.filename().native();
    if (!normalizedPath.is_absolute() || parentPath.empty() || leaf.empty())
    {
        return std::nullopt;
    }

    UniqueHandle parentHandle(CreateFileW(parentPath.c_str(),
                                          FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                          nullptr));
    if (parentHandle.get() == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }

    FILE_ID_INFO identity{};
    FILE_CASE_SENSITIVE_INFO casePolicy{};
    if (GetFileInformationByHandleEx(parentHandle.get(), FileIdInfo, &identity, sizeof(identity)) == 0 ||
        GetFileInformationByHandleEx(parentHandle.get(), FileCaseSensitiveInfo, &casePolicy, sizeof(casePolicy)) == 0)
    {
        return std::nullopt;
    }

    const bool caseSensitive = (casePolicy.Flags & CaseSensitiveDirectoryFlag) != 0;
    const std::optional<std::wstring> comparableLeaf =
        caseSensitive ? std::optional<std::wstring>{leaf} : uppercaseLeaf(leaf);
    if (!comparableLeaf.has_value())
    {
        return std::nullopt;
    }

    TransientPathKey key;
    key.bytes.reserve(sizeof(identity.VolumeSerialNumber) + sizeof(identity.FileId.Identifier) + sizeof(caseSensitive) +
                      comparableLeaf->size() * sizeof(wchar_t));
    appendValue(key.bytes, identity.VolumeSerialNumber);
    key.bytes.append(reinterpret_cast<const char*>(identity.FileId.Identifier), sizeof(identity.FileId.Identifier));
    appendValue(key.bytes, caseSensitive);
    key.bytes.append(reinterpret_cast<const char*>(comparableLeaf->data()), comparableLeaf->size() * sizeof(wchar_t));
    return key;
}

}  // namespace flexraw::platform
