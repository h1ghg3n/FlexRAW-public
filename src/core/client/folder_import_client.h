#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "catalog_photo_client.h"
#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

struct FolderOperationId
{
    std::uint64_t value{0};

    bool operator==(const FolderOperationId&) const = default;
};

enum class FolderOperationKind : std::uint8_t
{
    Scan,
    Import,
};

struct ScanFolderCommand
{
    std::string folderPath;
};

struct ImportFolderCommand
{
    std::string folderPath;
    std::string expectedCatalogPath;
};

struct FolderOperationReceipt
{
    FolderOperationId id;
    FolderOperationKind kind{FolderOperationKind::Scan};
    std::string folderPath;

    bool operator==(const FolderOperationReceipt&) const = default;
};

struct FolderItemSnapshot
{
    std::string sourcePath;
    std::string extension;
    std::string displayName;
    CatalogFileKind kind{CatalogFileKind::Unknown};

    bool operator==(const FolderItemSnapshot&) const = default;
};

struct FolderScanCompletion
{
    std::vector<FolderItemSnapshot> items;

    bool operator==(const FolderScanCompletion&) const = default;
};

struct FolderImportCompletion
{
    std::int64_t discoveredCount{0};
    std::int64_t appliedCount{0};
    std::vector<ClientPhotoId> photoIds;

    bool operator==(const FolderImportCompletion&) const = default;
};

using FolderOperationCompletion = std::variant<FolderScanCompletion, FolderImportCompletion>;

enum class FolderOperationTerminalState : std::uint8_t
{
    Completed,
    Failed,
    Cancelled,
};

struct FolderOperationTerminal
{
    FolderOperationReceipt receipt;
    FolderOperationTerminalState state{FolderOperationTerminalState::Completed};
    std::optional<FolderOperationCompletion> completion;
    std::optional<ClientError> error;

    bool operator==(const FolderOperationTerminal&) const = default;
};

struct FolderOperationEventSequence
{
    std::uint64_t value{0};

    bool operator==(const FolderOperationEventSequence&) const = default;
};

struct FolderOperationEvent
{
    FolderOperationEventSequence sequence;
    bool initial{false};
    std::optional<FolderOperationReceipt> activeOperation;
    std::optional<FolderOperationTerminal> terminal;

    bool operator==(const FolderOperationEvent&) const = default;
};

using FolderOperationResult = ClientResult<FolderOperationReceipt, ClientError>;
using FolderOperationCallback = std::function<void(const FolderOperationEvent&)>;

class IFolderOperationSubscription
{
public:
    // 목적: implementation별 Folder operation subscription resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IFolderOperationSubscription() = default;

    // 목적: queued event와 이후 Folder operation callback 전달 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using FolderOperationSubscriptionHandle = std::shared_ptr<IFolderOperationSubscription>;
using FolderOperationSubscriptionResult = ClientResult<FolderOperationSubscriptionHandle, ClientError>;

class IFolderImportClient
{
public:
    // 목적: implementation별 Folder operation command resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IFolderImportClient() = default;

    // 목적: Catalog session 없이도 사용할 수 있는 단일 folder scan 제출
    // 입력: command: scan할 UTF-8 folder path
    // 출력: accepted operation receipt 또는 validation·busy 오류
    [[nodiscard]] virtual FolderOperationResult submitFolderScan(const ScanFolderCommand& command) = 0;

    // 목적: expected Catalog session에 묶인 단일 folder scan과 persistence 제출
    // 입력: command: scan folder와 submit 시점의 normalized Catalog path
    // 출력: accepted operation receipt 또는 validation·session·busy 오류
    [[nodiscard]] virtual FolderOperationResult submitFolderImport(const ImportFolderCommand& command) = 0;
};

class IFolderImportEventSource
{
public:
    // 목적: implementation별 Folder operation event resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IFolderImportEventSource() = default;

    // 목적: adapter delivery context에서 initial active state와 이후 exact terminal 구독
    // 입력: callback: immutable Folder operation event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual FolderOperationSubscriptionResult subscribeToFolderOperations(
        FolderOperationCallback callback) = 0;
};

}  // namespace flexraw::core::client
