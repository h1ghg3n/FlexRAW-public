#include "worker_health_panel.h"

#include <utility>

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace flexraw::ui::settings
{
namespace
{

// 목적: optional Unix millisecond timestamp를 현재 locale의 짧은 local time으로 표시
// 입력: timestamp: Product Contract의 optional Unix millisecond
// 출력: 값이 있으면 localized text, 없으면 빈 문자열
[[nodiscard]] QString formattedTime(const std::optional<std::int64_t>& timestamp)
{
    if (!timestamp.has_value())
    {
        return {};
    }
    const QDateTime localTime = QDateTime::fromMSecsSinceEpoch(*timestamp).toLocalTime();
    return QLocale().toString(localTime, QLocale::ShortFormat);
}

}  // namespace

// 목적: 선택된 Worker profile의 manual one-shot health control과 snapshot 표시 구성
// 입력: healthClient/eventSource: optional Product health capability, parent: Qt 부모 widget
// 출력: Test/Refresh와 reachability/load/timing presentation을 가진 panel
WorkerHealthPanel::WorkerHealthPanel(core::client::IWorkerHealthClient* const healthClient,
                                     core::client::IWorkerHealthEventSource* const eventSource,
                                     QWidget* const parent)
    : QGroupBox(tr("Health"), parent),
      m_healthClient(healthClient),
      m_probeButton(new QPushButton(tr("Test / Refresh"), this)),
      m_summaryLabel(new QLabel(this)),
      m_loadLabel(new QLabel(this)),
      m_timingLabel(new QLabel(this))
{
    setObjectName(QStringLiteral("workerHealthPanel"));
    m_probeButton->setObjectName(QStringLiteral("workerHealthProbeButton"));
    m_summaryLabel->setObjectName(QStringLiteral("workerHealthSummaryLabel"));
    m_loadLabel->setObjectName(QStringLiteral("workerHealthLoadLabel"));
    m_timingLabel->setObjectName(QStringLiteral("workerHealthTimingLabel"));
    m_summaryLabel->setWordWrap(true);
    m_timingLabel->setWordWrap(true);

    auto* headingLayout = new QHBoxLayout();
    headingLayout->addWidget(m_summaryLabel, 1);
    headingLayout->addWidget(m_probeButton);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(headingLayout);
    layout->addWidget(m_loadLabel);
    layout->addWidget(m_timingLabel);
    connect(m_probeButton, &QPushButton::clicked, this, &WorkerHealthPanel::requestProbe);

    if (eventSource != nullptr)
    {
        const core::client::WorkerHealthSubscriptionResult subscription = eventSource->subscribeToWorkerHealth(
            [this](const core::client::WorkerHealthEvent& event) { applyEvent(event); });
        if (subscription.hasValue())
        {
            m_subscription = subscription.value();
        }
    }
    renderUnavailable();
}

// 목적: UI state보다 먼저 health callback 입구를 명시적으로 차단
// 입력: 없음
// 출력: queued/future callback이 이 panel에 도달하지 않는 상태
WorkerHealthPanel::~WorkerHealthPanel()
{
    m_subscription.reset();
}

// 목적: health control이 관찰할 현재 Worker profile 교체
// 입력: profile: 저장된 profile 또는 new-profile 편집 상태의 nullopt
// 출력: endpoint에 맞는 최근 snapshot 또는 Not checked 표시
void WorkerHealthPanel::setProfile(std::optional<core::client::WorkerProfileSnapshot> profile)
{
    m_profile = std::move(profile);
    if (!m_profile.has_value() || m_healthClient == nullptr)
    {
        renderUnavailable();
        return;
    }
    if (m_activeProbe.has_value() && m_activeProbe->profileId == m_profile->id)
    {
        m_probeButton->setEnabled(false);
        m_summaryLabel->setText(tr("Checking Worker health..."));
        m_loadLabel->clear();
        m_timingLabel->clear();
        return;
    }
    refreshSnapshot();
}

// 목적: 선택 profile에 one-shot probe를 제출하고 checking 상태 표시
// 입력: 없음
// 출력: accepted면 button 비활성화, immediate 오류면 일반 사용자 안내
void WorkerHealthPanel::requestProbe()
{
    if (!m_profile.has_value() || m_healthClient == nullptr)
    {
        return;
    }
    const core::client::WorkerHealthProbeResult result = m_healthClient->probeWorker(m_profile->id);
    if (result.hasError())
    {
        m_summaryLabel->setText(result.error().code == core::client::ClientErrorCode::Conflict
                                    ? tr("Another Worker health check is already running.")
                                    : tr("Worker health check could not be started."));
        return;
    }
    m_activeProbe = result.value();
    m_probeButton->setEnabled(false);
    m_summaryLabel->setText(tr("Checking Worker health..."));
    m_loadLabel->clear();
    m_timingLabel->clear();
}

// 목적: serialized Product health event를 현재 선택 profile presentation에 반영
// 입력: event: active probe와 optional terminal snapshot
// 출력: checking 또는 최신 health 표시
void WorkerHealthPanel::applyEvent(const core::client::WorkerHealthEvent& event)
{
    m_activeProbe = event.activeProbe;
    if (!m_profile.has_value())
    {
        renderUnavailable();
        return;
    }
    if (event.completion.has_value() && event.completion->receipt.profileId == m_profile->id)
    {
        renderSnapshot(event.completion->snapshot);
        return;
    }
    if (m_activeProbe.has_value() && m_activeProbe->profileId == m_profile->id)
    {
        m_probeButton->setEnabled(false);
        m_summaryLabel->setText(tr("Checking Worker health..."));
        m_loadLabel->clear();
        m_timingLabel->clear();
        return;
    }
    refreshSnapshot();
}

// 목적: Health Client cache에서 현재 endpoint의 최근 snapshot 조회
// 입력: 없음
// 출력: snapshot 또는 generic unavailable 표시
void WorkerHealthPanel::refreshSnapshot()
{
    if (!m_profile.has_value() || m_healthClient == nullptr)
    {
        renderUnavailable();
        return;
    }
    const core::client::WorkerHealthSnapshotResult result = m_healthClient->workerHealthSnapshot(m_profile->id);
    if (result.hasError())
    {
        m_probeButton->setEnabled(!m_activeProbe.has_value());
        m_summaryLabel->setText(tr("Worker health is unavailable."));
        m_loadLabel->clear();
        m_timingLabel->clear();
        return;
    }
    renderSnapshot(result.value());
}

// 목적: structured health dimensions를 localized Settings text로 표시
// 입력: snapshot: 현재 profile endpoint에 대응하는 observation
// 출력: summary/load/timing label과 Test button 상태 갱신
void WorkerHealthPanel::renderSnapshot(const core::client::WorkerHealthSnapshot& snapshot)
{
    m_probeButton->setEnabled(m_profile.has_value() && m_healthClient != nullptr && !m_activeProbe.has_value());
    if (!snapshot.observedAtUnixMilliseconds.has_value())
    {
        m_summaryLabel->setText(tr("Not checked."));
    }
    else if (snapshot.reachability == core::client::WorkerReachability::Unreachable)
    {
        m_summaryLabel->setText(tr("Worker is unreachable."));
    }
    else if (snapshot.compatibility == core::client::WorkerCompatibility::Incompatible)
    {
        m_summaryLabel->setText(tr("Worker protocol is incompatible."));
    }
    else if (snapshot.reachability == core::client::WorkerReachability::Reachable &&
             snapshot.compatibility == core::client::WorkerCompatibility::Compatible)
    {
        switch (snapshot.serviceState)
        {
        case core::client::WorkerServiceState::Ready:
            m_summaryLabel->setText(tr("Worker is ready."));
            break;
        case core::client::WorkerServiceState::Draining:
            m_summaryLabel->setText(tr("Worker is draining."));
            break;
        case core::client::WorkerServiceState::ShuttingDown:
            m_summaryLabel->setText(tr("Worker is shutting down."));
            break;
        case core::client::WorkerServiceState::Unknown:
            m_summaryLabel->setText(tr("Worker endpoint is reachable."));
            break;
        }
    }
    else if (snapshot.reachability == core::client::WorkerReachability::Reachable)
    {
        m_summaryLabel->setText(tr("Worker endpoint was reached, but health details are unavailable."));
    }
    else
    {
        m_summaryLabel->setText(tr("Worker health is unavailable."));
    }

    if (snapshot.load.has_value())
    {
        m_loadLabel->setText(tr("Running %1 of %2 · queued %3 of %4")
                                 .arg(snapshot.load->runningJobs)
                                 .arg(snapshot.load->maximumConcurrentJobs)
                                 .arg(snapshot.load->queuedJobs)
                                 .arg(snapshot.load->queueCapacity));
    }
    else
    {
        m_loadLabel->setText(tr("Load unavailable."));
    }

    QStringList timing;
    if (snapshot.roundTripMilliseconds.has_value())
    {
        timing.push_back(tr("Round trip: %1 ms").arg(*snapshot.roundTripMilliseconds));
    }
    const QString checked = formattedTime(snapshot.observedAtUnixMilliseconds);
    if (!checked.isEmpty())
    {
        timing.push_back(tr("Checked: %1").arg(checked));
    }
    const QString lastSeen = formattedTime(snapshot.lastSeenAtUnixMilliseconds);
    if (!lastSeen.isEmpty())
    {
        timing.push_back(tr("Last seen: %1").arg(lastSeen));
    }
    m_timingLabel->setText(timing.join(QStringLiteral(" · ")));
}

// 목적: 선택 profile 또는 health capability가 없는 상태 표시
// 입력: 없음
// 출력: detail 초기화와 Test button 비활성화
void WorkerHealthPanel::renderUnavailable()
{
    m_probeButton->setEnabled(false);
    m_summaryLabel->setText(m_profile.has_value() ? tr("Health checks are not available in this build.")
                                                  : tr("Select a saved Worker profile to check its health."));
    m_loadLabel->clear();
    m_timingLabel->clear();
}

}  // namespace flexraw::ui::settings
