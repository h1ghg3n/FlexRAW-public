#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "activity_client.h"
#include "catalog_culling_client.h"
#include "catalog_folder_client.h"
#include "catalog_inclusion_client.h"
#include "catalog_photo_client.h"
#include "catalog_project_client.h"
#include "catalog_session_client.h"
#include "catalog_startup_settings_client.h"
#include "catalog_thumbnail_client.h"
#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"
#include "display_frame.h"
#include "editor_client.h"
#include "editor_event_client.h"
#include "export_client.h"
#include "export_defaults_client.h"
#include "folder_import_client.h"
#include "preview_presentation_client.h"
#include "source_resolution_client.h"
#include "worker_health_client.h"
#include "worker_profile_client.h"

namespace flexraw::core::client
{
namespace
{

class TestActivitySubscription final : public IActivitySubscription
{
public:
    // 목적: portable consumer가 subscription 전달 상태를 반복 안전하게 차단
    // 입력: 없음
    // 출력: active 상태를 false로 변경
    void unsubscribe() noexcept override
    {
        m_active = false;
    }

    // 목적: portable consumer가 이후 callback 전달 가능 여부 조회
    // 입력: 없음
    // 출력: unsubscribe 전이면 true
    [[nodiscard]] bool isActive() const noexcept override
    {
        return m_active;
    }

private:
    bool m_active{true};
};

static_assert(std::is_abstract_v<IActivityClient>);
static_assert(std::is_abstract_v<ICatalogCullingClient>);
static_assert(std::is_abstract_v<ICatalogFolderClient>);
static_assert(std::is_abstract_v<ICatalogInclusionClient>);
static_assert(std::is_abstract_v<ICatalogPhotoClient>);
static_assert(std::is_abstract_v<ICatalogProjectClient>);
static_assert(std::is_abstract_v<ICatalogSessionClient>);
static_assert(std::is_abstract_v<ICatalogStartupSettingsClient>);
static_assert(std::is_abstract_v<ICatalogThumbnailClient>);
static_assert(std::is_abstract_v<ICatalogThumbnailEventSource>);
static_assert(std::is_abstract_v<IEditorClient>);
static_assert(std::is_abstract_v<IEditorStateEventSource>);
static_assert(std::is_abstract_v<IExportClient>);
static_assert(std::is_abstract_v<IExportDefaultsClient>);
static_assert(std::is_abstract_v<IExportEventSource>);
static_assert(std::is_abstract_v<IFolderImportClient>);
static_assert(std::is_abstract_v<IFolderImportEventSource>);
static_assert(std::is_abstract_v<IPreviewComparisonClient>);
static_assert(std::is_abstract_v<IPreviewPresentationClient>);
static_assert(std::is_abstract_v<IPreviewPresentationEventSource>);
static_assert(std::is_abstract_v<ISourceResolutionClient>);
static_assert(std::is_abstract_v<ISourceResolutionEventSource>);
static_assert(std::is_abstract_v<IWorkerHealthClient>);
static_assert(std::is_abstract_v<IWorkerHealthEventSource>);
static_assert(std::is_abstract_v<IWorkerProfileClient>);

TEST(ClientSurfaceContractTest, PreservesFixedWidthIdentityAndBoundedPageDefaults)
{
    static_assert(std::is_same_v<decltype(ClientPhotoId::value), std::int64_t>);
    static_assert(std::is_same_v<decltype(ClientProjectId::value), std::int64_t>);
    static_assert(std::is_same_v<decltype(WorkerHealthProbeId::value), std::uint64_t>);

    EXPECT_EQ(100, DefaultCatalogPhotoPageSize);
    EXPECT_EQ(200, MaximumCatalogPhotoPageSize);
    EXPECT_EQ(200U, MaximumCatalogCullingBatchSize);
    EXPECT_EQ(200U, MaximumCatalogInclusionBatchSize);
    EXPECT_EQ(CatalogInclusionFilter::IncludedOnly, CatalogPhotoPageRequest{}.inclusionFilter);
    EXPECT_EQ(ClientPhotoId{42}, ClientPhotoId{42});
    EXPECT_NE(ClientProjectId{3}, ClientProjectId{4});
}

TEST(ClientSurfaceContractTest, KeepsCullingMutationSeparateFromBoundedReadFilter)
{
    CatalogPhotoPageRequest request;
    request.cullingFilter = CatalogCullingFilter::Pick;
    CatalogPhotoPageCursor cursor;
    cursor.cullingFilter = CatalogCullingFilter::Pick;
    request.cursor = cursor;
    SetCatalogCullingCommand command;
    command.expectedCatalogPath = "library.flexraw-catalog";
    command.state = CatalogCullingState::Reject;
    command.photoIds = {ClientPhotoId{7}, ClientPhotoId{3}, ClientPhotoId{7}};

    EXPECT_EQ(CatalogCullingFilter::Pick, request.cursor->cullingFilter);
    EXPECT_EQ(CatalogCullingState::Reject, command.state);
    EXPECT_EQ(3U, command.photoIds.size());
}

TEST(ClientSurfaceContractTest, KeepsInclusionMutationSeparateFromBoundedReadFilter)
{
    CatalogPhotoPageRequest request;
    request.inclusionFilter = CatalogInclusionFilter::ExcludedOnly;
    CatalogPhotoPageCursor cursor;
    cursor.inclusionFilter = CatalogInclusionFilter::ExcludedOnly;
    request.cursor = cursor;
    SetCatalogInclusionCommand command;
    command.expectedCatalogPath = "library.flexraw-catalog";
    command.state = CatalogInclusionState::Excluded;
    command.photoIds = {ClientPhotoId{7}, ClientPhotoId{3}, ClientPhotoId{7}};

    EXPECT_EQ(CatalogInclusionFilter::ExcludedOnly, request.cursor->inclusionFilter);
    EXPECT_EQ(CatalogInclusionState::Excluded, command.state);
    EXPECT_EQ(3U, command.photoIds.size());
}

TEST(ClientSurfaceContractTest, PreservesSuccessAndTypedFailureWithoutFrameworkTypes)
{
    const auto success = ClientResult<std::string, ClientError>::success("portable");
    const auto failure =
        ClientResult<std::string, ClientError>::failure(ClientError{ClientErrorCode::Conflict, "revision conflict"});

    ASSERT_TRUE(success.hasValue());
    EXPECT_EQ("portable", success.value());
    ASSERT_TRUE(failure.hasError());
    EXPECT_EQ(ClientErrorCode::Conflict, failure.error().code);
    EXPECT_EQ("revision conflict", failure.error().technicalMessage);
}

TEST(ClientSurfaceContractTest, SubscriptionHandleMakesUnsubscribeIdempotent)
{
    ActivitySubscriptionHandle subscription = std::make_shared<TestActivitySubscription>();

    ASSERT_TRUE(subscription->isActive());
    subscription->unsubscribe();
    EXPECT_FALSE(subscription->isActive());
    subscription->unsubscribe();
    EXPECT_FALSE(subscription->isActive());
}

TEST(ClientSurfaceContractTest, KeepsDefaultDevelopComparisonSeparateFromCurrentPreview)
{
    const std::optional<DisplayFrame> frame = DisplayFrame::create(1, 1, 4, 7, {1, 2, 3, 255});
    ASSERT_TRUE(frame.has_value());

    PreviewPresentationSnapshot snapshot{};
    snapshot.selectedPhotoId = ClientPhotoId{17};
    snapshot.developRevision = SessionDevelopRevision{8};
    snapshot.previewSequence = PreviewSequence{7};
    snapshot.activeRequestId = PreviewRequestId{21};
    snapshot.comparison.enabled = true;
    snapshot.comparison.activeRequestId = PreviewRequestId{22};
    snapshot.comparison.baselineFrame = PreviewComparisonFrameSnapshot{
        PreviewRequestId{22}, ClientPhotoId{17}, SessionDevelopRevision{6}, PreviewFrameTier::Standard, *frame};

    ASSERT_TRUE(snapshot.comparison.baselineFrame.has_value());
    EXPECT_NE(snapshot.activeRequestId, snapshot.comparison.activeRequestId);
    EXPECT_EQ(SessionDevelopRevision{6}, snapshot.comparison.baselineFrame->capturedDevelopRevision);
    EXPECT_EQ(SessionDevelopRevision{8}, snapshot.developRevision);
    EXPECT_EQ(4U, snapshot.comparison.baselineFrame->frame.bytes().size());

    PreviewPresentationEvent event{};
    event.snapshot = snapshot;
    event.comparison = PreviewComparisonTransition{false,
                                                   true,
                                                   std::nullopt,
                                                   PreviewTerminal{PreviewRequestId{22},
                                                                   ClientPhotoId{17},
                                                                   SessionDevelopRevision{6},
                                                                   PreviewSequence{5},
                                                                   PreviewTerminalState::Completed,
                                                                   std::nullopt}};
    ASSERT_TRUE(event.comparison.has_value());
    ASSERT_TRUE(event.comparison->terminal.has_value());
    EXPECT_EQ(PreviewRequestId{22}, event.comparison->terminal->requestId);
    EXPECT_FALSE(event.terminal.has_value());
    EXPECT_FALSE(event.frameUpdated);

    const PreviewComparisonResult disabled = PreviewComparisonResult::success(PreviewComparisonCommandState{});
    const PreviewComparisonResult rejected =
        PreviewComparisonResult::failure({ClientErrorCode::Conflict, "No selected Preview source."});
    EXPECT_FALSE(disabled.value().enabled);
    ASSERT_TRUE(rejected.hasError());
    EXPECT_EQ(ClientErrorCode::Conflict, rejected.error().code);
}

}  // namespace
}  // namespace flexraw::core::client
