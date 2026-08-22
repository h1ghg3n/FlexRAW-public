#include <QPushButton>

#include <gtest/gtest.h>

#include "source_resolution_widget.h"

namespace flexraw::ui::catalog
{
namespace
{

// 목적: source resolution widget test용 선택 Editor state 생성
// 입력: sourceState: 표시할 binding state, capabilities: 노출할 command 집합
// 출력: stable PhotoId selection을 가진 최소 EditorState
[[nodiscard]] core::orchestration::EditorState makeEditorState(
    core::catalog::SourceBindingState sourceState, core::orchestration::SourceResolutionCapabilities capabilities)
{
    core::orchestration::EditorState state;
    state.hasSelection = true;
    state.photo.photoId = core::types::PhotoId{17};
    state.sourceState = sourceState;
    state.sourceResolution = capabilities;
    return state;
}

TEST(SourceResolutionWidgetTest, ShowsOnlyCommandsAllowedByEditorCapabilities)
{
    SourceResolutionWidget widget;
    QPushButton* const accept = widget.findChild<QPushButton*>(QStringLiteral("acceptReplacementButton"));
    QPushButton* const registerAsNew = widget.findChild<QPushButton*>(QStringLiteral("registerReplacementAsNewButton"));
    QPushButton* const relink = widget.findChild<QPushButton*>(QStringLiteral("relinkSourceButton"));
    ASSERT_NE(nullptr, accept);
    ASSERT_NE(nullptr, registerAsNew);
    ASSERT_NE(nullptr, relink);
    int acceptCount = 0;
    int registerAsNewCount = 0;
    int relinkCount = 0;
    QObject::connect(&widget, &SourceResolutionWidget::acceptReplacementRequested, [&acceptCount] { ++acceptCount; });
    QObject::connect(&widget, &SourceResolutionWidget::registerReplacementAsNewRequested, [&registerAsNewCount] {
        ++registerAsNewCount;
    });
    QObject::connect(&widget, &SourceResolutionWidget::relinkSourceRequested, [&relinkCount] { ++relinkCount; });

    widget.setEditorState(makeEditorState(core::catalog::SourceBindingState::ReplacementDetected, {true, true, true}));

    EXPECT_FALSE(widget.isHidden());
    EXPECT_FALSE(accept->isHidden());
    EXPECT_FALSE(registerAsNew->isHidden());
    EXPECT_FALSE(relink->isHidden());
    accept->click();
    registerAsNew->click();
    relink->click();
    EXPECT_EQ(1, acceptCount);
    EXPECT_EQ(1, registerAsNewCount);
    EXPECT_EQ(1, relinkCount);

    widget.setEditorState(makeEditorState(core::catalog::SourceBindingState::Missing, {false, false, true}));
    EXPECT_TRUE(accept->isHidden());
    EXPECT_TRUE(registerAsNew->isHidden());
    EXPECT_FALSE(relink->isHidden());
}

TEST(SourceResolutionWidgetTest, DisablesCommandsWhileRequestIsPendingAndHidesNormalState)
{
    SourceResolutionWidget widget;
    QPushButton* const accept = widget.findChild<QPushButton*>(QStringLiteral("acceptReplacementButton"));
    ASSERT_NE(nullptr, accept);
    widget.setEditorState(makeEditorState(core::catalog::SourceBindingState::IdentityUnverified, {true, true, false}));

    widget.setRequestPending(true);

    EXPECT_TRUE(widget.isRequestPending());
    EXPECT_FALSE(accept->isEnabled());
    widget.setEditorState(makeEditorState(core::catalog::SourceBindingState::Missing, {false, false, true}));
    QPushButton* const relink = widget.findChild<QPushButton*>(QStringLiteral("relinkSourceButton"));
    ASSERT_NE(nullptr, relink);
    EXPECT_FALSE(widget.isRequestPending());
    EXPECT_TRUE(relink->isEnabled());

    widget.setRequestPending(true);
    widget.setEditorState(makeEditorState(core::catalog::SourceBindingState::Available, {}));
    EXPECT_TRUE(widget.isHidden());
    EXPECT_FALSE(widget.isRequestPending());
}

}  // namespace
}  // namespace flexraw::ui::catalog
