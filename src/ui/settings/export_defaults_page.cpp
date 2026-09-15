#include "export_defaults_page.h"

#include <limits>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>

namespace flexraw::ui::settings
{

// 목적: frontend-neutral Export 기본값을 편집하는 Settings page 구성
// 입력: exportDefaultsClient: application 기본값 계약, workerProfileClient: preferred Worker 조회 계약, parent: Qt 부모
// 출력: persisted snapshot을 표시하는 Export 기본값 page
ExportDefaultsPage::ExportDefaultsPage(core::client::IExportDefaultsClient& exportDefaultsClient,
                                       core::client::IWorkerProfileClient& workerProfileClient,
                                       QWidget* parent)
    : QWidget(parent),
      m_exportDefaultsClient(&exportDefaultsClient),
      m_workerProfileClient(&workerProfileClient),
      m_formatComboBox(new QComboBox(this)),
      m_jpegQualitySpinBox(new QSpinBox(this)),
      m_pngCompressionSpinBox(new QSpinBox(this)),
      m_tiffCompressionComboBox(new QComboBox(this)),
      m_maximumDimensionSpinBox(new QSpinBox(this)),
      m_outputColorSpaceComboBox(new QComboBox(this)),
      m_includeMetadataCheckBox(new QCheckBox(tr("Include metadata"), this)),
      m_placementComboBox(new QComboBox(this)),
      m_workerProfileComboBox(new QComboBox(this)),
      m_statusLabel(new QLabel(this))
{
    setObjectName(QStringLiteral("exportDefaultsPage"));
    m_formatComboBox->setObjectName(QStringLiteral("exportDefaultsFormatComboBox"));
    m_formatComboBox->addItem(tr("JPEG"), static_cast<int>(core::client::ExportRasterFormat::Jpeg));
    m_formatComboBox->addItem(tr("PNG"), static_cast<int>(core::client::ExportRasterFormat::Png));
    m_formatComboBox->addItem(tr("TIFF"), static_cast<int>(core::client::ExportRasterFormat::Tiff));
    m_jpegQualitySpinBox->setObjectName(QStringLiteral("exportDefaultsJpegQualitySpinBox"));
    m_jpegQualitySpinBox->setRange(1, 100);
    m_jpegQualitySpinBox->setSuffix(tr(" %"));
    m_pngCompressionSpinBox->setObjectName(QStringLiteral("exportDefaultsPngCompressionSpinBox"));
    m_pngCompressionSpinBox->setRange(0, 9);
    m_tiffCompressionComboBox->setObjectName(QStringLiteral("exportDefaultsTiffCompressionComboBox"));
    m_tiffCompressionComboBox->addItem(tr("None"), static_cast<int>(core::client::ExportTiffCompression::None));
    m_tiffCompressionComboBox->addItem(tr("LZW"), static_cast<int>(core::client::ExportTiffCompression::Lzw));
    m_maximumDimensionSpinBox->setObjectName(QStringLiteral("exportDefaultsMaximumDimensionSpinBox"));
    m_maximumDimensionSpinBox->setRange(0, std::numeric_limits<int>::max());
    m_maximumDimensionSpinBox->setSpecialValueText(tr("Original"));
    m_maximumDimensionSpinBox->setSuffix(tr(" px"));
    m_outputColorSpaceComboBox->setObjectName(QStringLiteral("exportDefaultsColorSpaceComboBox"));
    m_outputColorSpaceComboBox->addItem(tr("sRGB"), static_cast<int>(core::client::ExportOutputColorSpace::Srgb));
    m_outputColorSpaceComboBox->addItem(tr("Adobe RGB"),
                                        static_cast<int>(core::client::ExportOutputColorSpace::AdobeRgb));
    m_outputColorSpaceComboBox->addItem(tr("Display P3"),
                                        static_cast<int>(core::client::ExportOutputColorSpace::DisplayP3));
    m_includeMetadataCheckBox->setObjectName(QStringLiteral("exportDefaultsMetadataCheckBox"));

    auto* rasterGroup = new QGroupBox(tr("Raster defaults"), this);
    auto* rasterLayout = new QFormLayout(rasterGroup);
    rasterLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    rasterLayout->addRow(tr("Format"), m_formatComboBox);
    rasterLayout->addRow(tr("JPEG quality"), m_jpegQualitySpinBox);
    rasterLayout->addRow(tr("PNG compression"), m_pngCompressionSpinBox);
    rasterLayout->addRow(tr("TIFF compression"), m_tiffCompressionComboBox);
    rasterLayout->addRow(tr("Maximum dimension"), m_maximumDimensionSpinBox);
    rasterLayout->addRow(tr("Output color space"), m_outputColorSpaceComboBox);
    rasterLayout->addRow(QString{}, m_includeMetadataCheckBox);

    m_placementComboBox->setObjectName(QStringLiteral("exportDefaultsPlacementComboBox"));
    m_placementComboBox->addItem(tr("Local"), static_cast<int>(core::client::ExportPlacementPolicy::LocalOnly));
    m_placementComboBox->addItem(tr("Remote"), static_cast<int>(core::client::ExportPlacementPolicy::RemoteOnly));
    m_placementComboBox->addItem(tr("Auto"), static_cast<int>(core::client::ExportPlacementPolicy::Auto));
    m_workerProfileComboBox->setObjectName(QStringLiteral("exportDefaultsWorkerProfileComboBox"));

    auto* executionGroup = new QGroupBox(tr("Execution defaults"), this);
    auto* executionLayout = new QFormLayout(executionGroup);
    executionLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    executionLayout->addRow(tr("Default placement"), m_placementComboBox);
    executionLayout->addRow(tr("Preferred Worker"), m_workerProfileComboBox);

    m_statusLabel->setObjectName(QStringLiteral("exportDefaultsStatusLabel"));
    m_statusLabel->setWordWrap(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(rasterGroup);
    layout->addWidget(executionGroup);
    layout->addWidget(m_statusLabel);
    layout->addStretch(1);

    loadDefaults();
}

// 목적: 현재 widget 값을 application-wide Export 기본값으로 저장
// 입력: 없음
// 출력: raster와 execution 기본값이 모두 저장되면 true
bool ExportDefaultsPage::saveDefaults()
{
    if (!m_defaultsLoaded)
    {
        m_statusLabel->setText(tr("Export defaults are unavailable."));
        return false;
    }

    const core::client::ExportRasterDefaultsResult rasterResult =
        m_exportDefaultsClient->saveRasterExportDefaults(collectRasterOptions());
    if (rasterResult.hasError())
    {
        m_statusLabel->setText(tr("Export raster defaults could not be saved."));
        return false;
    }
    const core::client::ExportExecutionDefaultsResult executionResult =
        m_exportDefaultsClient->saveExportExecutionDefaults(collectExecutionDefaults());
    if (executionResult.hasError())
    {
        m_statusLabel->setText(tr("Export execution defaults could not be saved."));
        return false;
    }
    m_preferredWorkerProfileId = executionResult.value().preferredWorkerProfileId;
    m_statusLabel->setText(tr("Export defaults saved."));
    return true;
}

// 목적: application Worker profile 변경을 preferred Worker selector에 다시 반영
// 입력: 없음
// 출력: 현재 선택 identity를 보존한 profile 목록과 상태 안내 갱신
void ExportDefaultsPage::refreshWorkerProfiles()
{
    QString selectedId;
    if (m_workerProfilesInitialized)
    {
        selectedId = m_workerProfileComboBox->currentData().toString();
    }
    else if (m_preferredWorkerProfileId.has_value())
    {
        selectedId = QString::fromStdString(m_preferredWorkerProfileId->value);
    }

    m_workerProfileComboBox->clear();
    m_workerProfileComboBox->addItem(tr("No preferred Worker"), QString{});
    const core::client::WorkerProfileListResult profileResult = m_workerProfileClient->listWorkerProfiles();
    m_workerProfilesInitialized = true;
    if (profileResult.hasError())
    {
        if (!selectedId.isEmpty())
        {
            m_workerProfileComboBox->addItem(tr("Unavailable Worker profile"), selectedId);
            m_workerProfileComboBox->setCurrentIndex(1);
        }
        m_statusLabel->setText(m_defaultsLoaded
                                   ? tr("Worker profiles could not be loaded. The saved preference is unchanged.")
                                   : tr("Export defaults could not be loaded."));
        return;
    }

    int selectedIndex = 0;
    bool selectedProfileDisabled = false;
    for (const core::client::WorkerProfileSnapshot& profile : profileResult.value())
    {
        const QString id = QString::fromStdString(profile.id.value);
        const QString name = QString::fromUtf8(profile.displayName.c_str());
        m_workerProfileComboBox->addItem(profile.enabled ? name : tr("%1 (Disabled)").arg(name), id);
        if (id == selectedId)
        {
            selectedIndex = m_workerProfileComboBox->count() - 1;
            selectedProfileDisabled = !profile.enabled;
        }
    }
    const bool selectedProfileMissing = !selectedId.isEmpty() && selectedIndex == 0;
    if (!m_defaultsLoaded)
    {
        m_statusLabel->setText(tr("Export defaults could not be loaded."));
    }
    else if (selectedProfileMissing)
    {
        m_workerProfileComboBox->addItem(tr("Unavailable Worker profile"), selectedId);
        selectedIndex = m_workerProfileComboBox->count() - 1;
    }
    m_workerProfileComboBox->setCurrentIndex(selectedIndex);

    if (selectedProfileMissing)
    {
        m_statusLabel->setText(tr("The preferred Worker profile is unavailable. Default placement is unchanged."));
    }
    else if (selectedProfileDisabled)
    {
        m_statusLabel->setText(tr("The preferred Worker profile is disabled. Default placement is unchanged."));
    }
    else
    {
        m_statusLabel->clear();
    }
}

// 목적: 저장된 Export 기본값 snapshot을 widget에 복원
// 입력: 없음
// 출력: 성공 시 저장 가능한 controls와 preferred identity 초기화
void ExportDefaultsPage::loadDefaults()
{
    const core::client::ExportDefaultsResult result = m_exportDefaultsClient->exportDefaults();
    if (result.hasError())
    {
        m_statusLabel->setText(tr("Export defaults could not be loaded."));
        return;
    }

    const core::client::ExportDefaultsSnapshot& defaults = result.value();
    m_formatComboBox->setCurrentIndex(m_formatComboBox->findData(static_cast<int>(defaults.rasterOptions.format)));
    m_jpegQualitySpinBox->setValue(defaults.rasterOptions.jpegQuality);
    m_pngCompressionSpinBox->setValue(defaults.rasterOptions.pngCompression);
    m_tiffCompressionComboBox->setCurrentIndex(
        m_tiffCompressionComboBox->findData(static_cast<int>(defaults.rasterOptions.tiffCompression)));
    m_maximumDimensionSpinBox->setValue(defaults.rasterOptions.maximumDimension);
    m_outputColorSpaceComboBox->setCurrentIndex(
        m_outputColorSpaceComboBox->findData(static_cast<int>(defaults.rasterOptions.outputColorSpace)));
    m_includeMetadataCheckBox->setChecked(defaults.rasterOptions.includeMetadata);
    m_placementComboBox->setCurrentIndex(
        m_placementComboBox->findData(static_cast<int>(defaults.execution.placementPolicy)));
    m_preferredWorkerProfileId = defaults.execution.preferredWorkerProfileId;
    m_defaultsLoaded = true;
    refreshWorkerProfiles();
}

// 목적: 현재 widget 값을 Qt-free raster 기본값으로 조립
// 입력: 없음
// 출력: format, encoding, dimension, color와 metadata option
core::client::ExportRasterOptions ExportDefaultsPage::collectRasterOptions() const
{
    core::client::ExportRasterOptions options;
    options.format = static_cast<core::client::ExportRasterFormat>(m_formatComboBox->currentData().toInt());
    options.jpegQuality = m_jpegQualitySpinBox->value();
    options.pngCompression = m_pngCompressionSpinBox->value();
    options.tiffCompression =
        static_cast<core::client::ExportTiffCompression>(m_tiffCompressionComboBox->currentData().toInt());
    options.maximumDimension = m_maximumDimensionSpinBox->value();
    options.outputColorSpace =
        static_cast<core::client::ExportOutputColorSpace>(m_outputColorSpaceComboBox->currentData().toInt());
    options.includeMetadata = m_includeMetadataCheckBox->isChecked();
    return options;
}

// 목적: 현재 widget 값을 Qt-free placement 기본값으로 조립
// 입력: 없음
// 출력: Local/Remote/Auto와 optional preferred Worker identity
core::client::ExportExecutionDefaults ExportDefaultsPage::collectExecutionDefaults() const
{
    core::client::ExportExecutionDefaults defaults;
    defaults.placementPolicy =
        static_cast<core::client::ExportPlacementPolicy>(m_placementComboBox->currentData().toInt());
    const QString selectedId = m_workerProfileComboBox->currentData().toString();
    if (!selectedId.isEmpty())
    {
        defaults.preferredWorkerProfileId = core::client::WorkerProfileId{selectedId.toStdString()};
    }
    return defaults;
}

}  // namespace flexraw::ui::settings
