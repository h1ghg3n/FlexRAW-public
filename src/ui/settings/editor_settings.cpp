#include "editor_settings.h"

namespace flexraw::ui::settings
{

// 목적: 지정한 application settings에 접근하는 editor UI preference 저장소 초기화
// 입력: settings: 소유권을 유지하는 Qt settings 객체
// 출력: 초기화된 EditorSettings 객체
EditorSettings::EditorSettings(QSettings& settings) : m_settings(settings) {}

// 목적: 저장된 adjustment presentation preference를 검증하여 복원
// 입력: 없음
// 출력: 저장값이 없거나 손상됐으면 Classic, 아니면 저장된 style
editor::AdjustmentControlStyle EditorSettings::loadAdjustmentControlStyle() const
{
    m_settings.beginGroup(QStringLiteral("ui"));
    bool converted = false;
    const int stored =
        m_settings
            .value(QStringLiteral("adjustmentControlStyle"), static_cast<int>(editor::AdjustmentControlStyle::Classic))
            .toInt(&converted);
    m_settings.endGroup();
    if (!converted || stored < static_cast<int>(editor::AdjustmentControlStyle::Classic) ||
        stored > static_cast<int>(editor::AdjustmentControlStyle::Relative))
    {
        return editor::AdjustmentControlStyle::Classic;
    }
    return static_cast<editor::AdjustmentControlStyle>(stored);
}

// 목적: adjustment presentation preference를 UI 전용 설정으로 저장
// 입력: style: Classic 또는 Relative
// 출력: settings의 ui group이 갱신되고 sync됨
void EditorSettings::saveAdjustmentControlStyle(editor::AdjustmentControlStyle style) const
{
    m_settings.beginGroup(QStringLiteral("ui"));
    m_settings.setValue(QStringLiteral("adjustmentControlStyle"), static_cast<int>(style));
    m_settings.endGroup();
    m_settings.sync();
}

}  // namespace flexraw::ui::settings
