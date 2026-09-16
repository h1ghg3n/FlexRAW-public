#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "qt_export_settings_adapter.h"
#include "qt_worker_health_event_adapter.h"
#include "qt_worker_profile_settings_adapter.h"
#include "settings_dialog.h"
#include "worker_health_orchestrator.h"

namespace flexraw::ui::settings
{
namespace
{

using namespace std::chrono_literals;

// 목적: Qt event를 처리하며 Settings health presentation 조건 대기
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 predicate가 참이면 true
[[nodiscard]] bool waitUntil(const std::function<bool()>& predicate, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

class ReadyWorkerHealthProbe final : public core::orchestration::IWorkerHealthProbePort
{
public:
    // 목적: Settings test에 fixed ready/load observation 제공
    // 입력: target: 저장된 endpoint와 timeout
    // 출력: running 1/4, queued 2/8의 compatible snapshot
    [[nodiscard]] core::orchestration::WorkerHealthPortResult probe(
        const core::orchestration::WorkerHealthProbeTarget& target) const override
    {
        EXPECT_EQ("127.0.0.1", target.host);
        EXPECT_EQ(47331, target.port);
        EXPECT_EQ(2000ms, target.connectTimeout);
        EXPECT_EQ(2000ms, target.responseTimeout);
        return core::orchestration::WorkerHealthPortResult::success({core::client::WorkerReachability::Reachable,
                                                                     core::client::WorkerCompatibility::Compatible,
                                                                     core::client::WorkerServiceState::Ready,
                                                                     core::client::WorkerHealthLoadSnapshot{1, 2, 4, 8},
                                                                     9,
                                                                     std::nullopt});
    }
};

TEST(WorkerHealthSettingsTest, RunsOneShotProbeAndShowsReadyLoadSnapshot)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    const core::client::WorkerProfileResult profile =
        workerProfiles.createWorkerProfile({"Health Worker", "127.0.0.1", 47331, true, {}, {}});
    ASSERT_TRUE(profile.hasValue());
    core::orchestration::WorkerHealthOrchestrator health(workerProfiles, std::make_unique<ReadyWorkerHealthProbe>());
    QtWorkerHealthEventAdapter healthEvents(health);
    SettingsDialog dialog(storage, workerProfiles, exportDefaults, &health, &healthEvents);
    QListWidget* const list = dialog.findChild<QListWidget*>(QStringLiteral("workerProfileList"));
    QPushButton* const probeButton = dialog.findChild<QPushButton*>(QStringLiteral("workerHealthProbeButton"));
    QLabel* const summary = dialog.findChild<QLabel*>(QStringLiteral("workerHealthSummaryLabel"));
    QLabel* const load = dialog.findChild<QLabel*>(QStringLiteral("workerHealthLoadLabel"));
    ASSERT_NE(nullptr, list);
    ASSERT_NE(nullptr, probeButton);
    ASSERT_NE(nullptr, summary);
    ASSERT_NE(nullptr, load);

    int profileRow = -1;
    for (int row = 0; row < list->count(); ++row)
    {
        if (list->item(row)->data(Qt::UserRole).toString() == QString::fromStdString(profile.value().id.value))
        {
            profileRow = row;
            break;
        }
    }
    ASSERT_GE(profileRow, 0);
    list->setCurrentRow(profileRow);
    ASSERT_TRUE(probeButton->isEnabled());
    probeButton->click();

    ASSERT_TRUE(waitUntil([summary]() { return summary->text() == QStringLiteral("Worker is ready."); }));
    EXPECT_TRUE(probeButton->isEnabled());
    EXPECT_EQ(QStringLiteral("Running 1 of 4 · queued 2 of 8"), load->text());

    const core::client::WorkerHealthSnapshotResult cached = health.workerHealthSnapshot(profile.value().id);
    ASSERT_TRUE(cached.hasValue());
    EXPECT_EQ(core::client::WorkerCompatibility::Compatible, cached.value().compatibility);
}

TEST(WorkerHealthSettingsTest, ExplicitUnsubscribeDropsQueuedInitialEvent)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    core::orchestration::WorkerHealthOrchestrator health(workerProfiles, std::make_unique<ReadyWorkerHealthProbe>());
    QtWorkerHealthEventAdapter healthEvents(health);
    int callbackCount = 0;
    const core::client::WorkerHealthSubscriptionResult subscription = healthEvents.subscribeToWorkerHealth(
        [&callbackCount](const core::client::WorkerHealthEvent&) { ++callbackCount; });
    ASSERT_TRUE(subscription.hasValue());

    subscription.value()->unsubscribe();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    EXPECT_EQ(0, callbackCount);
    EXPECT_FALSE(subscription.value()->isActive());
}

}  // namespace
}  // namespace flexraw::ui::settings
