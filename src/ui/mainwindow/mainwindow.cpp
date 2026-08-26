#include "mainwindow.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <QAction>
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
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "catalog_editor_facade.h"
#include "catalog_list_widget.h"
#include "catalog_photo_page_qt_adapter.h"
#include "console_mode_widget.h"
#include "develop_panel.h"
#include "display_frame_qt_adapter.h"
#include "editor_client_projection.h"
#include "editor_settings.h"
#include "export_dialog.h"
#include "folder_scan_controller.h"
#include "log.h"
#include "photo_identity.h"
#include "preview_widget.h"
#include "qt_activity_adapter.h"
#include "settings_dialog.h"
#include "shared_storage_locator.h"
#include "source_binding.h"
#include "source_resolution_widget.h"

namespace flexraw::ui::mainwindow
{
namespace
{

// 목적: UTF-8 client string을 Qt presentation 문자열로 변환
// 입력: value: byte length가 명시된 UTF-8 string
// 출력: 같은 Unicode text를 보유한 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
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
// 입력: folderPath: optional exact Folder, projectId: optional Project identity
// 출력: 두 scope가 배타적인 첫 page request
[[nodiscard]] core::client::CatalogPhotoPageRequest makePhotoPageRequest(
    const std::optional<QString>& folderPath, const std::optional<core::catalog::ProjectId>& projectId)
{
    core::client::CatalogPhotoPageRequest request;
    request.exactFolderPath = projectId.has_value() ? std::nullopt : toClientFolderPath(folderPath);
    request.projectId = toClientProjectId(projectId);
    return request;
}

}  // namespace

// 목적: Flexraw 첫 catalog-to-preview window를 editor session adapter로 초기화
// 입력: catalogEditorFacade: GUI boundary, catalogOrchestrator: console catalog use case,
//       exportOrchestrator: Local/Remote/Auto export use case, parent: Qt 부모 widget
// 출력: 초기화된 MainWindow 객체
MainWindow::MainWindow(facade::CatalogEditorFacade& catalogEditorFacade,
                       core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                       core::orchestration::ExportOrchestrator& exportOrchestrator,
                       QWidget* parent)
    : QMainWindow(parent),
      m_catalogWidget(new catalog::CatalogListWidget(this)),
      m_developPanel(new editor::DevelopPanel(this)),
      m_sourceResolutionWidget(new catalog::SourceResolutionWidget(this)),
      m_previewWidget(new editor::PreviewWidget(this)),
      m_consoleWidget(new cli::ConsoleModeWidget(catalogOrchestrator, exportOrchestrator, this)),
      m_folderScanController(new FolderScanController(this)),
      m_catalogEditorFacade(&catalogEditorFacade),
      m_activityAdapter(new QtActivityAdapter(catalogEditorFacade, *m_folderScanController, this)),
      m_exportOrchestrator(&exportOrchestrator),
      m_contentStack(new QStackedWidget(this))
{
    setWindowTitle(tr("Flexraw"));
    resize(1200, 800);
    QSettings applicationSettings;
    m_developPanel->setAdjustmentControlStyle(
        settings::EditorSettings(applicationSettings).loadAdjustmentControlStyle());

    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    m_newCatalogAction = fileMenu->addAction(tr("New Catalog..."));
    connect(m_newCatalogAction, &QAction::triggered, this, &MainWindow::createCatalog);
    m_openCatalogAction = fileMenu->addAction(tr("Open Catalog..."));
    connect(m_openCatalogAction, &QAction::triggered, this, &MainWindow::openCatalog);
    m_importFolderAction = fileMenu->addAction(tr("Import Folder..."));
    m_importFolderAction->setEnabled(false);
    connect(m_importFolderAction, &QAction::triggered, this, &MainWindow::importFolder);
    m_openFolderAction = fileMenu->addAction(tr("Open Folder..."));
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
    m_settingsAction = editMenu->addAction(tr("Settings..."));
    m_settingsAction->setObjectName(QStringLiteral("settingsAction"));
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    auto* splitter = new QSplitter(this);
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
            [this](QVector<core::types::FileDescriptor> sources, const QSize& targetSize) {
                const core::orchestration::CatalogThumbnailWindowResult result =
                    m_catalogEditorFacade->updateThumbnailWindow(std::move(sources), targetSize);
                if (result.hasError())
                {
                    LOG_WARN("catalog", "Unable to update thumbnail window: {}", result.error().message.toStdString());
                }
            });
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::catalogThumbnailReady,
            this,
            [this](const core::orchestration::CatalogThumbnailFrame& frame) {
                m_catalogWidget->applyThumbnail(frame.sourcePath, frame.image);
            });
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::catalogThumbnailFailed,
            this,
            [this](const core::orchestration::CatalogThumbnailIssue& issue) {
                m_catalogWidget->markThumbnailFailed(issue.sourcePath);
                LOG_DEBUG("catalog",
                          "Catalog thumbnail unavailable for {}: {}",
                          issue.sourcePath.toStdString(),
                          issue.error.message.toStdString());
            });
    connect(m_developPanel, &editor::DevelopPanel::paramsChanged, this, &MainWindow::applyDevelopParams);
    connect(m_developPanel, &editor::DevelopPanel::adjustmentStarted, this, &MainWindow::beginDevelopAdjustment);
    connect(m_developPanel, &editor::DevelopPanel::adjustmentFinished, this, &MainWindow::finishDevelopAdjustment);
    connect(m_previewWidget, &editor::PreviewWidget::viewportSizeChanged, this, [this](const QSize& size) {
        (void)m_catalogEditorFacade->updatePreviewTargetSize(size);
    });
    const core::client::EditorStateSubscriptionResult editorStateSubscription =
        m_catalogEditorFacade->subscribeToEditorState([this](const core::client::EditorStateEvent& event) {
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
        connect(m_catalogEditorFacade,
                &facade::CatalogEditorFacade::editorStateChanged,
                this,
                &MainWindow::updateEditorStateUi);
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
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::sourceBindingUpdated,
            this,
            &MainWindow::handleSourceBindingUpdated);
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::sourceBindingFailed,
            this,
            &MainWindow::handleSourceBindingFailed);
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::sourceBindingCancelled,
            this,
            &MainWindow::handleSourceBindingCancelled);
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::displayFrameUpdated,
            this,
            [this](const core::client::DisplayFrame& frame) {
                const QImage image = facade::toQImage(frame);
                if (!image.isNull())
                {
                    m_previewWidget->showPreview(image);
                }
            });
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::previewUpdated,
            this,
            [this](const core::orchestration::PreviewResult& result) {
                if (result.renderMode == core::orchestration::PreviewRenderMode::Final)
                {
                    m_developPanel->setHistogram(result.histogram);
                    m_previewWidget->setClippingSummary(result.clipping);
                }

                m_developPanel->setEnabled(true);
                const QString displayName = m_catalogEditorFacade->editorState().source.displayName;

                if (result.tier == core::orchestration::PreviewTier::Standard)
                {
                    statusBar()->showMessage(tr("Showing standard preview for %1.").arg(displayName));
                }
                else
                {
                    statusBar()->showMessage(tr("Showing %1.").arg(displayName));
                }
            });
    connect(m_catalogEditorFacade,
            &facade::CatalogEditorFacade::previewFailed,
            this,
            [this](const core::orchestration::PreviewIssue&) {
                m_developPanel->setEnabled(false);
                m_previewWidget->showMessage(tr("Unable to load preview."));
                statusBar()->showMessage(
                    tr("Preview unavailable for %1.").arg(m_catalogEditorFacade->editorState().source.displayName));
            });
    connect(m_consoleWidget, &cli::ConsoleModeWidget::exitRequested, this, [this] {
        m_consoleModeAction->setChecked(false);
    });
    connect(m_folderScanController, &FolderScanController::scanStarted, this, [this](const QString&) {
        m_newCatalogAction->setEnabled(false);
        m_openCatalogAction->setEnabled(false);
        m_openFolderAction->setEnabled(false);
        m_importFolderAction->setEnabled(false);
        m_previousCatalogPageButton->setEnabled(false);
        m_nextCatalogPageButton->setEnabled(false);
        m_catalogFolderScopeComboBox->setEnabled(false);
        m_catalogProjectScopeComboBox->setEnabled(false);
        m_catalogProjectMenuButton->setEnabled(false);

        if (m_importingFolder)
        {
            statusBar()->showMessage(tr("Scanning folder for import..."));
            return;
        }

        m_developPanel->setEnabled(false);
        m_developPanel->clearHistogram();
        m_previewWidget->clearClippingSummary();
        m_catalogEditorFacade->clearSelection();
        resetCatalogPhotoPage();
        m_catalogFolderPath.reset();
        m_catalogProjectId.reset();
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
    });
    connect(m_folderScanController,
            &FolderScanController::scanFinished,
            this,
            [this](const core::catalog::CatalogScanResult& scanResult) {
                const bool importingFolder = m_importingFolder;
                m_importingFolder = false;
                m_newCatalogAction->setEnabled(true);
                m_openCatalogAction->setEnabled(true);
                m_openFolderAction->setEnabled(true);
                m_importFolderAction->setEnabled(m_catalogEditorFacade->catalogState().isOpen);
                updateProjectActions();

                if (scanResult.hasError())
                {
                    if (!importingFolder)
                    {
                        m_catalogWidget->clearEntries();
                        m_previewWidget->showMessage(tr("Unable to scan folder."));
                    }
                    statusBar()->showMessage(tr("Folder scan failed."));
                    updateCatalogPageActions();
                    return;
                }

                if (importingFolder)
                {
                    if (scanResult.value().isEmpty())
                    {
                        updateCatalogPageActions();
                        statusBar()->showMessage(tr("No supported files found in this folder."));
                        return;
                    }

                    const core::orchestration::CatalogImportResult imported =
                        m_catalogEditorFacade->importScannedEntries(scanResult.value());
                    if (imported.hasError())
                    {
                        LOG_WARN("catalog", "Unable to import GUI folder: {}", imported.error().message.toStdString());
                        updateCatalogPageActions();
                        statusBar()->showMessage(tr("Folder import failed."));
                        return;
                    }

                    if (!refreshCatalogPhotos())
                    {
                        return;
                    }

                    statusBar()->showMessage(tr("Imported %1 photos.").arg(imported.value().storedCount));
                    return;
                }

                resetCatalogPhotoPage();
                m_catalogWidget->setEntries(scanResult.value());

                if (scanResult.value().isEmpty())
                {
                    m_previewWidget->showMessage(tr("No supported files found in this folder."));
                }
                else
                {
                    m_previewWidget->showMessage(tr("Select a photo to edit."));
                }

                statusBar()->showMessage(tr("Loaded %1 files.").arg(scanResult.value().size()));
            });

    m_developPanel->setEnabled(false);
    updateEditorActions();
    if (m_catalogEditorFacade->catalogState().isOpen)
    {
        (void)refreshCatalogPhotos();
    }
    else
    {
        m_previewWidget->showMessage(tr("Open a catalog or folder to view photos."));
        statusBar()->showMessage(tr("Ready."));
    }
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

// 목적: immutable active 목록을 status bar progress와 owner cancel target에 투영
// 입력: event: initial 또는 lifecycle transition 뒤 Activity snapshot
// 출력: active 여부와 cancellability에 맞는 compact status UI
void MainWindow::updateActivityUi(const core::client::ActivityEvent& event)
{
    if (event.activeActivities.empty())
    {
        m_cancelActivityId.reset();
        m_activityLabel->hide();
        m_activityProgressBar->hide();
        m_cancelActivityButton->hide();
        return;
    }

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
    case core::client::ActivityKind::SourceVerification:
        activityName = tr("Verifying source...");
        break;
    }
    m_activityLabel->setText(event.activeActivities.size() == 1
                                 ? activityName
                                 : tr("%1 background activities...").arg(event.activeActivities.size()));
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

    if (openCatalogPath(catalogPath))
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
        (void)openCatalogPath(catalogPath);
    }
}

// 목적: 지정 catalog를 단일 active session으로 열고 stable PhotoId 목록 표시
// 입력: catalogPath: 생성하거나 열 catalog file 경로
// 출력: catalog와 photo list를 모두 열었으면 true
bool MainWindow::openCatalogPath(const QString& catalogPath)
{
    const core::orchestration::CatalogSessionState session = m_catalogEditorFacade->catalogState();
    if (session.isOpen && QFileInfo(catalogPath).absoluteFilePath() == session.catalogPath)
    {
        m_importFolderAction->setEnabled(true);
        statusBar()->showMessage(tr("This catalog is already open."));
        return true;
    }

    if (!confirmPendingSave())
    {
        return false;
    }

    m_catalogEditorFacade->clearSelection();
    resetCatalogPhotoPage();
    m_catalogFolderPath.reset();
    m_catalogProjectId.reset();
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
    m_catalogProjectMenuButton->setEnabled(false);
    m_catalogWidget->clearEntries();
    m_importFolderAction->setEnabled(false);
    if (session.isOpen)
    {
        (void)m_catalogEditorFacade->closeCatalog();
    }

    const core::orchestration::CatalogSessionResult opened = m_catalogEditorFacade->openCatalog(catalogPath);
    if (opened.hasError())
    {
        LOG_WARN("catalog", "Unable to open GUI catalog: {}", opened.error().message.toStdString());
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
        (void)m_catalogEditorFacade->closeCatalog();
        m_previewWidget->showMessage(tr("Unable to load catalog navigation."));
        statusBar()->showMessage(tr("Catalog navigation failed."));
        return false;
    }

    const core::client::CatalogPhotoPageRequest request = makePhotoPageRequest(m_catalogFolderPath, m_catalogProjectId);
    const core::client::CatalogPhotoPageResult photos = m_catalogEditorFacade->queryPhotoPage(request);
    if (photos.hasError())
    {
        LOG_WARN("catalog", "Unable to list GUI catalog photos: {}", photos.error().technicalMessage);
        resetCatalogPhotoPage();
        (void)m_catalogEditorFacade->closeCatalog();
        m_catalogFolderScopeComboBox->setEnabled(false);
        m_catalogProjectScopeComboBox->setEnabled(false);
        m_catalogProjectMenuButton->setEnabled(false);
        m_previewWidget->showMessage(tr("Unable to load catalog photos."));
        statusBar()->showMessage(tr("Catalog photo list failed."));
        return false;
    }

    m_importFolderAction->setEnabled(true);
    m_catalogPhotoPage = photos.value();
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
    const core::orchestration::CatalogFolderListResult folders = m_catalogEditorFacade->queryFolders();
    if (folders.hasError())
    {
        LOG_WARN("catalog", "Unable to list GUI catalog folders: {}", folders.error().message.toStdString());
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
    for (const core::catalog::CatalogFolderSummary& folder : folders.value())
    {
        const QString displayPath = QDir::toNativeSeparators(folder.path);
        const int index = m_catalogFolderScopeComboBox->count();
        m_catalogFolderScopeComboBox->addItem(tr("%1 (%2)").arg(displayPath).arg(folder.photoCount), folder.path);
        m_catalogFolderScopeComboBox->setItemData(index, displayPath, Qt::ToolTipRole);
        if (m_catalogFolderPath.has_value() && folder.path == *m_catalogFolderPath)
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
    const core::client::CatalogProjectListResult projects = m_catalogEditorFacade->listProjects();
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
    if (index < 0 || !m_catalogEditorFacade->catalogState().isOpen)
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
    if (index < 0 || !m_catalogEditorFacade->catalogState().isOpen)
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
        m_catalogEditorFacade->createProject({name.toUtf8().toStdString()});
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
        m_catalogEditorFacade->renameProject({{m_catalogProjectId->value}, name.toUtf8().toStdString()});
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
        m_catalogEditorFacade->deleteProject({{m_catalogProjectId->value}});
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
    const core::client::CatalogProjectListResult projects = m_catalogEditorFacade->listProjects();
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
            m_catalogEditorFacade->addPhotoToProject({project.id, {photo.id.value}});
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
            m_catalogEditorFacade->removePhotoFromProject({{m_catalogProjectId->value}, {photo.id.value}});
        if (removed.hasError())
        {
            LOG_WARN("catalog", "Unable to remove GUI project membership: {}", removed.error().technicalMessage);
            break;
        }
        ++removedCount;
    }

    core::client::CatalogPhotoPageRequest request;
    request.projectId = toClientProjectId(m_catalogProjectId);
    (void)loadCatalogPhotoPage(request);
    statusBar()->showMessage(
        tr("Removed %1 of %2 selected photos from the project.").arg(removedCount).arg(selectedPhotos.size()));
}

// 목적: Catalog session, active Project와 selected Photo에 맞춰 Project action 상태 갱신
// 입력: 없음
// 출력: 유효한 Project command만 활성화
void MainWindow::updateProjectActions()
{
    const bool available = m_catalogEditorFacade->catalogState().isOpen && !m_folderScanController->isScanning();
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
}

// 목적: cursor 요청에 해당하는 bounded Catalog photo page를 현재 목록에 적용
// 입력: request: page 크기, 이동 방향과 exclusive cursor
// 출력: 조회와 UI 적용에 성공하면 true
bool MainWindow::loadCatalogPhotoPage(const core::client::CatalogPhotoPageRequest& request)
{
    const core::client::CatalogPhotoPageResult result = m_catalogEditorFacade->queryPhotoPage(request);
    if (result.hasError())
    {
        LOG_WARN("catalog", "Unable to navigate GUI catalog photos: {}", result.error().technicalMessage);
        statusBar()->showMessage(tr("Catalog page navigation failed."));
        return false;
    }

    m_catalogPhotoPage = result.value();
    m_catalogEditorFacade->clearSelection();
    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    m_previewWidget->clearClippingSummary();
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
    (void)loadCatalogPhotoPage(request);
}

// 목적: Catalog page cursor state와 navigation control 초기화
// 입력: 없음
// 출력: 현재 page가 제거되고 navigation button 비활성화
void MainWindow::resetCatalogPhotoPage()
{
    m_catalogPhotoPage.reset();
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
    if (!m_catalogEditorFacade->catalogState().isOpen || m_folderScanController->isScanning())
    {
        statusBar()->showMessage(tr("Open or create a catalog before importing photos."));
        return;
    }

    const QString folderPath = QFileDialog::getExistingDirectory(this, tr("Import Folder"));
    if (folderPath.isEmpty() || !persistCurrentPhoto())
    {
        return;
    }

    m_importingFolder = true;
    m_folderScanController->scanFolder(folderPath);
}

// 목적: folder 선택 dialog를 열고 shared-storage root marker를 보장한 뒤 scan 시작
// 입력: 없음
// 출력: marker 실패와 무관하게 선택된 local folder scan 시작
void MainWindow::openFolder()
{
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
    m_folderScanController->scanFolder(folderPath);
}

// 목적: 선택된 Folder photo를 active Catalog에 등록·resolve하고 Editor에 연결
// 입력: entry: 사용자가 선택한 CatalogEntry 값
// 출력: stable PhotoId activation 성공 시 preview 예약, 실패 시 이전 선택 복원
void MainWindow::showSelectedEntry(const core::catalog::CatalogEntry& entry)
{
    const core::orchestration::EditorState previousState = m_catalogEditorFacade->editorState();
    if (!persistCurrentPhoto())
    {
        m_catalogWidget->restoreEntrySelection(previousState.source.path);
        return;
    }

    m_developPanel->setEnabled(false);
    m_developPanel->clearHistogram();
    m_previewWidget->clearClippingSummary();
    m_previewWidget->showMessage(tr("Loading preview..."));
    const core::orchestration::EditorStateResult selected =
        m_catalogEditorFacade->activatePhoto(entry, m_previewWidget->size());
    if (selected.hasError())
    {
        LOG_WARN("catalog", "Unable to activate Folder photo: {}", selected.error().message.toStdString());
        m_catalogWidget->restoreEntrySelection(previousState.source.path);
        m_previewWidget->showMessage(tr("Unable to register this photo in the catalog."));
        statusBar()->showMessage(tr("Photo activation failed."));
        return;
    }

    m_developPanel->setParams(selected.value().params);
}

// 목적: 선택된 catalog-backed photo를 persisted develop state와 함께 editor에 연결
// 입력: photo: stable PhotoId와 source binding을 포함한 catalog record
// 출력: 없음
void MainWindow::showSelectedCatalogPhoto(const core::catalog::CatalogPhotoRecord& photo)
{
    const core::orchestration::EditorState currentState = m_catalogEditorFacade->editorState();
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
    m_previewWidget->clearClippingSummary();
    m_previewWidget->showMessage(tr("Loading preview..."));
    const core::orchestration::EditorStateResult selected =
        m_catalogEditorFacade->selectCatalogPhoto(photo.id, m_previewWidget->size());

    if (selected.hasError())
    {
        LOG_WARN("catalog", "Unable to select GUI catalog photo: {}", selected.error().message.toStdString());
        m_catalogWidget->restorePhotoSelection(currentState.photo.photoId);
        m_previewWidget->showMessage(tr("Unable to select catalog photo."));
        statusBar()->showMessage(tr("Catalog photo selection failed."));
        return;
    }

    m_developPanel->setParams(selected.value().params);
}

// 목적: 선택 photo의 replacement source를 기존 identity의 새 baseline으로 수용
// 입력: 없음
// 출력: accepted source request를 inline pending 상태로 표시
void MainWindow::acceptReplacement()
{
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    if (!core::types::isValidPhotoId(state.photo.photoId) || !state.sourceResolution.canAcceptReplacement)
    {
        return;
    }

    (void)beginSourceResolution(m_catalogEditorFacade->acceptReplacement(state.photo.photoId),
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

    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    if (!core::types::isValidPhotoId(state.photo.photoId) || !state.sourceResolution.canRegisterReplacementAsNew)
    {
        return;
    }

    (void)beginSourceResolution(m_catalogEditorFacade->registerReplacementAsNew(state.photo.photoId),
                                tr("Unable to register the replacement as a new photo."));
}

// 목적: 선택 photo와 동일한 content의 다른 source locator 선택
// 입력: 없음
// 출력: file 선택 후 accepted relink request 표시
void MainWindow::relinkSource()
{
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
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

    (void)beginSourceResolution(
        m_catalogEditorFacade->relinkSource(state.photo.photoId, core::types::SourceLocator{sourcePath}),
        tr("Unable to relink this source."));
}

// 목적: source resolution command 제출 결과를 공통 inline 상태로 변환
// 입력: result: request ID 또는 오류, failureMessage: 사용자용 실패 안내
// 출력: request가 accepted되면 true
bool MainWindow::beginSourceResolution(const core::orchestration::CatalogSourceSubmissionResult& result,
                                       const QString& failureMessage)
{
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    if (result.hasError())
    {
        LOG_WARN("catalog", "Unable to submit source resolution: {}", result.error().message.toStdString());
        m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
        statusBar()->showMessage(failureMessage);
        return false;
    }

    m_sourceResolutionRequests.insert(state.photo.photoId.value, result.value());
    m_sourceResolutionWidget->setRequestPending(true);
    statusBar()->showMessage(tr("Resolving source..."));
    return true;
}

// 목적: source transition을 현재 Editor selection과 catalog list에 반영
// 입력: update: 갱신된 기존 photo와 optional 신규 photo
// 출력: 현재 selection의 Register as New이면 신규 PhotoId로 전환
void MainWindow::handleSourceBindingUpdated(const core::orchestration::CatalogSourceUpdate& update)
{
    core::orchestration::EditorState editorState = m_catalogEditorFacade->editorState();
    const bool currentPhoto = editorState.photo.photoId.value == update.photoId.value;
    const std::optional<core::types::PhotoId> explicitPhoto = takeSourceResolutionRequest(update.requestId);
    const bool explicitRequest = explicitPhoto.has_value() && explicitPhoto->value == update.photoId.value;

    if (update.createdPhoto.has_value() && currentPhoto)
    {
        const core::orchestration::EditorStateResult selected =
            m_catalogEditorFacade->selectCatalogPhoto(update.createdPhoto->id, m_previewWidget->size());
        if (selected.hasError())
        {
            LOG_WARN("catalog", "Unable to select newly registered photo: {}", selected.error().message.toStdString());
            statusBar()->showMessage(tr("New photo was registered but could not be selected."));
        }
        else
        {
            editorState = selected.value();
            m_developPanel->setParams(editorState.params);
        }
    }

    m_catalogWidget->applyPhotoUpdate(update.photo, update.createdPhoto, editorState.photo.photoId);
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
void MainWindow::handleSourceBindingFailed(const core::orchestration::CatalogIssue& issue)
{
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    const bool currentPhoto = state.photo.photoId.value == issue.photoId.value;
    const std::optional<core::types::PhotoId> explicitPhoto = takeSourceResolutionRequest(issue.requestId);
    const bool explicitRequest = explicitPhoto.has_value() && explicitPhoto->value == issue.photoId.value;
    if (!currentPhoto)
    {
        return;
    }

    if (explicitRequest)
    {
        m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
    }
    LOG_WARN("catalog", "Source binding request failed: {}", issue.error.message.toStdString());
    statusBar()->showMessage(explicitRequest ? tr("Source resolution failed.") : tr("Source verification failed."));
}

// 목적: source request terminal cancellation을 inline 상태에 반영
// 입력: requestId: 취소된 request identity
// 출력: pending 상태 해제
void MainWindow::handleSourceBindingCancelled(core::types::RequestId requestId)
{
    const std::optional<core::types::PhotoId> explicitPhoto = takeSourceResolutionRequest(requestId);
    if (!explicitPhoto.has_value())
    {
        return;
    }

    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    if (state.photo.photoId.value == explicitPhoto->value)
    {
        m_sourceResolutionWidget->setRequestPending(m_sourceResolutionRequests.contains(state.photo.photoId.value));
        statusBar()->showMessage(tr("Source resolution cancelled."));
    }
}

// 목적: terminal event와 일치하는 explicit source request 추적을 제거
// 입력: requestId: 완료·실패·취소된 request identity
// 출력: 해당 request가 명시적 GUI command이면 대상 PhotoId
std::optional<core::types::PhotoId> MainWindow::takeSourceResolutionRequest(core::types::RequestId requestId)
{
    for (auto request = m_sourceResolutionRequests.begin(); request != m_sourceResolutionRequests.end(); ++request)
    {
        if (request.value() == requestId)
        {
            const core::types::PhotoId photoId{request.key()};
            m_sourceResolutionRequests.erase(request);
            return photoId;
        }
    }

    return std::nullopt;
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
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    if (!state.hasSelection || !state.sourceProcessingAllowed)
    {
        statusBar()->showMessage(tr("Select an available photo before exporting."));
        return;
    }

    QVector<export_::ExportDialogSource> sources;
    const QString catalogPath = m_catalogEditorFacade->catalogState().catalogPath;
    const QVector<core::catalog::CatalogPhotoRecord> selectedPhotos = m_catalogWidget->selectedPhotos();
    for (const core::catalog::CatalogPhotoRecord& photo : selectedPhotos)
    {
        if (!photo.source.has_value() || !core::catalog::allowsSourceProcessing(photo.sourceState))
        {
            continue;
        }

        core::types::FileDescriptor source{
            photo.source->path,
            photo.extension,
            photo.displayName,
            photo.kind,
        };
        const bool isCurrentPhoto = state.photo.photoId.value == photo.id.value;
        sources.push_back({source,
                           photo.kind == core::types::SupportedFileKind::Raw ? catalogPath : QString{},
                           isCurrentPhoto ? std::optional<core::types::DevelopParams>{state.params} : std::nullopt});
    }

    const QVector<core::catalog::CatalogEntry> selectedEntries = m_catalogWidget->selectedEntries();
    for (const core::catalog::CatalogEntry& entry : selectedEntries)
    {
        if (entry.status != core::types::FileScanStatus::Ready)
        {
            continue;
        }
        const bool isCurrentPhoto = entry.file.path == state.source.path;
        sources.push_back({entry.file,
                           entry.file.kind == core::types::SupportedFileKind::Raw ? catalogPath : QString{},
                           isCurrentPhoto ? std::optional<core::types::DevelopParams>{state.params} : std::nullopt,
                           !isCurrentPhoto && entry.file.kind == core::types::SupportedFileKind::Raw});
    }

    if (sources.isEmpty())
    {
        sources.push_back({state.source, {}, state.params});
    }

    QSettings settings;
    export_::ExportDialog dialog(*m_exportOrchestrator, std::move(sources), settings, this);
    (void)dialog.exec();
}

// 목적: 현재 catalog-backed photo 저장 command를 실행하고 결과를 UI에 표시
// 입력: 없음
// 출력: 저장 성공 또는 저장할 dirty state가 없으면 true
bool MainWindow::persistCurrentPhoto()
{
    m_developPanel->finishActiveAdjustment();
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    if (!state.hasSelection || !core::types::isValidPhotoId(state.photo.photoId) || !state.dirty)
    {
        return true;
    }

    const core::orchestration::EditorStateResult saved = m_catalogEditorFacade->saveCurrentPhoto();
    if (saved.hasError())
    {
        LOG_WARN("catalog", "Unable to save GUI develop state: {}", saved.error().message.toStdString());
        statusBar()->showMessage(tr("Develop settings were not saved."));
        return false;
    }

    statusBar()->showMessage(tr("Develop settings saved for %1.").arg(saved.value().source.displayName));
    return true;
}

// 목적: catalog 전환 또는 window 종료 전에 dirty edit의 저장·폐기·취소 의사 확인
// 입력: 없음
// 출력: 호출자가 전환을 계속해도 되면 true
bool MainWindow::confirmPendingSave()
{
    m_developPanel->finishActiveAdjustment();
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
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

    return choice == QMessageBox::Discard || persistCurrentPhoto();
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

// 목적: application UI preference를 편집하고 accepted style을 즉시 DevelopPanel에 반영
// 입력: 없음
// 출력: Settings dialog가 modal로 표시되고 accept 시 preference 저장
void MainWindow::openSettings()
{
    QSettings applicationSettings;
    settings::SettingsDialog dialog(applicationSettings, this);
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
    (void)m_catalogEditorFacade->updateDevelopParams(params, m_previewWidget->size());
}

// 목적: 현재 사진의 연속 parameter 조작을 하나의 undo 단계로 시작
// 입력: 없음
// 출력: 없음
void MainWindow::beginDevelopAdjustment()
{
    m_catalogEditorFacade->beginEdit();
}

// 목적: 현재 사진의 연속 parameter 조작을 완료하고 undo action 상태 갱신
// 입력: 없음
// 출력: 없음
void MainWindow::finishDevelopAdjustment()
{
    m_catalogEditorFacade->endEdit();
}

// 목적: 현재 사진의 마지막 develop parameter 변경을 undo
// 입력: 없음
// 출력: 없음
void MainWindow::undoDevelopAdjustment()
{
    const std::optional<core::orchestration::EditorState> state = m_catalogEditorFacade->undo(m_previewWidget->size());

    if (state.has_value())
    {
        m_developPanel->setParams(state->params);
    }
}

// 목적: 현재 사진의 마지막 undo된 develop parameter 변경을 redo
// 입력: 없음
// 출력: 없음
void MainWindow::redoDevelopAdjustment()
{
    const std::optional<core::orchestration::EditorState> state = m_catalogEditorFacade->redo(m_previewWidget->size());

    if (state.has_value())
    {
        m_developPanel->setParams(state->params);
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
    m_previewWidget->clearClippingSummary();
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
    const core::orchestration::EditorState state = m_catalogEditorFacade->editorState();
    m_saveDevelopAction->setEnabled(state.hasSelection && core::types::isValidPhotoId(state.photo.photoId) &&
                                    state.dirty);
    m_exportAction->setEnabled(state.hasSelection && state.sourceProcessingAllowed && !state.source.path.isEmpty());
    m_undoDevelopAction->setEnabled(state.hasSelection && state.canUndo);
    m_redoDevelopAction->setEnabled(state.hasSelection && state.canRedo);
}

}  // namespace flexraw::ui::mainwindow
