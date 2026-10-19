#include "settings_page_header.h"

#include <QFont>
#include <QLabel>
#include <QVBoxLayout>

namespace flexraw::ui::settings
{

// 목적: Settings page마다 같은 title과 설명 hierarchy 제공
// 입력: title: page 이름, description: page 역할 설명, parent: Qt 부모 widget
// 출력: native palette와 font를 따르는 reusable header widget
SettingsPageHeader::SettingsPageHeader(const QString& title, const QString& description, QWidget* parent)
    : QWidget(parent), m_titleLabel(new QLabel(title, this)), m_descriptionLabel(new QLabel(description, this))
{
    setObjectName(QStringLiteral("settingsPageHeader"));
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setObjectName(QStringLiteral("settingsPageTitle"));
    m_descriptionLabel->setObjectName(QStringLiteral("settingsPageDescription"));
    m_descriptionLabel->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    layout->setSpacing(4);
    layout->addWidget(m_titleLabel);
    layout->addWidget(m_descriptionLabel);
}

}  // namespace flexraw::ui::settings
