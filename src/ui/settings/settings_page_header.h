#pragma once

#include <QWidget>

class QLabel;
class QString;

namespace flexraw::ui::settings
{

class SettingsPageHeader final : public QWidget
{
public:
    // 목적: Settings page마다 같은 title과 설명 hierarchy 제공
    // 입력: title: page 이름, description: page 역할 설명, parent: Qt 부모 widget
    // 출력: native palette와 font를 따르는 reusable header widget
    explicit SettingsPageHeader(const QString& title, const QString& description, QWidget* parent = nullptr);

private:
    QLabel* m_titleLabel{nullptr};
    QLabel* m_descriptionLabel{nullptr};
};

}  // namespace flexraw::ui::settings
