// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PreferencesDialog.h"

#include <QAbstractItemModel>
#include <QBrush>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QSvgRenderer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#include "DebugMode.h"
#include "MainWindow.h"
#include "PreferencesNavRow.h"
#include "Splashscreen.h"
#include "Theme.h"
#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/HttpGet.h"
#include "pj_runtime/RecordingService.h"
#ifndef __EMSCRIPTEN__
#include "pj_runtime/SourceCacheStore.h"
#endif
#include "pj_runtime/UsageCounters.h"
#include "pj_widgets/AppSettings.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/ToggleSwitch.h"
#include "ui_PreferencesDialog.h"
#ifdef PJ_WITH_SCENE3D
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#endif
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Must equal MainWindow.cpp's k*Default constants (kept by hand): Reset matches a first launch.
constexpr int kDefaultIconSize = 24;
constexpr int kDefaultIconPadding = 4;
constexpr int kDefaultLayoutPadding = 2;
constexpr int kDefaultLayoutSpacing = 2;

QIcon loadIconInkIcon(const QString& resource_path, theme::Theme token_theme) {
  QFile file(resource_path);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    return {};
  }
  QString svg = QString::fromUtf8(file.readAll());
  const QString ink = theme::iconInk(token_theme).name(QColor::HexRgb);
  svg.replace(QRegularExpression(QStringLiteral(R"(fill="#[0-9A-Fa-f]{6}")")), QStringLiteral("fill=\"%1\"").arg(ink));
  QSvgRenderer renderer(svg.toUtf8());
  QImage image(64, 64, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  return QIcon(QPixmap::fromImage(image));
}

// Creates `dir` first: a folder nothing has written to yet would otherwise fail to open, silently.
void openFolder(const QString& dir) {
  QDir().mkpath(dir);
  QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

}  // namespace

PreferencesDialog::PreferencesDialog(Theme& theme, QWidget* parent)
    : Dialog(parent),
      ui_(new Ui::PreferencesContent),
      theme_(theme),
      original_theme_(theme.currentTheme()),
      original_metrics_(
          qobject_cast<MainWindow*>(parent) != nullptr ? qobject_cast<MainWindow*>(parent)->chromeMetrics()
                                                       : ChromeMetrics{}) {
  setDialogTitle(tr("Preferences"));
  // The Dialog content area has its own layout, hence a body widget rather than setupUi(this).
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
  // SetDefaultConstraint would raise the minimum to the layout's on every activation, which the Appearance
  // page's fixed-height scrubber rows make too tall to shrink (or to restore a smaller size). The explicit
  // minimum below is the only floor.
  if (auto* root = layout()) {
    root->setSizeConstraint(QLayout::SetNoConstraint);
  }
  // Narrower than the pages' minimum, the forms squeeze the fixed-size segmented selectors below their
  // sizeHint and their option labels overlap.
  const int min_width = std::max(420, minimumSizeHint().width());
  setMinimumSize(min_width, 300);
  // The destructor saves the geometry.
  {
    QSettings settings;
    const QByteArray geometry = settings.value(u"Preferences::dialog_geometry"_s).toByteArray();
    if (geometry.isEmpty() || !restoreGeometry(geometry)) {
      resize(std::max(560, min_width), 400);
    }
  }

  auto* nav_layout = qobject_cast<QVBoxLayout*>(ui_->navContainer->layout());
  std::vector<QString> nav_names{
      tr("Appearance"),
      tr("Plotting"),
      tr("Scene 2D"),
      tr("Scene 3D"),
  };
#ifdef PJ_TARGET_WASM
  // The browser loads only built-in plugins and has no marketplace. Row indices still match page indices only
  // because the one page after Plugins, Recording, is hidden in the browser too.
  ui_->pagePlugins->hide();
#else
  nav_names.push_back(tr("Plugins"));
#endif
  if (RecordingService::isSupported()) {
    nav_names.push_back(tr("Recording"));
  } else {
    // Hidden also to keep it out of the stack's size hint.
    ui_->pageRecording->hide();
  }
  nav_rows_.reserve(nav_names.size());
  for (const QString& label : nav_names) {
    const int index = static_cast<int>(nav_rows_.size());  // rows follow pagesStack order
    auto* row = new PreferencesNavRow(label, ui_->navContainer);
    nav_layout->insertWidget(index, row);
    nav_rows_.push_back(row);
    connect(row, &PreferencesNavRow::clicked, this, [this, row, index]() {
      for (auto* other : nav_rows_) {
        other->setSelected(other == row);
      }
      ui_->pagesStack->setCurrentIndex(index);
    });
  }
  if (!nav_rows_.empty()) {
    nav_rows_.front()->setSelected(true);
    ui_->pagesStack->setCurrentIndex(kAppearancePage);
  }

  // The chrome-metric scrubbers show only under --debug-mode. Hide the whole form row: hiding the widget
  // alone leaves its gap.
  if (auto* appearance_form = qobject_cast<QFormLayout*>(ui_->pageAppearance->layout())) {
    int row = -1;
    QFormLayout::ItemRole role{};
    appearance_form->getWidgetPosition(ui_->debugChromeWidget, &row, &role);
    if (row >= 0) {
      appearance_form->setRowVisible(row, isDebugMode());
    }
  }
  // It resets only those scrubbers.
  ui_->buttonResetDefaults->setVisible(isDebugMode());

#if !defined(PJ_WITH_SCENE3D) || defined(PJ_TARGET_WASM)
  // The browser grants no access to host folders, and a build without 3D views loads no robot models.
  ui_->pageScene3D->setEnabled(false);
#endif

  // Ranges match MainWindow's k*Range clamps. Each tick previews live through MainWindow's setter; OK persists,
  // reject restores the snapshot.
  auto* main_window = qobject_cast<MainWindow*>(parent);
  ui_->iconSizeScrubber->setRange(12, 48);
  ui_->iconSizeScrubber->setSingleStep(1);
  ui_->iconSizeScrubber->setSuffix(u" px"_s);
  ui_->iconSizeScrubber->setValue(original_metrics_.icon_size);

  ui_->iconPaddingScrubber->setRange(0, 32);
  ui_->iconPaddingScrubber->setSingleStep(1);
  ui_->iconPaddingScrubber->setSuffix(u" px"_s);
  ui_->iconPaddingScrubber->setValue(original_metrics_.icon_padding);

  ui_->layoutPaddingScrubber->setRange(0, 16);
  ui_->layoutPaddingScrubber->setSingleStep(1);
  ui_->layoutPaddingScrubber->setSuffix(u" px"_s);
  ui_->layoutPaddingScrubber->setValue(original_metrics_.layout_padding);

  ui_->layoutSpacingScrubber->setRange(0, 16);
  ui_->layoutSpacingScrubber->setSingleStep(1);
  ui_->layoutSpacingScrubber->setSuffix(u" px"_s);
  ui_->layoutSpacingScrubber->setValue(original_metrics_.layout_spacing);

  // Committed on OK. When each applies: precision at once (valuePrecisionChanged); the frameless window at the next
  // launch (MainWindow reads it once); the GPU canvas to plots created later (--disable-opengl overrides it
  // for a session without touching it).
  ui_->scrubberFloatPrecision->setRange(1, 6);
  ui_->scrubberFloatPrecision->setSingleStep(1);
  ui_->curveColorMode->setOptions(tr("global"), tr("per plot"));
  ui_->splashMode->setOptions(tr("memes"), tr("serious"));
  ui_->derivedBatchMode->setOptions(tr("Automatic"), tr("Manual"));
  ui_->clockTimeZone->setOptions(tr("local"), tr("UTC"));
  ui_->scrubberDerivedReloadHz->setRange(BatchEngine::kMinReloadHz, BatchEngine::kMaxReloadHz);
  {
    QSettings settings;
    ui_->scrubberFloatPrecision->setValue(valuePrecision());
    ui_->curveColorMode->setSelectedIndex(settings.value(u"Preferences::curve_color_global"_s, true).toBool() ? 0 : 1);
    ui_->clockTimeZone->setSelectedIndex(settings.value(MainWindow::kClockUtcKey, false).toBool() ? 1 : 0);
    const BatchEngine::Settings batch = BatchEngine::loadSettings();
    ui_->derivedBatchMode->setSelectedIndex(batch.mode == BatchEngine::Mode::kManual ? 1 : 0);
    ui_->scrubberDerivedReloadHz->setValue(batch.reload_hz);
    ui_->openglToggle->setChecked(
        settings.value(u"Preferences::use_opengl"_s, true).toBool(),
        /*animate=*/false);
    ui_->customTitleBarToggle->setChecked(
        settings.value(MainWindow::kCustomTitleBarKey, false).toBool(),
        /*animate=*/false);
    ui_->checkUpdatesToggle->setChecked(
        settings.value(u"Preferences::check_updates_on_startup"_s, true).toBool(),
        /*animate=*/false);
    ui_->telemetryToggle->setChecked(
        settings.value(u"Preferences::send_anonymous_stats"_s, true).toBool(),
        /*animate=*/false);
    ui_->usageStatsToggle->setChecked(settings.value(MainWindow::kUsageStatsKey, true).toBool(), /*animate=*/false);
    ui_->splashMode->setSelectedIndex(
        settings.value(kSplashModeKey, kSplashModeMemes).toString() == kSplashModeSerious ? 1 : 0);
    if (!MainWindow::customFrameSelectable()) {
      ui_->labelCustomTitleBar->hide();
      ui_->customTitleBarToggle->hide();
    }
    ui_->systemFileDialogToggle->setChecked(
        settings.value(MainWindow::kSystemFileDialogKey, false).toBool(),
        /*animate=*/false);
#ifndef PJ_TARGET_WASM
    // Whether public access is open now (--mcp included), so OK can always revoke it. A plugin's private
    // endpoint does not count.
    ui_->mcpServerToggle->setChecked(main_window != nullptr && main_window->mcpPublicAccess(), /*animate=*/false);
#endif
    if (!FileDialog::systemDialogSupported()) {
      ui_->labelSystemFileDialog->hide();
      ui_->systemFileDialogToggle->hide();
    }
#ifdef PJ_TARGET_WASM
    // The browser build makes neither startup network call (main.cpp).
    ui_->labelCheckUpdates->hide();
    ui_->checkUpdatesToggle->hide();
    ui_->labelTelemetry->hide();
    ui_->telemetryToggle->hide();
    ui_->labelUsageStats->hide();
    ui_->usageStatsToggle->hide();
    // The browser has no raster canvas: plots always render through WebGL.
    ui_->labelOpenGL->hide();
    ui_->openglToggle->hide();
    // The browser build carries no memes: its splash is always the serious one.
    ui_->labelSplashscreen->hide();
    ui_->splashMode->hide();
    // A page cannot listen on a socket.
    ui_->labelMcpServer->hide();
    ui_->mcpServerToggle->hide();
#endif
  }

  if (main_window != nullptr) {
    connect(ui_->iconSizeScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconSize);
    connect(ui_->iconPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconPadding);
    connect(ui_->layoutPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutPadding);
    connect(ui_->layoutSpacingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutSpacing);
  }

  // The custom plugin folders persist on OK and apply at the next launch (no hot reload of extensions).
  if (main_window != nullptr) {
    auto paint_missing = [this](QListWidget* list) {
      const auto token_theme = theme::themeFor(theme_.currentTheme() == QLatin1String("light"));
      const QBrush missing_brush(theme::interaction(theme::Variant::Highlight, theme::State::Nominal, token_theme));
      for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        const bool missing = !QDir(item->text()).exists();
        item->setForeground(missing ? missing_brush : QBrush());
        item->setToolTip(missing ? tr("This folder does not exist.") : QString());
      }
    };
    auto wire_folder_list = [this, paint_missing](
                                QListWidget* list, SvgButton* add, SvgButton* remove, const QString& add_tooltip,
                                const QString& dialog_title) {
      paint_missing(list);
      connect(&theme_, &Theme::themeChanged, this, [list, paint_missing](const QString&) { paint_missing(list); });
      add->setIconPath(u":/resources/svg/add.svg"_s);
      add->setExtent(26, 24);
      remove->setIconPath(u":/resources/svg/trash.svg"_s);
      remove->setExtent(26, 24);
      add->setToolTip(add_tooltip);
      remove->setToolTip(tr("Remove the selected folder"));
      connect(add, &QToolButton::clicked, this, [this, list, paint_missing, dialog_title]() {
        const QString dir = PJ::FileDialog::getExistingDirectory(this, dialog_title);
        if (!dir.isEmpty() && list->findItems(dir, Qt::MatchExactly).isEmpty()) {
          list->addItem(dir);
          paint_missing(list);
        }
      });
      connect(remove, &QToolButton::clicked, this, [list]() { qDeleteAll(list->selectedItems()); });
    };

    ui_->listCustomPluginFolders->setDragDropMode(QAbstractItemView::InternalMove);
    ui_->listCustomPluginFolders->addItems(main_window->customPluginFolders());
    wire_folder_list(
        ui_->listCustomPluginFolders, ui_->buttonAddPluginFolder, ui_->buttonRemovePluginFolder,
        tr("Add a plugin folder…"), tr("Add plugin folder"));
    // A drag-reorder drops the items' foreground brush.
    connect(ui_->listCustomPluginFolders->model(), &QAbstractItemModel::rowsMoved, this, [this, paint_missing]() {
      paint_missing(ui_->listCustomPluginFolders);
    });

    ui_->listDefaultPluginFolders->addItems(main_window->builtinPluginFolders());
    ui_->listDefaultPluginFolders->setSelectionMode(QAbstractItemView::NoSelection);
    ui_->listDefaultPluginFolders->setFocusPolicy(Qt::NoFocus);
    paint_missing(ui_->listDefaultPluginFolders);
    connect(&theme_, &Theme::themeChanged, this, [this, paint_missing](const QString&) {
      paint_missing(ui_->listDefaultPluginFolders);
    });

    ui_->registryUrlMode->setOptions(tr("default"), tr("custom"));
    const QString stored_override = main_window->registryUrlSetting();
    ui_->registryUrlMode->setSelectedIndex(stored_override.isEmpty() ? 0 : 1);
    ui_->lineEditRegistryUrl->setText(stored_override.isEmpty() ? MainWindow::defaultRegistryUrl() : stored_override);
    ui_->lineEditRegistryUrl->setEnabled(!stored_override.isEmpty());
    connect(ui_->registryUrlMode, &DualOptionsWidget::selectionChanged, this, [this](int index) {
      const bool custom = index == 1;
      ui_->lineEditRegistryUrl->setEnabled(custom);
      if (custom) {
        ui_->lineEditRegistryUrl->setFocus();
        ui_->lineEditRegistryUrl->selectAll();
      } else {
        ui_->lineEditRegistryUrl->setText(MainWindow::defaultRegistryUrl());
        last_checked_registry_url_.clear();
        setRegistryUrlError(false);
      }
    });
    connect(
        ui_->lineEditRegistryUrl, &QLineEdit::editingFinished, this, &PreferencesDialog::onRegistryUrlEditingFinished);
    // A stale stored override shows red without touching the field.
    if (!stored_override.isEmpty()) {
      onRegistryUrlEditingFinished();
    }

#if defined(PJ_WITH_SCENE3D) && !defined(PJ_TARGET_WASM)
    ui_->listAssetFolders->addItems(Scene3DDockWidget::packageSearchFolders(QSettings()));
    wire_folder_list(
        ui_->listAssetFolders, ui_->buttonAddAssetsFolder, ui_->buttonRemoveAssetFolder, tr("Add a folder…"),
        tr("Add a folder with robot packages"));
#endif
  }

  // Wired only where the page is reachable: an unreachable page's .ui defaults must never overwrite the stored
  // settings.
  if (RecordingService::isSupported()) {
    const RecordingService::Settings stored = RecordingService::loadSettings();
    ui_->lineEditRecordingDir->setText(stored.directory);
    ui_->lineEditRecordingDir->setToolTip(tr("Leave empty to use the default folder: %1")
                                              .arg(QDir::toNativeSeparators(RecordingService::defaultDirectory())));
    ui_->scrubberRecordingQueueMiB->setValue(stored.queue_budget_mib);

    ui_->buttonRecordingDirBrowse->setIconPath(u":/resources/svg/folder_open.svg"_s);
    ui_->buttonRecordingDirBrowse->setExtent(26, 24);
    ui_->buttonRecordingDirBrowse->setToolTip(tr("Choose the recordings folder…"));
    connect(ui_->buttonRecordingDirBrowse, &QToolButton::clicked, this, [this]() {
      const QString field = ui_->lineEditRecordingDir->text().trimmed();
      const QString dir = PJ::FileDialog::getExistingDirectory(
          this, tr("Choose the recordings folder"), field.isEmpty() ? RecordingService::defaultDirectory() : field);
      if (!dir.isEmpty()) {
        ui_->lineEditRecordingDir->setText(dir);
      }
    });

    ui_->buttonRecordingDirOpen->setIconPath(u":/resources/svg/open_in_new.svg"_s);
    ui_->buttonRecordingDirOpen->setExtent(26, 24);
    ui_->buttonRecordingDirOpen->setToolTip(tr("Open the recordings folder"));
    connect(ui_->buttonRecordingDirOpen, &QToolButton::clicked, this, [this]() {
      const QString field = ui_->lineEditRecordingDir->text().trimmed();
      openFolder(field.isEmpty() ? RecordingService::defaultDirectory() : field);
    });

    // One handler for the whole page: the cache settings must be saved before recordingSettingsChanged,
    // whose MainWindow handler re-reads them to apply the capture toggle at once.
    connect(this, &QDialog::accepted, this, [this]() {
      RecordingService::Settings settings;
      // Trimmed: a stray space would turn the empty "default folder" value into a relative path.
      settings.directory = ui_->lineEditRecordingDir->text().trimmed();
      settings.queue_budget_mib = ui_->scrubberRecordingQueueMiB->value();
      RecordingService::saveSettings(settings);
#ifndef __EMSCRIPTEN__
      SourceCacheStore::Settings cache_settings;
      cache_settings.directory = ui_->lineEditSourceCacheDir->text().trimmed();
      cache_settings.budget_gb = ui_->scrubberSourceCacheBudgetGB->value();
      cache_settings.capture_enabled = ui_->sourceCacheCaptureToggle->isChecked();
      SourceCacheStore::saveSettings(cache_settings);
#endif
      emit recordingSettingsChanged();
    });

#ifndef __EMSCRIPTEN__
    // Folder and budget apply at the next launch (armed captures hold the store they opened); the capture
    // toggle applies at once.
    const SourceCacheStore::Settings cache_stored = SourceCacheStore::loadSettings();
    ui_->lineEditSourceCacheDir->setText(cache_stored.directory);
    const QString cache_default = QString::fromStdU16String(SourceCacheStore::defaultRoot().u16string());
    ui_->lineEditSourceCacheDir->setToolTip(
        tr("Leave empty to use the default folder: %1").arg(QDir::toNativeSeparators(cache_default)));
    ui_->scrubberSourceCacheBudgetGB->setValue(cache_stored.budget_gb);
    ui_->sourceCacheCaptureToggle->setChecked(cache_stored.capture_enabled, /*animate=*/false);
    ui_->buttonSourceCacheDirBrowse->setIconPath(u":/resources/svg/folder_open.svg"_s);
    ui_->buttonSourceCacheDirBrowse->setExtent(26, 24);
    ui_->buttonSourceCacheDirBrowse->setToolTip(tr("Choose the source cache folder…"));
    connect(ui_->buttonSourceCacheDirBrowse, &QToolButton::clicked, this, [this, cache_default]() {
      const QString field = ui_->lineEditSourceCacheDir->text().trimmed();
      const QString dir = PJ::FileDialog::getExistingDirectory(
          this, tr("Choose the source cache folder"), field.isEmpty() ? cache_default : field);
      if (!dir.isEmpty()) {
        ui_->lineEditSourceCacheDir->setText(dir);
      }
    });
    ui_->buttonSourceCacheDirOpen->setIconPath(u":/resources/svg/open_in_new.svg"_s);
    ui_->buttonSourceCacheDirOpen->setExtent(26, 24);
    ui_->buttonSourceCacheDirOpen->setToolTip(tr("Open the source cache folder"));
    connect(ui_->buttonSourceCacheDirOpen, &QToolButton::clicked, this, [this, cache_default]() {
      const QString field = ui_->lineEditSourceCacheDir->text().trimmed();
      openFolder(field.isEmpty() ? cache_default : field);
    });
#endif
  }

  // Previews live through the scrubbers; reject still reverts to the snapshot.
  ui_->buttonResetDefaults->setIconPath(u":/resources/svg/restart_alt.svg"_s);
  ui_->buttonResetDefaults->setSize(SvgButton::Size::kDefault);
  connect(ui_->buttonResetDefaults, &QToolButton::clicked, this, [this]() {
    ui_->iconSizeScrubber->setValue(kDefaultIconSize);
    ui_->iconPaddingScrubber->setValue(kDefaultIconPadding);
    ui_->layoutPaddingScrubber->setValue(kDefaultLayoutPadding);
    ui_->layoutSpacingScrubber->setValue(kDefaultLayoutSpacing);
  });

  // Checked (thumb right) = light, the sun showing in the left slot; unchecked = dark, the moon on the right.
  ui_->themeToggle->setFixedSize(44, 24);
  auto apply_toggle_icons = [this]() {
    const auto token_theme = theme::themeFor(theme_.currentTheme() == QLatin1String("light"));
    ui_->themeToggle->setLeftIcon(loadIconInkIcon(QStringLiteral(":/resources/svg/light_mode_light.svg"), token_theme));
    ui_->themeToggle->setRightIcon(loadIconInkIcon(QStringLiteral(":/resources/svg/dark_mode_light.svg"), token_theme));
  };
  apply_toggle_icons();
  ui_->themeToggle->setChecked(original_theme_ == QLatin1String("light"), /*animate=*/false);

  // The toggle is locked from click to the end of the theme change: `clicked` (as the slide starts) disables it,
  // `toggled` (when the slide ends) runs setTheme once, and stylesheetChanged re-enables it. MainWindow emits
  // that from its queued onThemeChanged; this queued slot runs after it has finished.
  if (main_window != nullptr) {
    connect(
        main_window, &MainWindow::stylesheetChanged, this,
        [this, apply_toggle_icons](const QString&) {
          apply_toggle_icons();
          ui_->themeToggle->setEnabled(true);
        },
        Qt::QueuedConnection);
  }
  connect(ui_->themeToggle, &ToggleSwitch::clicked, this, [this]() { ui_->themeToggle->setEnabled(false); });
  connect(ui_->themeToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
    theme_.setTheme(checked ? u"light"_s : u"dark"_s);
  });

  {
    QSettings settings;
    ui_->autoZoomToggle->setChecked(
        settings.value(u"Preferences::auto_zoom_plots"_s, true).toBool(),
        /*animate=*/false);
  }
  connect(this, &QDialog::rejected, this, [this, main_window]() {
    theme_.setTheme(original_theme_);
    if (main_window != nullptr) {
      main_window->setIconSize(original_metrics_.icon_size);
      main_window->setIconPadding(original_metrics_.icon_padding);
      main_window->setLayoutPadding(original_metrics_.layout_padding);
      main_window->setLayoutSpacing(original_metrics_.layout_spacing);
    }
  });
  // The chrome setters only preview; OK persists them.
  connect(this, &QDialog::accepted, this, [this, main_window]() {
    if (main_window != nullptr) {
      main_window->persistChromeMetrics();
      const auto items_of = [](const QListWidget* list) {
        QStringList texts;
        for (int row = 0; row < list->count(); ++row) {
          texts << list->item(row)->text();
        }
        return texts;
      };
      main_window->setCustomPluginFolders(items_of(ui_->listCustomPluginFolders));
#if defined(PJ_WITH_SCENE3D) && !defined(PJ_TARGET_WASM)
      if (QSettings settings; items_of(ui_->listAssetFolders) != Scene3DDockWidget::packageSearchFolders(settings)) {
        Scene3DDockWidget::setPackageSearchFolders(settings, items_of(ui_->listAssetFolders));
        emit packageSearchFoldersChanged();
      }
#endif

      // The default (or a custom value equal to it) stores no override, so future defaults still apply. An
      // invalid custom URL is not stored; an unreachable one is.
      const bool custom_registry = ui_->registryUrlMode->selectedIndex() == 1;
      const QString registry_url = ui_->lineEditRegistryUrl->text().trimmed();
      if (!custom_registry || registry_url == MainWindow::defaultRegistryUrl()) {
        main_window->setRegistryUrlSetting({});
      } else if (MainWindow::isValidRegistryUrl(registry_url)) {
        main_window->setRegistryUrlSetting(registry_url);
      }
    }
    QSettings settings;
    const bool precision_changed = ui_->scrubberFloatPrecision->value() != valuePrecision();
    settings.setValue(QString::fromLatin1(kPrecisionSettingsKey), ui_->scrubberFloatPrecision->value());
    settings.setValue(u"Preferences::use_opengl"_s, ui_->openglToggle->isChecked());
    settings.setValue(MainWindow::kCustomTitleBarKey, ui_->customTitleBarToggle->isChecked());
    settings.setValue(MainWindow::kSystemFileDialogKey, ui_->systemFileDialogToggle->isChecked());
    FileDialog::setUseSystemDialog(ui_->systemFileDialogToggle->isChecked());
    settings.setValue(u"Preferences::check_updates_on_startup"_s, ui_->checkUpdatesToggle->isChecked());
    settings.setValue(u"Preferences::send_anonymous_stats"_s, ui_->telemetryToggle->isChecked());
    settings.setValue(MainWindow::kUsageStatsKey, ui_->usageStatsToggle->isChecked());
    if (!ui_->usageStatsToggle->isChecked()) {
      usageCounters().setEnabled(false);  // an opt-out also deletes what was counted before it
    }
    settings.setValue(u"Preferences::curve_color_global"_s, ui_->curveColorMode->selectedIndex() == 0);
    settings.setValue(kSplashModeKey, ui_->splashMode->selectedIndex() == 1 ? kSplashModeSerious : kSplashModeMemes);
    settings.setValue(u"Preferences::auto_zoom_plots"_s, ui_->autoZoomToggle->isChecked());
    BatchEngine::saveSettings(
        {.mode =
             ui_->derivedBatchMode->selectedIndex() == 1 ? BatchEngine::Mode::kManual : BatchEngine::Mode::kAutomatic,
         .reload_hz = ui_->scrubberDerivedReloadHz->value()});
    emit derivedBatchSettingsChanged();
    settings.setValue(MainWindow::kClockUtcKey, ui_->clockTimeZone->selectedIndex() == 1);
    emit timeLabelSettingsChanged();
    if (precision_changed) {
      emit valuePrecisionChanged();
    }
#ifndef PJ_TARGET_WASM
    // Applies only when it differs from the current state: access opened with --mcp stays open, the stored
    // setting untouched, unless turned off here.
    if (const bool mcp_on = ui_->mcpServerToggle->isChecked();
        main_window != nullptr && mcp_on != main_window->mcpPublicAccess()) {
      settings.setValue(MainWindow::kMcpServerKey, mcp_on);
      if (mcp_on) {
        main_window->openMcpPublicAccess();
      } else {
        main_window->closeMcpPublicAccess();
      }
    }
#endif
  });

  connect(ui_->buttonOk, &QPushButton::clicked, this, &QDialog::accept);
}

PreferencesDialog::~PreferencesDialog() {
  // Saved on Cancel too: the size is not a setting the dialog edits.
  QSettings settings;
  settings.setValue(u"Preferences::dialog_geometry"_s, saveGeometry());
  delete ui_;
}

void PreferencesDialog::showPage(int index) {
  for (std::size_t i = 0; i < nav_rows_.size(); ++i) {
    nav_rows_[i]->setSelected(static_cast<int>(i) == index);
  }
  ui_->pagesStack->setCurrentIndex(index);
}

void PreferencesDialog::onRegistryUrlEditingFinished() {
  const QString url_text = ui_->lineEditRegistryUrl->text().trimmed();
  if (url_text == last_checked_registry_url_) {
    return;
  }
  last_checked_registry_url_ = url_text;

  if (url_text.isEmpty() || !MainWindow::isValidRegistryUrl(url_text)) {
    setRegistryUrlError(true);
    return;
  }

  const QUrl url(url_text);
  if (url.isLocalFile()) {
    setRegistryUrlError(!QFile::exists(url.toLocalFile()));
    return;
  }

  // Not reported Online (or no backend to tell): an outage must not paint red a URL that works later.
  [[maybe_unused]] static const bool backend_loaded = QNetworkInformation::loadDefaultBackend();
  QNetworkInformation* network_info = QNetworkInformation::instance();
  if (network_info == nullptr || network_info->reachability() != QNetworkInformation::Reachability::Online) {
    setRegistryUrlError(false);
    return;
  }

  if (network_ == nullptr) {
    network_ = new QNetworkAccessManager(this);
  }
  httpGetWithTimeout(
      *network_, QNetworkRequest(url), std::chrono::seconds(5), this, [this, url_text](QNetworkReply& reply) {
        // A stale probe must not repaint newer text.
        if (ui_->lineEditRegistryUrl->text().trimmed() != url_text) {
          return;
        }
        setRegistryUrlError(reply.error() != QNetworkReply::NoError);
      });
}

void PreferencesDialog::setRegistryUrlError(bool error) {
  QLineEdit* edit = ui_->lineEditRegistryUrl;
  if (edit->property("urlError").toBool() == error) {
    return;
  }
  edit->setProperty("urlError", error);
  edit->style()->unpolish(edit);
  edit->style()->polish(edit);
}

}  // namespace PJ
