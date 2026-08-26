#pragma once

#include <QDialog>

#include "adjustment_control_style.h"

class QComboBox;
class QSettings;

namespace flexraw::ui::settings
{

class SettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    // 목적: 현재 UI preference를 편집하는 최소 Settings window 초기화
    // 입력: settings: application preference 저장소, parent: Qt 부모 widget
    // 출력: View tab과 adjustment style selector를 포함한 dialog
    explicit SettingsDialog(QSettings& settings, QWidget* parent = nullptr);

    // 목적: dialog에서 현재 선택된 adjustment presentation 반환
    // 입력: 없음
    // 출력: Classic 또는 Relative style
    [[nodiscard]] editor::AdjustmentControlStyle adjustmentControlStyle() const;

private:
    QSettings& m_settings;
    QComboBox* m_adjustmentControlStyleComboBox{nullptr};
};

}  // namespace flexraw::ui::settings
