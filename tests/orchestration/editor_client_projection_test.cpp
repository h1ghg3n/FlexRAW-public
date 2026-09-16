#include <array>
#include <utility>

#include <gtest/gtest.h>

#include "editor_client_projection.h"

namespace flexraw::core::orchestration
{
namespace
{

TEST(EditorClientProjectionTest, RoundTripsEverySourceBindingState)
{
    constexpr std::array mappings{
        std::pair{catalog::SourceBindingState::FingerprintPending, client::CatalogSourceState::FingerprintPending},
        std::pair{catalog::SourceBindingState::Available, client::CatalogSourceState::Available},
        std::pair{catalog::SourceBindingState::Missing, client::CatalogSourceState::Missing},
        std::pair{catalog::SourceBindingState::VerificationRequired, client::CatalogSourceState::VerificationRequired},
        std::pair{catalog::SourceBindingState::IdentityUnverified, client::CatalogSourceState::IdentityUnverified},
        std::pair{catalog::SourceBindingState::ReplacementDetected, client::CatalogSourceState::ReplacementDetected},
        std::pair{catalog::SourceBindingState::Unreadable, client::CatalogSourceState::Unreadable},
        std::pair{catalog::SourceBindingState::Unlinked, client::CatalogSourceState::Unlinked},
    };

    for (const auto& [domainState, clientState] : mappings)
    {
        EditorState state;
        state.sourceState = domainState;

        const client::EditorSnapshot projected = toClientEditorSnapshot(state, false);
        const EditorState restored = fromClientEditorSnapshot(projected);

        ASSERT_TRUE(projected.sourceState.has_value());
        EXPECT_EQ(clientState, *projected.sourceState);
        ASSERT_TRUE(restored.sourceState.has_value());
        EXPECT_EQ(domainState, *restored.sourceState);
    }
}

}  // namespace
}  // namespace flexraw::core::orchestration
