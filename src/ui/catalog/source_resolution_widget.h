#pragma once

#include <QWidget>

#include "editor_contracts.h"

class QLabel;
class QPushButton;

namespace flexraw::ui::catalog
{

class SourceResolutionWidget final : public QWidget
{
    Q_OBJECT

public:
    // 목적: 선택 photo의 source resolution 상태와 명시적 command를 표시하는 inline 영역 구성
    // 입력: parent: Qt 부모 widget
    // 출력: 기본적으로 숨겨진 SourceResolutionWidget 객체
    explicit SourceResolutionWidget(QWidget* parent = nullptr);

    // 목적: Editor state를 사용자 안내와 실행 가능한 source command에 투영
    // 입력: state: source state와 capability를 포함한 current Editor snapshot
    // 출력: resolution 불필요 시 숨김, 필요 시 가능한 button만 표시
    void setEditorState(const core::orchestration::EditorState& state);

    // 목적: accepted background resolution request 동안 중복 command 차단
    // 입력: pending: terminal event를 기다리는 중이면 true
    // 출력: 표시된 command의 enabled 상태 갱신
    void setRequestPending(bool pending);

    // 목적: 명시적 source resolution request 대기 상태 조회
    // 입력: 없음
    // 출력: request terminal event를 기다리고 있으면 true
    [[nodiscard]] bool isRequestPending() const noexcept;

signals:
    void acceptReplacementRequested();
    void registerReplacementAsNewRequested();
    void relinkSourceRequested();

private:
    // 목적: 현재 capability와 pending 상태를 실제 button visibility/enabled 상태에 반영
    // 입력: 없음
    // 출력: 불가능한 command는 숨기고 pending 중 가능한 command는 비활성화
    void updateControls();

    QLabel* m_messageLabel{nullptr};
    QPushButton* m_acceptReplacementButton{nullptr};
    QPushButton* m_registerAsNewButton{nullptr};
    QPushButton* m_relinkButton{nullptr};
    core::orchestration::SourceResolutionCapabilities m_capabilities;
    bool m_requestPending{false};
};

}  // namespace flexraw::ui::catalog
