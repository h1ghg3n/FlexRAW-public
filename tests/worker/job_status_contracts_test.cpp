#include <type_traits>

#include <gtest/gtest.h>

#include "job_id.h"
#include "job_status_contracts.h"

namespace flexraw::worker::runtime
{
namespace
{

static_assert(!std::is_same_v<RenderJobId, protocol::JobId>);

TEST(JobStatusContractsTest, DistinguishesActiveAndTerminalStates)
{
    EXPECT_FALSE(isTerminalRenderJobState(RenderJobState::Queued));
    EXPECT_FALSE(isTerminalRenderJobState(RenderJobState::Running));
    EXPECT_TRUE(isTerminalRenderJobState(RenderJobState::Succeeded));
    EXPECT_TRUE(isTerminalRenderJobState(RenderJobState::Failed));
    EXPECT_TRUE(isTerminalRenderJobState(RenderJobState::Cancelled));
}

TEST(JobStatusContractsTest, CorrelatesStatusWithRuntimeOwnedSessionScopedJobIdentity)
{
    const RenderJobStatus status{{7, {11}}, RenderJobState::Running};

    EXPECT_EQ(7U, status.key.sessionId);
    EXPECT_EQ(11U, status.key.jobId.value);
    EXPECT_EQ(RenderJobState::Running, status.state);
}

}  // namespace
}  // namespace flexraw::worker::runtime
