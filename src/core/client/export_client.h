#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "client_error.h"
#include "client_result.h"
#include "editor_client.h"
#include "worker_profile_client.h"

namespace flexraw::core::client
{

struct ExportRequestId
{
    std::uint64_t value{0};

    bool operator==(const ExportRequestId&) const = default;
};

enum class ExportPlacementPolicy : std::uint8_t
{
    LocalOnly,
    RemoteOnly,
    Auto,
};

enum class ExportRasterFormat : std::uint8_t
{
    Jpeg,
    Png,
    Tiff,
};

enum class ExportTiffCompression : std::uint8_t
{
    None,
    Lzw,
};

enum class ExportOutputColorSpace : std::uint8_t
{
    Srgb,
    AdobeRgb,
    DisplayP3,
};

enum class ExportFailureKind : std::uint8_t
{
    None,
    Eligibility,
    DispatchExhausted,
    Execution,
    Ambiguous,
};

struct ExportRasterOptions
{
    ExportRasterFormat format{ExportRasterFormat::Jpeg};
    std::int32_t jpegQuality{90};
    std::int32_t pngCompression{6};
    ExportTiffCompression tiffCompression{ExportTiffCompression::Lzw};
    std::int32_t maximumDimension{0};
    ExportOutputColorSpace outputColorSpace{ExportOutputColorSpace::Srgb};
    bool includeMetadata{true};

    bool operator==(const ExportRasterOptions&) const = default;
};

struct ExportFileRequest
{
    EditorSourceSnapshot source;
    std::string outputLocator;
    std::string catalogLocator;
    std::optional<EditorDevelopParams> developParams;
    ExportRasterOptions options;
    bool useDefaultDevelopParamsWhenCatalogPhotoMissing{false};

    bool operator==(const ExportFileRequest&) const = default;
};

struct ExportBatchRequest
{
    std::string inputFolderLocator;
    std::string outputFolderLocator;
    std::string catalogLocator;
    std::optional<EditorDevelopParams> developParams;
    std::uint32_t workerCount{0};
    ExportRasterOptions options;

    bool operator==(const ExportBatchRequest&) const = default;
};

struct ExportItemListRequest
{
    std::vector<ExportFileRequest> items;

    bool operator==(const ExportItemListRequest&) const = default;
};

using ExportRequest = std::variant<ExportFileRequest, ExportBatchRequest, ExportItemListRequest>;

struct ExportPlacementOptions
{
    ExportPlacementPolicy policy{ExportPlacementPolicy::LocalOnly};
    std::optional<WorkerProfileId> workerProfileId;

    bool operator==(const ExportPlacementOptions&) const = default;
};

struct SubmitExportCommand
{
    ExportRequest request;
    ExportPlacementOptions placement;

    bool operator==(const SubmitExportCommand&) const = default;
};

struct ExportSchedulingMetrics
{
    std::uint64_t submitted{0};
    std::uint64_t succeeded{0};
    std::uint64_t failed{0};
    std::uint64_t cancelled{0};
    std::uint64_t ambiguous{0};
    std::uint64_t localDispatchAttempts{0};
    std::uint64_t remoteDispatchAttempts{0};
    std::uint64_t serverBusyCount{0};
    std::uint64_t resourceBusyCount{0};
    std::uint64_t connectionFailedCount{0};
    std::uint64_t localExecuted{0};
    std::uint64_t remoteExecuted{0};

    bool operator==(const ExportSchedulingMetrics&) const = default;
};

struct ExportItemResult
{
    std::string sourceLocator;
    std::string outputLocator;
    bool succeeded{false};
    std::optional<ClientError> error;
    ExportFailureKind failureKind{ExportFailureKind::None};

    bool operator==(const ExportItemResult&) const = default;
};

struct ExportReport
{
    std::uint64_t totalCount{0};
    std::uint64_t succeededCount{0};
    std::uint64_t failedCount{0};
    std::vector<ExportItemResult> items;
    ExportSchedulingMetrics scheduling;

    bool operator==(const ExportReport&) const = default;
};

struct ExportRequestReceipt
{
    ExportRequestId requestId;

    bool operator==(const ExportRequestReceipt&) const = default;
};

struct ExportProgress
{
    ExportRequestId requestId;
    std::uint64_t completedCount{0};
    std::uint64_t totalCount{0};
    std::uint64_t succeededCount{0};
    std::uint64_t failedCount{0};
    std::string currentSourceLocator;

    bool operator==(const ExportProgress&) const = default;
};

struct ExportResult
{
    ExportRequestId requestId;
    ExportReport report;

    bool operator==(const ExportResult&) const = default;
};

struct ExportIssue
{
    ExportRequestId requestId;
    ClientError error;

    bool operator==(const ExportIssue&) const = default;
};

struct ExportCancellation
{
    ExportRequestId requestId;

    bool operator==(const ExportCancellation&) const = default;
};

struct ActiveExportSnapshot
{
    ExportRequestId requestId;
    std::uint64_t completedCount{0};
    std::uint64_t totalCount{0};
    std::uint64_t succeededCount{0};
    std::uint64_t failedCount{0};

    bool operator==(const ActiveExportSnapshot&) const = default;
};

struct ExportSnapshot
{
    std::vector<ActiveExportSnapshot> activeRequests;
    ExportSchedulingMetrics scheduling;

    bool operator==(const ExportSnapshot&) const = default;
};

struct ExportEventSequence
{
    std::uint64_t value{0};

    bool operator==(const ExportEventSequence&) const = default;
};

struct ExportEvent
{
    ExportEventSequence sequence;
    bool initial{false};
    ExportSnapshot snapshot;
    std::optional<ExportRequestReceipt> accepted;
    std::optional<ExportProgress> progress;
    std::optional<ExportResult> completed;
    std::optional<ExportIssue> failed;
    std::optional<ExportCancellation> cancelled;

    bool operator==(const ExportEvent&) const = default;
};

using ExportSubmissionResult = ClientResult<ExportRequestReceipt, ClientError>;
using ExportCancellationResult = ClientResult<ExportRequestId, ClientError>;
using ExportSnapshotResult = ClientResult<ExportSnapshot, ClientError>;
using ExportCallback = std::function<void(const ExportEvent&)>;

class IExportSubscription
{
public:
    // 목적: implementation별 Export subscription resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IExportSubscription() = default;

    // 목적: queued event와 이후 Export callback 전달 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using ExportSubscriptionHandle = std::shared_ptr<IExportSubscription>;
using ExportSubscriptionResult = ClientResult<ExportSubscriptionHandle, ClientError>;

class IExportClient
{
public:
    // 목적: implementation별 Export command resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IExportClient() = default;

    // 목적: file·item-list·batch request를 placement policy와 함께 제출
    // 입력: command: immutable request와 Local/Remote/Auto, optional Worker profile identity
    // 출력: accepted request receipt 또는 즉시 validation·profile 오류
    [[nodiscard]] virtual ExportSubmissionResult submitExport(const SubmitExportCommand& command) = 0;

    // 목적: accepted Export request의 queued/running item cancellation 요청
    // 입력: requestId: owner가 발급한 Export request identity
    // 출력: 취소가 수락된 identity 또는 stale·validation 오류
    [[nodiscard]] virtual ExportCancellationResult cancelExport(ExportRequestId requestId) = 0;

    // 목적: 전체 active Export 또는 지정 request의 immutable 현재 상태 조회
    // 입력: requestId: 생략하면 전체 active request, 지정하면 해당 active request만 조회
    // 출력: active progress와 application lifetime scheduling metric 또는 not-found 오류
    [[nodiscard]] virtual ExportSnapshotResult exportSnapshot(
        std::optional<ExportRequestId> requestId = std::nullopt) const = 0;
};

class IExportEventSource
{
public:
    // 목적: implementation별 Export event resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IExportEventSource() = default;

    // 목적: initial active snapshot과 이후 accepted·progress·exact terminal event 구독
    // 입력: callback: immutable Export event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual ExportSubscriptionResult subscribeToExports(ExportCallback callback) = 0;
};

}  // namespace flexraw::core::client
