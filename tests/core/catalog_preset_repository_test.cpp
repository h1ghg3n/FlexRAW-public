#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_preset_repository.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: Qt SQL test에 필요한 단일 QCoreApplication instance 보장
// 입력: 없음
// 출력: 없음
void ensureCoreApplication()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }

    static int argumentCount = 1;
    static char applicationName[] = "flexraw_core_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
}

class CatalogPresetRepositoryTest : public testing::Test
{
protected:
    // 목적: CatalogPresetRepository test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        ensureCoreApplication();
    }
};

TEST_F(CatalogPresetRepositoryTest, SavesUpdatesListsAndRemovesPreset)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPresetRepository repository(*database.value());

    preset::PresetDefinition definition;
    definition.name = QStringLiteral("  Warm Portrait  ");
    definition.category = QStringLiteral("  Portraits ");
    definition.params.exposureEv = 0.75F;
    definition.params.vibrance = 0.35F;

    const CatalogPresetStoreResult saved = repository.save(definition);

    ASSERT_TRUE(saved.hasValue());
    EXPECT_GT(saved.value(), 0);
    const CatalogPresetLoadResult loaded = repository.load(saved.value());
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(QStringLiteral("Warm Portrait"), loaded.value().definition.name);
    EXPECT_EQ(QStringLiteral("Portraits"), loaded.value().definition.category);
    EXPECT_EQ(definition.params, loaded.value().definition.params);

    definition.params.exposureEv = -0.5F;
    const CatalogPresetStoreResult updated = repository.save(definition);
    ASSERT_TRUE(updated.hasValue());
    EXPECT_EQ(saved.value(), updated.value());

    const CatalogPresetListResult listed = repository.list();
    ASSERT_TRUE(listed.hasValue());
    ASSERT_EQ(1, listed.value().size());
    EXPECT_EQ(-0.5F, listed.value().front().definition.params.exposureEv);

    ASSERT_TRUE(repository.remove(saved.value()).hasValue());
    const CatalogPresetListResult afterRemoval = repository.list();
    ASSERT_TRUE(afterRemoval.hasValue());
    EXPECT_TRUE(afterRemoval.value().isEmpty());

    const CatalogPresetLoadResult missing = repository.load(saved.value());
    ASSERT_TRUE(missing.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, missing.error().code);
}

TEST_F(CatalogPresetRepositoryTest, RejectsInvalidPresetDefinitions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogPresetRepository repository(*database.value());

    preset::PresetDefinition emptyName;
    emptyName.name = QStringLiteral(" ");
    const CatalogPresetStoreResult emptyNameResult = repository.save(emptyName);
    ASSERT_TRUE(emptyNameResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, emptyNameResult.error().code);

    preset::PresetDefinition invalidParams;
    invalidParams.name = QStringLiteral("Invalid");
    invalidParams.params.contrast = 2.0F;
    const CatalogPresetStoreResult invalidParamsResult = repository.save(invalidParams);
    ASSERT_TRUE(invalidParamsResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidParamsResult.error().code);
}

}  // namespace
}  // namespace flexraw::core::catalog
