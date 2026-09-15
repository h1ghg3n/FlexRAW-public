#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

#include "qt_worker_profile_settings_adapter.h"

namespace flexraw::ui::settings
{
namespace
{

using core::client::ClientErrorCode;
using core::client::CreateWorkerProfileCommand;
using core::client::SetWorkerProfileEnabledCommand;
using core::client::UpdateWorkerProfileCommand;

// 목적: isolated INI store에 이미 초기화된 빈 Worker profile schema 생성
// 입력: settings: test별 temporary QSettings
// 출력: migration 없이 CRUD를 검증할 빈 schema
void initializeEmptySchema(QSettings& settings)
{
    settings.setValue(QStringLiteral("workerProfiles/schemaVersion"), 1);
    settings.setValue(QStringLiteral("workerProfiles/order"), QStringList{});
    settings.sync();
}

TEST(WorkerProfileSettingsAdapterTest, MigratesLegacyEndpointIntoDefaultProfile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const QString sourceStorageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString outputStorageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    settings.setValue(QStringLiteral("export/remoteHost"), QStringLiteral("192.168.1.80"));
    settings.setValue(QStringLiteral("export/remotePort"), 49000);
    settings.setValue(QStringLiteral("export/expectedSourceStorageId"), sourceStorageId);
    settings.setValue(QStringLiteral("export/expectedOutputStorageId"), outputStorageId);
    settings.setValue(QStringLiteral("export/localSourceRoot"), QStringLiteral("S:/raw"));
    QtWorkerProfileSettingsAdapter adapter(settings);

    const core::client::WorkerProfileListResult result = adapter.listWorkerProfiles();

    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(1U, result.value().size());
    EXPECT_EQ("192.168.1.80", result.value().front().host);
    EXPECT_EQ(49000, result.value().front().port);
    EXPECT_EQ(sourceStorageId.toStdString(), result.value().front().expectedSourceStorageId);
    EXPECT_EQ(outputStorageId.toStdString(), result.value().front().expectedOutputStorageId);
    EXPECT_EQ(QString::fromStdString(result.value().front().id.value),
              settings.value(QStringLiteral("export/preferredWorkerProfileId")).toString());
    EXPECT_FALSE(settings.contains(QStringLiteral("export/remoteHost")));
    EXPECT_FALSE(settings.contains(QStringLiteral("export/localSourceRoot")));
}

TEST(WorkerProfileSettingsAdapterTest, FreshStoreUsesCompatibilityEndpointAndDoesNotRecreateDeletedProfile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter adapter(settings);

    const core::client::WorkerProfileListResult initial = adapter.listWorkerProfiles();
    ASSERT_TRUE(initial.hasValue());
    ASSERT_EQ(1U, initial.value().size());
    EXPECT_EQ("127.0.0.1", initial.value().front().host);
    EXPECT_EQ(47331, initial.value().front().port);
    ASSERT_TRUE(adapter.removeWorkerProfile(initial.value().front().id).hasValue());

    QtWorkerProfileSettingsAdapter reopened(settings);
    const core::client::WorkerProfileListResult restored = reopened.listWorkerProfiles();
    ASSERT_TRUE(restored.hasValue());
    EXPECT_TRUE(restored.value().empty());
}

TEST(WorkerProfileSettingsAdapterTest, CrudPreservesStableIdentityAndOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    initializeEmptySchema(settings);
    QtWorkerProfileSettingsAdapter adapter(settings);
    const CreateWorkerProfileCommand firstCommand{"Desk", "127.0.0.1", 47331, true, {}, {}};
    const CreateWorkerProfileCommand secondCommand{"Jetson", "192.168.1.90", 48000, true, {}, {}};

    const core::client::WorkerProfileResult first = adapter.createWorkerProfile(firstCommand);
    const core::client::WorkerProfileResult second = adapter.createWorkerProfile(secondCommand);
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    const core::client::WorkerProfileResult updated = adapter.updateWorkerProfile(
        UpdateWorkerProfileCommand{first.value().id, "Desk Renamed", "localhost", 47332, true, {}, {}});
    ASSERT_TRUE(updated.hasValue());
    EXPECT_EQ(first.value().id, updated.value().id);
    ASSERT_TRUE(adapter.setWorkerProfileEnabled(SetWorkerProfileEnabledCommand{second.value().id, false}).hasValue());

    const core::client::WorkerProfileListResult profiles = adapter.listWorkerProfiles();
    ASSERT_TRUE(profiles.hasValue());
    ASSERT_EQ(2U, profiles.value().size());
    EXPECT_EQ(first.value().id, profiles.value()[0].id);
    EXPECT_EQ("Desk Renamed", profiles.value()[0].displayName);
    EXPECT_EQ(second.value().id, profiles.value()[1].id);
    EXPECT_FALSE(profiles.value()[1].enabled);
}

TEST(WorkerProfileSettingsAdapterTest, RejectsInvalidAndDuplicateProfiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    initializeEmptySchema(settings);
    QtWorkerProfileSettingsAdapter adapter(settings);
    ASSERT_TRUE(
        adapter.createWorkerProfile(CreateWorkerProfileCommand{"Desk", "localhost", 47331, true, {}, {}}).hasValue());

    const core::client::WorkerProfileResult duplicate =
        adapter.createWorkerProfile(CreateWorkerProfileCommand{" desk ", "other-host", 48000, true, {}, {}});
    const core::client::WorkerProfileResult invalidStorage = adapter.createWorkerProfile(
        CreateWorkerProfileCommand{"Jetson", "192.168.1.90", 48000, true, "not-a-uuid", {}});
    const core::client::WorkerProfileResult invalidHost =
        adapter.createWorkerProfile(CreateWorkerProfileCommand{"Remote", "   ", 48000, true, {}, {}});

    ASSERT_TRUE(duplicate.hasError());
    EXPECT_EQ(ClientErrorCode::Conflict, duplicate.error().code);
    ASSERT_TRUE(invalidStorage.hasError());
    EXPECT_EQ(ClientErrorCode::InvalidArgument, invalidStorage.error().code);
    ASSERT_TRUE(invalidHost.hasError());
    EXPECT_EQ(ClientErrorCode::InvalidArgument, invalidHost.error().code);
}

TEST(WorkerProfileSettingsAdapterTest, ReportsCorruptedStoredProfile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter adapter(settings);
    const core::client::WorkerProfileListResult initial = adapter.listWorkerProfiles();
    ASSERT_TRUE(initial.hasValue());
    ASSERT_EQ(1U, initial.value().size());
    settings.remove(QStringLiteral("workerProfiles/profiles/%1/host")
                        .arg(QString::fromStdString(initial.value().front().id.value)));

    const core::client::WorkerProfileListResult corrupted = adapter.listWorkerProfiles();

    ASSERT_TRUE(corrupted.hasError());
    EXPECT_EQ(ClientErrorCode::DatabaseError, corrupted.error().code);
}

}  // namespace
}  // namespace flexraw::ui::settings
