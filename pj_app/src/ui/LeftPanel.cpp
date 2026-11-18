// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/LeftPanel.h"

#include <QAction>
#include <QComboBox>
#include <QDomDocument>
#include <QDomElement>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
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
#include <algorithm>
#include <initializer_list>
#include <nlohmann/json.hpp>

#ifdef PJ_TARGET_WASM
#include "BrowserPersistence.h"
#endif
#include "pj_runtime/ToolboxTags.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/ScrubberBase.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_LeftPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
#ifndef PJ_TARGET_WASM
constexpr const char* kRecentFilesKey = "File/recent";
// Written by MainWindow::recordRecentLayout: must match MainWindow's kRecentLayoutsKey.
constexpr const char* kRecentLayoutsKey = "Layout/recent";
#endif
constexpr const char* kRecentIconCollapsed = ":/resources/svg/keyboard_arrow_right_light.svg";
constexpr const char* kRecentIconExpanded = ":/resources/svg/keyboard_arrow_down_light.svg";
// Persisted name: StreamingSourceManager reads the same key and BrowserPersistence allowlists it.
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";
// By name: an index would not survive plugin rediscovery reordering the combo.
constexpr const char* kStreamingSourceKey = "MainWindow.streamingSource";
// By plugin id, stable across sessions unlike the display name or a combo index.
constexpr const char* kCloudSourceKey = "MainWindow.cloudSource";
}  // namespace

LeftPanel::LeftPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::LeftPanel) {
  ui_->setupUi(this);

  applyIcons(currentTheme());

  // Rebuilt on every aboutToShow from what MainWindow stored (it dedupes, caps and orders the lists). Section
  // headers are disabled bold items, not addSection() titles: the PJMenu stylesheet styles QMenu::separator, which
  // puts Qt on the stylesheet separator path and drops addSection() text.
  auto* recent_menu = new QMenu(this);
  recent_menu->setObjectName(u"PJMenu"_s);
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
    // Prunes and persists missing files, so they do not come back.
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
  // Reload lives in the dataset context menu, which covers per-dataset reload; the button stays in the .ui, hidden.
  ui_->buttonReloadData->setVisible(false);

  ui_->buttonReloadData->setEnabled(false);
  ui_->buttonRecentFiles->setEnabled(false);

  // autoExclusive in the .ui keeps one tab checked; the stack is still driven by hand so the initial check routes.
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

  // QStackedWidget's sizeHint is the union of its pages', so a short page would reserve the tallest one's room:
  // only the current page keeps a vertical policy that counts.
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

  connect(ui_->buttonStreamingOptions, &QPushButton::clicked, this, &LeftPanel::streamingStartRequested);
  connect(ui_->buttonStreamingPause, &QPushButton::toggled, this, &LeftPanel::streamingPauseToggled);
  connect(
      ui_->buttonStreamingPause, &QPushButton::toggled, this, [this](bool) { applyPauseButtonState(currentTheme()); });
  connect(ui_->buttonStreamingRecord, &QPushButton::toggled, this, &LeftPanel::streamingRecordToggled);
  connect(ui_->buttonStreamingRecord, &QPushButton::toggled, this, [this](bool) {
    applyRecordButtonState(currentTheme());
  });
  // The label's width is known only after layout: re-elide whenever it moves.
  ui_->labelStreamingRecordStatus->installEventFilter(this);
  applyRecordingStatusMinimumWidth();
  // Only a user's pick reaches here: setStreamingSources() and restoreSourcesState() block the signal and emit
  // streamingSourceChanged themselves.
  connect(ui_->comboStreaming, &QComboBox::currentTextChanged, this, [this](const QString& source) {
    QSettings().setValue(kStreamingSourceKey, source);
    emit streamingSourceChanged(source);
  });

  // On editingFinished, not on every drag tick: the value matters only once the user settles.
  ui_->streamingSpinBox->setValue(QSettings().value(kStreamingBufferKey, 5).toInt());
  connect(ui_->streamingSpinBox, &ScrubberBase::editingFinished, this, [this]() {
    const int seconds = ui_->streamingSpinBox->value();
    QSettings().setValue(kStreamingBufferKey, seconds);
    emit streamingBufferChanged(seconds);
  });

  // Only a user's pick reaches here: populateCloudToolboxes blocks the signal.
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
    const QString desired = previous.isEmpty() ? QSettings().value(kStreamingSourceKey).toString() : previous;
    const int idx = ui_->comboStreaming->findText(desired);
    if (idx >= 0) {
      ui_->comboStreaming->setCurrentIndex(idx);
    }
  }
  // The blocker swallowed currentTextChanged (QComboBox auto-selects index 0 on the first addItems()): emit once so
  // StreamingSourceManager holds what the combo shows.
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

void LeftPanel::applyRecordButtonState(const QString& theme) {
  const bool recording = ui_->buttonStreamingRecord->isChecked();
  // The active glyph keeps its own red fill through the theme recolor, so recording reads the same in both themes.
  ui_->buttonStreamingRecord->setIcon(
      loadSvg(recording ? ":/resources/svg/record_active.svg" : ":/resources/svg/record.svg", theme));
  ui_->buttonStreamingRecord->setToolTip(
      recording ? tr("Stop recording")
                : tr("Record the subscribed topics of every active stream, one MCAP file per stream"));
}

void LeftPanel::setRecordingSupported(bool supported) {
  ui_->buttonStreamingRecord->setVisible(supported);
  if (!supported) {
    ui_->labelStreamingRecordStatus->hide();
  }
}

void LeftPanel::setRecordingActive(bool active) {
  const QSignalBlocker blocker(ui_->buttonStreamingRecord);  // reflecting state must not re-trigger start/stop
  ui_->buttonStreamingRecord->setChecked(active);
  ui_->labelStreamingRecordStatus->setVisible(active);
  if (!active) {
    recording_status_text_.clear();
    ui_->labelStreamingRecordStatus->clear();
  }
  applyRecordButtonState(currentTheme());
}

void LeftPanel::setRecordingStatusText(const QString& text) {
  recording_status_text_ = text;
  ui_->labelStreamingRecordStatus->setToolTip(text);
  elideRecordingStatus();
}

void LeftPanel::elideRecordingStatus() {
  // The label's horizontal policy is Ignored, so a longer text never resizes the row.
  QLabel* label = ui_->labelStreamingRecordStatus;
  const int available = label->contentsRect().width();
  label->setText(QFontMetrics(label->font()).elidedText(recording_status_text_, Qt::ElideRight, available));
}

void LeftPanel::applyRecordingStatusMinimumWidth() {
  QLabel* label = ui_->labelStreamingRecordStatus;
  const QMargins margins = label->contentsMargins();
  label->setMinimumWidth(
      QFontMetrics(label->font()).horizontalAdvance(QStringLiteral("00:00 | 000.0 kB")) + margins.left() +
      margins.right());
}

bool LeftPanel::eventFilter(QObject* watched, QEvent* event) {
  if (watched == ui_->labelStreamingRecordStatus) {
    switch (event->type()) {
      case QEvent::Resize:
        elideRecordingStatus();
        break;
      case QEvent::FontChange:
      case QEvent::StyleChange:
        applyRecordingStatusMinimumWidth();
        elideRecordingStatus();
        break;
      default:
        break;
    }
  }
  return QWidget::eventFilter(watched, event);
}

QDomElement LeftPanel::saveSourcesState(QDomDocument& doc, bool stream_bound) const {
  QDomElement element = doc.createElement(u"left_panel_state"_s);

  if (ui_->tabFile->isChecked()) {
    element.setAttribute(u"sources_tab"_s, u"file"_s);
  } else if (ui_->tabStream->isChecked()) {
    element.setAttribute(u"sources_tab"_s, u"stream"_s);
  } else if (ui_->tabCloud->isChecked()) {
    element.setAttribute(u"sources_tab"_s, u"cloud"_s);
  }

  element.setAttribute(u"streaming_source"_s, ui_->comboStreaming->currentText());
  if (stream_bound) {
    element.setAttribute(u"streaming_buffer"_s, QString::number(ui_->streamingSpinBox->value()));
  }
  return element;
}

void LeftPanel::restoreSourcesState(const QDomElement& element, const StreamingRestore& streams) {
  if (element.isNull() || element.tagName() != "left_panel_state"_L1) {
    return;
  }

  // Not blocked: the toggled handlers switch the visible page.
  if (element.hasAttribute(u"sources_tab"_s)) {
    const QString tab = element.attribute(u"sources_tab"_s);
    if (tab == "file"_L1) {
      ui_->tabFile->setChecked(true);
    } else if (tab == "stream"_L1) {
      ui_->tabStream->setChecked(true);
    } else if (tab == "cloud"_L1) {
      ui_->tabCloud->setChecked(true);
    }
  }

  // Blocked so QSettings is not written; the signals are emitted by hand because StreamingSourceManager learns the
  // selection only from them, and Start would otherwise launch the source picked before the restore. A source that is
  // not installed is skipped.
  if (element.hasAttribute(u"streaming_source"_s)) {
    const QString src = element.attribute(u"streaming_source"_s);
    const int idx = ui_->comboStreaming->findText(src);
    if (idx >= 0) {
      {
        const QSignalBlocker blocker(ui_->comboStreaming);
        ui_->comboStreaming->setCurrentIndex(idx);
      }
      emit streamingSourceChanged(ui_->comboStreaming->currentText());
    }
  }

  // setValue does not emit editingFinished, which is what writes QSettings.
  if (streams.stream_bound && element.hasAttribute(u"streaming_buffer"_s)) {
    bool ok = false;
    int seconds = element.attribute(u"streaming_buffer"_s).toInt(&ok);
    if (ok) {
      if (streams.live_retention_s.has_value()) {
        seconds = std::max(seconds, *streams.live_retention_s);
      }
      {
        const QSignalBlocker blocker(ui_->streamingSpinBox);
        ui_->streamingSpinBox->setValue(seconds);
      }
      emit streamingBufferChanged(ui_->streamingSpinBox->value());
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
  applyRecordButtonState(theme);

  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  // Includes the layout padding, so the inner margins grow the band instead of squeezing the buttons.
  const int band_extent = chrome_metrics_.bandHeight();
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
  // Replaces the .ui's fixed heights: the header band grows to band_extent, the row controls stay button-tall to line
  // up with the buttons beside them.
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
  // No left inset: labelInput's own padding-left leads the band, and both would double the leading the other
  // section bands have.
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
      // A null vtable is a load failure already reported through the diagnostic sink.
      const auto* vtable = tb.library.vtable();
      if (vtable == nullptr || vtable->manifest_json == nullptr) {
        continue;
      }
      auto manifest = nlohmann::json::parse(vtable->manifest_json, nullptr, /*allow_exceptions=*/false);
      if (!manifest.is_object()) {
        qWarning("LeftPanel: toolbox '%s' has an invalid manifest_json; skipping", tb.id.c_str());
        continue;
      }
      if (!manifestHasTag(manifest, kToolboxTagCloud)) {
        continue;
      }

      ui_->comboCloud->addItem(QString::fromStdString(tb.name), QString::fromStdString(tb.id));
      if (auto desc = manifest.find("description"); desc != manifest.end() && desc->is_string()) {
        ui_->comboCloud->setItemData(
            ui_->comboCloud->count() - 1, QString::fromStdString(desc->get<std::string>()), Qt::ToolTipRole);
      }
    }

    // The live pick wins over the persisted one: a rescan must not yank the user's selection.
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
