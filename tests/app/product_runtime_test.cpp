#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "catalog_photo_client.h"
#include "editor_client.h"
#include "editor_orchestrator.h"
#include "preview_pipeline.h"
#include "product_runtime.h"
#include "source_resolution_client.h"

namespace flexraw::runtime
{
namespace
{

class DormantPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: Product Runtime ownership test에서 실행될 필요가 없는 Preview를 명시적으로 종료
    // 입력: request/tier/cancellationToken: interface 일치를 위한 미사용 Preview context
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Product Runtime test does not render previews.")});
    }
};

// 목적: Product Runtime의 QObject owner와 queued event test에 필요한 application instance 보장
// 입력: 없음
// 출력: process lifetime 동안 유지되는 QCoreApplication instance
void ensureCoreApplication()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "flexraw_product_runtime_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
}

TEST(ProductRuntimeTest, SharesCatalogEditorAndSourceEventAuthorityWithBoundedLifetime)
{
    ensureCoreApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("runtime.flexraw-catalog"));
    std::vector<core::client::SourceResolutionEvent> events;
    core::client::SourceResolutionSubscriptionHandle subscription;

    {
        ProductRuntime runtime(std::make_unique<DormantPreviewPipeline>());
        ASSERT_TRUE(runtime.catalogOrchestrator().openCatalog(catalogPath).hasValue());

        core::client::ICatalogPhotoClient& catalogPhotoClient = runtime.catalogOrchestrator();
        const core::client::CatalogPhotoPageResult page =
            catalogPhotoClient.queryPhotoPage(core::client::CatalogPhotoPageRequest{});
        ASSERT_TRUE(page.hasValue());
        EXPECT_TRUE(page.value().photos.empty());

        core::client::IEditorClient& editorClient = runtime.editorOrchestrator();
        EXPECT_FALSE(editorClient.editorSnapshot().hasSelection);

        const core::client::SourceResolutionSubscriptionResult subscribed =
            runtime.sourceResolutionEventSource().subscribeToSourceResolution(
                [&events](const core::client::SourceResolutionEvent& event) { events.push_back(event); });
        ASSERT_TRUE(subscribed.hasValue());
        subscription = subscribed.value();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        ASSERT_EQ(events.size(), 1);
        EXPECT_TRUE(events.front().initial);
    }

    ASSERT_NE(subscription, nullptr);
    EXPECT_FALSE(subscription->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(events.size(), 1);
}

}  // namespace
}  // namespace flexraw::runtime
