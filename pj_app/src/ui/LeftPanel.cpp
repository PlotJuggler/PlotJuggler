// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/LeftPanel.h"

#include <QAction>
#include <QComboBox>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QMargins>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <initializer_list>
#include <nlohmann/json.hpp>

#ifdef PJ_TARGET_WASM
#include "BrowserPersistence.h"
#endif
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/ScrubberBase.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_LeftPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
#ifndef PJ_TARGET_WASM
constexpr const char* kRecentFilesKey = "File/recent";
// Recent-layouts list, written by MainWindow::recordRecentLayout. Mirrored here
// so the single recent popup can render the Layouts section without coupling to
// MainWindow — kept in sync with MainWindow's kRecentLayoutsKey.
constexpr const char* kRecentLayoutsKey = "Layout/recent";
#endif
// Recent-button chevron: points right when the popup is closed, down while it
// is open. Both are the _light asset, recolored per theme by loadSvg.
constexpr const char* kRecentIconCollapsed = ":/resources/svg/keyboard_arrow_right_light.svg";
constexpr const char* kRecentIconExpanded = ":/resources/svg/keyboard_arrow_down_light.svg";
// Streaming-buffer setting key — preserved verbatim from when the
// scrubber lived on the timeline so user-saved values survive the move.
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";
// Last-selected streaming source, persisted by name so it survives plugin
// re-discovery reordering across sessions (an index would not).
constexpr const char* kStreamingSourceKey = "MainWindow.streamingSource";
// Last-selected cloud toolbox, persisted by plugin id (stable across sessions,
// unlike the display name or a combo index). Mirrors kStreamingSourceKey.
constexpr const char* kCloudSourceKey = "MainWindow.cloudSource";
}  // namespace

LeftPanel::LeftPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::LeftPanel) {
  ui_->setupUi(this);

  applyIcons(currentTheme());

  // Recent popup: two sections — Layouts first, Files second. Each list is
  // deduped/capped/ordered (most-recent-first) by MainWindow when it writes
  // QSettings; here we only render what is stored. Lazy-rebuilt on aboutToShow
  // so it always reflects the latest Load/Save.
  //
  // Section headers are disabled (bold) menu items rather than addSection()
  // titles: the PJMenu stylesheet styles QMenu::separator, which switches Qt to
  // the stylesheet separator path and suppresses addSection() text. A disabled
  // QAction flows through the themed ::item:disabled rule and renders reliably.
  auto* recent_menu = new QMenu(this);
  recent_menu->setObjectName(u"PJMenu"_s);
  // Entries show only the file name; the hover tooltip carries the full path.
  recent_menu->setToolTipsVisible(true);
  connect(recent_menu, &QMenu::aboutToShow, this, [this, recent_menu]() {
    recent_menu->clear();
#ifdef PJ_TARGET_WASM
    const QList<BrowserPersistence::LayoutRecipe> recipes = BrowserPersistence::instance() != nullptr
                                                                ? BrowserPersistence::instance()->recentLayouts()
                                                                : QList<BrowserPersistence::LayoutRecipe>{};
    if (recipes.isEmpty()) {
      QAction* placeholder = recent_menu->addAction(tr("(no recent layouts)"));
      placeholder->setEnabled(false);
      return;
    }
    QAction* header = recent_menu->addAction(tr("Layouts"));
    header->setEnabled(false);
    QFont header_font = header->font();
    header_font.setWeight(QFont::DemiBold);
    header->setFont(header_font);
    for (const BrowserPersistence::LayoutRecipe& recipe : recipes) {
      QAction* action = recent_menu->addAction(recipe.name);
      action->setToolTip(tr("Stored in this browser; no source file bytes are retained."));
      connect(action, &QAction::triggered, this, [this, id = recipe.id]() { emit recentLayoutSelected(id); });
    }
    recent_menu->addSeparator();
    QAction* clear = recent_menu->addAction(tr("Clear recent layouts"));
    connect(clear, &QAction::triggered, this, &LeftPanel::clearRecentLayoutsRequested);
#else
    // Drop entries whose file no longer exists, and persist the pruned list so
    // they do not come back next time.
    auto load_existing = [](const char* key) {
      QSettings settings;
      QStringList paths = settings.value(key).toStringList();
      const qsizetype before = paths.size();
      paths.removeIf([](const QString& path) { return !QFileInfo::exists(path); });
      if (paths.size() != before) {
        settings.setValue(key, paths);
      }
      return paths;
    };
    const QStringList layouts = load_existing(kRecentLayoutsKey);
    const QStringList files = load_existing(kRecentFilesKey);
    if (layouts.isEmpty() && files.isEmpty()) {
      QAction* placeholder = recent_menu->addAction(tr("(no recent files or layouts)"));
      placeholder->setEnabled(false);
      return;
    }
    auto add_header = [recent_menu](const QString& title) {
      QAction* header = recent_menu->addAction(title);
      header->setEnabled(false);
      QFont font = header->font();
      font.setWeight(QFont::DemiBold);
      header->setFont(font);
    };
    if (!layouts.isEmpty()) {
      add_header(tr("Layouts"));
      for (const QString& path : layouts) {
        QAction* action = recent_menu->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path]() { emit recentLayoutSelected(path); });
      }
    }
    if (!files.isEmpty()) {
      add_header(tr("Files"));
      for (const QString& path : files) {
        QAction* action = recent_menu->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path]() { emit recentFileSelected(path); });
      }
    }
#endif
  });
  // Flip the chevron to point down while the popup is open, back to right when
  // it closes (loadSvg recolors the _light asset for the current theme).
  connect(recent_menu, &QMenu::aboutToShow, this, [this]() {
    ui_->buttonRecentFiles->setIcon(loadSvg(kRecentIconExpanded, currentTheme()));
  });
  connect(recent_menu, &QMenu::aboutToHide, this, [this]() {
    ui_->buttonRecentFiles->setIcon(loadSvg(kRecentIconCollapsed, currentTheme()));
  });
  connect(ui_->buttonRecentFiles, &QToolButton::clicked, this, [this, recent_menu]() {
    const QPoint anchor = ui_->buttonRecentFiles->mapToGlobal(QPoint(0, ui_->buttonRecentFiles->height()));
    recent_menu->popup(anchor);
  });

  connect(ui_->buttonLoadDatafile, &QPushButton::clicked, this, &LeftPanel::loadDataRequested);
  connect(ui_->buttonReloadData, &QPushButton::clicked, this, &LeftPanel::reloadDataRequested);
  // Reload moved to the dataset context menu ("Reload"), which covers
  // per-dataset reload in multi-file sessions; the global button is hidden
  // rather than removed while that arrangement is trialed.
  ui_->buttonReloadData->setVisible(false);

  // No data loaded yet -> nothing to reload, no recent entries.
  ui_->buttonReloadData->setEnabled(false);
  ui_->buttonRecentFiles->setEnabled(false);

  // Input section is tabbed: each toggle selects the matching page in
  // the stacked widget. autoExclusive=true on the .ui keeps only one
  // checked at a time, but we still drive the stack manually so the
  // check that fires on initial show also routes correctly.
  connect(ui_->tabFile, &QToolButton::toggled, this, [this](bool on) {
    if (on) {
      ui_->inputStack->setCurrentWidget(ui_->pageFile);
    }
  });
  connect(ui_->tabStream, &QToolButton::toggled, this, [this](bool on) {
    if (on) {
      ui_->inputStack->setCurrentWidget(ui_->pageStream);
    }
  });
  connect(ui_->tabCloud, &QToolButton::toggled, this, [this](bool on) {
    if (on) {
      ui_->inputStack->setCurrentWidget(ui_->pageCloud);
    }
  });

  // QStackedWidget's sizeHint is the union of every page's sizeHint, so
  // shorter pages still reserve room for the tallest one. Mark non-
  // current pages as Ignored so only the visible page contributes to
  // the parent's vertical sizing, and refresh on every page switch.
  auto adapt_stack_to_current_page = [this]() {
    QStackedWidget* stack = ui_->inputStack;
    const int current = stack->currentIndex();
    for (int i = 0; i < stack->count(); ++i) {
      QWidget* page = stack->widget(i);
      QSizePolicy policy = page->sizePolicy();
      policy.setVerticalPolicy(i == current ? QSizePolicy::Preferred : QSizePolicy::Ignored);
      page->setSizePolicy(policy);
    }
    stack->adjustSize();
  };
  adapt_stack_to_current_page();
  connect(ui_->inputStack, &QStackedWidget::currentChanged, this, [adapt_stack_to_current_page](int) {
    adapt_stack_to_current_page();
  });

  // The cog is a one-shot Start trigger for the streaming source. There is
  // no stop affordance — the session lives until app shutdown or error.
  connect(ui_->buttonStreamingOptions, &QPushButton::clicked, this, &LeftPanel::streamingStartRequested);
  // Pause/resume of follow-live, independent of start/stop. PJ3 parity.
  connect(ui_->buttonStreamingPause, &QPushButton::toggled, this, &LeftPanel::streamingPauseToggled);
  connect(
      ui_->buttonStreamingPause, &QPushButton::toggled, this, [this](bool) { applyPauseButtonState(currentTheme()); });
  // Persist the user's choice so it is restored next session. The signal only
  // fires on genuine user selection — setStreamingSources() blocks it while
  // repopulating — so this never re-saves a programmatic restore.
  connect(ui_->comboStreaming, &QComboBox::currentTextChanged, this, [this](const QString& source) {
    QSettings().setValue(kStreamingSourceKey, source);
    emit streamingSourceChanged(source);
  });

  // Buffer scrubber: restore from QSettings on construct. The buffer length only
  // matters once the user finishes adjusting it — reconfiguring the live stream
  // on every drag tick is pointless churn — so both the reconfigure
  // (streamingBufferChanged) and the QSettings write fire on editingFinished.
  ui_->streamingSpinBox->setValue(QSettings().value(kStreamingBufferKey, 5).toInt());
  connect(ui_->streamingSpinBox, &ScrubberBase::editingFinished, this, [this]() {
    const int seconds = ui_->streamingSpinBox->value();
    QSettings().setValue(kStreamingBufferKey, seconds);
    emit streamingBufferChanged(seconds);
  });

  // Cloud row (mirrors the streaming row): the combo lists the cloud-tagged
  // toolboxes (populateCloudToolboxes), the button opens the selected one.
  // Selection is persisted by plugin id; the signal only fires on genuine user
  // selection — populateCloudToolboxes blocks it while repopulating.
  connect(ui_->comboCloud, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    const QString id = ui_->comboCloud->currentData().toString();
    if (!id.isEmpty()) {
      QSettings().setValue(kCloudSourceKey, id);
    }
  });
  connect(ui_->buttonCloudOpen, &QPushButton::clicked, this, [this]() {
    const QString id = ui_->comboCloud->currentData().toString();
    if (!id.isEmpty()) {
      emit cloudToolboxRequested(id);
    }
  });
  // Empty until the extension catalog is scanned (populateCloudToolboxes).
  ui_->comboCloud->setEnabled(false);
  ui_->buttonCloudOpen->setEnabled(false);
}

LeftPanel::~LeftPanel() {
  delete ui_;
}

void LeftPanel::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void LeftPanel::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIcons(currentTheme());
}

void LeftPanel::setReloadEnabled(bool enabled) {
  ui_->buttonReloadData->setEnabled(enabled);
}

void LeftPanel::setRecentEnabled(bool enabled) {
  ui_->buttonRecentFiles->setEnabled(enabled);
}

void LeftPanel::setStreamingSources(const QStringList& names) {
  const QString previous = ui_->comboStreaming->currentText();
  {
    QSignalBlocker block(ui_->comboStreaming);
    ui_->comboStreaming->clear();
    ui_->comboStreaming->addItems(names);
    // Keep the current selection across a mid-session refresh; on the first
    // (empty) population fall back to the source persisted last session.
    const QString desired = previous.isEmpty() ? QSettings().value(kStreamingSourceKey).toString() : previous;
    const int idx = ui_->comboStreaming->findText(desired);
    if (idx >= 0) {
      ui_->comboStreaming->setCurrentIndex(idx);
    }
  }
  // QComboBox auto-selects index 0 on the first addItems(), but the blocker
  // above swallows the corresponding currentTextChanged; emit once so the
  // visible selection matches what listeners (StreamingSourceManager) hold.
  const QString current = ui_->comboStreaming->currentText();
  if (current != previous) {
    emit streamingSourceChanged(current);
  }
}

void LeftPanel::applyPauseButtonState(QString theme) {
  const bool paused = ui_->buttonStreamingPause->isChecked();
  ui_->buttonStreamingPause->setIcon(
      loadSvg(paused ? ":/resources/svg/play_arrow.svg" : ":/resources/svg/pause.svg", theme));
  ui_->buttonStreamingPause->setToolTip(paused ? tr("Resume streaming") : tr("Pause streaming"));
}

QDomElement LeftPanel::saveSourcesState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(u"left_panel_state"_s);

  // sources_tab: report which of the three autoExclusive tabs is checked.
  if (ui_->tabFile->isChecked()) {
    element.setAttribute(u"sources_tab"_s, u"file"_s);
  } else if (ui_->tabStream->isChecked()) {
    element.setAttribute(u"sources_tab"_s, u"stream"_s);
  } else if (ui_->tabCloud->isChecked()) {
    element.setAttribute(u"sources_tab"_s, u"cloud"_s);
  }

  element.setAttribute(u"streaming_source"_s, ui_->comboStreaming->currentText());
  element.setAttribute(u"streaming_buffer"_s, QString::number(ui_->streamingSpinBox->value()));
  return element;
}

void LeftPanel::restoreSourcesState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != "left_panel_state"_L1) {
    return;
  }

  // sources_tab: setChecked(true) propagates via autoExclusive and the
  // connected lambdas (which switch the inputStack page). We deliberately
  // do NOT block these signals — switching the visible page is the
  // intended side-effect of selecting a tab.
  if (element.hasAttribute(u"sources_tab"_s)) {
    const QString tab = element.attribute(u"sources_tab"_s);
    if (tab == "file"_L1) {
      ui_->tabFile->setChecked(true);
    } else if (tab == "stream"_L1) {
      ui_->tabStream->setChecked(true);
    } else if (tab == "cloud"_L1) {
      ui_->tabCloud->setChecked(true);
    }
    // Unknown tab string -> silent no-op.
  }

  // streaming_source: pick the combo entry by display text. -1 from
  // findText means the source isn't currently in the combo (plugin
  // not installed) -> silent no-op. Block signals so we don't emit
  // streamingSourceChanged during restore.
  if (element.hasAttribute(u"streaming_source"_s)) {
    const QString src = element.attribute(u"streaming_source"_s);
    const int idx = ui_->comboStreaming->findText(src);
    if (idx >= 0) {
      const QSignalBlocker blocker(ui_->comboStreaming);
      ui_->comboStreaming->setCurrentIndex(idx);
    }
  }

  // streaming_buffer: setValue triggers the connected lambda which
  // writes QSettings AND emits streamingBufferChanged. Block signals
  // to suppress both.
  if (element.hasAttribute(u"streaming_buffer"_s)) {
    bool ok = false;
    const int seconds = element.attribute(u"streaming_buffer"_s).toInt(&ok);
    if (ok) {
      const QSignalBlocker blocker(ui_->streamingSpinBox);
      ui_->streamingSpinBox->setValue(seconds);
    }
  }
}

void LeftPanel::applyIcons(QString theme) {
  ui_->tabFile->setIcon(loadSvg(":/resources/svg/draft.svg", theme));
  ui_->tabStream->setIcon(loadSvg(":/resources/svg/cast.svg", theme));
  ui_->tabCloud->setIcon(loadSvg(":/resources/svg/cloud.svg", theme));
  ui_->buttonLoadDatafile->setIcon(loadSvg(":/resources/svg/upload_file.svg", theme));
  ui_->buttonReloadData->setIcon(loadSvg(":/resources/svg/restore_page.svg", theme));
  ui_->buttonRecentFiles->setIcon(loadSvg(kRecentIconCollapsed, theme));
  ui_->buttonStreamingOptions->setIcon(loadSvg(":/resources/svg/add.svg", theme));
  ui_->buttonCloudOpen->setIcon(loadSvg(":/resources/svg/add.svg", theme));
  applyPauseButtonState(theme);

  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  // Band height grows by 2 * layout_padding so the contentsMargins
  // applied to inner layouts are absorbed by the container instead of
  // squeezing the buttons.
  const int band_extent = chrome_metrics_.bandHeight();
  // Every icon-bearing button in this panel uses the same square chrome
  // pattern; iterate by type rather than by name.
  for (auto* btn : findChildren<QToolButton*>()) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  for (auto* btn : findChildren<QPushButton*>()) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  // Override the .ui-baked 24-px height pins on the input header band
  // and the streaming-row controls. Header bands grow to band_extent;
  // the inline streaming controls stay button-tall so they line up
  // visually with the buttons in their row.
  ui_->widgetLabelInput->setMinimumHeight(band_extent);
  ui_->widgetLabelInput->setMaximumHeight(band_extent);
  ui_->comboStreaming->setMinimumHeight(button_extent);
  ui_->comboStreaming->setMaximumHeight(button_extent);
  ui_->comboCloud->setMinimumHeight(button_extent);
  ui_->comboCloud->setMaximumHeight(button_extent);
  ui_->streamingSpinBox->setMinimumHeight(button_extent);
  ui_->streamingSpinBox->setMaximumHeight(button_extent);
  ui_->labelBuffer->setMinimumSize(button_extent, button_extent);
  ui_->labelBuffer->setMaximumSize(button_extent, button_extent);
  ui_->labelBuffer->setPixmap(renderSvgPixmap(":/resources/svg/share_eta.svg", theme, icon_sz, devicePixelRatioF()));
  // Push layout_padding into every relevant layout — Sources header,
  // the file/stream/cloud page outer layouts, and the two streaming
  // rows. Spacing follows so individual items inside a row gain the
  // same breathing room as the band edges.
  const QMargins margins(
      chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
      chrome_metrics_.layout_padding);
  for (auto* layout : std::initializer_list<QLayout*>{
           ui_->pageFile->layout(), ui_->pageStream->layout(), ui_->pageCloud->layout(), ui_->streamSourceRow,
           ui_->streamBufferRow, ui_->cloudSourceRow}) {
    if (layout != nullptr) {
      layout->setContentsMargins(margins);
      layout->setSpacing(chrome_metrics_.layout_spacing);
    }
  }
  // The "Sources" title band leads via labelInput's own canonical padding-left
  // (Tight), so its layout adds no left inset — otherwise the two stack into a
  // doubled leading that no longer matches the other section bands.
  if (auto* layout = ui_->inputHeaderLayout) {
    layout->setContentsMargins(
        0, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }
}

void LeftPanel::populateCloudToolboxes(const std::vector<RuntimeToolboxPlugin>& toolboxes) {
  const QString previous = ui_->comboCloud->currentData().toString();
  {
    const QSignalBlocker block(ui_->comboCloud);
    ui_->comboCloud->clear();
    for (const auto& tb : toolboxes) {
      // The manifest lives on the loaded vtable as a constexpr char[]; a null
      // vtable is a load failure already reported through the diagnostic sink.
      const auto* vtable = tb.library.vtable();
      if (vtable == nullptr || vtable->manifest_json == nullptr) {
        continue;
      }
      auto manifest = nlohmann::json::parse(vtable->manifest_json, nullptr, /*allow_exceptions=*/false);
      if (!manifest.is_object()) {
        // The plugin loaded but ships a malformed manifest; warn so a toolbox that
        // silently never appears in the cloud list is diagnosable, then skip it.
        qWarning("LeftPanel: toolbox '%s' has an invalid manifest_json; skipping", tb.id.c_str());
        continue;
      }
      bool is_cloud = false;
      if (auto it = manifest.find("tags"); it != manifest.end() && it->is_array()) {
        for (const auto& tag : *it) {
          if (tag.is_string() && tag.get<std::string>() == "cloud") {
            is_cloud = true;
            break;
          }
        }
      }
      if (!is_cloud) {
        continue;
      }

      // One combo entry per cloud source: display name, plugin id as data
      // (ids are stable across sessions; names/order are not).
      ui_->comboCloud->addItem(QString::fromStdString(tb.name), QString::fromStdString(tb.id));
      if (auto desc = manifest.find("description"); desc != manifest.end() && desc->is_string()) {
        ui_->comboCloud->setItemData(
            ui_->comboCloud->count() - 1, QString::fromStdString(desc->get<std::string>()), Qt::ToolTipRole);
      }
    }

    // Restore selection: current pick wins over the persisted one (a rescan
    // must not yank the user's live selection).
    const QString desired = previous.isEmpty() ? QSettings().value(kCloudSourceKey).toString() : previous;
    const int idx = ui_->comboCloud->findData(desired);
    if (idx >= 0) {
      ui_->comboCloud->setCurrentIndex(idx);
    }
  }

  const bool any_cloud = ui_->comboCloud->count() > 0;
  ui_->comboCloud->setPlaceholderText(any_cloud ? QString() : tr("(no cloud plugins)"));
  ui_->comboCloud->setEnabled(any_cloud);
  ui_->buttonCloudOpen->setEnabled(any_cloud);
}

}  // namespace PJ
