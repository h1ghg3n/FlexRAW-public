#include <optional>

#include <gtest/gtest.h>

#include "develop_history.h"

namespace flexraw::core::history
{
namespace
{

TEST(DevelopHistoryTest, RestoresSingleEditWithUndoAndRedo)
{
    DevelopHistory history;
    const QString photoPath = QStringLiteral("C:/photos/one.raw");
    const types::DevelopParams initialParams = history.selectPhoto(photoPath);
    types::DevelopParams changedParams;
    changedParams.exposureEv = 1.5F;

    EXPECT_EQ(0U, history.revision(photoPath));
    EXPECT_TRUE(history.update(photoPath, changedParams));
    EXPECT_EQ(1U, history.revision(photoPath));
    EXPECT_TRUE(history.canUndo(photoPath));
    EXPECT_FALSE(history.canRedo(photoPath));

    const std::optional<types::DevelopParams> undoneParams = history.undo(photoPath);
    ASSERT_TRUE(undoneParams.has_value());
    EXPECT_EQ(*undoneParams, initialParams);
    EXPECT_EQ(2U, history.revision(photoPath));
    EXPECT_TRUE(history.canRedo(photoPath));

    const std::optional<types::DevelopParams> redoneParams = history.redo(photoPath);
    ASSERT_TRUE(redoneParams.has_value());
    EXPECT_EQ(*redoneParams, changedParams);
    EXPECT_EQ(3U, history.revision(photoPath));
}

TEST(DevelopHistoryTest, CoalescesOneContinuousEditIntoSingleUndoStep)
{
    DevelopHistory history;
    const QString photoPath = QStringLiteral("C:/photos/one.raw");
    types::DevelopParams firstParams;
    firstParams.exposureEv = 0.4F;
    types::DevelopParams finalParams;
    finalParams.exposureEv = 1.2F;

    (void)history.selectPhoto(photoPath);
    history.beginEdit(photoPath);
    EXPECT_TRUE(history.update(photoPath, firstParams));
    EXPECT_TRUE(history.update(photoPath, finalParams));
    history.endEdit(photoPath);

    const std::optional<types::DevelopParams> undoneParams = history.undo(photoPath);
    ASSERT_TRUE(undoneParams.has_value());
    EXPECT_EQ(*undoneParams, types::DevelopParams{});
    EXPECT_FALSE(history.canUndo(photoPath));
}

TEST(DevelopHistoryTest, KeepsPhotoHistoriesIndependent)
{
    DevelopHistory history;
    const QString firstPhotoPath = QStringLiteral("C:/photos/one.raw");
    const QString secondPhotoPath = QStringLiteral("C:/photos/two.raw");
    types::DevelopParams firstPhotoParams;
    firstPhotoParams.saturation = 0.5F;

    EXPECT_TRUE(history.update(firstPhotoPath, firstPhotoParams));

    EXPECT_EQ(history.selectPhoto(secondPhotoPath), types::DevelopParams{});
    EXPECT_FALSE(history.canUndo(secondPhotoPath));
    EXPECT_EQ(history.selectPhoto(firstPhotoPath), firstPhotoParams);
}

}  // namespace
}  // namespace flexraw::core::history
