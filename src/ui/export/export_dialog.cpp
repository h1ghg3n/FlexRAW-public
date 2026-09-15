#include "export_dialog.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include "editor_client_projection.h"
#include "export.h"
#include "log.h"
#include "shared_storage_locator.h"

namespace flexraw::ui::export_
{
namespace
{

// 목적: Qt string을 byte length가 보존된 UTF-8 client string으로 변환
// 입력: value: source/output/catalog locator 또는 display text
// 출력: Qt-free UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: byte length가 보존된 UTF-8 client string을 Qt presentation text로 변환
// 입력: value: Export result locator
// 출력: 같은 Unicode text의 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: current Core source descriptor를 Qt-free Export source snapshot으로 투영
// 입력: source: Editor/Catalog가 해석한 source path와 kind
// 출력: product contract source value
[[nodiscard]] core::client::EditorSourceSnapshot toClientSource(const core::types::FileDescriptor& source)
{
    core::client::CatalogFileKind kind = core::client::CatalogFileKind::Unknown;
    switch (source.kind)
    {
    case core::types::SupportedFileKind::Raw:
        kind = core::client::CatalogFileKind::Raw;
        break;
    case core::types::SupportedFileKind::RasterImage:
        kind = core::client::CatalogFileKind::RasterImage;
        break;
    case core::types::SupportedFileKind::Unknown:
        break;
    }
    return {toClientString(source.path), toClientString(source.extension), toClientString(source.displayName), kind};
}

// 목적: current Qt settings raster option을 Qt-free Export option으로 투영
// 입력: options: dialog widget에서 수집한 processing option
// 출력: product contract format/quality/color snapshot
[[nodiscard]] core::client::ExportRasterOptions toClientOptions(const core::export_::RasterExportOptions& options)
{
    core::client::ExportRasterOptions projected;
    switch (options.format)
    {
    case core::export_::RasterExportFormat::Jpeg:
        projected.format = core::client::ExportRasterFormat::Jpeg;
        break;
    case core::export_::RasterExportFormat::Png:
        projected.format = core::client::ExportRasterFormat::Png;
        break;
    case core::export_::RasterExportFormat::Tiff:
        projected.format = core::client::ExportRasterFormat::Tiff;
        break;
    }
    projected.jpegQuality = options.jpegQuality;
    projected.pngCompression = options.pngCompression;
    projected.tiffCompression = options.tiffCompression == core::export_::TiffCompression::None
                                    ? core::client::ExportTiffCompression::None
                                    : core::client::ExportTiffCompression::Lzw;
    projected.maximumDimension = options.maximumDimension;
    switch (options.outputColorSpace)
    {
    case core::export_::RasterOutputColorSpace::Srgb:
        projected.outputColorSpace = core::client::ExportOutputColorSpace::Srgb;
        break;
    case core::export_::RasterOutputColorSpace::AdobeRgb:
        projected.outputColorSpace = core::client::ExportOutputColorSpace::AdobeRgb;
        break;
    case core::export_::RasterOutputColorSpace::DisplayP3:
        projected.outputColorSpace = core::client::ExportOutputColorSpace::DisplayP3;
        break;
    }
    projected.includeMetadata = options.includeMetadata;
    return projected;
}

// 목적: frontend-neutral Export default option을 current raster processing option으로 투영
// 입력: options: application-wide product default snapshot
// 출력: 기존 Export dialog control과 processing service가 사용하는 option
[[nodiscard]] core::export_::RasterExportOptions fromClientOptions(const core::client::ExportRasterOptions& options)
{
    core::export_::RasterExportOptions projected;
    switch (options.format)
    {
    case core::client::ExportRasterFormat::Jpeg:
        projected.format = core::export_::RasterExportFormat::Jpeg;
        break;
    case core::client::ExportRasterFormat::Png:
        projected.format = core::export_::RasterExportFormat::Png;
        break;
    case core::client::ExportRasterFormat::Tiff:
        projected.format = core::export_::RasterExportFormat::Tiff;
        break;
    }
    projected.jpegQuality = options.jpegQuality;
    projected.pngCompression = options.pngCompression;
    projected.tiffCompression = options.tiffCompression == core::client::ExportTiffCompression::None
                                    ? core::export_::TiffCompression::None
                                    : core::export_::TiffCompression::Lzw;
    projected.maximumDimension = options.maximumDimension;
    switch (options.outputColorSpace)
    {
    case core::client::ExportOutputColorSpace::Srgb:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::Srgb;
        break;
    case core::client::ExportOutputColorSpace::AdobeRgb:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::AdobeRgb;
        break;
    case core::client::ExportOutputColorSpace::DisplayP3:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::DisplayP3;
        break;
    }
    projected.includeMetadata = options.includeMetadata;
    return projected;
}

// 목적: raster format을 canonical output file 확장자로 변환
// 입력: format: JPEG, PNG 또는 TIFF format
// 출력: 선행 점이 없는 lowercase 확장자
[[nodiscard]] QString extensionForFormat(const core::export_::RasterExportFormat format)
{
    switch (format)
    {
    case core::export_::RasterExportFormat::Jpeg:
        return QStringLiteral("jpg");
    case core::export_::RasterExportFormat::Png:
        return QStringLiteral("png");
    case core::export_::RasterExportFormat::Tiff:
        return QStringLiteral("tif");
    }

    return QStringLiteral("jpg");
}

// 목적: 기존 filename을 선택 format의 canonical 확장자로 정규화
// 입력: path: 사용자 입력 output path, format: 선택한 raster format
// 출력: 같은 directory와 base name을 유지한 format 일치 path
[[nodiscard]] QString pathForFormat(const QString& path, const core::export_::RasterExportFormat format)
{
    if (path.isEmpty() || path != path.trimmed())
    {
        return path;
    }

    const QFileInfo info(QDir::cleanPath(path));
    const QString extension = extensionForFormat(format);
    if (info.suffix().compare(extension, Qt::CaseInsensitive) == 0 ||
        (format == core::export_::RasterExportFormat::Jpeg &&
         info.suffix().compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0) ||
        (format == core::export_::RasterExportFormat::Tiff &&
         info.suffix().compare(QStringLiteral("tiff"), Qt::CaseInsensitive) == 0))
    {
        return info.filePath();
    }

    const QString baseName = info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName();
    return info.dir().filePath(baseName + QLatin1Char('.') + extension);
}

// 목적: source와 format에서 원본을 덮어쓰지 않는 기본 output path 생성
// 입력: source: 현재 Editor source descriptor, format: 기본 raster format
// 출력: source directory의 <base>-export.<ext> path
[[nodiscard]] QString defaultOutputPath(const core::types::FileDescriptor& source,
                                        const core::export_::RasterExportFormat format)
{
    const QFileInfo sourceInfo(source.path);
    const QString baseName =
        sourceInfo.completeBaseName().isEmpty() ? QStringLiteral("export") : sourceInfo.completeBaseName();
    return sourceInfo.dir().filePath(baseName + QStringLiteral("-export.") + extensionForFormat(format));
}

}  // namespace

// 목적: 현재 Editor snapshot을 통합 Local/Remote/Auto export 실행 경계에 연결
// 입력: Export command/event contract, editorState: 현재 photo, settings: 기본값 저장소
// 출력: marker 결합을 보조하되 correctness는 Core에 위임하는 raster export dialog
ExportDialog::ExportDialog(core::client::IExportClient& exportClient,
                           core::client::IExportEventSource& exportEventSource,
                           const core::orchestration::EditorState& editorState,
                           core::client::IWorkerProfileClient& workerProfileClient,
                           core::client::IExportDefaultsClient& exportDefaultsClient,
                           QWidget* parent)
    : ExportDialog(exportClient,
                   exportEventSource,
                   QVector<ExportDialogSource>{{editorState.source, {}, editorState.params}},
                   workerProfileClient,
                   exportDefaultsClient,
                   parent)
{}

// 목적: 선택된 여러 photo source를 editable output 목록과 통합 placement 경계에 연결
// 입력: Export command/event contract, sources: source별 develop resolution 정보, settings: 기본값 저장소
// 출력: item 단위 scheduling을 제출하고 marker correctness를 Core에 위임하는 dialog
ExportDialog::ExportDialog(core::client::IExportClient& exportClient,
                           core::client::IExportEventSource& exportEventSource,
                           QVector<ExportDialogSource> sources,
                           core::client::IWorkerProfileClient& workerProfileClient,
                           core::client::IExportDefaultsClient& exportDefaultsClient,
                           QWidget* parent)
    : QDialog(parent),
      m_exportClient(&exportClient),
      m_workerProfileClient(&workerProfileClient),
      m_exportDefaultsClient(&exportDefaultsClient),
      m_sources(std::move(sources)),
      m_source(m_sources.isEmpty() ? core::types::FileDescriptor{} : m_sources.constFirst().source),
      m_developParams(m_sources.isEmpty() || !m_sources.constFirst().developParams.has_value()
                          ? core::types::DevelopParams{}
                          : *m_sources.constFirst().developParams),
      m_sourceLabel(new QLabel(this)),
      m_outputTable(new QTableWidget(this)),
      m_browseButton(new QToolButton(this)),
      m_formatCombo(new QComboBox(this)),
      m_executionCombo(new QComboBox(this)),
      m_remoteSettingsWidget(new QWidget(this)),
      m_workerProfileCombo(new QComboBox(m_remoteSettingsWidget)),
      m_jpegQualityLabel(new QLabel(tr("JPEG quality"), this)),
      m_jpegQualitySpinBox(new QSpinBox(this)),
      m_pngCompressionLabel(new QLabel(tr("PNG compression"), this)),
      m_pngCompressionSpinBox(new QSpinBox(this)),
      m_tiffCompressionLabel(new QLabel(tr("TIFF compression"), this)),
      m_tiffCompressionCombo(new QComboBox(this)),
      m_maximumDimensionSpinBox(new QSpinBox(this)),
      m_outputColorSpaceCombo(new QComboBox(this)),
      m_includeMetadataCheckBox(new QCheckBox(tr("Include metadata"), this)),
      m_statusLabel(new QLabel(this)),
      m_elapsedLabel(new QLabel(this)),
      m_elapsedTimer(new QTimer(this))
{
    setWindowTitle(tr("Export"));
    setModal(true);
    setMinimumWidth(520);

    m_sourceLabel->setObjectName(QStringLiteral("exportSourceLabel"));
    m_sourceLabel->setText(m_sources.size() == 1 ? tr("1 photo selected")
                                                 : tr("%1 photos selected").arg(m_sources.size()));
    m_sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_outputTable->setObjectName(QStringLiteral("exportOutputTable"));
    m_outputTable->setColumnCount(2);
    m_outputTable->setHorizontalHeaderLabels({tr("Source"), tr("Output file")});
    m_outputTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_outputTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_outputTable->verticalHeader()->setVisible(false);
    m_outputTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_outputTable->setMinimumHeight(140);
    m_outputTable->setMaximumHeight(260);
    m_browseButton->setObjectName(QStringLiteral("exportBrowseButton"));
    m_browseButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_browseButton->setToolTip(m_sources.size() == 1 ? tr("Choose output file") : tr("Choose output folder"));
    auto* outputPathLayout = new QHBoxLayout();
    outputPathLayout->setContentsMargins(0, 0, 0, 0);
    outputPathLayout->setSpacing(4);
    outputPathLayout->addWidget(m_outputTable, 1);
    outputPathLayout->addWidget(m_browseButton);

    m_formatCombo->setObjectName(QStringLiteral("exportFormatCombo"));
    m_formatCombo->addItem(tr("JPEG"), static_cast<int>(core::export_::RasterExportFormat::Jpeg));
    m_formatCombo->addItem(tr("PNG"), static_cast<int>(core::export_::RasterExportFormat::Png));
    m_formatCombo->addItem(tr("TIFF"), static_cast<int>(core::export_::RasterExportFormat::Tiff));

    m_executionCombo->setObjectName(QStringLiteral("exportExecutionCombo"));
    m_executionCombo->addItem(tr("Local"), static_cast<int>(core::client::ExportPlacementPolicy::LocalOnly));
    m_executionCombo->addItem(tr("Remote"), static_cast<int>(core::client::ExportPlacementPolicy::RemoteOnly));
    m_executionCombo->addItem(tr("Auto"), static_cast<int>(core::client::ExportPlacementPolicy::Auto));
    m_remoteSettingsWidget->setObjectName(QStringLiteral("exportRemoteSettingsWidget"));
    m_workerProfileCombo->setObjectName(QStringLiteral("exportWorkerProfileCombo"));
    auto* remoteSettingsLayout = new QFormLayout(m_remoteSettingsWidget);
    remoteSettingsLayout->setContentsMargins(0, 0, 0, 0);
    remoteSettingsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    remoteSettingsLayout->addRow(tr("Worker profile"), m_workerProfileCombo);

    m_jpegQualitySpinBox->setObjectName(QStringLiteral("exportJpegQualitySpinBox"));
    m_jpegQualitySpinBox->setRange(1, 100);
    m_jpegQualitySpinBox->setSuffix(tr(" %"));
    m_pngCompressionSpinBox->setObjectName(QStringLiteral("exportPngCompressionSpinBox"));
    m_pngCompressionSpinBox->setRange(0, 9);
    m_tiffCompressionCombo->setObjectName(QStringLiteral("exportTiffCompressionCombo"));
    m_tiffCompressionCombo->addItem(tr("None"), static_cast<int>(core::export_::TiffCompression::None));
    m_tiffCompressionCombo->addItem(tr("LZW"), static_cast<int>(core::export_::TiffCompression::Lzw));

    m_maximumDimensionSpinBox->setObjectName(QStringLiteral("exportMaximumDimensionSpinBox"));
    m_maximumDimensionSpinBox->setRange(0, 100000);
    m_maximumDimensionSpinBox->setSpecialValueText(tr("Original"));
    m_maximumDimensionSpinBox->setSuffix(tr(" px"));
    m_outputColorSpaceCombo->setObjectName(QStringLiteral("exportColorSpaceCombo"));
    m_outputColorSpaceCombo->addItem(tr("sRGB"), static_cast<int>(core::export_::RasterOutputColorSpace::Srgb));
    m_outputColorSpaceCombo->addItem(tr("Adobe RGB"),
                                     static_cast<int>(core::export_::RasterOutputColorSpace::AdobeRgb));
    m_outputColorSpaceCombo->addItem(tr("Display P3"),
                                     static_cast<int>(core::export_::RasterOutputColorSpace::DisplayP3));
    m_includeMetadataCheckBox->setObjectName(QStringLiteral("exportMetadataCheckBox"));

    const core::client::WorkerProfileListResult profileResult = m_workerProfileClient->listWorkerProfiles();
    const bool profileLoadFailed = profileResult.hasError();
    if (profileResult.hasValue())
    {
        m_workerProfiles = profileResult.value();
    }
    core::client::ExportDefaultsSnapshot defaultSnapshot;
    const core::client::ExportDefaultsResult defaultsResult = m_exportDefaultsClient->exportDefaults();
    if (defaultsResult.hasValue())
    {
        defaultSnapshot = defaultsResult.value();
    }
    else
    {
        LOG_WARN("export", "Unable to load Export defaults: {}", defaultsResult.error().technicalMessage);
    }
    const core::export_::RasterExportOptions defaults = fromClientOptions(defaultSnapshot.rasterOptions);
    m_executionDefaults = defaultSnapshot.execution;
    m_formatCombo->setCurrentIndex(m_formatCombo->findData(static_cast<int>(defaults.format)));
    m_jpegQualitySpinBox->setValue(defaults.jpegQuality);
    m_pngCompressionSpinBox->setValue(defaults.pngCompression);
    m_tiffCompressionCombo->setCurrentIndex(
        m_tiffCompressionCombo->findData(static_cast<int>(defaults.tiffCompression)));
    m_maximumDimensionSpinBox->setValue(defaults.maximumDimension);
    m_outputColorSpaceCombo->setCurrentIndex(
        m_outputColorSpaceCombo->findData(static_cast<int>(defaults.outputColorSpace)));
    m_includeMetadataCheckBox->setChecked(defaults.includeMetadata);
    m_outputTable->setRowCount(static_cast<int>(m_sources.size()));
    m_outputPathEdits.reserve(m_sources.size());
    for (qsizetype row = 0; row < m_sources.size(); ++row)
    {
        const ExportDialogSource& source = m_sources.at(row);
        auto* sourceItem = new QTableWidgetItem(source.source.displayName);
        sourceItem->setToolTip(source.source.path);
        sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
        m_outputTable->setItem(static_cast<int>(row), 0, sourceItem);
        auto* outputPathEdit = new QLineEdit(defaultOutputPath(source.source, defaults.format), m_outputTable);
        outputPathEdit->setObjectName(row == 0 ? QStringLiteral("exportOutputPathEdit")
                                               : QStringLiteral("exportOutputPathEdit%1").arg(row + 1));
        m_outputTable->setCellWidget(static_cast<int>(row), 1, outputPathEdit);
        m_outputPathEdits.push_back(outputPathEdit);
    }
    m_outputPathEdit = m_outputPathEdits.isEmpty() ? nullptr : m_outputPathEdits.constFirst();
    m_executionCombo->setCurrentIndex(
        m_executionCombo->findData(static_cast<int>(m_executionDefaults.placementPolicy)));
    m_workerProfileCombo->addItem(tr("Select a Worker profile"), QString{});
    for (const core::client::WorkerProfileSnapshot& profile : m_workerProfiles)
    {
        const QString name = QString::fromUtf8(profile.displayName.c_str());
        m_workerProfileCombo->addItem(profile.enabled ? name : tr("%1 (Disabled)").arg(name),
                                      QString::fromStdString(profile.id.value));
    }
    if (m_executionDefaults.preferredWorkerProfileId.has_value())
    {
        const int profileIndex =
            m_workerProfileCombo->findData(QString::fromStdString(m_executionDefaults.preferredWorkerProfileId->value));
        m_workerProfileCombo->setCurrentIndex(profileIndex >= 0 ? profileIndex : 0);
    }

    auto* formLayout = new QFormLayout();
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    formLayout->addRow(tr("Files"), outputPathLayout);
    formLayout->addRow(tr("Format"), m_formatCombo);
    formLayout->addRow(tr("Execution"), m_executionCombo);
    formLayout->addRow(QString(), m_remoteSettingsWidget);
    formLayout->addRow(m_jpegQualityLabel, m_jpegQualitySpinBox);
    formLayout->addRow(m_pngCompressionLabel, m_pngCompressionSpinBox);
    formLayout->addRow(m_tiffCompressionLabel, m_tiffCompressionCombo);
    formLayout->addRow(tr("Maximum dimension"), m_maximumDimensionSpinBox);
    formLayout->addRow(tr("Output color space"), m_outputColorSpaceCombo);
    formLayout->addRow(QString(), m_includeMetadataCheckBox);

    m_statusLabel->setObjectName(QStringLiteral("exportStatusLabel"));
    m_statusLabel->setText(profileLoadFailed ? tr("Worker profiles are unavailable.") : tr("Ready."));
    m_statusLabel->setWordWrap(true);
    m_elapsedLabel->setObjectName(QStringLiteral("exportElapsedLabel"));
    m_elapsedLabel->setText(tr("Elapsed: 0.0 s"));
    auto* statusLayout = new QHBoxLayout();
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->addWidget(m_statusLabel, 1);
    statusLayout->addWidget(m_elapsedLabel);

    auto* buttonBox = new QDialogButtonBox(this);
    m_cancelButton = buttonBox->addButton(tr("Close"), QDialogButtonBox::RejectRole);
    m_cancelButton->setObjectName(QStringLiteral("exportCancelButton"));
    m_exportButton = buttonBox->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    m_exportButton->setObjectName(QStringLiteral("exportStartButton"));
    m_exportButton->setDefault(true);

    auto* dialogLayout = new QVBoxLayout(this);
    dialogLayout->addWidget(m_sourceLabel);
    dialogLayout->addLayout(formLayout);
    dialogLayout->addSpacing(6);
    dialogLayout->addLayout(statusLayout);
    dialogLayout->addWidget(buttonBox);

    m_elapsedTimer->setInterval(100);
    connect(m_elapsedTimer, &QTimer::timeout, this, &ExportDialog::updateElapsedTime);
    connect(m_browseButton, &QToolButton::clicked, this, &ExportDialog::browseOutputPath);
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &ExportDialog::updateFormatControls);
    connect(m_executionCombo, &QComboBox::currentIndexChanged, this, &ExportDialog::updateExecutionControls);
    connect(m_exportButton, &QPushButton::clicked, this, &ExportDialog::startExport);
    connect(m_cancelButton, &QPushButton::clicked, this, &ExportDialog::cancelOrClose);
    subscribeToExportEvents(exportEventSource);
    updateFormatControls();
    updateExecutionControls();
}

// 목적: dialog 종료 중 active export가 남아 있으면 cancellation 요청
// 입력: 없음
// 출력: dialog가 소유한 request publish 중단
ExportDialog::~ExportDialog()
{
    if (m_activeRequestId.has_value())
    {
        const core::client::ExportRequestId requestId = *m_activeRequestId;
        m_activeRequestId.reset();
        (void)m_exportClient->cancelExport(requestId);
    }
}

// 목적: window close를 active export cancellation과 함께 처리
// 입력: event: Qt close event
// 출력: active request cancellation 후 dialog 닫힘
void ExportDialog::closeEvent(QCloseEvent* event)
{
    if (m_activeRequestId.has_value())
    {
        const core::client::ExportRequestId requestId = *m_activeRequestId;
        m_activeRequestId.reset();
        (void)m_exportClient->cancelExport(requestId);
    }
    QDialog::closeEvent(event);
}

// 목적: current widget 값으로 placement-aware single-photo ExportRequest 제출
// 입력: 없음
// 출력: accepted request 추적 또는 validation 오류 표시
void ExportDialog::startExport()
{
    if (m_activeRequestId.has_value())
    {
        return;
    }

    if (m_sources.size() > 1)
    {
        startItemListExport();
        return;
    }
    if (m_outputPathEdit == nullptr)
    {
        m_statusLabel->setText(tr("Export cannot start: no source photo is selected."));
        return;
    }

    const core::export_::RasterExportOptions options = collectOptions();
    const QString outputPath = pathForFormat(m_outputPathEdit->text(), options.format);
    m_outputPathEdit->setText(outputPath);
    const core::export_::RasterExportPathResult validatedPath = core::export_::validateRasterExportPath(outputPath);
    if (validatedPath.hasError())
    {
        m_statusLabel->setText(tr("Export cannot start: %1").arg(validatedPath.error().message));
        return;
    }

    if (QFileInfo::exists(validatedPath.value()) &&
        QMessageBox::question(this,
                              tr("Replace Export"),
                              tr("The output file already exists. Replace it?"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
    {
        m_statusLabel->setText(tr("Export was not started."));
        return;
    }

    core::client::ExportExecutionDefaults execution = collectExecutionDefaults();
    std::optional<core::client::WorkerProfileSnapshot> workerProfile = selectedWorkerProfile();
    if (execution.placementPolicy != core::client::ExportPlacementPolicy::LocalOnly)
    {
        if (execution.placementPolicy == core::client::ExportPlacementPolicy::RemoteOnly &&
            (!workerProfile.has_value() || !workerProfile->enabled))
        {
            m_statusLabel->setText(tr("Export cannot start: select an enabled Worker profile."));
            return;
        }
        if (workerProfile.has_value() && workerProfile->enabled)
        {
            discoverMissingStorageBinding(*workerProfile, validatedPath.value());
        }
    }

    core::client::ExportFileRequest request{
        toClientSource(m_source),
        toClientString(validatedPath.value()),
        {},
        core::orchestration::toClientDevelopParams(m_developParams),
        toClientOptions(options),
    };
    const core::client::SubmitExportCommand command{core::client::ExportRequest{std::move(request)},
                                                    collectPlacementOptions(execution)};
    const core::client::ExportSubmissionResult submitted = m_exportClient->submitExport(command);
    if (submitted.hasError())
    {
        LOG_WARN("export", "GUI Export submission failed: {}", submitted.error().technicalMessage);
        m_statusLabel->setText(tr("Export cannot start: %1").arg(exportErrorText(submitted.error())));
        return;
    }

    const core::client::ExportExecutionDefaultsResult savedDefaults =
        m_exportDefaultsClient->saveExportExecutionDefaults(execution);
    if (savedDefaults.hasError())
    {
        LOG_WARN("export", "Unable to save Export execution defaults: {}", savedDefaults.error().technicalMessage);
    }
    m_executionDefaults = execution;
    beginRequest(submitted.value().requestId, execution.placementPolicy, options, validatedPath.value());
}

// 목적: output 표의 여러 행을 하나의 item-list ExportRequest로 제출
// 입력: 없음
// 출력: accepted request 추적 또는 첫 validation 오류 표시
void ExportDialog::startItemListExport()
{
    const core::export_::RasterExportOptions options = collectOptions();
    std::vector<core::client::ExportFileRequest> items;
    int existingOutputCount = 0;
    items.reserve(static_cast<std::size_t>(m_sources.size()));

    for (qsizetype index = 0; index < m_sources.size(); ++index)
    {
        QLineEdit* const outputEdit = m_outputPathEdits.value(index, nullptr);
        if (outputEdit == nullptr)
        {
            m_statusLabel->setText(tr("Export cannot start: an output row is unavailable."));
            return;
        }

        const QString outputPath = pathForFormat(outputEdit->text(), options.format);
        outputEdit->setText(outputPath);
        const core::export_::RasterExportPathResult validatedPath = core::export_::validateRasterExportPath(outputPath);
        if (validatedPath.hasError())
        {
            m_statusLabel->setText(tr("Export cannot start for %1: %2")
                                       .arg(m_sources.at(index).source.displayName, validatedPath.error().message));
            return;
        }

        existingOutputCount += QFileInfo::exists(validatedPath.value()) ? 1 : 0;

        const ExportDialogSource& source = m_sources.at(index);
        std::optional<core::client::EditorDevelopParams> developParams;
        if (source.developParams.has_value())
        {
            developParams = core::orchestration::toClientDevelopParams(*source.developParams);
        }
        items.push_back({toClientSource(source.source),
                         toClientString(validatedPath.value()),
                         toClientString(source.catalogPath),
                         std::move(developParams),
                         toClientOptions(options),
                         source.useDefaultDevelopParamsWhenCatalogPhotoMissing});
    }

    if (existingOutputCount > 0 &&
        QMessageBox::question(this,
                              tr("Replace Exports"),
                              tr("%1 output file(s) already exist. Replace them?").arg(existingOutputCount),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
    {
        m_statusLabel->setText(tr("Export was not started."));
        return;
    }

    core::client::ExportExecutionDefaults execution = collectExecutionDefaults();
    std::optional<core::client::WorkerProfileSnapshot> workerProfile = selectedWorkerProfile();
    if (execution.placementPolicy != core::client::ExportPlacementPolicy::LocalOnly)
    {
        if (execution.placementPolicy == core::client::ExportPlacementPolicy::RemoteOnly &&
            (!workerProfile.has_value() || !workerProfile->enabled))
        {
            m_statusLabel->setText(tr("Export cannot start: select an enabled Worker profile."));
            return;
        }
        if (workerProfile.has_value() && workerProfile->enabled)
        {
            discoverMissingStorageBinding(*workerProfile, fromClientString(items.front().outputLocator));
        }
    }

    core::client::ExportItemListRequest request{std::move(items)};
    const core::client::SubmitExportCommand command{core::client::ExportRequest{std::move(request)},
                                                    collectPlacementOptions(execution)};
    const core::client::ExportSubmissionResult submitted = m_exportClient->submitExport(command);
    if (submitted.hasError())
    {
        LOG_WARN("export", "GUI item-list Export submission failed: {}", submitted.error().technicalMessage);
        m_statusLabel->setText(tr("Export cannot start: %1").arg(exportErrorText(submitted.error())));
        return;
    }

    const core::client::ExportExecutionDefaultsResult savedDefaults =
        m_exportDefaultsClient->saveExportExecutionDefaults(execution);
    if (savedDefaults.hasError())
    {
        LOG_WARN("export", "Unable to save Export execution defaults: {}", savedDefaults.error().technicalMessage);
    }
    m_executionDefaults = execution;
    beginRequest(submitted.value().requestId, execution.placementPolicy, options, {});
}

// 목적: active export를 취소하거나 terminal dialog를 닫음
// 입력: 없음
// 출력: cancellation 요청 또는 dialog reject
void ExportDialog::cancelOrClose()
{
    if (!m_activeRequestId.has_value())
    {
        reject();
        return;
    }

    m_cancelButton->setEnabled(false);
    m_statusLabel->setText(tr("Cancelling export..."));
    (void)m_exportClient->cancelExport(*m_activeRequestId);
}

// 목적: 현재 format filter를 사용해 output file 선택 dialog 표시
// 입력: 없음
// 출력: 선택한 output path를 field에 반영
void ExportDialog::browseOutputPath()
{
    if (m_outputPathEdit == nullptr)
    {
        return;
    }

    const auto format = static_cast<core::export_::RasterExportFormat>(m_formatCombo->currentData().toInt());
    if (m_outputPathEdits.size() > 1)
    {
        const QString selectedFolder = QFileDialog::getExistingDirectory(
            this, tr("Choose Export Folder"), QFileInfo(m_outputPathEdit->text()).absolutePath());
        if (!selectedFolder.isEmpty())
        {
            for (QLineEdit* const outputEdit : std::as_const(m_outputPathEdits))
            {
                outputEdit->setText(
                    pathForFormat(QDir(selectedFolder).filePath(QFileInfo(outputEdit->text()).fileName()), format));
            }
        }
        return;
    }

    const QString selectedPath =
        QFileDialog::getSaveFileName(this, tr("Choose Export File"), m_outputPathEdit->text(), outputFilter(format));
    if (!selectedPath.isEmpty())
    {
        m_outputPathEdit->setText(pathForFormat(selectedPath, format));
    }
}

// 목적: 선택 format에 해당하는 quality/compression control만 표시
// 입력: 없음
// 출력: format별 row visibility와 output 확장자 갱신
void ExportDialog::updateFormatControls()
{
    const auto format = static_cast<core::export_::RasterExportFormat>(m_formatCombo->currentData().toInt());
    const bool jpeg = format == core::export_::RasterExportFormat::Jpeg;
    const bool png = format == core::export_::RasterExportFormat::Png;
    const bool tiff = format == core::export_::RasterExportFormat::Tiff;
    m_jpegQualityLabel->setVisible(jpeg);
    m_jpegQualitySpinBox->setVisible(jpeg);
    m_pngCompressionLabel->setVisible(png);
    m_pngCompressionSpinBox->setVisible(png);
    m_tiffCompressionLabel->setVisible(tiff);
    m_tiffCompressionCombo->setVisible(tiff);
    for (QLineEdit* const outputEdit : std::as_const(m_outputPathEdits))
    {
        outputEdit->setText(pathForFormat(outputEdit->text(), format));
    }
}

// 목적: 선택 실행 mode에 따라 manual Remote endpoint control 표시
// 입력: 없음
// 출력: Remote 또는 Auto mode에서 endpoint editor 표시
void ExportDialog::updateExecutionControls()
{
    m_remoteSettingsWidget->setVisible(selectedPlacementPolicy() != core::client::ExportPlacementPolicy::LocalOnly);
}

// 목적: Qt-free Export event source를 current dialog request presentation에 연결
// 입력: eventSource: application-scoped Export lifecycle source
// 출력: RAII subscription 보관 또는 구조화된 오류 logging
void ExportDialog::subscribeToExportEvents(core::client::IExportEventSource& eventSource)
{
    const core::client::ExportSubscriptionResult subscribed =
        eventSource.subscribeToExports([this](const core::client::ExportEvent& event) { handleExportEvent(event); });
    if (subscribed.hasError())
    {
        LOG_ERROR("export", "Unable to subscribe to Export events: {}", subscribed.error().technicalMessage);
        m_statusLabel->setText(tr("Export status updates are unavailable."));
        m_exportButton->setEnabled(false);
        return;
    }
    m_exportSubscription = subscribed.value();
}

// 목적: immutable Export event에서 current request progress와 exact terminal만 처리
// 입력: event: initial snapshot 또는 accepted/progress/completed/failed/cancelled transition
// 출력: matching request presentation 갱신
void ExportDialog::handleExportEvent(const core::client::ExportEvent& event)
{
    if (event.progress.has_value())
    {
        handleProgress(*event.progress);
    }
    if (event.completed.has_value())
    {
        handleCompleted(*event.completed);
    }
    if (event.failed.has_value())
    {
        handleFailed(*event.failed);
    }
    if (event.cancelled.has_value())
    {
        handleCancelled(event.cancelled->requestId);
    }
}

// 목적: widget 값을 Core raster export option으로 조립
// 입력: 없음
// 출력: Export client command로 투영할 RasterExportOptions
core::export_::RasterExportOptions ExportDialog::collectOptions() const
{
    core::export_::RasterExportOptions options;
    options.format = static_cast<core::export_::RasterExportFormat>(m_formatCombo->currentData().toInt());
    options.jpegQuality = m_jpegQualitySpinBox->value();
    options.pngCompression = m_pngCompressionSpinBox->value();
    options.tiffCompression =
        static_cast<core::export_::TiffCompression>(m_tiffCompressionCombo->currentData().toInt());
    options.maximumDimension = m_maximumDimensionSpinBox->value();
    options.outputColorSpace =
        static_cast<core::export_::RasterOutputColorSpace>(m_outputColorSpaceCombo->currentData().toInt());
    options.includeMetadata = m_includeMetadataCheckBox->isChecked();
    return options;
}

// 목적: widget 값을 저장 가능한 placement mode와 preferred Worker identity로 조립
// 입력: 없음
// 출력: 현재 ExportExecutionDefaults snapshot
core::client::ExportExecutionDefaults ExportDialog::collectExecutionDefaults() const
{
    core::client::ExportExecutionDefaults defaults;
    defaults.placementPolicy = selectedPlacementPolicy();
    const QString selectedId = m_workerProfileCombo->currentData().toString();
    if (!selectedId.isEmpty())
    {
        defaults.preferredWorkerProfileId = core::client::WorkerProfileId{selectedId.toStdString()};
    }
    return defaults;
}

// 목적: 비어 있는 profile storage binding을 현재 source/output marker로 best-effort 보완
// 입력: profile: 선택된 Worker snapshot, outputPath: Desktop absolute output path
// 출력: 두 marker를 찾으면 profile 저장소와 현재 snapshot 갱신
void ExportDialog::discoverMissingStorageBinding(core::client::WorkerProfileSnapshot& profile,
                                                 const QString& outputPath) const
{
    if (!QUuid::fromString(QString::fromStdString(profile.expectedSourceStorageId)).isNull() &&
        !QUuid::fromString(QString::fromStdString(profile.expectedOutputStorageId)).isNull())
    {
        return;
    }

    const worker::client::LocateSharedStorageResult sourceStorage =
        worker::client::SharedStorageLocator::locateSource(m_source.path);
    const worker::client::LocateSharedStorageResult outputStorage =
        worker::client::SharedStorageLocator::locateOutput(outputPath);
    if (sourceStorage.hasValue() && outputStorage.hasValue())
    {
        profile.expectedSourceStorageId = sourceStorage.value().storageId.toString(QUuid::WithoutBraces).toStdString();
        profile.expectedOutputStorageId = outputStorage.value().storageId.toString(QUuid::WithoutBraces).toStdString();
        const core::client::WorkerProfileResult updated =
            m_workerProfileClient->updateWorkerProfile(core::client::UpdateWorkerProfileCommand{
                profile.id,
                profile.displayName,
                profile.host,
                profile.port,
                profile.enabled,
                profile.expectedSourceStorageId,
                profile.expectedOutputStorageId,
            });
        if (updated.hasValue())
        {
            profile = updated.value();
        }
    }
}

// 목적: UI 실행 mode와 preferred Worker identity를 Qt-free placement option으로 변환
// 입력: defaults: 저장 가능한 mode와 optional profile identity
// 출력: endpoint detail을 포함하지 않는 LocalOnly, RemoteOnly 또는 Auto placement 계약
core::client::ExportPlacementOptions ExportDialog::collectPlacementOptions(
    const core::client::ExportExecutionDefaults& defaults) const
{
    core::client::ExportPlacementOptions placement;
    placement.workerProfileId = defaults.preferredWorkerProfileId;
    switch (defaults.placementPolicy)
    {
    case core::client::ExportPlacementPolicy::LocalOnly:
        placement.policy = core::client::ExportPlacementPolicy::LocalOnly;
        placement.workerProfileId.reset();
        break;
    case core::client::ExportPlacementPolicy::RemoteOnly:
        placement.policy = core::client::ExportPlacementPolicy::RemoteOnly;
        break;
    case core::client::ExportPlacementPolicy::Auto:
        placement.policy = core::client::ExportPlacementPolicy::Auto;
        break;
    }
    return placement;
}

// 목적: combo에서 현재 선택된 stable identity를 Worker profile snapshot으로 해석
// 입력: 없음
// 출력: 저장된 profile과 일치하면 snapshot, placeholder·missing이면 nullopt
std::optional<core::client::WorkerProfileSnapshot> ExportDialog::selectedWorkerProfile() const
{
    const std::string selectedId = m_workerProfileCombo->currentData().toString().toStdString();
    const auto match = std::ranges::find(
        m_workerProfiles, core::client::WorkerProfileId{selectedId}, &core::client::WorkerProfileSnapshot::id);
    return match == m_workerProfiles.end() ? std::nullopt : std::optional<core::client::WorkerProfileSnapshot>{*match};
}

// 목적: execution combo의 현재 선택을 typed mode로 변환
// 입력: 없음
// 출력: Local, Remote 또는 Auto 실행 mode
core::client::ExportPlacementPolicy ExportDialog::selectedPlacementPolicy() const
{
    return static_cast<core::client::ExportPlacementPolicy>(m_executionCombo->currentData().toInt());
}

// 목적: export 실행 여부에 따라 output option control과 action 상태 갱신
// 입력: enabled: option을 편집할 수 있으면 true
// 출력: 관련 widget enabled 상태 변경
void ExportDialog::setOptionControlsEnabled(const bool enabled)
{
    m_outputTable->setEnabled(enabled);
    m_browseButton->setEnabled(enabled);
    m_formatCombo->setEnabled(enabled);
    m_executionCombo->setEnabled(enabled);
    m_remoteSettingsWidget->setEnabled(enabled);
    m_jpegQualitySpinBox->setEnabled(enabled);
    m_pngCompressionSpinBox->setEnabled(enabled);
    m_tiffCompressionCombo->setEnabled(enabled);
    m_maximumDimensionSpinBox->setEnabled(enabled);
    m_outputColorSpaceCombo->setEnabled(enabled);
    m_includeMetadataCheckBox->setEnabled(enabled);
}

// 목적: accepted request의 elapsed timer와 running presentation 시작
// 입력: requestId: 선택 executor가 발급한 identity, mode/options/outputPath: terminal 처리용 snapshot
// 출력: active request 추적과 Running 상태 표시
void ExportDialog::beginRequest(const core::client::ExportRequestId requestId,
                                const core::client::ExportPlacementPolicy placementPolicy,
                                core::export_::RasterExportOptions options,
                                QString outputPath)
{
    m_activeRequestId = requestId;
    m_submittedOptions = options;
    m_submittedOutputPath = std::move(outputPath);
    m_elapsedClock.start();
    m_elapsedTimer->start();
    setOptionControlsEnabled(false);
    m_exportButton->setEnabled(false);
    m_cancelButton->setText(tr("Cancel"));
    m_cancelButton->setEnabled(true);
    switch (placementPolicy)
    {
    case core::client::ExportPlacementPolicy::LocalOnly:
        m_statusLabel->setText(tr("Running local export..."));
        break;
    case core::client::ExportPlacementPolicy::RemoteOnly:
        m_statusLabel->setText(tr("Running remote export..."));
        break;
    case core::client::ExportPlacementPolicy::Auto:
        m_statusLabel->setText(tr("Running automatic export..."));
        break;
    }
    updateElapsedTime();
}

// 목적: terminal request 추적과 elapsed timer 종료
// 입력: allowRetry: option 수정 후 재제출을 허용하면 true
// 출력: active identity 제거와 button 상태 갱신
void ExportDialog::finishRequest(const bool allowRetry)
{
    m_activeRequestId.reset();
    m_elapsedTimer->stop();
    updateElapsedTime();
    setOptionControlsEnabled(allowRetry);
    m_exportButton->setEnabled(allowRetry);
    m_cancelButton->setText(tr("Close"));
    m_cancelButton->setEnabled(true);
}

// 목적: current request progress를 사용자용 running 상태에 반영
// 입력: progress: request identity와 item 누적 상태
// 출력: 일치하는 request의 status text 갱신
void ExportDialog::handleProgress(const core::client::ExportProgress& progress)
{
    if (!m_activeRequestId.has_value() || progress.requestId != *m_activeRequestId)
    {
        return;
    }

    m_statusLabel->setText(tr("Exporting %1 of %2...").arg(progress.completedCount).arg(progress.totalCount));
}

// 목적: current request item report를 성공 또는 실패 presentation으로 변환
// 입력: result: request identity와 item별 terminal report
// 출력: 성공 설정 저장 또는 실패 상세 표시
void ExportDialog::handleCompleted(const core::client::ExportResult& result)
{
    if (!m_activeRequestId.has_value() || result.requestId != *m_activeRequestId)
    {
        return;
    }

    if (result.report.items.size() == 1 && result.report.items.front().succeeded)
    {
        const QString outputPath = result.report.items.front().outputLocator.empty()
                                       ? m_submittedOutputPath
                                       : fromClientString(result.report.items.front().outputLocator);
        const core::client::ExportRasterDefaultsResult savedDefaults =
            m_exportDefaultsClient->saveRasterExportDefaults(toClientOptions(m_submittedOptions));
        if (savedDefaults.hasError())
        {
            LOG_WARN("export", "Unable to save raster Export defaults: {}", savedDefaults.error().technicalMessage);
        }
        m_statusLabel->setText(tr("Export completed: %1").arg(outputPath));
        finishRequest(false);
        return;
    }

    if (result.report.totalCount > 1)
    {
        if (result.report.failedCount == 0)
        {
            const core::client::ExportRasterDefaultsResult savedDefaults =
                m_exportDefaultsClient->saveRasterExportDefaults(toClientOptions(m_submittedOptions));
            if (savedDefaults.hasError())
            {
                LOG_WARN("export", "Unable to save raster Export defaults: {}", savedDefaults.error().technicalMessage);
            }
            m_statusLabel->setText(tr("Export completed: %1 files.").arg(result.report.succeededCount));
            finishRequest(false);
            return;
        }

        QString firstError;
        for (const core::client::ExportItemResult& item : result.report.items)
        {
            if (!item.succeeded && item.error.has_value())
            {
                firstError = exportErrorText(*item.error, item.failureKind);
                break;
            }
        }
        m_statusLabel->setText(firstError.isEmpty() ? tr("Export completed: %1 succeeded, %2 failed.")
                                                          .arg(result.report.succeededCount)
                                                          .arg(result.report.failedCount)
                                                    : tr("Export completed: %1 succeeded, %2 failed. %3")
                                                          .arg(result.report.succeededCount)
                                                          .arg(result.report.failedCount)
                                                          .arg(firstError));
        finishRequest(true);
        return;
    }

    QString errorMessage = tr("Export did not produce a successful output file.");
    if (!result.report.items.empty() && result.report.items.front().error.has_value())
    {
        errorMessage = exportErrorText(*result.report.items.front().error, result.report.items.front().failureKind);
    }
    m_statusLabel->setText(tr("Export failed: %1").arg(errorMessage));
    finishRequest(true);
}

// 목적: pipeline-level current request 실패를 사용자에게 표시
// 입력: issue: request identity와 structured error
// 출력: retry 가능한 Failed 상태 표시
void ExportDialog::handleFailed(const core::client::ExportIssue& issue)
{
    if (!m_activeRequestId.has_value() || issue.requestId != *m_activeRequestId)
    {
        return;
    }

    LOG_WARN("export", "GUI Export request failed: {}", issue.error.technicalMessage);
    m_statusLabel->setText(tr("Export failed: %1").arg(exportErrorText(issue.error)));
    finishRequest(true);
}

// 목적: current request cancellation terminal event를 사용자에게 표시
// 입력: requestId: 취소된 request identity
// 출력: retry 가능한 Cancelled 상태 표시
void ExportDialog::handleCancelled(const core::client::ExportRequestId requestId)
{
    if (!m_activeRequestId.has_value() || requestId != *m_activeRequestId)
    {
        return;
    }

    m_statusLabel->setText(tr("Export cancelled."));
    finishRequest(true);
}

// 목적: common ClientError와 Export domain failure를 localized presentation으로 변환
// 입력: error: diagnostic-only detail을 가진 common error, failureKind: optional Export domain 의미
// 출력: technicalMessage를 직접 노출하지 않는 사용자용 문자열
QString ExportDialog::exportErrorText(const core::client::ClientError& error,
                                      const core::client::ExportFailureKind failureKind) const
{
    switch (failureKind)
    {
    case core::client::ExportFailureKind::Eligibility:
        return tr("The selected source or destination is not eligible for this export target.");
    case core::client::ExportFailureKind::DispatchExhausted:
        return tr("The Worker could not accept this export after the allowed attempts.");
    case core::client::ExportFailureKind::Ambiguous:
        return tr("The Worker result is uncertain. Verify the output before trying again.");
    case core::client::ExportFailureKind::Execution:
    case core::client::ExportFailureKind::None:
        break;
    }

    switch (error.code)
    {
    case core::client::ClientErrorCode::InvalidArgument:
        return tr("Check the export options and destination.");
    case core::client::ClientErrorCode::NotFound:
        return tr("The source, destination, or selected Worker profile is no longer available.");
    case core::client::ClientErrorCode::PermissionDenied:
        return tr("Flexraw cannot access the selected source or destination.");
    case core::client::ClientErrorCode::UnsupportedFormat:
        return tr("The selected input or output format is not supported.");
    case core::client::ClientErrorCode::DecodeFailed:
        return tr("The source image could not be decoded.");
    case core::client::ClientErrorCode::DatabaseError:
        return tr("The Catalog state required for export could not be read.");
    case core::client::ClientErrorCode::Conflict:
        return tr("The export state changed. Refresh it and try again.");
    case core::client::ClientErrorCode::Cancelled:
        return tr("The export was cancelled.");
    case core::client::ClientErrorCode::ThumbnailUnavailable:
    case core::client::ClientErrorCode::Unknown:
        return tr("The export could not be completed.");
    }
    return tr("The export could not be completed.");
}

// 목적: active request 경과시간 label 갱신
// 입력: 없음
// 출력: 0.1초 단위 elapsed text 표시
void ExportDialog::updateElapsedTime()
{
    const qint64 elapsedMilliseconds = m_elapsedClock.isValid() ? m_elapsedClock.elapsed() : 0;
    m_elapsedLabel->setText(tr("Elapsed: %1 s").arg(static_cast<double>(elapsedMilliseconds) / 1000.0, 0, 'f', 1));
}

// 목적: 현재 format에 맞는 QFileDialog filter 생성
// 입력: format: 선택한 raster format
// 출력: 번역된 file filter 문자열
QString ExportDialog::outputFilter(const core::export_::RasterExportFormat format) const
{
    switch (format)
    {
    case core::export_::RasterExportFormat::Jpeg:
        return tr("JPEG image (*.jpg *.jpeg)");
    case core::export_::RasterExportFormat::Png:
        return tr("PNG image (*.png)");
    case core::export_::RasterExportFormat::Tiff:
        return tr("TIFF image (*.tif *.tiff)");
    }

    return tr("Image files (*)");
}

}  // namespace flexraw::ui::export_
