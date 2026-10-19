#include "mainwindow.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QAction>
#include <QByteArray>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include "catalog_editor_facade.h"
#include "catalog_list_widget.h"
#include "catalog_photo_page_qt_adapter.h"
#include "console_mode_widget.h"
#include "develop_panel.h"
#include "display_frame_qt_adapter.h"
#include "editor_client_projection.h"
#include "editor_settings.h"
#include "export_dialog.h"
#include "export_source_projection.h"
#include "log.h"
#include "photo_identity.h"
#include "preview_widget.h"
#include "qt_activity_adapter.h"
#include "qt_preview_presentation_event_adapter.h"
#include "settings_dialog.h"
#include "shared_storage_locator.h"
#include "source_binding.h"
#include "source_resolution_widget.h"

namespace flexraw::ui::mainwindow
{
namespace
{
constexpr int ActivityHideDelayMs = 700;

// 목적: UTF-8 client string을 Qt presentation 문자열로 변환
// 입력: value: byte length가 명시된 UTF-8 string
// 출력: 같은 Unicode text를 보유한 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Qt presentation path를 Qt-free client command용 UTF-8 string으로 변환
// 입력: value: native file dialog 또는 Qt adapter가 보유한 path
// 출력: byte length가 보존된 UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: optional Qt Folder scope를 Qt-free UTF-8 client scope로 변환
// 입력: folderPath: exact Folder path 또는 전체 Catalog를 뜻하는 빈 값
// 출력: byte length가 보존된 optional UTF-8 path
[[nodiscard]] std::optional<std::string> toClientFolderPath(const std::optional<QString>& folderPath)
{
    if (!folderPath.has_value())
    {
        return std::nullopt;
    }
    return folderPath->toUtf8().toStdString();
}

// 목적: optional internal Project identity를 fixed-width client identity로 변환
// 입력: projectId: Project scope 또는 Catalog/Folder scope를 뜻하는 빈 값
// 출력: optional client Project identity
[[nodiscard]] std::optional<core::client::ClientProjectId> toClientProjectId(
    const std::optional<core::catalog::ProjectId>& projectId)
{
    if (!projectId.has_value())
    {
        return std::nullopt;
    }
    return core::client::ClientProjectId{projectId->value};
}

// 목적: 현재 MainWindow scope를 Qt-free bounded page request로 조립
// 입력: folderPath: optional exact Folder, projectId: optional Project identity, inclusionFilter/cullingFilter: 조회 범위
// 출력: 두 scope가 배타적이고 filter가 고정된 첫 page request
[[nodiscard]] core::client::CatalogPhotoPageRequest makePhotoPageRequest(
    const std::optional<QString>& folderPath,
    const std::optional<core::catalog::ProjectId>& projectId,
    core::client::CatalogInclusionFilter inclusionFilter,
    core::client::CatalogCullingFilter cullingFilter)
{
    core::client::CatalogPhotoPageRequest request;
    request.exactFolderPath = projectId.has_value() ? std::nullopt : toClientFolderPath(folderPath);
    request.projectId = toClientProjectId(projectId);
    request.inclusionFilter = inclusionFilter;
    request.cullingFilter = cullingFilter;
    return request;
}

// 목적: target 선별 상태가 현재 filter 결과에 남는지 판정
// 입력: filter: active page filter, state: mutation 이후 Photo 상태
// 출력: All 또는 filter와 state가 일치하면 true
[[nodiscard]] bool cullingFilterIncludes(core::client::CatalogCullingFilter filter,
                                         core::client::CatalogCullingState state) noexcept
{
    return filter == core::client::CatalogCullingFilter::All ||
           (filter == core::client::CatalogCullingFilter::Pick && state == core::client::CatalogCullingState::Pick) ||
           (filter == core::client::CatalogCullingFilter::Reject && state == core::client::CatalogCullingState::Reject);
}

// 목적: target 등록 포함 상태가 현재 filter 결과에 남는지 판정
// 입력: filter: active page filter, state: mutation 이후 Photo 상태
// 출력: filter와 state가 일치하면 true
[[nodiscard]] bool inclusionFilterIncludes(core::client::CatalogInclusionFilter filter,
                                           core::client::CatalogInclusionState state) noexcept
{
    return (filter == core::client::CatalogInclusionFilter::IncludedOnly &&
            state == core::client::CatalogInclusionState::Included) ||
           (filter == core::client::CatalogInclusionFilter::ExcludedOnly &&
            state == core::client::CatalogInclusionState::Excluded);
}

// 목적: Qt-free Folder item kind를 기존 Folder list용 domain kind로 변환
// 입력: kind: client snapshot file 분류
// 출력: 같은 의미의 supported file kind
[[nodiscard]] core::types::SupportedFileKind toSupportedFileKind(core::client::CatalogFileKind kind) noexcept
{
    switch (kind)
    {
    case core::client::CatalogFileKind::Raw:
        return core::types::SupportedFileKind::Raw;
    case core::client::CatalogFileKind::RasterImage:
        return core::types::SupportedFileKind::RasterImage;
    case core::client::CatalogFileKind::Unknown:
        return core::types::SupportedFileKind::Unknown;
    }
    return core::types::SupportedFileKind::Unknown;
}

// 목적: Folder presentation의 Core file kind를 Editor client command enum으로 변환
// 입력: kind: scan entry의 지원 file 종류
// 출력: 같은 의미의 Qt-free client enum
[[nodiscard]] core::client::CatalogFileKind toClientFileKind(core::types::SupportedFileKind kind) noexcept
{
    switch (kind)
    {
    case core::types::SupportedFileKind::Raw:
        return core::client::CatalogFileKind::Raw;
    case core::types::SupportedFileKind::RasterImage:
        return core::client::CatalogFileKind::RasterImage;
    case core::types::SupportedFileKind::Unknown:
        return core::client::CatalogFileKind::Unknown;
    }
    return core::client::CatalogFileKind::Unknown;
}

// 목적: Qt-free Folder item snapshot을 기존 Folder list presentation record로 변환
// 입력: item: UTF-8 source/display state와 file kind
// 출력: Ready 상태의 CatalogEntry
[[nodiscard]] core::catalog::CatalogEntry toCatalogEntry(const core::client::FolderItemSnapshot& item)
{
    return {
        {fromClientString(item.sourcePath),
         fromClientString(item.extension),
         fromClientString(item.displayName),
         toSupportedFileKind(item.kind)},
        core::types::FileScanStatus::Ready,
    };
}

// 목적: 선택된 Folder entry를 Qt-free Editor source activation command로 변환
// 입력: entry: scan 결과의 normalized source와 표시 metadata
// 출력: Catalog 등록과 Editor 선택에 사용할 client command
[[nodiscard]] core::client::ActivateEditorSourceCommand toActivateEditorSourceCommand(
    const core::catalog::CatalogEntry& entry)
{
    return {toClientString(entry.file.path),
            toClientString(entry.file.extension),
            toClientString(entry.file.displayName),
            toClientFileKind(entry.file.kind)};
}

// 목적: Qt-free Editor client snapshot을 현재 Qt presentation model로 변환
// 입력: editorClient: authoritative Editor state contract
// 출력: 기존 Qt widget이 소비하는 transitional Editor state
[[nodiscard]] core::orchestration::EditorState currentEditorState(const core::client::IEditorClient& editorClient)
{
    return core::orchestration::fromClientEditorSnapshot(editorClient.editorSnapshot());
}

// 목적: Qt widget viewport를 fixed-width Preview presentation command로 동기화
// 입력: previewClient: authoritative Preview command contract, size: 현재 widget pixel 크기
// 출력: 유효한 크기만 전달하고 실패는 diagnostics log로 기록
void syncPreviewViewport(core::client::IPreviewPresentationClient& previewClient, const QSize& size)
{
    if (size.width() <= 0 || size.height() <= 0)
    {
        return;
    }
    const core::client::PreviewViewportResult updated = previewClient.setPreviewViewport(
        {{static_cast<std::uint32_t>(size.width()), static_cast<std::uint32_t>(size.height())}});
    if (updated.hasError())
    {
        LOG_WARN("preview", "Unable to update Preview viewport: {}", updated.error().technicalMessage);
    }
}

// 목적: source request가 사용자 명시적 resolution command인지 구분
// 입력: kind: baseline/verification/resolution request 종류
// 출력: Accept/Register/Relink이면 true
[[nodiscard]] bool isExplicitSourceResolution(core::client::SourceRequestKind kind) noexcept
{
    return kind == core::client::SourceRequestKind::AcceptReplacement ||
           kind == core::client::SourceRequestKind::RegisterReplacementAsNew ||
           kind == core::client::SourceRequestKind::RelinkSource;
}

}  // namespace

// 목적: Flexraw 첫 catalog-to-preview window를 editor session adapter로 초기화
// 입력: catalogEditorFacade: GUI와 Folder lifecycle boundary,
//       Export command/event/default contract, applicationSettings/profile client, optional Worker health와
//       Catalog startup notice/settings capability, parent: Qt 부모 widget
// 출력: 초기화된 MainWindow 객체
MainWindow::MainWindow(facade::CatalogEditorFacade& catalogEditorFacade,
                       core::client::IExportClient& exportClient,
                       core::client::IExportEventSource& exportEventSource,
                       core::client::IExportDefaultsClient& exportDefaultsClient,
                       QSettings& applicationSettings,
                       core::client::IWorkerProfileClient& workerProfileClient,
                       CatalogStartupNotice catalogStartupNotice,
                       core::client::IWorkerHealthClient* const workerHealthClient,
                       core::client::IWorkerHealthEventSource* const workerHealthEventSource,
                       core::client::ICatalogStartupSettingsClient* const catalogStartupSettingsClient,
                       platform::IDisplayProfileService* const displayProfileService,
                       facade::DisplayTargetResolver displayTargetResolver,
                       QWidget* parent)
    : QMainWindow(parent),
      m_catalogWidget(new catalog::CatalogListWidget(this)),
      m_developPanel(new editor::DevelopPanel(this)),
      m_sourceResolutionWidget(new catalog::SourceResolutionWidget(this)),
      m_previewWidget(new editor::PreviewWidget(this)),
      m_displayPresentationAdapter(std::make_unique<facade::QtDisplayPresentationAdapter>(
          displayProfileService, std::move(displayTargetResolver))),
      m_consoleWidget(new cli::ConsoleModeWidget(
          catalogEditorFacade, catalogEditorFacade, exportClient, exportEventSource, exportDefaultsClient, this)),
      m_catalogPhotoClient(&catalogEditorFacade),
      m_catalogCullingClient(&catalogEditorFacade),
      m_catalogInclusionClient(&catalogEditorFacade),
      m_catalogProjectClient(&catalogEditorFacade),
      m_catalogFolderClient(&catalogEditorFacade),
      m_catalogSessionClient(&catalogEditorFacade),
      m_editorClient(&catalogEditorFacade),
      m_editorStateEventSource(&catalogEditorFacade),
      m_folderImportClient(&catalogEditorFacade),
      m_folderImportEventSource(&catalogEditorFacade),
      m_catalogThumbnailClient(&catalogEditorFacade),
      m_catalogThumbnailEventSource(&catalogEditorFacade),
      m_previewPresentationClient(&catalogEditorFacade),
      m_previewComparisonClient(&catalogEditorFacade),
      m_previewPresentationEventSource(&catalogEditorFacade),
      m_sourceResolutionClient(&catalogEditorFacade),
      m_sourceResolutionEventSource(&catalogEditorFacade),
      m_activityAdapter(new QtActivityAdapter(catalogEditorFacade,
                                              catalogEditorFacade,
                                              catalogEditorFacade,
                                              catalogEditorFacade,
                                              catalogEditorFacade,
                                              this)),
      m_exportClient(&exportClient),
      m_exportEventSource(&exportEventSource),
      m_exportDefaultsClient(&exportDefaultsClient),
      m_applicationSettings(&applicationSettings),
      m_workerProfileClient(&workerProfileClient),
      m_workerHealthClient(workerHealthClient),
      m_workerHealthEventSource(workerHealthEventSource),
      m_catalogStartupSettingsClient(catalogStartupSettingsClient),
      m_catalogStartupNotice(std::move(catalogStartupNotice)),
      m_contentStack(new QStackedWidget(this))
{
    setWindowTitle(tr("Flexraw"));
    resize(1200, 800);
    m_developPanel->setAdjustmentControlStyle(
        settings::EditorSettings(*m_applicationSettings).loadAdjustmentControlStyle());

    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    m_newCatalogAction = fileMenu->addAction(tr("New Catalog..."));
    connect(m_newCatalogAction, &QAction::triggered, this, &MainWindow::createCatalog);
    m_openCatalogAction = fileMenu->addAction(tr("Open Catalog..."));
    connect(m_openCatalogAction, &QAction::triggered, this, &MainWindow::openCatalog);
    m_closeCatalogAction = fileMenu->addAction(tr("Close Catalog"));
    m_closeCatalogAction->setObjectName(QStringLiteral("closeCatalogAction"));
    m_closeCatalogAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
    connect(m_closeCatalogAction, &QAction::triggered, this, &MainWindow::closeCatalog);
    m_importFolderAction = fileMenu->addAction(tr("Import Folder..."));
    m_importFolderAction->setEnabled(false);
    connect(m_importFolderAction, &QAction::triggered, this, &MainWindow::importFolder);
    m_openFolderAction = fileMenu->addAction(tr("Open Folder..."));
    m_openFolderAction->setObjectName(QStringLiteral("openFolderAction"));
    m_openFolderAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
    connect(m_openFolderAction, &QAction::triggered, this, &MainWindow::openFolder);
    m_saveDevelopAction = fileMenu->addAction(tr("Save Develop Settings"));
    m_saveDevelopAction->setShortcut(QKeySequence::Save);
    connect(m_saveDevelopAction, &QAction::triggered, this, &MainWindow::saveCurrentPhoto);
    m_exportAction = fileMenu->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Export..."));
    m_exportAction->setObjectName(QStringLiteral("exportPhotoAction"));
    m_exportAction->setShortcut(QKeySequence::fromString(QStringLiteral("Ctrl+Shift+E")));
    connect(m_exportAction, &QAction::triggered, this, &MainWindow::exportCurrentPhoto);
    fileMenu->addSeparator();
    m_consoleModeAction = fileMenu->addAction(tr("Console Mode"));
    m_consoleModeAction->setCheckable(true);
    m_consoleModeAction->setShortcut(QKeySequence::fromString(QStringLiteral("Ctrl+Alt+C")));
    connect(m_consoleModeAction, &QAction::toggled, this, &MainWindow::setConsoleMode);
    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    m_undoDevelopAction = editMenu->addAction(tr("Undo"));
    m_undoDevelopAction->setShortcut(QKeySequence::Undo);
    connect(m_undoDevelopAction, &QAction::triggered, this, &MainWindow::undoDevelopAdjustment);
    m_redoDevelopAction = editMenu->addAction(tr("Redo"));
    m_redoDevelopAction->setShortcut(QKeySequence::Redo);
    connect(m_redoDevelopAction, &QAction::triggered, this, &MainWindow::redoDevelopAdjustment);
    editMenu->addSeparator();
    m_pickSelectedPhotosAction = editMenu->addAction(tr("Mark Selected as Pick"));
    m_pickSelectedPhotosAction->setObjectName(QStringLiteral("pickSelectedPhotosAction"));
    m_pickSelectedPhotosAction->setShortcut(QKeySequence(Qt::Key_P));
    connect(m_pickSelectedPhotosAction, &QAction::triggered, this, [this] {
        setSelectedPhotoCulling(core::client::CatalogCullingState::Pick);
    });
    m_rejectSelectedPhotosAction = editMenu->addAction(tr("Mark Selected as Reject"));
    m_rejectSelectedPhotosAction->setObjectName(QStringLiteral("rejectSelectedPhotosAction"));
    m_rejectSelectedPhotosAction->setShortcut(QKeySequence(Qt::Key_X));
    connect(m_rejectSelectedPhotosAction, &QAction::triggered, this, [this] {
        setSelectedPhotoCulling(core::client::CatalogCullingState::Reject);
    });
    m_clearSelectedCullingAction = editMenu->addAction(tr("Clear Selected Pick/Reject"));
    m_clearSelectedCullingAction->setObjectName(QStringLiteral("clearSelectedCullingAction"));
    m_clearSelectedCullingAction->setShortcut(QKeySequence(Qt::Key_U));
    connect(m_clearSelectedCullingAction, &QAction::triggered, this, [this] {
        setSelectedPhotoCulling(core::client::CatalogCullingState::Neutral);
    });
    editMenu->addSeparator();
    m_excludeSelectedPhotosAction = editMenu->addAction(tr("Exclude Selected from Catalog"));
    m_excludeSelectedPhotosAction->setObjectName(QStringLiteral("excludeSelectedPhotosAction"));
    connect(m_excludeSelectedPhotosAction, &QAction::triggered, this, [this] {
        setSelectedPhotoInclusion(core::client::CatalogInclusionState::Excluded);
    });
    m_restoreSelectedPhotosAction = editMenu->addAction(tr("Restore Selected to Catalog"));
    m_restoreSelectedPhotosAction->setObjectName(QStringLiteral("restoreSelectedPhotosAction"));
    connect(m_restoreSelectedPhotosAction, &QAction::triggered, this, [this] {
        setSelectedPhotoInclusion(core::client::CatalogInclusionState::Included);
    });
    editMenu->addSeparator();
    m_settingsAction = editMenu->addAction(tr("Settings..."));
    m_settingsAction->setObjectName(QStringLiteral("settingsAction"));
    m_settingsAction->setMenuRole(QAction::PreferencesRole);
    m_settingsAction->setShortcut(QKeySequence::Preferences);
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    QAction* fitPreviewAction = viewMenu->addAction(tr("Fit Preview to Window"));
    fitPreviewAction->setObjectName(QStringLiteral("fitPreviewAction"));
    fitPreviewAction->setShortcut(QKeySequence::fromString(QStringLiteral("Ctrl+0")));
    connect(fitPreviewAction, &QAction::triggered, m_previewWidget, &editor::PreviewWidget::fitToWindow);
    QAction* previewPixelsAction = viewMenu->addAction(tr("Preview Pixels at 100%"));
    previewPixelsAction->setObjectName(QStringLiteral("previewPixelsAction"));
    previewPixelsAction->setShortcut(QKeySequence::fromString(QStringLiteral("Ctrl+1")));
    connect(previewPixelsAction,
            &QAction::triggered,
            m_previewWidget,
            &editor::PreviewWidget::showPreviewPixelsAt100Percent);
    viewMenu->addSeparator();
    QAction* zoomInAction = viewMenu->addAction(tr("Zoom In"));
    zoomInAction->setObjectName(QStringLiteral("zoomPreviewInAction"));
    zoomInAction->setShortcuts(QKeySequence::keyBindings(QKeySequence::ZoomIn));
    connect(zoomInAction, &QAction::triggered, m_previewWidget, &editor::PreviewWidget::zoomIn);
    QAction* zoomOutAction = viewMenu->addAction(tr("Zoom Out"));
    zoomOutAction->setObjectName(QStringLiteral("zoomPreviewOutAction"));
    zoomOutAction->setShortcuts(QKeySequence::keyBindings(QKeySequence::ZoomOut));
    connect(zoomOutAction, &QAction::triggered, m_previewWidget, &editor::PreviewWidget::zoomOut);
    viewMenu->addSeparator();
    m_beforePreviewAction = viewMenu->addAction(tr("Before (Default Develop)"));
    m_beforePreviewAction->setObjectName(QStringLiteral("beforePreviewAction"));
    m_beforePreviewAction->setCheckable(true);
    connect(m_beforePreviewAction, &QAction::toggled, this, [this](bool enabled) {
        setPreviewComparisonMode(false, enabled);
    });
    m_splitPreviewAction = viewMenu->addAction(tr("Split Before / After"));
    m_splitPreviewAction->setObjectName(QStringLiteral("splitPreviewAction"));
    m_splitPreviewAction->setCheckable(true);
    connect(m_splitPreviewAction, &QAction::toggled, this, [this](bool enabled) {
        setPreviewComparisonMode(true, enabled);
    });

    auto* splitter = new QSplitter(this);
    splitter->setObjectName(QStringLiteral("workspaceSplitter"));
    splitter->setHandleWidth(5);
    splitter->setStyleSheet(
        QStringLiteral("QSplitter#workspaceSplitter::handle:horizontal { background: palette(mid); margin: 0 2px; }"));
    auto* catalogSurface = new QWidget(splitter);
    auto* catalogLayout = new QVBoxLayout(catalogSurface);
    catalogLayout->setContentsMargins(0, 0, 0, 0);
    catalogLayout->setSpacing(0);
    m_catalogFolderScopeComboBox = new QComboBox(catalogSurface);
    m_catalogFolderScopeComboBox->setObjectName(QStringLiteral("catalogFolderScopeComboBox"));
    m_catalogFolderScopeComboBox->setPlaceholderText(tr("No catalog"));
    m_catalogFolderScopeComboBox->setToolTip(tr("Catalog photo scope"));
    m_catalogFolderScopeComboBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_catalogFolderScopeComboBox->setMinimumContentsLength(20);
    m_catalogFolderScopeComboBox->setEnabled(false);
    catalogLayout->addWidget(m_catalogFolderScopeComboBox);
    auto* projectNavigation = new QWidget(catalogSurface);
    auto* projectNavigationLayout = new QHBoxLayout(projectNavigation);
    projectNavigationLayout->setContentsMargins(0, 0, 0, 0);
    m_catalogProjectScopeComboBox = new QComboBox(projectNavigation);
    m_catalogProjectScopeComboBox->setObjectName(QStringLiteral("catalogProjectScopeComboBox"));
    m_catalogProjectScopeComboBox->setPlaceholderText(tr("No catalog"));
    m_catalogProjectScopeComboBox->setEnabled(false);
    projectNavigationLayout->addWidget(m_catalogProjectScopeComboBox, 1);
    m_catalogProjectMenuButton = new QToolButton(projectNavigation);
    m_catalogProjectMenuButton->setObjectName(QStringLiteral("catalogProjectMenuButton"));
    m_catalogProjectMenuButton->setText(tr("Project"));
    m_catalogProjectMenuButton->setPopupMode(QToolButton::InstantPopup);
    auto* projectMenu = new QMenu(m_catalogProjectMenuButton);
    m_createProjectAction = projectMenu->addAction(tr("New Project..."));
    m_createProjectAction->setObjectName(QStringLiteral("createProjectAction"));
    m_renameProjectAction = projectMenu->addAction(tr("Rename Project..."));
    m_renameProjectAction->setObjectName(QStringLiteral("renameProjectAction"));
    m_removeProjectAction = projectMenu->addAction(tr("Delete Project"));
    m_removeProjectAction->setObjectName(QStringLiteral("removeProjectAction"));
    projectMenu->addSeparator();
    m_addSelectedPhotosToProjectAction = projectMenu->addAction(tr("Add Selected Photos to Project..."));
    m_addSelectedPhotosToProjectAction->setObjectName(QStringLiteral("addSelectedPhotosToProjectAction"));
    m_removeSelectedPhotosFromProjectAction = projectMenu->addAction(tr("Remove Selected Photos from Project"));
    m_removeSelectedPhotosFromProjectAction->setObjectName(QStringLiteral("removeSelectedPhotosFromProjectAction"));
    m_catalogProjectMenuButton->setMenu(projectMenu);
    m_catalogProjectMenuButton->setEnabled(false);
    projectNavigationLayout->addWidget(m_catalogProjectMenuButton);
    catalogLayout->addWidget(projectNavigation);
    m_catalogInclusionFilterComboBox = new QComboBox(catalogSurface);
    m_catalogInclusionFilterComboBox->setObjectName(QStringLiteral("catalogInclusionFilterComboBox"));
    m_catalogInclusionFilterComboBox->setToolTip(tr("Catalog inclusion view"));
    m_catalogInclusionFilterComboBox->addItem(
        tr("Included Photos"), static_cast<int>(core::client::CatalogInclusionFilter::IncludedOnly));
    m_catalogInclusionFilterComboBox->addItem(
        tr("Excluded Photos"), static_cast<int>(core::client::CatalogInclusionFilter::ExcludedOnly));
    m_catalogInclusionFilterComboBox->setEnabled(false);
    catalogLayout->addWidget(m_catalogInclusionFilterComboBox);
    m_catalogCullingFilterComboBox = new QComboBox(catalogSurface);
    m_catalogCullingFilterComboBox->setObjectName(QStringLiteral("catalogCullingFilterComboBox"));
    m_catalogCullingFilterComboBox->setToolTip(tr("Catalog pick/reject filter"));
    m_catalogCullingFilterComboBox->addItem(tr("All Photos"),
                                            static_cast<int>(core::client::CatalogCullingFilter::All));
    m_catalogCullingFilterComboBox->addItem(tr("Picked"), static_cast<int>(core::client::CatalogCullingFilter::Pick));
    m_catalogCullingFilterComboBox->addItem(tr("Rejected"),
                                            static_cast<int>(core::client::CatalogCullingFilter::Reject));
    m_catalogCullingFilterComboBox->setEnabled(false);
    catalogLayout->addWidget(m_catalogCullingFilterComboBox);
    catalogLayout->addWidget(m_catalogWidget, 1);
    auto* catalogNavigationLayout = new QHBoxLayout();
    catalogNavigationLayout->setContentsMargins(4, 2, 4, 2);
    catalogNavigationLayout->addStretch();
    m_previousCatalogPageButton = new QToolButton(catalogSurface);
    m_previousCatalogPageButton->setObjectName(QStringLiteral("catalogPreviousPageButton"));
    m_previousCatalogPageButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_previousCatalogPageButton->setToolTip(tr("Previous catalog page"));
    m_previousCatalogPageButton->setAutoRaise(true);
    m_previousCatalogPageButton->setEnabled(false);
    catalogNavigationLayout->addWidget(m_previousCatalogPageButton);
    m_nextCatalogPageButton = new QToolButton(catalogSurface);
    m_nextCatalogPageButton->setObjectName(QStringLiteral("catalogNextPageButton"));
    m_nextCatalogPageButton->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    m_nextCatalogPageButton->setToolTip(tr("Next catalog page"));
    m_nextCatalogPageButton->setAutoRaise(true);
    m_nextCatalogPageButton->setEnabled(false);
    catalogNavigationLayout->addWidget(m_nextCatalogPageButton);
    catalogLayout->addLayout(catalogNavigationLayout);
    auto* editorSurface = new QWidget(splitter);
    auto* editorLayout = new QVBoxLayout(editorSurface);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    editorLayout->addWidget(m_sourceResolutionWidget);
    editorLayout->addWidget(m_previewWidget, 1);
    splitter->addWidget(catalogSurface);
    splitter->addWidget(editorSurface);
    splitter->addWidget(m_developPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({280, 920, 280});
    m_contentStack->addWidget(splitter);
    m_contentStack->addWidget(m_consoleWidget);
    setCentralWidget(m_contentStack);
    initializeActivityStatus();
    subscribeToActivityEvents();
    subscribeToFolderOperationEvents();
    subscribeToCatalogThumbnailEvents();
    subscribeToPreviewPresentationEvents();
    subscribeToSourceResolutionEvents();

    connect(m_catalogWidget, &catalog::CatalogListWidget::entrySelected, this, &MainWindow::showSelectedEntry);
    connect(m_catalogWidget, &catalog::CatalogListWidget::photoSelected, this, &MainWindow::showSelectedCatalogPhoto);
    connect(m_previousCatalogPageButton, &QToolButton::clicked, this, &MainWindow::showPreviousCatalogPage);
    connect(m_nextCatalogPageButton, &QToolButton::clicked, this, &MainWindow::showNextCatalogPage);
    connect(m_catalogFolderScopeComboBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        changeCatalogFolderScope(index);
    });
    connect(m_catalogProjectScopeComboBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        changeCatalogProjectScope(index);
    });
    connect(m_catalogInclusionFilterComboBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        changeCatalogInclusionFilter(index);
    });
    connect(m_catalogCullingFilterComboBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        changeCatalogCullingFilter(index);
    });
    connect(m_createProjectAction, &QAction::triggered, this, &MainWindow::createProject);
    connect(m_renameProjectAction, &QAction::triggered, this, &MainWindow::renameCurrentProject);
    connect(m_removeProjectAction, &QAction::triggered, this, &MainWindow::removeCurrentProject);
    connect(m_addSelectedPhotosToProjectAction, &QAction::triggered, this, &MainWindow::addSelectedPhotosToProject);
    connect(m_removeSelectedPhotosFromProjectAction,
            &QAction::triggered,
            this,
            &MainWindow::removeSelectedPhotosFromProject);
    connect(m_catalogWidget, &QListWidget::itemSelectionChanged, this, &MainWindow::updateProjectActions);
    connect(m_catalogWidget,
            &catalog::CatalogListWidget::thumbnailWindowChanged,
            this,
            [this](const core::client::ReplaceCatalogThumbnailWindowCommand& command) {
                const core::client::CatalogThumbnailWindowResult result =
                    m_catalogThumbnailClient->replaceThumbnailWindow(command);
                if (result.hasError())
                {
                    m_catalogWidget->clearThumbnailWindowGeneration();
                    LOG_WARN("catalog", "Unable to replace thumbnail window: {}", result.error().technicalMessage);
                    return;
                }
                m_catalogWidget->acceptThumbnailWindow(result.value().generation);
            });
    connect(m_catalogWidget, &catalog::CatalogListWidget::thumbnailWindowCleared, this, [this] {
        m_catalogWidget->clearThumbnailWindowGeneration();
        const core::client::CatalogThumbnailClearResult result = m_catalogThumbnailClient->clearThumbnailWindow();
        if (result.hasError())
        {
            LOG_WARN("catalog", "Unable to clear thumbnail window: {}", result.error().technicalMessage);
        }
    });
    connect(m_developPanel, &editor::DevelopPanel::paramsChanged, this, &MainWindow::applyDevelopParams);
    connect(m_developPanel, &editor::DevelopPanel::adjustmentStarted, this, &MainWindow::beginDevelopAdjustment);
    connect(m_developPanel, &editor::DevelopPanel::adjustmentFinished, this, &MainWindow::finishDevelopAdjustment);
    connect(m_previewWidget, &editor::PreviewWidget::viewportSizeChanged, this, [this](const QSize& size) {
        syncPreviewViewport(*m_previewPresentationClient, size);
    });
    const core::client::EditorStateSubscriptionResult editorStateSubscription =
        m_editorStateEventSource->subscribeToEditorState([this](const core::client::EditorStateEvent& event) {
            updateEditorStateUi(core::orchestration::fromClientEditorSnapshot(event.snapshot));
        });
    if (editorStateSubscription.hasValue())
    {
        m_editorStateSubscription = editorStateSubscription.value();
    }
    else
    {
        LOG_ERROR(
            "app", "Unable to subscribe to Editor state events: {}", editorStateSubscription.error().technicalMessage);
        updateEditorStateUi(currentEditorState(*m_editorClient));
    }
    connect(m_sourceResolutionWidget,
            &catalog::SourceResolutionWidget::acceptReplacementRequested,
            this,
            &MainWindow::acceptReplacement);
    connect(m_sourceResolutionWidget,
            &catalog::SourceResolutionWidget::registerReplacementAsNewRequested,
            this,
            &MainWindow::registerReplacementAsNew);
    connect(m_sourceResolutionWidget,
            &catalog::SourceResolutionWidget::relinkSourceRequested,
            this,
            &MainWindow::relinkSource);
    connect(m_consoleWidget, &cli::ConsoleModeWidget::exitRequested, this, [this] {
        m_consoleModeAction->setChecked(false);
    });
    m_developPanel->setEnabled(false);
    updateEditorActions();
    if (m_catalogSessionClient->catalogSnapshot().isOpen)
    {
        (void)refreshCatalogPhotos();
    }
    else
    {
        m_previewWidget->showMessage(tr("Open or create a catalog to browse photos."));
        statusBar()->showMessage(tr("Ready."));
    }
    QTimer::singleShot(0, this, &MainWindow::presentCatalogStartupNotice);
}

// 목적: Managed Catalog startup open, 복구 또는 실패 결과를 비차단 사용자 안내로 표시
// 입력: 없음
// 출력: 정상 open 상태 표시 또는 복구·실패 warning dialog 생성
void MainWindow::presentCatalogStartupNotice()
{
    if (m_catalogStartupNotice.kind == CatalogStartupNoticeKind::None)
    {
        return;
    }
    if (m_catalogStartupNotice.kind == CatalogStartupNoticeKind::CatalogOpened)
    {
        statusBar()->showMessage(
            tr("Opened catalog: %1").arg(QDir::toNativeSeparators(m_catalogStartupNotice.requestedCatalogPath)));
        return;
    }

    const bool recovered = m_catalogStartupNotice.kind == CatalogStartupNoticeKind::RecoveredWithManagedCatalog;
    auto* notice = new QMessageBox(
        QMessageBox::Warning,
        recovered ? tr("Catalog Recovered") : tr("Catalog Unavailable"),
        recovered ? tr("The previous catalog could not be opened. Flexraw opened the managed catalog instead.")
                  : tr("The startup catalog could not be opened. Open or create a catalog to continue."),
        QMessageBox::Ok,
        this);
    notice->setObjectName(QStringLiteral("catalogStartupNotice"));
    if (!m_catalogStartupNotice.requestedCatalogPath.isEmpty())
    {
        notice->setInformativeText(
            tr("Requested catalog: %1").arg(QDir::toNativeSeparators(m_catalogStartupNotice.requestedCatalogPath)));
    }
    notice->setAttribute(Qt::WA_DeleteOnClose);
    notice->open();
}

// 목적: status bar의 compact Activity label, indeterminate progress와 cancel control 구성
// 입력: 없음
// 출력: 초기에는 숨겨진 Activity presentation widget 생성
void MainWindow::initializeActivityStatus()
{
    m_activityLabel = new QLabel(statusBar());
    m_activityLabel->setObjectName(QStringLiteral("activityStatusLabel"));
    m_activityProgressBar = new QProgressBar(statusBar());
    m_activityProgressBar->setObjectName(QStringLiteral("activityProgressBar"));
    m_activityProgressBar->setRange(0, 0);
    m_activityProgressBar->setTextVisible(false);
    m_activityProgressBar->setMaximumWidth(80);
    m_cancelActivityButton = new QToolButton(statusBar());
    m_cancelActivityButton->setObjectName(QStringLiteral("cancelActivityButton"));
    m_cancelActivityButton->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    m_cancelActivityButton->setAutoRaise(true);
    connect(m_cancelActivityButton, &QToolButton::clicked, this, &MainWindow::cancelDisplayedActivity);
    m_activityHideTimer = new QTimer(this);
    m_activityHideTimer->setObjectName(QStringLiteral("activityHideTimer"));
    m_activityHideTimer->setInterval(ActivityHideDelayMs);
    m_activityHideTimer->setSingleShot(true);
    connect(m_activityHideTimer, &QTimer::timeout, this, [this] {
        m_activityLabel->hide();
        m_activityProgressBar->hide();
        m_cancelActivityButton->hide();
    });
    statusBar()->addPermanentWidget(m_activityLabel);
    statusBar()->addPermanentWidget(m_activityProgressBar);
    statusBar()->addPermanentWidget(m_cancelActivityButton);
    m_activityLabel->hide();
    m_activityProgressBar->hide();
    m_cancelActivityButton->hide();
}

// 목적: Qt-free Activity client를 실제 MainWindow presentation consumer에 연결
// 입력: 없음
// 출력: RAII subscription 보관 또는 구조화된 오류 logging
void MainWindow::subscribeToActivityEvents()
{
    const core::client::ActivitySubscriptionResult subscription = m_activityAdapter->subscribeToActivities(
        [this](const core::client::ActivityEvent& event) { updateActivityUi(event); });
    if (subscription.hasValue())
    {
        m_activitySubscription = subscription.value();
        return;
    }
    LOG_ERROR("app", "Unable to subscribe to Activity events: {}", subscription.error().technicalMessage);
}

// 목적: Qt-free Folder operation event source를 MainWindow presentation에 연결
// 입력: 없음
// 출력: RAII subscription 보관 또는 구조화된 오류 logging
void MainWindow::subscribeToFolderOperationEvents()
{
    const core::client::FolderOperationSubscriptionResult subscription =
        m_folderImportEventSource->subscribeToFolderOperations(
            [this](const core::client::FolderOperationEvent& event) { handleFolderOperationEvent(event); });
    if (subscription.hasValue())
    {
        m_folderOperationSubscription = subscription.value();
        return;
    }
    LOG_ERROR("app", "Unable to subscribe to Folder operations: {}", subscription.error().technicalMessage);
}

// 목적: Qt-free Catalog thumbnail event source를 MainWindow view에 연결
// 입력: 없음
// 출력: RAII subscription 보관 또는 구조화된 오류 logging
void MainWindow::subscribeToCatalogThumbnailEvents()
{
    const core::client::CatalogThumbnailSubscriptionResult subscription =
        m_catalogThumbnailEventSource->subscribeToCatalogThumbnails(
            [this](const core::client::CatalogThumbnailEvent& event) { handleCatalogThumbnailEvent(event); });
    if (subscription.hasError())
    {
        LOG_ERROR(
            "catalog", "Unable to subscribe to Catalog thumbnail events: {}", subscription.error().technicalMessage);
        return;
    }
    m_catalogThumbnailSubscription = subscription.value();
}

// 목적: Qt-free Source Resolution event source를 MainWindow presentation에 연결
// 입력: 없음
// 출력: RAII subscription 보관 또는 구조화된 오류 logging
void MainWindow::subscribeToSourceResolutionEvents()
{
    const core::client::SourceResolutionSubscriptionResult subscription =
        m_sourceResolutionEventSource->subscribeToSourceResolution(
            [this](const core::client::SourceResolutionEvent& event) { handleSourceResolutionEvent(event); });
    if (subscription.hasError())
    {
        LOG_ERROR(
            "catalog", "Unable to subscribe to Source Resolution events: {}", subscription.error().technicalMessage);
        return;
    }
    m_sourceResolutionSubscription = subscription.value();
}

// 목적: Qt-free Preview presentation event source를 MainWindow view에 연결
// 입력: 없음
// 출력: RAII subscription 보관 또는 구조화된 오류 logging
void MainWindow::subscribeToPreviewPresentationEvents()
{
    const core::client::PreviewPresentationSubscriptionResult subscription =
        m_previewPresentationEventSource->subscribeToPreviewPresentation(
            [this](const core::client::PreviewPresentationEvent& event) { handlePreviewPresentationEvent(event); });
    if (subscription.hasError())
    {
        LOG_ERROR(
            "preview", "Unable to subscribe to Preview presentation events: {}", subscription.error().technicalMessage);
        return;
    }
    m_previewPresentationSubscription = subscription.value();
}

// 목적: native top-level window 생성 뒤 current QScreen 관찰과 첫 ICC presentation target 설정
// 입력: event: Qt show lifecycle event
// 출력: screenChanged 연결과 retained Preview frame의 current monitor 재투영
void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QWindow* const topLevelWindow = windowHandle();
    if (topLevelWindow == nullptr)
    {
        return;
    }
    if (!m_observingDisplayScreen)
    {
        connect(topLevelWindow, &QWindow::screenChanged, this, [this](QScreen* screen) {
            updateDisplayPresentationTarget(screen);
        });
        m_observingDisplayScreen = true;
    }
    updateDisplayPresentationTarget(topLevelWindow->screen());
}

// 목적: QWindow current screen의 ICC target으로 retained Preview presentation 갱신
// 입력: screen: Qt backend가 선택한 current screen
// 출력: target이 바뀌면 Current/Before frame을 RAW 재처리 없이 다시 표시
void MainWindow::updateDisplayPresentationTarget(QScreen* screen)
{
    if (!m_displayPresentationAdapter->setScreen(screen))
    {
        return;
    }

    const QImage current = m_displayPresentationAdapter->presentedFrame(facade::DisplayPresentationSlot::Current);
    if (!current.isNull())
    {
        m_previewWidget->showPreview(current);
    }
    const QImage comparison = m_displayPresentationAdapter->presentedFrame(facade::DisplayPresentationSlot::Comparison);
    if (!comparison.isNull())
    {
        m_previewWidget->setComparisonPreview(comparison);
    }

    const facade::DisplayPresentationFallbackReason fallback = m_displayPresentationAdapter->fallbackReason();
    if (fallback == facade::DisplayPresentationFallbackReason::None ||
        fallback == facade::DisplayPresentationFallbackReason::NoExplicitProfile ||
        fallback == facade::DisplayPresentationFallbackReason::ServiceUnavailable)
    {
        return;
    }
    LOG_WARN("color", "Monitor ICC presentation fell back to sRGB: {}", static_cast<int>(fallback));
    statusBar()->showMessage(tr("Monitor color profile unavailable; showing sRGB."), 5000);
}

// 목적: immutable Preview snapshot/event를 image·analysis·status presentation에 반영
// 입력: event: initial state, frame update, warning 또는 terminal
// 출력: PreviewWidget과 DevelopPanel 표시 갱신
void MainWindow::handlePreviewPresentationEvent(const core::client::PreviewPresentationEvent& event)
{
    handlePreviewComparisonEvent(event);
    const core::client::EditorSnapshot editor = m_editorClient->editorSnapshot();
    const core::client::PreviewFrameSnapshot* frame = nullptr;
    if ((event.frameUpdated || event.initial) && event.snapshot.currentFrame.has_value())
    {
        frame = &*event.snapshot.currentFrame;
        if (!editor.hasSelection || editor.photoId != frame->photoId ||
            editor.developRevision != frame->developRevision)
        {
            frame = nullptr;
        }
    }
    if (frame != nullptr)
    {
        const QImage image =
            m_displayPresentationAdapter->presentFrame(facade::DisplayPresentationSlot::Current, frame->frame);
        if (!image.isNull())
        {
            m_previewWidget->showPreview(image);
        }
        if (frame->mode == core::client::PreviewPresentationMode::Final && frame->analysis.has_value())
        {
            m_developPanel->setHistogram(facade::toDevelopHistogram(frame->analysis->histogram));
            m_developPanel->setClippingSummary(facade::toDevelopClipping(frame->analysis->clipping));
        }
        m_developPanel->setEnabled(true);

        const QString displayName =
            editor.source.has_value() ? fromClientString(editor.source->displayName) : QString{};
        statusBar()->showMessage(frame->tier == core::client::PreviewFrameTier::Standard
                                     ? tr("Showing standard preview for %1.").arg(displayName)
                                     : tr("Showing %1.").arg(displayName));
    }
    else if (!event.snapshot.currentFrame.has_value())
    {
        m_displayPresentationAdapter->clearFrame(facade::DisplayPresentationSlot::Current);
    }
    if (event.warning.has_value())
    {
        LOG_WARN("preview", "Preview warning: {}", event.warning->error.technicalMessage);
    }
    if (!event.terminal.has_value() || event.terminal->state != core::client::PreviewTerminalState::Failed)
    {
        return;
    }

    if (event.terminal->photoId.has_value() && (!editor.hasSelection || editor.photoId != *event.terminal->photoId ||
                                                editor.developRevision != event.terminal->developRevision))
    {
        return;
    }

    if (event.terminal->error.has_value())
    {
        LOG_WARN("preview", "Preview failed: {}", event.terminal->error->technicalMessage);
    }
    m_developPanel->setEnabled(false);
    (void)m_previewComparisonClient->setPreviewComparison({false});
    m_displayPresentationAdapter->clearFrame(facade::DisplayPresentationSlot::Current);
    m_previewWidget->showMessage(tr("Unable to load preview."));
    const QString displayName = editor.source.has_value() ? fromClientString(editor.source->displayName) : QString{};
    statusBar()->showMessage(tr("Preview unavailable for %1.").arg(displayName));
}

// 목적: 비교 snapshot과 warning/terminal을 After frame과 독립적으로 표시
// 입력: event: immutable 비교 transition 또는 초기 snapshot
// 출력: 비교 frame·action 상태 갱신
void MainWindow::handlePreviewComparisonEvent(const core::client::PreviewPresentationEvent& event)
{
    if (!event.initial && !event.comparison.has_value())
    {
        return;
    }
    const core::client::EditorSnapshot editor = m_editorClient->editorSnapshot();
    if (!event.snapshot.comparison.enabled || !editor.hasSelection || event.snapshot.selectedPhotoId != editor.photoId)
    {
        const QSignalBlocker beforeBlocker(m_beforePreviewAction);
        const QSignalBlocker splitBlocker(m_splitPreviewAction);
        m_beforePreviewAction->setChecked(false);
        m_splitPreviewAction->setChecked(false);
        m_displayPresentationAdapter->clearFrame(facade::DisplayPresentationSlot::Comparison);
        m_previewWidget->clearComparisonPreview();
        m_previewWidget->setComparisonDisplayMode(editor::PreviewWidget::ComparisonDisplayMode::After);
    }
    else if (event.snapshot.comparison.baselineFrame.has_value() &&
             event.snapshot.comparison.baselineFrame->photoId == editor.photoId)
    {
        const QImage image = m_displayPresentationAdapter->presentFrame(facade::DisplayPresentationSlot::Comparison,
                                                                        event.snapshot.comparison.baselineFrame->frame);
        if (!image.isNull())
        {
            m_previewWidget->setComparisonPreview(image);
        }
    }
    if (event.comparison.has_value())
    {
        if (event.comparison->warning.has_value())
        {
            LOG_WARN("preview", "Preview comparison warning: {}", event.comparison->warning->error.technicalMessage);
            statusBar()->showMessage(tr("Before preview is unavailable."));
        }
        if (event.comparison->terminal.has_value() && event.comparison->terminal->error.has_value())
        {
            LOG_WARN("preview", "Preview comparison failed: {}", event.comparison->terminal->error->technicalMessage);
            statusBar()->showMessage(tr("Before preview is unavailable."));
        }
    }
}

// 목적: Before·Split GUI mode를 Core 비교 command와 동기화
// 입력: split: 분할 표시 여부, enabled: 선택한 action의 활성 상태
// 출력: 비교 요청 또는 After 표시 복귀
void MainWindow::setPreviewComparisonMode(bool split, bool enabled)
{
    QAction* const otherAction = split ? m_beforePreviewAction : m_splitPreviewAction;
    if (enabled)
    {
        const QSignalBlocker blocker(otherAction);
        otherAction->setChecked(false);
    }
    const bool comparing = m_beforePreviewAction->isChecked() || m_splitPreviewAction->isChecked();
    if (!comparing)
    {
        m_displayPresentationAdapter->clearFrame(facade::DisplayPresentationSlot::Comparison);
    }
    const core::client::PreviewComparisonResult result = m_previewComparisonClient->setPreviewComparison({comparing});
    if (result.hasError())
    {
        const QSignalBlocker beforeBlocker(m_beforePreviewAction);
        const QSignalBlocker splitBlocker(m_splitPreviewAction);
        m_beforePreviewAction->setChecked(false);
        m_splitPreviewAction->setChecked(false);
        m_previewWidget->setComparisonDisplayMode(editor::PreviewWidget::ComparisonDisplayMode::After);
        statusBar()->showMessage(tr("Before preview is unavailable."));
        return;
    }
    const auto mode = !comparing ? editor::PreviewWidget::ComparisonDisplayMode::After
                      : split    ? editor::PreviewWidget::ComparisonDisplayMode::Split
                                 : editor::PreviewWidget::ComparisonDisplayMode::Before;
    m_previewWidget->setComparisonDisplayMode(mode);
    if (comparing)
    {
        statusBar()->showMessage(tr("Preparing default Develop preview..."));
    }
}

// 목적: immutable Catalog thumbnail frame/issue/terminal을 list presentation에 반영
// 입력: event: initial state 또는 window lifecycle transition
// 출력: current generation row icon/terminal 상태 갱신
void MainWindow::handleCatalogThumbnailEvent(const core::client::CatalogThumbnailEvent& event)
{
    if (event.frame.has_value())
    {
        const QImage image = facade::toQImage(event.frame->frame);
        m_catalogWidget->applyThumbnail(event.frame->generation, event.frame->identity, image);
    }
    if (event.issue.has_value())
    {
        m_catalogWidget->markThumbnailFailed(event.issue->generation, event.issue->identity);
        const std::string identity =
            event.issue->identity.kind == core::client::CatalogThumbnailIdentityKind::CatalogPhoto
                ? std::string("PhotoId ") + std::to_string(event.issue->identity.photoId.value)
                : event.issue->identity.transientSourceLocator;
        LOG_DEBUG("catalog", "Catalog thumbnail unavailable for {}: {}", identity, event.issue->error.technicalMessage);
    }
    if (event.terminal.has_value() && event.terminal->state == core::client::CatalogThumbnailTerminalState::Failed &&
        event.terminal->error.has_value())
    {
        LOG_WARN("catalog", "Catalog thumbnail window failed: {}", event.terminal->error->technicalMessage);
    }
}

// 목적: initial/active/terminal Folder lifecycle을 MainWindow 상태로 투영
// 입력: event: immutable Folder operation event
// 출력: action enablement와 scan/import presentation 갱신
void MainWindow::handleFolderOperationEvent(const core::client::FolderOperationEvent& event)
{
    if (event.terminal.has_value())
    {
        handleFolderOperationTerminal(*event.terminal);
        return;
    }
    if (event.activeOperation.has_value())
    {
        handleFolderOperationStarted(*event.activeOperation);
        return;
    }
    if (event.initial)
    {
        m_folderOperationActive = false;
        updateProjectActions();
    }
}

// 목적: accepted Folder scan/import 시작 상태를 action과 preview presentation에 반영
// 입력: receipt: operation kind와 normalized folder path
// 출력: 중복 command 차단과 scan 종류별 status 표시
void MainWindow::handleFolderOperationStarted(const core::client::FolderOperationReceipt& receipt)
{
    m_folderOperationActive = true;
    m_newCatalogAction->setEnabled(false);
    m_openCatalogAction->setEnabled(false);
    m_closeCatalogAction->setEnabled(false);
    m_openFolderAction->setEnabled(false);
    m_importFolderAction->setEnabled(false);
    m_previousCatalogPageButton->setEnabled(false);
    m_nextCatalogPageButton->setEnabled(false);
    m_catalogFolderScopeComboBox->setEnabled(false);
    m_catalogProjectScopeComboBox->setEnabled(false);
    m_catalogProjectMenuButton->setEnabled(false);
    if (receipt.kind == core::client::FolderOperationKind::Import)
    {
        statusBar()->showMessage(tr("Scanning folder for import..."));
        return;
    }

    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    (void)m_editorClient->clearEditorSelection();
    resetCatalogPhotoPage();
    m_catalogFolderPath.reset();
    m_catalogProjectId.reset();
    m_catalogInclusionFilter = core::client::CatalogInclusionFilter::IncludedOnly;
    m_catalogCullingFilter = core::client::CatalogCullingFilter::All;
    {
        const QSignalBlocker blocker(m_catalogInclusionFilterComboBox);
        m_catalogInclusionFilterComboBox->setCurrentIndex(0);
    }
    {
        const QSignalBlocker blocker(m_catalogCullingFilterComboBox);
        m_catalogCullingFilterComboBox->setCurrentIndex(0);
    }
    {
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->setCurrentIndex(-1);
        m_catalogFolderScopeComboBox->setPlaceholderText(tr("Open Folder"));
    }
    {
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->setCurrentIndex(0);
    }
    m_previewWidget->showMessage(tr("Scanning folder..."));
    statusBar()->showMessage(tr("Scanning folder..."));
}

// 목적: Folder scan/import exact terminal을 list 또는 Catalog navigation에 적용
// 입력: terminal: completion snapshot 또는 구조화된 실패
// 출력: action 복원과 scan/import 결과 표시
void MainWindow::handleFolderOperationTerminal(const core::client::FolderOperationTerminal& terminal)
{
    m_folderOperationActive = false;
    m_newCatalogAction->setEnabled(true);
    m_openCatalogAction->setEnabled(true);
    m_closeCatalogAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
    m_openFolderAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
    m_importFolderAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
    updateProjectActions();

    if (terminal.state != core::client::FolderOperationTerminalState::Completed || !terminal.completion.has_value())
    {
        if (terminal.error.has_value())
        {
            LOG_WARN("catalog", "Folder operation failed: {}", terminal.error->technicalMessage);
        }
        if (terminal.receipt.kind == core::client::FolderOperationKind::Scan)
        {
            m_catalogWidget->clearEntries();
            m_previewWidget->showMessage(tr("Unable to scan folder."));
            statusBar()->showMessage(tr("Folder scan failed."));
        }
        else
        {
            statusBar()->showMessage(tr("Folder import failed."));
        }
        updateCatalogPageActions();
        return;
    }

    if (terminal.receipt.kind == core::client::FolderOperationKind::Import)
    {
        if (!std::holds_alternative<core::client::FolderImportCompletion>(*terminal.completion))
        {
            LOG_ERROR("catalog", "Folder import terminal did not contain an import completion");
            statusBar()->showMessage(tr("Folder import failed."));
            return;
        }
        const core::client::FolderImportCompletion& completion =
            std::get<core::client::FolderImportCompletion>(*terminal.completion);
        if (completion.discoveredCount == 0)
        {
            updateCatalogPageActions();
            statusBar()->showMessage(tr("No supported files found in this folder."));
            return;
        }
        if (!refreshCatalogPhotos())
        {
            return;
        }
        statusBar()->showMessage(tr("Imported %1 photos.").arg(completion.appliedCount));
        return;
    }

    if (!std::holds_alternative<core::client::FolderScanCompletion>(*terminal.completion))
    {
        LOG_ERROR("catalog", "Folder scan terminal did not contain a scan completion");
        statusBar()->showMessage(tr("Folder scan failed."));
        return;
    }
    const core::client::FolderScanCompletion& completion =
        std::get<core::client::FolderScanCompletion>(*terminal.completion);
    QVector<core::catalog::CatalogEntry> entries;
    entries.reserve(static_cast<qsizetype>(completion.items.size()));
    for (const core::client::FolderItemSnapshot& item : completion.items)
    {
        entries.push_back(toCatalogEntry(item));
    }
    resetCatalogPhotoPage();
    m_catalogWidget->setEntries(entries);
    m_previewWidget->showMessage(entries.isEmpty() ? tr("No supported files found in this folder.")
                                                   : tr("Select a photo to edit."));
    statusBar()->showMessage(tr("Loaded %1 files.").arg(entries.size()));
}

// 목적: immutable active 목록을 status bar progress와 owner cancel target에 투영
// 입력: event: initial 또는 lifecycle transition 뒤 Activity snapshot
// 출력: active 여부와 cancellability에 맞는 compact status UI
void MainWindow::updateActivityUi(const core::client::ActivityEvent& event)
{
    if (event.activeActivities.empty())
    {
        m_cancelActivityId.reset();
        m_cancelActivityButton->hide();
        if (!m_activityLabel->isHidden() && !m_activityHideTimer->isActive())
        {
            m_activityHideTimer->start();
        }
        return;
    }

    m_activityHideTimer->stop();
    const core::client::ActiveActivity& displayed = event.activeActivities.back();
    QString activityName;
    switch (displayed.id.kind)
    {
    case core::client::ActivityKind::Preview:
        activityName = tr("Rendering preview...");
        break;
    case core::client::ActivityKind::FolderScan:
        activityName = tr("Scanning folder...");
        break;
    case core::client::ActivityKind::FolderImport:
        activityName = tr("Importing folder...");
        break;
    case core::client::ActivityKind::SourceVerification:
        activityName = tr("Verifying source...");
        break;
    }
    const QString labelText = event.activeActivities.size() == 1
                                  ? activityName
                                  : tr("%1 background activities...").arg(event.activeActivities.size());
    if (m_activityLabel->text() != labelText)
    {
        m_activityLabel->setText(labelText);
    }
    m_activityLabel->show();
    m_activityProgressBar->show();

    m_cancelActivityId.reset();
    for (auto activity = event.activeActivities.crbegin(); activity != event.activeActivities.crend(); ++activity)
    {
        if (activity->canCancel)
        {
            m_cancelActivityId = activity->id;
            break;
        }
    }
    m_cancelActivityButton->setToolTip(tr("Cancel background activity"));
    m_cancelActivityButton->setVisible(m_cancelActivityId.has_value());
}

// 목적: status bar에 선택된 cancellable Activity를 실제 owner에서 취소
// 입력: 없음
// 출력: cancellation command 전달 또는 non-modal 실패 안내
void MainWindow::cancelDisplayedActivity()
{
    if (!m_cancelActivityId.has_value())
    {
        return;
    }
    const core::client::ActivityCancelResult cancelled = m_activityAdapter->cancelActivity(*m_cancelActivityId);
    if (cancelled.hasError())
    {
        LOG_WARN("app", "Unable to cancel Activity: {}", cancelled.error().technicalMessage);
        statusBar()->showMessage(tr("Background activity could not be cancelled."));
    }
}

// 목적: explicit save policy에서 dirty catalog edit의 종료 전 저장 여부 확인
// 입력: event: Qt window close event
// 출력: 저장·폐기 선택 시 종료, 취소·저장 실패 시 종료 중단
void MainWindow::closeEvent(QCloseEvent* event)
{
    if (confirmPendingSave())
    {
        QMainWindow::closeEvent(event);
        return;
    }

    event->ignore();
}

// 목적: 새 catalog file 경로를 선택하고 migration된 빈 catalog 생성
// 입력: 없음
// 출력: 없음
void MainWindow::createCatalog()
{
    QString catalogPath = QFileDialog::getSaveFileName(this,
                                                       tr("New Catalog"),
                                                       {},
                                                       tr("Flexraw Catalog (*.flexraw-catalog)"),
                                                       nullptr,
                                                       QFileDialog::DontConfirmOverwrite);
    if (catalogPath.isEmpty())
    {
        return;
    }

    if (!catalogPath.endsWith(QStringLiteral(".flexraw-catalog"), Qt::CaseInsensitive))
    {
        catalogPath.append(QStringLiteral(".flexraw-catalog"));
    }

    if (QFileInfo::exists(catalogPath))
    {
        QMessageBox::information(
            this, tr("Catalog Already Exists"), tr("Choose Open Catalog to use an existing catalog file."));
        return;
    }

    if (openCatalogPath(catalogPath, core::client::CatalogOpenMode::CreateNew))
    {
        statusBar()->showMessage(tr("Catalog created. Import a folder to add photos."));
    }
}

// 목적: catalog file 선택 dialog를 열고 stable PhotoId 목록을 현재 window에 연결
// 입력: 없음
// 출력: 없음
void MainWindow::openCatalog()
{
    const QString catalogPath = QFileDialog::getOpenFileName(
        this, tr("Open Catalog"), {}, tr("Flexraw Catalog (*.flexraw-catalog);;All Files (*)"));
    if (!catalogPath.isEmpty())
    {
        (void)openCatalogPath(catalogPath, core::client::CatalogOpenMode::OpenExisting);
    }
}

// 목적: active Catalog를 닫고 Catalog·Editor presentation을 비활성 상태로 초기화
// 입력: 없음
// 출력: 없음
void MainWindow::closeCatalog()
{
    if (m_folderOperationActive || !m_catalogSessionClient->catalogSnapshot().isOpen || !confirmPendingSave())
    {
        return;
    }

    const core::client::CatalogSessionResult closed = m_catalogSessionClient->closeCatalog();
    if (closed.hasError())
    {
        LOG_WARN("catalog", "Unable to close GUI catalog: {}", closed.error().technicalMessage);
        statusBar()->showMessage(tr("Catalog could not be closed."));
        return;
    }

    resetCatalogPhotoPage();
    m_catalogFolderPath.reset();
    m_catalogProjectId.reset();
    m_catalogInclusionFilter = core::client::CatalogInclusionFilter::IncludedOnly;
    m_catalogCullingFilter = core::client::CatalogCullingFilter::All;
    {
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->clear();
        m_catalogFolderScopeComboBox->setPlaceholderText(tr("No catalog"));
    }
    {
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->clear();
        m_catalogProjectScopeComboBox->setPlaceholderText(tr("No catalog"));
    }
    {
        const QSignalBlocker blocker(m_catalogInclusionFilterComboBox);
        m_catalogInclusionFilterComboBox->setCurrentIndex(0);
    }
    {
        const QSignalBlocker blocker(m_catalogCullingFilterComboBox);
        m_catalogCullingFilterComboBox->setCurrentIndex(0);
    }
    {
        const QSignalBlocker beforeBlocker(m_beforePreviewAction);
        const QSignalBlocker splitBlocker(m_splitPreviewAction);
        m_beforePreviewAction->setChecked(false);
        m_splitPreviewAction->setChecked(false);
    }
    m_catalogFolderScopeComboBox->setEnabled(false);
    m_catalogProjectScopeComboBox->setEnabled(false);
    m_catalogProjectMenuButton->setEnabled(false);
    m_catalogWidget->clearEntries();
    m_importFolderAction->setEnabled(false);
    m_closeCatalogAction->setEnabled(false);
    m_openFolderAction->setEnabled(false);
    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    m_previewWidget->showMessage(tr("Open or create a catalog to browse photos."));
    updateEditorActions();
    updateProjectActions();
    statusBar()->showMessage(tr("Catalog closed."));
}

// 목적: 지정 catalog를 단일 active session으로 열고 stable PhotoId 목록 표시
// 입력: catalogPath: 생성하거나 열 catalog file 경로, openMode: explicit create/open 의도
// 출력: catalog와 photo list를 모두 열었으면 true
bool MainWindow::openCatalogPath(const QString& catalogPath, core::client::CatalogOpenMode openMode)
{
    const core::client::CatalogSessionSnapshot session = m_catalogSessionClient->catalogSnapshot();
    if (session.isOpen && QFileInfo(catalogPath).absoluteFilePath() == fromClientString(session.catalogPath))
    {
        m_importFolderAction->setEnabled(true);
        m_closeCatalogAction->setEnabled(true);
        m_openFolderAction->setEnabled(true);
        statusBar()->showMessage(tr("This catalog is already open."));
        return true;
    }

    if (!confirmPendingSave())
    {
        return false;
    }

    resetCatalogPhotoPage();
    m_catalogFolderPath.reset();
    m_catalogProjectId.reset();
    m_catalogInclusionFilter = core::client::CatalogInclusionFilter::IncludedOnly;
    m_catalogCullingFilter = core::client::CatalogCullingFilter::All;
    {
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->clear();
        m_catalogFolderScopeComboBox->setPlaceholderText(tr("No catalog"));
    }
    m_catalogFolderScopeComboBox->setEnabled(false);
    {
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->clear();
        m_catalogProjectScopeComboBox->setPlaceholderText(tr("No catalog"));
    }
    m_catalogProjectScopeComboBox->setEnabled(false);
    {
        const QSignalBlocker blocker(m_catalogInclusionFilterComboBox);
        m_catalogInclusionFilterComboBox->setCurrentIndex(0);
    }
    m_catalogInclusionFilterComboBox->setEnabled(false);
    {
        const QSignalBlocker blocker(m_catalogCullingFilterComboBox);
        m_catalogCullingFilterComboBox->setCurrentIndex(0);
    }
    m_catalogCullingFilterComboBox->setEnabled(false);
    m_catalogProjectMenuButton->setEnabled(false);
    m_catalogWidget->clearEntries();
    m_importFolderAction->setEnabled(false);
    const core::client::CatalogSessionResult opened = m_catalogSessionClient->openCatalog(
        {toClientString(catalogPath), openMode, core::client::CatalogReplacementPolicy::ReplaceCurrent});
    if (opened.hasError())
    {
        LOG_WARN("catalog", "Unable to open GUI catalog: {}", opened.error().technicalMessage);
        if (m_catalogSessionClient->catalogSnapshot().isOpen)
        {
            (void)refreshCatalogPhotos();
        }
        m_closeCatalogAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
        m_openFolderAction->setEnabled(m_catalogSessionClient->catalogSnapshot().isOpen);
        m_previewWidget->showMessage(tr("Unable to open catalog."));
        statusBar()->showMessage(tr("Catalog open failed."));
        return false;
    }

    return refreshCatalogPhotos();
}

// 목적: 이미 열린 Catalog의 photo 목록과 관련 action을 현재 window에 반영
// 입력: 없음
// 출력: photo 목록을 정상 조회해 표시했으면 true
bool MainWindow::refreshCatalogPhotos()
{
    if (!refreshCatalogFolders() || !refreshCatalogProjects(m_catalogProjectId))
    {
        resetCatalogPhotoPage();
        (void)m_catalogSessionClient->closeCatalog();
        m_closeCatalogAction->setEnabled(false);
        m_openFolderAction->setEnabled(false);
        m_previewWidget->showMessage(tr("Unable to load catalog navigation."));
        statusBar()->showMessage(tr("Catalog navigation failed."));
        return false;
    }

    const core::client::CatalogPhotoPageRequest request = makePhotoPageRequest(
        m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter);
    const core::client::CatalogPhotoPageResult photos = m_catalogPhotoClient->queryPhotoPage(request);
    if (photos.hasError())
    {
        LOG_WARN("catalog", "Unable to list GUI catalog photos: {}", photos.error().technicalMessage);
        resetCatalogPhotoPage();
        (void)m_catalogSessionClient->closeCatalog();
        m_closeCatalogAction->setEnabled(false);
        m_openFolderAction->setEnabled(false);
        m_catalogFolderScopeComboBox->setEnabled(false);
        m_catalogProjectScopeComboBox->setEnabled(false);
        m_catalogProjectMenuButton->setEnabled(false);
        m_previewWidget->showMessage(tr("Unable to load catalog photos."));
        statusBar()->showMessage(tr("Catalog photo list failed."));
        return false;
    }

    m_importFolderAction->setEnabled(true);
    m_closeCatalogAction->setEnabled(true);
    m_openFolderAction->setEnabled(true);
    m_catalogPhotoPage = photos.value();
    m_catalogPhotoPageRequest = request;
    m_catalogWidget->setPhotos(facade::toCatalogPhotoRecords(photos.value().photos));
    updateCatalogPageActions();
    updateProjectActions();
    if (photos.value().photos.empty())
    {
        m_previewWidget->showMessage(tr("No photos are registered in this catalog."));
    }
    statusBar()->showMessage(tr("Showing %1 catalog photos.").arg(photos.value().photos.size()));
    return true;
}

// 목적: active Catalog의 distinct Folder scope를 navigation control에 반영
// 입력: 없음
// 출력: Folder summary 조회와 control 갱신에 성공하면 true
bool MainWindow::refreshCatalogFolders()
{
    const core::client::CatalogFolderListResult folders = m_catalogFolderClient->listFolders();
    if (folders.hasError())
    {
        LOG_WARN("catalog", "Unable to list GUI catalog folders: {}", folders.error().technicalMessage);
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->clear();
        m_catalogFolderScopeComboBox->setPlaceholderText(tr("No catalog"));
        m_catalogFolderScopeComboBox->setEnabled(false);
        return false;
    }

    const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
    m_catalogFolderScopeComboBox->clear();
    m_catalogFolderScopeComboBox->addItem(tr("All Photos"), QString{});
    int selectedIndex = m_catalogFolderPath.has_value() ? -1 : 0;
    for (const core::client::CatalogFolderSnapshot& folder : folders.value())
    {
        const QString folderPath = fromClientString(folder.path);
        const QString displayPath = QDir::toNativeSeparators(folderPath);
        const int index = m_catalogFolderScopeComboBox->count();
        m_catalogFolderScopeComboBox->addItem(tr("%1 (%2)").arg(displayPath).arg(folder.photoCount), folderPath);
        m_catalogFolderScopeComboBox->setItemData(index, displayPath, Qt::ToolTipRole);
        if (m_catalogFolderPath.has_value() && folderPath == *m_catalogFolderPath)
        {
            selectedIndex = index;
        }
    }

    if (selectedIndex < 0)
    {
        m_catalogFolderPath.reset();
        selectedIndex = 0;
    }
    m_catalogFolderScopeComboBox->setCurrentIndex(selectedIndex);
    m_catalogFolderScopeComboBox->setEnabled(!m_catalogProjectId.has_value());
    return true;
}

// 목적: active Catalog Project 목록을 navigation control에 반영
// 입력: preferredProjectId: refresh 뒤 유지할 optional Project identity
// 출력: Project 조회와 control 갱신에 성공하면 true
bool MainWindow::refreshCatalogProjects(std::optional<core::catalog::ProjectId> preferredProjectId)
{
    const core::client::CatalogProjectListResult projects = m_catalogProjectClient->listProjects();
    if (projects.hasError())
    {
        LOG_WARN("catalog", "Unable to list GUI catalog projects: {}", projects.error().technicalMessage);
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->clear();
        m_catalogProjectScopeComboBox->setPlaceholderText(tr("No catalog"));
        m_catalogProjectScopeComboBox->setEnabled(false);
        m_catalogProjectMenuButton->setEnabled(false);
        return false;
    }

    const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
    m_catalogProjectScopeComboBox->clear();
    m_catalogProjectScopeComboBox->addItem(tr("Catalog / Folders"), qint64{0});
    int selectedIndex = preferredProjectId.has_value() ? -1 : 0;
    for (const core::client::CatalogProjectSnapshot& project : projects.value())
    {
        const QString projectName = fromClientString(project.name);
        const int index = m_catalogProjectScopeComboBox->count();
        m_catalogProjectScopeComboBox->addItem(projectName, project.id.value);
        m_catalogProjectScopeComboBox->setItemData(
            index, tr("%1 (Project #%2)").arg(projectName).arg(project.id.value), Qt::ToolTipRole);
        if (preferredProjectId.has_value() && project.id.value == preferredProjectId->value)
        {
            selectedIndex = index;
        }
    }

    if (selectedIndex < 0)
    {
        m_catalogProjectId.reset();
        selectedIndex = 0;
    }
    m_catalogProjectScopeComboBox->setCurrentIndex(selectedIndex);
    updateProjectActions();
    return true;
}

// 목적: 사용자가 선택한 전체 Catalog 또는 exact Folder scope로 photo page 전환
// 입력: index: Folder scope combo box의 선택 index
// 출력: active adjustment와 dirty state 정리 후 선택 scope의 첫 page 표시
void MainWindow::changeCatalogFolderScope(int index)
{
    if (index < 0 || !m_catalogSessionClient->catalogSnapshot().isOpen)
    {
        return;
    }

    const QString folderPath = m_catalogFolderScopeComboBox->itemData(index).toString();
    const std::optional<QString> requestedScope =
        folderPath.isEmpty() ? std::nullopt : std::optional<QString>{folderPath};
    if (requestedScope == m_catalogFolderPath && !m_catalogProjectId.has_value())
    {
        return;
    }

    const std::optional<QString> previousScope = m_catalogFolderPath;
    const std::optional<core::catalog::ProjectId> previousProjectId = m_catalogProjectId;
    if (!persistCurrentPhoto())
    {
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->setCurrentIndex(
            previousScope.has_value() ? m_catalogFolderScopeComboBox->findData(*previousScope) : 0);
        return;
    }

    core::client::CatalogPhotoPageRequest request;
    request.exactFolderPath = toClientFolderPath(requestedScope);
    request.inclusionFilter = m_catalogInclusionFilter;
    request.cullingFilter = m_catalogCullingFilter;
    m_catalogFolderPath = requestedScope;
    m_catalogProjectId.reset();
    {
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->setCurrentIndex(0);
    }
    if (!loadCatalogPhotoPage(request))
    {
        m_catalogFolderPath = previousScope;
        m_catalogProjectId = previousProjectId;
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->setCurrentIndex(
            previousScope.has_value() ? m_catalogFolderScopeComboBox->findData(*previousScope) : 0);
    }
    updateProjectActions();
}

// 목적: 사용자가 선택한 Catalog/Folder 또는 Project scope로 photo page 전환
// 입력: index: Project scope combo box의 선택 index
// 출력: active adjustment와 dirty state 정리 후 선택 scope의 첫 page 표시
void MainWindow::changeCatalogProjectScope(int index)
{
    if (index < 0 || !m_catalogSessionClient->catalogSnapshot().isOpen)
    {
        return;
    }

    const qint64 value = m_catalogProjectScopeComboBox->itemData(index).toLongLong();
    const std::optional<core::catalog::ProjectId> requestedProjectId =
        value > 0 ? std::optional<core::catalog::ProjectId>{{value}} : std::nullopt;
    if (requestedProjectId == m_catalogProjectId)
    {
        return;
    }

    const std::optional<core::catalog::ProjectId> previousProjectId = m_catalogProjectId;
    const std::optional<QString> previousFolderPath = m_catalogFolderPath;
    if (!persistCurrentPhoto())
    {
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->setCurrentIndex(
            previousProjectId.has_value() ? m_catalogProjectScopeComboBox->findData(previousProjectId->value) : 0);
        return;
    }

    core::client::CatalogPhotoPageRequest request;
    request.projectId = toClientProjectId(requestedProjectId);
    request.inclusionFilter = m_catalogInclusionFilter;
    request.cullingFilter = m_catalogCullingFilter;
    m_catalogProjectId = requestedProjectId;
    m_catalogFolderPath.reset();
    {
        const QSignalBlocker blocker(m_catalogFolderScopeComboBox);
        m_catalogFolderScopeComboBox->setCurrentIndex(0);
    }
    if (!loadCatalogPhotoPage(request))
    {
        m_catalogProjectId = previousProjectId;
        m_catalogFolderPath = previousFolderPath;
        const QSignalBlocker blocker(m_catalogProjectScopeComboBox);
        m_catalogProjectScopeComboBox->setCurrentIndex(
            previousProjectId.has_value() ? m_catalogProjectScopeComboBox->findData(previousProjectId->value) : 0);
    }
    updateProjectActions();
}

// 목적: 이름 입력을 받아 active Catalog에 Project 생성
// 입력: 없음
// 출력: 생성 성공 시 새 Project scope로 이동
void MainWindow::createProject()
{
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, tr("New Project"), tr("Project name:"), QLineEdit::Normal, {}, &accepted);
    if (!accepted)
    {
        return;
    }

    const core::client::CatalogProjectResult created =
        m_catalogProjectClient->createProject({name.toUtf8().toStdString()});
    if (created.hasError())
    {
        LOG_WARN("catalog", "Unable to create GUI project: {}", created.error().technicalMessage);
        statusBar()->showMessage(tr("Project creation failed."));
        return;
    }

    const core::catalog::ProjectId projectId{created.value().id.value};
    if (!refreshCatalogProjects(projectId))
    {
        statusBar()->showMessage(tr("Project created, but navigation refresh failed."));
        return;
    }
    changeCatalogProjectScope(m_catalogProjectScopeComboBox->findData(projectId.value));
    statusBar()->showMessage(tr("Created project %1.").arg(fromClientString(created.value().name)));
}

// 목적: 현재 Project의 표시 이름 변경
// 입력: 없음
// 출력: 변경 성공 시 Project navigation 갱신
void MainWindow::renameCurrentProject()
{
    if (!m_catalogProjectId.has_value())
    {
        return;
    }

    bool accepted = false;
    const QString currentName = m_catalogProjectScopeComboBox->currentText();
    const QString name = QInputDialog::getText(
        this, tr("Rename Project"), tr("Project name:"), QLineEdit::Normal, currentName, &accepted);
    if (!accepted)
    {
        return;
    }

    const core::client::CatalogProjectResult renamed =
        m_catalogProjectClient->renameProject({{m_catalogProjectId->value}, name.toUtf8().toStdString()});
    if (renamed.hasError())
    {
        LOG_WARN("catalog", "Unable to rename GUI project: {}", renamed.error().technicalMessage);
        statusBar()->showMessage(tr("Project rename failed."));
        return;
    }

    (void)refreshCatalogProjects(core::catalog::ProjectId{renamed.value().id.value});
    statusBar()->showMessage(tr("Renamed project to %1.").arg(fromClientString(renamed.value().name)));
}

// 목적: 현재 Project와 membership 삭제
// 입력: 없음
// 출력: 사용자 확인 후 Photo를 보존하고 Catalog scope로 이동
void MainWindow::removeCurrentProject()
{
    if (!m_catalogProjectId.has_value() ||
        QMessageBox::question(this,
                              tr("Delete Project"),
                              tr("Delete project \"%1\"? Photos will remain in the catalog.")
                                  .arg(m_catalogProjectScopeComboBox->currentText())) != QMessageBox::Yes ||
        !persistCurrentPhoto())
    {
        return;
    }

    const core::client::CatalogProjectDeleteResult removed =
        m_catalogProjectClient->deleteProject({{m_catalogProjectId->value}});
    if (removed.hasError())
    {
        LOG_WARN("catalog", "Unable to remove GUI project: {}", removed.error().technicalMessage);
        statusBar()->showMessage(tr("Project deletion failed."));
        return;
    }

    m_catalogProjectId.reset();
    m_catalogFolderPath.reset();
    (void)refreshCatalogProjects();
    core::client::CatalogPhotoPageRequest request;
    request.inclusionFilter = m_catalogInclusionFilter;
    request.cullingFilter = m_catalogCullingFilter;
    (void)loadCatalogPhotoPage(request);
    updateProjectActions();
    statusBar()->showMessage(tr("Project deleted. Photos remain in the catalog."));
}

// 목적: 현재 multi-selection Photo를 사용자가 고른 Project에 추가
// 입력: 없음
// 출력: photo별 idempotent membership command 결과를 status에 표시
void MainWindow::addSelectedPhotosToProject()
{
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = m_catalogWidget->selectedPhotos();
    const core::client::CatalogProjectListResult projects = m_catalogProjectClient->listProjects();
    if (selectedPhotos.isEmpty() || projects.hasError() || projects.value().empty())
    {
        statusBar()->showMessage(tr("Select catalog photos and create a project first."));
        return;
    }

    QStringList projectLabels;
    for (const core::client::CatalogProjectSnapshot& project : projects.value())
    {
        projectLabels.append(tr("%1 (#%2)").arg(fromClientString(project.name)).arg(project.id.value));
    }

    bool accepted = false;
    const QString selectedLabel =
        QInputDialog::getItem(this, tr("Add to Project"), tr("Project:"), projectLabels, 0, false, &accepted);
    const qsizetype projectIndex = projectLabels.indexOf(selectedLabel);
    if (!accepted || projectIndex < 0)
    {
        return;
    }
    const core::client::CatalogProjectSnapshot& project = projects.value().at(static_cast<std::size_t>(projectIndex));

    qsizetype storedCount = 0;
    for (const core::catalog::CatalogPhotoRecord& photo : selectedPhotos)
    {
        const core::client::CatalogProjectMembershipResult stored =
            m_catalogProjectClient->addPhotoToProject({project.id, {photo.id.value}});
        if (stored.hasError())
        {
            LOG_WARN("catalog", "Unable to add GUI project membership: {}", stored.error().technicalMessage);
            statusBar()->showMessage(
                tr("Added %1 of %2 selected photos to the project.").arg(storedCount).arg(selectedPhotos.size()));
            return;
        }
        ++storedCount;
    }

    statusBar()->showMessage(tr("Added %1 selected photos to the project.").arg(storedCount));
}

// 목적: 현재 multi-selection Photo를 active Project에서 제거
// 입력: 없음
// 출력: membership 제거 후 active Project 첫 page reload
void MainWindow::removeSelectedPhotosFromProject()
{
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = m_catalogWidget->selectedPhotos();
    if (!m_catalogProjectId.has_value() || selectedPhotos.isEmpty() || !persistCurrentPhoto())
    {
        return;
    }

    qsizetype removedCount = 0;
    for (const core::catalog::CatalogPhotoRecord& photo : selectedPhotos)
    {
        const core::client::CatalogProjectMembershipResult removed =
            m_catalogProjectClient->removePhotoFromProject({{m_catalogProjectId->value}, {photo.id.value}});
        if (removed.hasError())
        {
            LOG_WARN("catalog", "Unable to remove GUI project membership: {}", removed.error().technicalMessage);
            break;
        }
        ++removedCount;
    }

    core::client::CatalogPhotoPageRequest request;
    request.projectId = toClientProjectId(m_catalogProjectId);
    request.inclusionFilter = m_catalogInclusionFilter;
    request.cullingFilter = m_catalogCullingFilter;
    (void)loadCatalogPhotoPage(request);
    statusBar()->showMessage(
        tr("Removed %1 of %2 selected photos from the project.").arg(removedCount).arg(selectedPhotos.size()));
}

// 목적: Catalog session, active Project와 selected Photo에 맞춰 Project action 상태 갱신
// 입력: 없음
// 출력: 유효한 Project command만 활성화
void MainWindow::updateProjectActions()
{
    const bool available = m_catalogSessionClient->catalogSnapshot().isOpen && !m_folderOperationActive;
    const bool hasProject = m_catalogProjectId.has_value();
    const bool hasSelectedPhotos = !m_catalogWidget->selectedPhotos().isEmpty();
    m_catalogFolderScopeComboBox->setEnabled(available && !hasProject);
    m_catalogProjectScopeComboBox->setEnabled(available);
    m_catalogProjectMenuButton->setEnabled(available);
    m_createProjectAction->setEnabled(available);
    m_renameProjectAction->setEnabled(available && hasProject);
    m_removeProjectAction->setEnabled(available && hasProject);
    m_addSelectedPhotosToProjectAction->setEnabled(available && hasSelectedPhotos);
    m_removeSelectedPhotosFromProjectAction->setEnabled(available && hasProject && hasSelectedPhotos);
    updateCatalogCullingActions();
    updateCatalogInclusionActions();
}

// 목적: 선택된 Catalog Photo batch에 persisted Pick·Reject·Neutral 상태 적용
// 입력: state: 선택 Photo 전체에 적용할 target culling state
// 출력: mutation receipt 뒤 같은 scope/filter page와 selection presentation 갱신
void MainWindow::setSelectedPhotoCulling(core::client::CatalogCullingState state)
{
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = m_catalogWidget->selectedPhotos();
    const core::client::CatalogSessionSnapshot session = m_catalogSessionClient->catalogSnapshot();
    if (!session.isOpen || selectedPhotos.isEmpty() || m_folderOperationActive)
    {
        return;
    }

    const core::orchestration::EditorState editorState = currentEditorState(*m_editorClient);
    const bool currentPhotoMutated =
        editorState.hasSelection && std::any_of(selectedPhotos.cbegin(), selectedPhotos.cend(), [&](const auto& photo) {
            return photo.id.value == editorState.photo.photoId.value;
        });
    if (currentPhotoMutated && !cullingFilterIncludes(m_catalogCullingFilter, state) && !persistCurrentPhoto())
    {
        return;
    }

    std::vector<core::client::ClientPhotoId> photoIds;
    photoIds.reserve(static_cast<std::size_t>(selectedPhotos.size()));
    for (const core::catalog::CatalogPhotoRecord& photo : selectedPhotos)
    {
        photoIds.push_back({photo.id.value});
    }
    const core::client::CatalogCullingResult stored =
        m_catalogCullingClient->setCatalogCulling({session.catalogPath, state, std::move(photoIds)});
    if (stored.hasError())
    {
        LOG_WARN("catalog", "Unable to update GUI Catalog culling: {}", stored.error().technicalMessage);
        statusBar()->showMessage(tr("Photo pick/reject update failed."));
        return;
    }

    core::client::CatalogPhotoPageRequest request = m_catalogPhotoPageRequest.value_or(makePhotoPageRequest(
        m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter));
    core::client::CatalogPhotoPageResult refreshed = m_catalogPhotoClient->queryPhotoPage(request);
    if (refreshed.hasValue() && refreshed.value().photos.empty() && request.cursor.has_value())
    {
        request = makePhotoPageRequest(
            m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter);
        refreshed = m_catalogPhotoClient->queryPhotoPage(request);
    }
    if (refreshed.hasError())
    {
        LOG_WARN("catalog", "Catalog culling stored but GUI refresh failed: {}", refreshed.error().technicalMessage);
        statusBar()->showMessage(tr("Pick/reject was saved, but the catalog could not be refreshed."));
        return;
    }

    std::optional<core::types::PhotoId> preferredPhotoId;
    if (editorState.hasSelection)
    {
        const auto current = std::find_if(refreshed.value().photos.cbegin(),
                                          refreshed.value().photos.cend(),
                                          [&](const core::client::CatalogPhotoSnapshot& photo) {
                                              return photo.id.value == editorState.photo.photoId.value;
                                          });
        if (current != refreshed.value().photos.cend())
        {
            preferredPhotoId = editorState.photo.photoId;
        }
    }

    m_catalogPhotoPage = refreshed.value();
    m_catalogPhotoPageRequest = request;
    if (!preferredPhotoId.has_value())
    {
        (void)m_editorClient->clearEditorSelection();
        m_developPanel->setEnabled(false);
        m_developPanel->clearHistogram();
    }
    m_catalogWidget->setPhotos(facade::toCatalogPhotoRecords(refreshed.value().photos), preferredPhotoId);
    updateCatalogPageActions();
    updateProjectActions();
    if (refreshed.value().photos.empty())
    {
        m_previewWidget->showMessage(tr("No photos match this filter."));
    }
    statusBar()->showMessage(
        tr("Updated pick/reject state for %1 photos.").arg(static_cast<qulonglong>(stored.value().photoIds.size())));
}

// 목적: 사용자가 선택한 선별 filter로 현재 Catalog/Folder/Project 첫 page 전환
// 입력: index: culling filter combo box index
// 출력: dirty state 저장 후 All·Pick·Reject filter 적용
void MainWindow::changeCatalogCullingFilter(int index)
{
    if (index < 0 || !m_catalogSessionClient->catalogSnapshot().isOpen || !m_catalogPhotoPage.has_value())
    {
        return;
    }
    const int value = m_catalogCullingFilterComboBox->itemData(index).toInt();
    if (value < static_cast<int>(core::client::CatalogCullingFilter::All) ||
        value > static_cast<int>(core::client::CatalogCullingFilter::Reject))
    {
        return;
    }
    const auto requestedFilter = static_cast<core::client::CatalogCullingFilter>(value);
    if (requestedFilter == m_catalogCullingFilter)
    {
        return;
    }

    const core::client::CatalogCullingFilter previousFilter = m_catalogCullingFilter;
    if (!persistCurrentPhoto())
    {
        const QSignalBlocker blocker(m_catalogCullingFilterComboBox);
        m_catalogCullingFilterComboBox->setCurrentIndex(static_cast<int>(previousFilter));
        return;
    }

    m_catalogCullingFilter = requestedFilter;
    const core::client::CatalogPhotoPageRequest request = makePhotoPageRequest(
        m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter);
    if (!loadCatalogPhotoPage(request))
    {
        m_catalogCullingFilter = previousFilter;
        const QSignalBlocker blocker(m_catalogCullingFilterComboBox);
        m_catalogCullingFilterComboBox->setCurrentIndex(static_cast<int>(previousFilter));
    }
}

// 목적: Catalog page와 selection availability에 맞춰 선별 controls 활성화 갱신
// 입력: 없음
// 출력: filter와 mutation action이 유효한 context에서만 활성화
void MainWindow::updateCatalogCullingActions()
{
    const bool pageAvailable =
        m_catalogSessionClient->catalogSnapshot().isOpen && m_catalogPhotoPage.has_value() && !m_folderOperationActive;
    const bool hasSelectedPhotos = !m_catalogWidget->selectedPhotos().isEmpty();
    m_catalogCullingFilterComboBox->setEnabled(pageAvailable);
    m_pickSelectedPhotosAction->setEnabled(pageAvailable && hasSelectedPhotos);
    m_rejectSelectedPhotosAction->setEnabled(pageAvailable && hasSelectedPhotos);
    m_clearSelectedCullingAction->setEnabled(pageAvailable && hasSelectedPhotos);
}

// 목적: 선택된 Catalog Photo batch에 persisted 포함·제외 상태 적용
// 입력: state: 선택 Photo 전체에 적용할 target inclusion state
// 출력: mutation receipt 뒤 같은 scope/filter page와 selection presentation 갱신
void MainWindow::setSelectedPhotoInclusion(core::client::CatalogInclusionState state)
{
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = m_catalogWidget->selectedPhotos();
    const core::client::CatalogSessionSnapshot session = m_catalogSessionClient->catalogSnapshot();
    if (!session.isOpen || selectedPhotos.isEmpty() || m_folderOperationActive ||
        inclusionFilterIncludes(m_catalogInclusionFilter, state))
    {
        return;
    }

    const core::orchestration::EditorState editorState = currentEditorState(*m_editorClient);
    const bool currentPhotoMutated =
        editorState.hasSelection && std::any_of(selectedPhotos.cbegin(), selectedPhotos.cend(), [&](const auto& photo) {
            return photo.id.value == editorState.photo.photoId.value;
        });
    if (currentPhotoMutated && !persistCurrentPhoto())
    {
        return;
    }

    std::vector<core::client::ClientPhotoId> photoIds;
    photoIds.reserve(static_cast<std::size_t>(selectedPhotos.size()));
    for (const core::catalog::CatalogPhotoRecord& photo : selectedPhotos)
    {
        photoIds.push_back({photo.id.value});
    }
    const core::client::CatalogInclusionResult stored =
        m_catalogInclusionClient->setCatalogInclusion({session.catalogPath, state, std::move(photoIds)});
    if (stored.hasError())
    {
        LOG_WARN("catalog", "Unable to update GUI Catalog inclusion: {}", stored.error().technicalMessage);
        statusBar()->showMessage(tr("Catalog inclusion update failed."));
        return;
    }

    core::client::CatalogPhotoPageRequest request = m_catalogPhotoPageRequest.value_or(makePhotoPageRequest(
        m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter));
    core::client::CatalogPhotoPageResult refreshed = m_catalogPhotoClient->queryPhotoPage(request);
    if (refreshed.hasValue() && refreshed.value().photos.empty() && request.cursor.has_value())
    {
        request = makePhotoPageRequest(
            m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter);
        refreshed = m_catalogPhotoClient->queryPhotoPage(request);
    }
    if (refreshed.hasError())
    {
        LOG_WARN("catalog", "Catalog inclusion stored but GUI refresh failed: {}", refreshed.error().technicalMessage);
        statusBar()->showMessage(tr("Catalog inclusion was saved, but the catalog could not be refreshed."));
        return;
    }

    std::optional<core::types::PhotoId> preferredPhotoId;
    if (editorState.hasSelection)
    {
        const auto current = std::find_if(refreshed.value().photos.cbegin(),
                                          refreshed.value().photos.cend(),
                                          [&](const core::client::CatalogPhotoSnapshot& photo) {
                                              return photo.id.value == editorState.photo.photoId.value;
                                          });
        if (current != refreshed.value().photos.cend())
        {
            preferredPhotoId = editorState.photo.photoId;
        }
    }

    m_catalogPhotoPage = refreshed.value();
    m_catalogPhotoPageRequest = request;
    if (!preferredPhotoId.has_value())
    {
        (void)m_editorClient->clearEditorSelection();
        m_developPanel->setEnabled(false);
        m_developPanel->clearHistogram();
    }
    m_catalogWidget->setPhotos(facade::toCatalogPhotoRecords(refreshed.value().photos), preferredPhotoId);
    updateCatalogPageActions();
    updateProjectActions();
    if (refreshed.value().photos.empty())
    {
        m_previewWidget->showMessage(tr("No photos match this filter."));
    }
    statusBar()->showMessage(
        state == core::client::CatalogInclusionState::Excluded
            ? tr("Excluded %1 photos from the catalog view.")
                  .arg(static_cast<qulonglong>(stored.value().photoIds.size()))
            : tr("Restored %1 photos to the catalog view.")
                  .arg(static_cast<qulonglong>(stored.value().photoIds.size())));
}

// 목적: 사용자가 선택한 등록 포함 filter로 현재 Catalog/Folder/Project 첫 page 전환
// 입력: index: inclusion filter combo box index
// 출력: dirty state 저장 후 Included·Excluded filter 적용
void MainWindow::changeCatalogInclusionFilter(int index)
{
    if (index < 0 || !m_catalogSessionClient->catalogSnapshot().isOpen || !m_catalogPhotoPage.has_value())
    {
        return;
    }
    const int value = m_catalogInclusionFilterComboBox->itemData(index).toInt();
    if (value < static_cast<int>(core::client::CatalogInclusionFilter::IncludedOnly) ||
        value > static_cast<int>(core::client::CatalogInclusionFilter::ExcludedOnly))
    {
        return;
    }
    const auto requestedFilter = static_cast<core::client::CatalogInclusionFilter>(value);
    if (requestedFilter == m_catalogInclusionFilter)
    {
        return;
    }

    const core::client::CatalogInclusionFilter previousFilter = m_catalogInclusionFilter;
    if (!persistCurrentPhoto())
    {
        const QSignalBlocker blocker(m_catalogInclusionFilterComboBox);
        m_catalogInclusionFilterComboBox->setCurrentIndex(
            m_catalogInclusionFilterComboBox->findData(static_cast<int>(previousFilter)));
        return;
    }

    m_catalogInclusionFilter = requestedFilter;
    const core::client::CatalogPhotoPageRequest request = makePhotoPageRequest(
        m_catalogFolderPath, m_catalogProjectId, m_catalogInclusionFilter, m_catalogCullingFilter);
    if (!loadCatalogPhotoPage(request))
    {
        m_catalogInclusionFilter = previousFilter;
        const QSignalBlocker blocker(m_catalogInclusionFilterComboBox);
        m_catalogInclusionFilterComboBox->setCurrentIndex(
            m_catalogInclusionFilterComboBox->findData(static_cast<int>(previousFilter)));
    }
}

// 목적: Catalog page와 selection availability에 맞춰 등록 포함 controls 활성화 갱신
// 입력: 없음
// 출력: 현재 view에서 유효한 제외 또는 복원 action만 활성화
void MainWindow::updateCatalogInclusionActions()
{
    const bool pageAvailable =
        m_catalogSessionClient->catalogSnapshot().isOpen && m_catalogPhotoPage.has_value() && !m_folderOperationActive;
    const bool hasSelectedPhotos = !m_catalogWidget->selectedPhotos().isEmpty();
    m_catalogInclusionFilterComboBox->setEnabled(pageAvailable);
    m_excludeSelectedPhotosAction->setEnabled(
        pageAvailable && hasSelectedPhotos &&
        m_catalogInclusionFilter == core::client::CatalogInclusionFilter::IncludedOnly);
    m_restoreSelectedPhotosAction->setEnabled(
        pageAvailable && hasSelectedPhotos &&
        m_catalogInclusionFilter == core::client::CatalogInclusionFilter::ExcludedOnly);
}

// 목적: cursor 요청에 해당하는 bounded Catalog photo page를 현재 목록에 적용
// 입력: request: page 크기, 이동 방향과 exclusive cursor
// 출력: 조회와 UI 적용에 성공하면 true
bool MainWindow::loadCatalogPhotoPage(const core::client::CatalogPhotoPageRequest& request)
{
    const core::client::CatalogPhotoPageResult result = m_catalogPhotoClient->queryPhotoPage(request);
    if (result.hasError())
    {
        LOG_WARN("catalog", "Unable to navigate GUI catalog photos: {}", result.error().technicalMessage);
        statusBar()->showMessage(tr("Catalog page navigation failed."));
        return false;
    }

    m_catalogPhotoPage = result.value();
    m_catalogPhotoPageRequest = request;
    (void)m_editorClient->clearEditorSelection();
    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    m_catalogWidget->setPhotos(facade::toCatalogPhotoRecords(result.value().photos));
    updateCatalogPageActions();
    updateProjectActions();
    if (result.value().photos.empty())
    {
        m_previewWidget->showMessage(tr("No photos in this scope."));
    }
    statusBar()->showMessage(tr("Showing %1 catalog photos.").arg(result.value().photos.size()));
    return true;
}

// 목적: 현재 첫 record 이전의 Catalog photo page로 이동
// 입력: 없음
// 출력: dirty state 저장 후 이전 page 적용
void MainWindow::showPreviousCatalogPage()
{
    if (!m_catalogPhotoPage.has_value() || !m_catalogPhotoPage->previousCursor.has_value() || !persistCurrentPhoto())
    {
        return;
    }

    core::client::CatalogPhotoPageRequest request;
    request.direction = core::client::CatalogPhotoPageDirection::Backward;
    request.cursor = m_catalogPhotoPage->previousCursor;
    request.exactFolderPath = m_catalogProjectId.has_value() ? std::nullopt : toClientFolderPath(m_catalogFolderPath);
    request.projectId = toClientProjectId(m_catalogProjectId);
    request.inclusionFilter = m_catalogInclusionFilter;
    request.cullingFilter = m_catalogCullingFilter;
    (void)loadCatalogPhotoPage(request);
}

// 목적: 현재 마지막 record 다음의 Catalog photo page로 이동
// 입력: 없음
// 출력: dirty state 저장 후 다음 page 적용
void MainWindow::showNextCatalogPage()
{
    if (!m_catalogPhotoPage.has_value() || !m_catalogPhotoPage->nextCursor.has_value() || !persistCurrentPhoto())
    {
        return;
    }

    core::client::CatalogPhotoPageRequest request;
    request.cursor = m_catalogPhotoPage->nextCursor;
    request.exactFolderPath = m_catalogProjectId.has_value() ? std::nullopt : toClientFolderPath(m_catalogFolderPath);
    request.projectId = toClientProjectId(m_catalogProjectId);
    request.inclusionFilter = m_catalogInclusionFilter;
    request.cullingFilter = m_catalogCullingFilter;
    (void)loadCatalogPhotoPage(request);
}

// 목적: Catalog page cursor state와 navigation control 초기화
// 입력: 없음
// 출력: 현재 page가 제거되고 navigation button 비활성화
void MainWindow::resetCatalogPhotoPage()
{
    m_catalogPhotoPage.reset();
    m_catalogPhotoPageRequest.reset();
    m_previousCatalogPageButton->setEnabled(false);
    m_nextCatalogPageButton->setEnabled(false);
}

// 목적: 현재 page의 이전·다음 cursor 존재 여부를 button 상태에 반영
// 입력: 없음
// 출력: 유효한 방향의 navigation button만 활성화
void MainWindow::updateCatalogPageActions()
{
    m_previousCatalogPageButton->setEnabled(m_catalogPhotoPage.has_value() &&
                                            m_catalogPhotoPage->previousCursor.has_value());
    m_nextCatalogPageButton->setEnabled(m_catalogPhotoPage.has_value() && m_catalogPhotoPage->nextCursor.has_value());
}

// 목적: 현재 열린 catalog에 추가할 photo folder를 선택하고 background scan 시작
// 입력: 없음
// 출력: 없음
void MainWindow::importFolder()
{
    if (!m_catalogSessionClient->catalogSnapshot().isOpen || m_folderOperationActive)
    {
        statusBar()->showMessage(tr("Open or create a catalog before importing photos."));
        return;
    }

    const QString folderPath = QFileDialog::getExistingDirectory(this, tr("Import Folder"));
    if (folderPath.isEmpty() || !persistCurrentPhoto())
    {
        return;
    }

    submitFolderImport(folderPath);
}

// 목적: 현재 Catalog snapshot을 baseline으로 Folder import command 제출
// 입력: folderPath: file dialog에서 선택한 folder
// 출력: accepted lifecycle 또는 non-modal submission 실패 표시
void MainWindow::submitFolderImport(const QString& folderPath)
{
    const core::client::CatalogSessionSnapshot session = m_catalogSessionClient->catalogSnapshot();
    const core::client::FolderOperationResult submitted =
        m_folderImportClient->submitFolderImport({toClientString(folderPath), session.catalogPath});
    if (submitted.hasError())
    {
        LOG_WARN("catalog", "Unable to submit GUI folder import: {}", submitted.error().technicalMessage);
        statusBar()->showMessage(tr("Folder import failed."));
    }
}

// 목적: folder 선택 dialog를 열고 shared-storage root marker를 보장한 뒤 scan 시작
// 입력: 없음
// 출력: marker 실패와 무관하게 선택된 local folder scan 시작
void MainWindow::openFolder()
{
    if (!m_catalogSessionClient->catalogSnapshot().isOpen)
    {
        statusBar()->showMessage(tr("Open or create a catalog before opening a folder."));
        return;
    }

    const QString folderPath = QFileDialog::getExistingDirectory(this, tr("Open Folder"));

    if (!folderPath.isEmpty() && persistCurrentPhoto())
    {
        const worker::client::EnsureSharedStorageMarkerResult marker =
            worker::client::SharedStorageLocator::ensureMarkerForDirectory(folderPath);
        if (marker.hasError())
        {
            LOG_WARN("worker", "Unable to ensure shared storage marker: {}", marker.error().message.toStdString());
        }
        loadFolder(folderPath);
    }
}

// 목적: UI thread를 차단하지 않고 지정 folder의 catalog scan 시작
// 입력: folderPath: scan할 folder 경로
// 출력: 비동기 scan 시작
void MainWindow::loadFolder(const QString& folderPath)
{
    const core::client::FolderOperationResult submitted =
        m_folderImportClient->submitFolderScan({toClientString(folderPath)});
    if (submitted.hasError())
    {
        LOG_WARN("catalog", "Unable to submit GUI folder scan: {}", submitted.error().technicalMessage);
        statusBar()->showMessage(tr("Folder scan failed."));
    }
}

// 목적: 선택된 Folder photo를 active Catalog에 등록·resolve하고 Editor에 연결
// 입력: entry: 사용자가 선택한 CatalogEntry 값
// 출력: stable PhotoId activation 성공 시 preview 예약, 실패 시 이전 선택 복원
void MainWindow::showSelectedEntry(const core::catalog::CatalogEntry& entry)
{
    const core::orchestration::EditorState previousState = currentEditorState(*m_editorClient);
    if (!persistCurrentPhoto())
    {
        m_catalogWidget->restoreEntrySelection(previousState.source.path);
        return;
    }

    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    m_previewWidget->showMessage(tr("Loading preview..."));
    syncPreviewViewport(*m_previewPresentationClient, m_previewWidget->size());
    const core::client::EditorResult selected = m_editorClient->activateSource(toActivateEditorSourceCommand(entry));
    if (selected.hasError())
    {
        LOG_WARN("catalog", "Unable to activate Folder photo: {}", selected.error().technicalMessage);
        m_catalogWidget->restoreEntrySelection(previousState.source.path);
        m_previewWidget->showMessage(tr("Unable to register this photo in the catalog."));
        statusBar()->showMessage(tr("Photo activation failed."));
        return;
    }

    m_developPanel->setParams(core::orchestration::fromClientDevelopParams(selected.value().params));
}

// 목적: 선택된 catalog-backed photo를 persisted develop state와 함께 editor에 연결
// 입력: photo: stable PhotoId와 source binding을 포함한 catalog record
// 출력: 없음
void MainWindow::showSelectedCatalogPhoto(const core::catalog::CatalogPhotoRecord& photo)
{
    const core::orchestration::EditorState currentState = currentEditorState(*m_editorClient);
    if (currentState.hasSelection && currentState.photo.photoId.value == photo.id.value)
    {
        return;
    }

    if (!persistCurrentPhoto())
    {
        m_catalogWidget->restorePhotoSelection(currentState.photo.photoId);
        return;
    }

    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    m_previewWidget->showMessage(tr("Loading preview..."));
    syncPreviewViewport(*m_previewPresentationClient, m_previewWidget->size());
    const core::client::EditorResult selected = m_editorClient->selectPhoto({{photo.id.value}});

    if (selected.hasError())
    {
        LOG_WARN("catalog", "Unable to select GUI catalog photo: {}", selected.error().technicalMessage);
        m_catalogWidget->restorePhotoSelection(currentState.photo.photoId);
        m_previewWidget->showMessage(tr("Unable to select catalog photo."));
        statusBar()->showMessage(tr("Catalog photo selection failed."));
        return;
    }

    m_developPanel->setParams(core::orchestration::fromClientDevelopParams(selected.value().params));
}

// 목적: 선택 photo의 replacement source를 기존 identity의 새 baseline으로 수용
// 입력: 없음
// 출력: accepted source request를 inline pending 상태로 표시
void MainWindow::acceptReplacement()
{
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (!core::types::isValidPhotoId(state.photo.photoId) || !state.sourceResolution.canAcceptReplacement)
    {
        return;
    }

    (void)beginSourceResolution(m_sourceResolutionClient->acceptReplacement({{state.photo.photoId.value}}),
                                tr("Unable to accept the replacement source."));
}

// 목적: 선택 photo의 replacement source를 새 identity로 등록
// 입력: 없음
// 출력: 기존 dirty state 저장 후 accepted source request 표시
void MainWindow::registerReplacementAsNew()
{
    if (!persistCurrentPhoto())
    {
        return;
    }

    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (!core::types::isValidPhotoId(state.photo.photoId) || !state.sourceResolution.canRegisterReplacementAsNew)
    {
        return;
    }

    (void)beginSourceResolution(m_sourceResolutionClient->registerReplacementAsNew({{state.photo.photoId.value}}),
                                tr("Unable to register the replacement as a new photo."));
}

// 목적: 선택 photo와 동일한 content의 다른 source locator 선택
// 입력: 없음
// 출력: file 선택 후 accepted relink request 표시
void MainWindow::relinkSource()
{
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (!core::types::isValidPhotoId(state.photo.photoId) || !state.sourceResolution.canRelinkSource)
    {
        return;
    }

    const QString initialPath = state.source.path.isEmpty() ? QString{} : QFileInfo(state.source.path).absolutePath();
    const QString sourcePath =
        QFileDialog::getOpenFileName(this, tr("Relink Source"), initialPath, tr("All Files (*)"));
    if (sourcePath.isEmpty())
    {
        return;
    }

    const QString normalizedSourcePath =
        QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(sourcePath).absoluteFilePath()));
    (void)beginSourceResolution(
        m_sourceResolutionClient->relinkSource({{state.photo.photoId.value}, toClientString(normalizedSourcePath)}),
        tr("Unable to relink this source."));
}

// 목적: source resolution command 제출 결과를 공통 inline 상태로 변환
// 입력: result: request ID 또는 오류, failureMessage: 사용자용 실패 안내
// 출력: request가 accepted되면 true
bool MainWindow::beginSourceResolution(const core::client::SourceRequestResult& result, const QString& failureMessage)
{
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (result.hasError())
    {
        LOG_WARN("catalog", "Unable to submit Source Resolution: {}", result.error().technicalMessage);
        m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
        statusBar()->showMessage(failureMessage);
        return false;
    }

    m_sourceResolutionRequests.insert(result.value().photoId.value, result.value().id);
    m_sourceResolutionWidget->setRequestPending(true);
    statusBar()->showMessage(tr("Resolving source..."));
    return true;
}

// 목적: immutable source request lifecycle을 inline control과 Catalog presentation에 반영
// 입력: event: initial active requests 또는 accepted·update·issue·terminal transition
// 출력: pending 상태와 source transition 결과 갱신
void MainWindow::handleSourceResolutionEvent(const core::client::SourceResolutionEvent& event)
{
    m_sourceResolutionRequests.clear();
    for (const core::client::SourceRequestReceipt& request : event.snapshot.activeRequests)
    {
        if (isExplicitSourceResolution(request.kind))
        {
            m_sourceResolutionRequests.insert(request.photoId.value, request.id);
        }
    }

    if (event.update.has_value())
    {
        handleSourceBindingUpdated(*event.update);
    }
    if (event.issue.has_value())
    {
        handleSourceBindingFailed(*event.issue);
    }
    if (event.terminal.has_value() && event.terminal->state == core::client::SourceResolutionTerminalState::Cancelled)
    {
        handleSourceBindingCancelled(*event.terminal);
    }

    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
}

// 목적: source transition을 현재 Editor selection과 catalog list에 반영
// 입력: update: 갱신된 기존 photo와 optional 신규 photo
// 출력: 현재 selection의 Register as New이면 신규 PhotoId로 전환
void MainWindow::handleSourceBindingUpdated(const core::client::SourceResolutionUpdate& update)
{
    core::orchestration::EditorState editorState = currentEditorState(*m_editorClient);
    const bool currentPhoto = editorState.photo.photoId.value == update.receipt.photoId.value;
    const bool explicitRequest = isExplicitSourceResolution(update.receipt.kind);

    if (update.createdPhoto.has_value() && currentPhoto)
    {
        syncPreviewViewport(*m_previewPresentationClient, m_previewWidget->size());
        const core::client::EditorResult selected = m_editorClient->selectPhoto({update.createdPhoto->id});
        if (selected.hasError())
        {
            LOG_WARN("catalog", "Unable to select newly registered photo: {}", selected.error().technicalMessage);
            statusBar()->showMessage(tr("New photo was registered but could not be selected."));
        }
        else
        {
            editorState = core::orchestration::fromClientEditorSnapshot(selected.value());
            m_developPanel->setParams(editorState.params);
        }
    }

    const core::catalog::CatalogPhotoRecord updatedPhoto = facade::toCatalogPhotoRecord(update.photo);
    std::optional<core::catalog::CatalogPhotoRecord> createdPhoto;
    if (update.createdPhoto.has_value())
    {
        createdPhoto = facade::toCatalogPhotoRecord(*update.createdPhoto);
    }
    m_catalogWidget->applyPhotoUpdate(updatedPhoto, createdPhoto, editorState.photo.photoId);
    if (explicitRequest && currentPhoto)
    {
        m_sourceResolutionWidget->setRequestPending(
            m_sourceResolutionRequests.contains(editorState.photo.photoId.value));
        statusBar()->showMessage(tr("Source resolution complete."));
    }
}

// 목적: current photo의 source verification/resolution 실패를 inline 상태로 표시
// 입력: issue: request·photo identity와 technical 오류
// 출력: pending 해제와 사용자용 실패 안내
void MainWindow::handleSourceBindingFailed(const core::client::SourceResolutionIssue& issue)
{
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    const bool currentPhoto = state.photo.photoId.value == issue.receipt.photoId.value;
    const bool explicitRequest = isExplicitSourceResolution(issue.receipt.kind);
    if (!currentPhoto)
    {
        return;
    }

    if (explicitRequest)
    {
        m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
    }
    LOG_WARN("catalog", "Source binding request failed: {}", issue.error.technicalMessage);
    statusBar()->showMessage(explicitRequest ? tr("Source resolution failed.") : tr("Source verification failed."));
}

// 목적: source request terminal cancellation을 inline 상태에 반영
// 입력: terminal: 취소된 request context와 terminal state
// 출력: pending 상태 해제
void MainWindow::handleSourceBindingCancelled(const core::client::SourceResolutionTerminal& terminal)
{
    if (!isExplicitSourceResolution(terminal.receipt.kind))
    {
        return;
    }

    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (state.photo.photoId.value == terminal.receipt.photoId.value)
    {
        m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
        statusBar()->showMessage(tr("Source resolution cancelled."));
    }
}

// 목적: 현재 catalog-backed photo의 develop state를 explicit user command로 저장
// 입력: 없음
// 출력: 성공 시 persisted baseline 갱신, 실패 시 non-modal 상태 표시
void MainWindow::saveCurrentPhoto()
{
    (void)persistCurrentPhoto();
}

// 목적: 현재 Editor snapshot과 Catalog multi-selection을 Local/Remote/Auto graphical export dialog에 연결
// 입력: 없음
// 출력: 처리 가능한 선택 photo를 item별 output 목록으로 제공하는 modal export workflow 실행
void MainWindow::exportCurrentPhoto()
{
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    QVector<export_::ExportDialogSource> sources;
    const QString catalogPath = fromClientString(m_catalogSessionClient->catalogSnapshot().catalogPath);
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = m_catalogWidget->selectedPhotos();
    for (const core::catalog::CatalogPhotoRecord& photo : selectedPhotos)
    {
        std::optional<export_::ExportDialogSource> source = makeCatalogExportSource(photo, state, catalogPath);
        if (source.has_value())
        {
            sources.push_back(std::move(*source));
        }
    }

    const QVector<core::catalog::CatalogEntry> selectedEntries = m_catalogWidget->selectedEntries();
    for (const core::catalog::CatalogEntry& entry : selectedEntries)
    {
        std::optional<export_::ExportDialogSource> source = makeFolderExportSource(entry, state, catalogPath);
        if (source.has_value())
        {
            sources.push_back(std::move(*source));
        }
    }

    const qsizetype skippedSourceCount = selectedPhotos.size() + selectedEntries.size() - sources.size();

    if (sources.isEmpty())
    {
        if (!state.hasSelection || !state.sourceProcessingAllowed)
        {
            statusBar()->showMessage(tr("Select an available photo before exporting."));
            return;
        }
        sources.push_back({state.source, {}, state.params});
    }

    export_::ExportDialog dialog(*m_exportClient,
                                 *m_exportEventSource,
                                 std::move(sources),
                                 *m_workerProfileClient,
                                 *m_exportDefaultsClient,
                                 this,
                                 skippedSourceCount);
    (void)dialog.exec();
}

// 목적: 현재 catalog-backed photo 저장 command를 실행하고 결과를 UI에 표시
// 입력: 없음
// 출력: 저장 성공 또는 저장할 dirty state가 없으면 true
bool MainWindow::persistCurrentPhoto()
{
    m_developPanel->finishActiveAdjustment();
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (!state.hasSelection || !core::types::isValidPhotoId(state.photo.photoId) || !state.dirty)
    {
        return true;
    }

    const core::client::EditorResult saved = m_editorClient->saveDevelopState();
    if (saved.hasError())
    {
        LOG_WARN("catalog", "Unable to save GUI develop state: {}", saved.error().technicalMessage);
        statusBar()->showMessage(tr("Develop settings were not saved."));
        return false;
    }

    const QString displayName =
        saved.value().source.has_value() ? fromClientString(saved.value().source->displayName) : QString{};
    statusBar()->showMessage(tr("Develop settings saved for %1.").arg(displayName));
    return true;
}

// 목적: catalog 전환 또는 window 종료 전에 dirty edit의 저장·폐기·취소 의사 확인
// 입력: 없음
// 출력: 호출자가 전환을 계속해도 되면 true
bool MainWindow::confirmPendingSave()
{
    m_developPanel->finishActiveAdjustment();
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    if (!state.hasSelection || !core::types::isValidPhotoId(state.photo.photoId) || !state.dirty)
    {
        return true;
    }

    const QMessageBox::StandardButton choice =
        QMessageBox::warning(this,
                             tr("Unsaved Develop Settings"),
                             tr("Save the develop settings for %1 before continuing?").arg(state.source.displayName),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Save);

    if (choice == QMessageBox::Cancel)
    {
        return false;
    }

    if (choice == QMessageBox::Discard)
    {
        const core::client::EditorResult cleared = m_editorClient->clearEditorSelection();
        if (cleared.hasError())
        {
            LOG_WARN("catalog", "Unable to discard GUI develop state: {}", cleared.error().technicalMessage);
            statusBar()->showMessage(tr("Develop settings could not be discarded."));
            return false;
        }
        return true;
    }

    return persistCurrentPhoto();
}

// 목적: 중앙 UI를 graphical 또는 console mode로 전환
// 입력: enabled: console mode 활성화 여부
// 출력: 없음
void MainWindow::setConsoleMode(bool enabled)
{
    m_contentStack->setCurrentIndex(enabled ? 1 : 0);
    menuBar()->setVisible(!enabled);
    statusBar()->setVisible(!enabled);

    if (enabled)
    {
        m_consoleWidget->focusInput();
    }
}

// 목적: application UI preference와 product setting을 편집하는 Settings dialog 표시
// 입력: 없음
// 출력: accept 시 Export 기본값 저장과 adjustment style 즉시 반영
void MainWindow::openSettings()
{
    settings::SettingsDialog dialog(*m_applicationSettings,
                                    *m_workerProfileClient,
                                    *m_exportDefaultsClient,
                                    m_workerHealthClient,
                                    m_workerHealthEventSource,
                                    m_catalogStartupSettingsClient,
                                    this);
    if (dialog.exec() == QDialog::Accepted)
    {
        m_developPanel->setAdjustmentControlStyle(dialog.adjustmentControlStyle());
    }
}

// 목적: 현재 선택 사진에 대한 develop parameter 변경을 preview와 history에 반영
// 입력: params: panel에서 변경된 develop parameter 값
// 출력: 없음
void MainWindow::applyDevelopParams(const core::types::DevelopParams& params)
{
    syncPreviewViewport(*m_previewPresentationClient, m_previewWidget->size());
    const core::client::EditorResult updated =
        m_editorClient->updateDevelopParams({core::orchestration::toClientDevelopParams(params)});
    if (updated.hasError())
    {
        LOG_WARN("catalog", "Unable to update Develop parameters: {}", updated.error().technicalMessage);
    }
}

// 목적: 현재 사진의 연속 parameter 조작을 하나의 undo 단계로 시작
// 입력: 없음
// 출력: 없음
void MainWindow::beginDevelopAdjustment()
{
    const core::client::EditorResult started = m_editorClient->beginAdjustment();
    if (started.hasError())
    {
        LOG_WARN("catalog", "Unable to begin Develop Adjustment: {}", started.error().technicalMessage);
    }
}

// 목적: 현재 사진의 연속 parameter 조작을 완료하고 undo action 상태 갱신
// 입력: 없음
// 출력: 없음
void MainWindow::finishDevelopAdjustment()
{
    const core::client::EditorResult finished = m_editorClient->endAdjustment();
    if (finished.hasError())
    {
        LOG_WARN("catalog", "Unable to finish Develop Adjustment: {}", finished.error().technicalMessage);
    }
}

// 목적: 현재 사진의 마지막 develop parameter 변경을 undo
// 입력: 없음
// 출력: 없음
void MainWindow::undoDevelopAdjustment()
{
    syncPreviewViewport(*m_previewPresentationClient, m_previewWidget->size());
    const core::client::EditorResult state = m_editorClient->undoDevelop();

    if (state.hasValue())
    {
        m_developPanel->setParams(core::orchestration::fromClientDevelopParams(state.value().params));
    }
}

// 목적: 현재 사진의 마지막 undo된 develop parameter 변경을 redo
// 입력: 없음
// 출력: 없음
void MainWindow::redoDevelopAdjustment()
{
    syncPreviewViewport(*m_previewPresentationClient, m_previewWidget->size());
    const core::client::EditorResult state = m_editorClient->redoDevelop();

    if (state.hasValue())
    {
        m_developPanel->setParams(core::orchestration::fromClientDevelopParams(state.value().params));
    }
}

// 목적: Editor state 변경을 action 활성화와 source-blocked 안내에 반영
// 입력: state: CatalogEditorFacade가 publish한 immutable state
// 출력: 없음
void MainWindow::updateEditorStateUi(const core::orchestration::EditorState& state)
{
    updateEditorActions();
    m_sourceResolutionWidget->setEditorState(state);
    m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));

    if (!state.hasSelection || state.sourceProcessingAllowed)
    {
        return;
    }

    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    QString message = tr("Source is unavailable for processing.");

    if (state.sourceState.has_value())
    {
        switch (*state.sourceState)
        {
        case core::catalog::SourceBindingState::Missing:
            message = tr("Source file is missing. Select another photo or relink it later.");
            break;
        case core::catalog::SourceBindingState::VerificationRequired:
            message = tr("Verifying source identity...");
            break;
        case core::catalog::SourceBindingState::IdentityUnverified:
            message = tr("Source identity needs confirmation.");
            break;
        case core::catalog::SourceBindingState::ReplacementDetected:
            message = tr("Source file was replaced. Select another photo or resolve it later.");
            break;
        case core::catalog::SourceBindingState::Unreadable:
            message = tr("Source file cannot be read.");
            break;
        case core::catalog::SourceBindingState::Unlinked:
            message = tr("Photo is not linked to a source file.");
            break;
        case core::catalog::SourceBindingState::FingerprintPending:
        case core::catalog::SourceBindingState::Available:
            break;
        }
    }

    m_previewWidget->showMessage(message);
    statusBar()->showMessage(message);
}

// 목적: 현재 사진 state에 맞춰 Save, undo와 redo action 활성화 갱신
// 입력: 없음
// 출력: 없음
void MainWindow::updateEditorActions()
{
    const core::orchestration::EditorState state = currentEditorState(*m_editorClient);
    m_saveDevelopAction->setEnabled(state.hasSelection && core::types::isValidPhotoId(state.photo.photoId) &&
                                    state.dirty);
    m_exportAction->setEnabled(state.hasSelection && state.sourceProcessingAllowed && !state.source.path.isEmpty());
    m_undoDevelopAction->setEnabled(state.hasSelection && state.canUndo);
    m_redoDevelopAction->setEnabled(state.hasSelection && state.canRedo);
}

}  // namespace flexraw::ui::mainwindow
