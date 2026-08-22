#include "source_resolution_widget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>

#include "source_binding.h"

namespace flexraw::ui::catalog
{

// 목적: 선택 photo의 source resolution 상태와 명시적 command를 표시하는 inline 영역 구성
// 입력: parent: Qt 부모 widget
// 출력: 기본적으로 숨겨진 SourceResolutionWidget 객체
SourceResolutionWidget::SourceResolutionWidget(QWidget* parent)
    : QWidget(parent),
      m_messageLabel(new QLabel(this)),
      m_acceptReplacementButton(new QPushButton(tr("Accept Replacement"), this)),
      m_registerAsNewButton(new QPushButton(tr("Register as New"), this)),
      m_relinkButton(new QPushButton(tr("Relink..."), this))
{
    setObjectName(QStringLiteral("sourceResolutionWidget"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_messageLabel->setObjectName(QStringLiteral("sourceResolutionMessage"));
    m_messageLabel->setWordWrap(true);
    m_acceptReplacementButton->setObjectName(QStringLiteral("acceptReplacementButton"));
    m_registerAsNewButton->setObjectName(QStringLiteral("registerReplacementAsNewButton"));
    m_relinkButton->setObjectName(QStringLiteral("relinkSourceButton"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);
    layout->addWidget(m_messageLabel, 1);
    layout->addWidget(m_acceptReplacementButton);
    layout->addWidget(m_registerAsNewButton);
    layout->addWidget(m_relinkButton);

    connect(m_acceptReplacementButton, &QPushButton::clicked, this, [this] { emit acceptReplacementRequested(); });
    connect(m_registerAsNewButton, &QPushButton::clicked, this, [this] { emit registerReplacementAsNewRequested(); });
    connect(m_relinkButton, &QPushButton::clicked, this, [this] { emit relinkSourceRequested(); });
    setVisible(false);
}

// 목적: Editor state를 사용자 안내와 실행 가능한 source command에 투영
// 입력: state: source state와 capability를 포함한 current Editor snapshot
// 출력: resolution 불필요 시 숨김, 필요 시 가능한 button만 표시
void SourceResolutionWidget::setEditorState(const core::orchestration::EditorState& state)
{
    m_requestPending = false;
    if (!state.hasSelection || !state.sourceState.has_value() ||
        !core::catalog::requiresSourceResolution(*state.sourceState))
    {
        m_capabilities = {};
        setVisible(false);
        updateControls();
        return;
    }

    switch (*state.sourceState)
    {
    case core::catalog::SourceBindingState::Missing:
        m_messageLabel->setText(tr("Source file is missing."));
        break;
    case core::catalog::SourceBindingState::IdentityUnverified:
        m_messageLabel->setText(tr("Source identity needs confirmation."));
        break;
    case core::catalog::SourceBindingState::ReplacementDetected:
        m_messageLabel->setText(tr("Source file was replaced."));
        break;
    case core::catalog::SourceBindingState::Unreadable:
        m_messageLabel->setText(tr("Source file cannot be read."));
        break;
    case core::catalog::SourceBindingState::Unlinked:
        m_messageLabel->setText(tr("Photo is not linked to a source file."));
        break;
    case core::catalog::SourceBindingState::FingerprintPending:
    case core::catalog::SourceBindingState::Available:
    case core::catalog::SourceBindingState::VerificationRequired:
        m_messageLabel->clear();
        break;
    }

    m_capabilities = state.sourceResolution;
    setVisible(true);
    updateControls();
}

// 목적: accepted background resolution request 동안 중복 command 차단
// 입력: pending: terminal event를 기다리는 중이면 true
// 출력: 표시된 command의 enabled 상태 갱신
void SourceResolutionWidget::setRequestPending(bool pending)
{
    m_requestPending = pending;
    updateControls();
}

// 목적: 명시적 source resolution request 대기 상태 조회
// 입력: 없음
// 출력: request terminal event를 기다리고 있으면 true
bool SourceResolutionWidget::isRequestPending() const noexcept
{
    return m_requestPending;
}

// 목적: 현재 capability와 pending 상태를 실제 button visibility/enabled 상태에 반영
// 입력: 없음
// 출력: 불가능한 command는 숨기고 pending 중 가능한 command는 비활성화
void SourceResolutionWidget::updateControls()
{
    m_acceptReplacementButton->setVisible(m_capabilities.canAcceptReplacement);
    m_acceptReplacementButton->setEnabled(m_capabilities.canAcceptReplacement && !m_requestPending);
    m_registerAsNewButton->setVisible(m_capabilities.canRegisterReplacementAsNew);
    m_registerAsNewButton->setEnabled(m_capabilities.canRegisterReplacementAsNew && !m_requestPending);
    m_relinkButton->setVisible(m_capabilities.canRelinkSource);
    m_relinkButton->setEnabled(m_capabilities.canRelinkSource && !m_requestPending);
}

}  // namespace flexraw::ui::catalog
