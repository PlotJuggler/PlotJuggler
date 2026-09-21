// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/marketplace_window.hpp"

#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <utility>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "pj_marketplace/version_compare.hpp"
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/ToggleSwitch.h"
#include "ui_marketplace_window.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Column order of the plugin table (see setupUi / rebuildTable).
enum Column {
  kColName = 0,
  kColCategory,            // the registry `category` value (data_loader, toolbox, …)
  kColInstalledVersion,    // the installed version, or "—" when not installed
  kColMarketplaceVersion,  // the registry version; highlighted when it's an update
  kColDescription,
  kColCount,
};

// Per-item string on the Category column carrying the row's plugin name, so
// CategoryItem::operator< can break ties alphabetically WITHIN a category
// (see the class comment). Stored via setData at row insertion time in
// rebuildTable().
constexpr int kCategoryNameKeyRole = Qt::UserRole + 2;

// Sort compound for the Category column: primary key = category text,
// secondary key = plugin name (case-insensitive). Grouping by category with an
// alphabetical run inside each group is the PJ3-era default sort Davide asked
// to restore. Applied uniformly for both ascending and descending — descending
// flips both keys (reverse-category, then reverse-name within), which keeps
// Category a single toggle rather than a stateful multi-dimension picker.
class CategoryItem : public QTableWidgetItem {
 public:
  using QTableWidgetItem::QTableWidgetItem;
  [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
    const int cat_cmp = QString::compare(text(), other.text(), Qt::CaseInsensitive);
    if (cat_cmp != 0) {
      return cat_cmp < 0;
    }
    return QString::compare(
               data(kCategoryNameKeyRole).toString(), other.data(kCategoryNameKeyRole).toString(),
               Qt::CaseInsensitive) < 0;
  }
};

// Sort item for the two version columns (Installed / Marketplace). Plain
// QTableWidgetItem sorts its display text lexicographically, which orders
// "10.0.0" before "9.0.0"; comparePluginVersions gives the package ordering
// the columns need. The em-dash placeholder for a not-installed row sorts below
// every valid version.
class VersionItem : public QTableWidgetItem {
 public:
  using QTableWidgetItem::QTableWidgetItem;
  [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
    return comparePluginVersions(semver(text()), semver(other.text())) < 0;
  }

 private:
  // Maps the em-dash placeholder to an empty invalid string; valid versions
  // outrank invalid recovery/display data.
  static std::string semver(const QString& display) {
    return display == u"—"_s ? std::string{} : display.toStdString();
  }
};

// Row-height floor: a comfortable minimum so rows read as clearly separated
// even when their text is short.
constexpr int kMinRowHeight = 44;

// Per-item fill QColor for the Marketplace-version cell highlight, or an invalid
// QColor for no highlight (set in rebuildTable, read by MarketplaceCellDelegate).
// The colour differs by reason — Destructive pink for an available update,
// Emphasis amber for an incompatible plugin.
constexpr int kHighlightColorRole = Qt::UserRole + 1;

// Paints the Marketplace-version cell in a per-item fill colour. A delegate is
// required because the app-wide QSS rule `QTableView::item { background-color: … }`
// unconditionally overrides any per-item setBackground(), so the highlight has to
// be drawn here (the delegate owns the cell's paint) rather than via item brushes.
class MarketplaceCellDelegate : public QStyledItemDelegate {
 public:
  MarketplaceCellDelegate(QColor fg, QObject* parent) : QStyledItemDelegate(parent), fg_(std::move(fg)) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const QColor fill = index.data(kHighlightColorRole).value<QColor>();
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    if (!fill.isValid() || selected) {
      QStyledItemDelegate::paint(painter, option, index);  // normal cells: let the QSS style paint
      return;
    }
    // Highlighted, unselected: fill the per-item background and draw the text
    // ourselves (calling the base would let the QSS repaint the cell backdrop
    // over our fill).
    painter->save();
    painter->fillRect(option.rect, fill);
    painter->setPen(fg_);
    const QRect text_rect = option.rect.adjusted(6, 0, -6, 0);
    // Respect the item's own alignment (e.g. the centered version columns);
    // fall back to left-aligned when the item sets none.
    const QVariant item_align = index.data(Qt::TextAlignmentRole);
    const int align = item_align.isValid() ? item_align.toInt() : (Qt::AlignLeft | Qt::AlignVCenter);
    painter->drawText(text_rect, align | Qt::TextWordWrap, index.data().toString());
    painter->restore();
  }

 private:
  QColor fg_;
};

bool installedStatesEqual(const QMap<QString, InstalledExtension>& lhs, const QMap<QString, InstalledExtension>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto it = lhs.cbegin(); it != lhs.cend(); ++it) {
    const auto rhs_it = rhs.find(it.key());
    if (rhs_it == rhs.cend()) {
      return false;
    }
    const InstalledExtension& a = it.value();
    const InstalledExtension& b = rhs_it.value();
    if (a.id != b.id || a.version != b.version || a.enabled != b.enabled || a.abi_major != b.abi_major ||
        a.min_sdk_required != b.min_sdk_required || a.min_plotjuggler_version != b.min_plotjuggler_version ||
        a.suggested_sdk_version != b.suggested_sdk_version) {
      return false;
    }
  }
  return true;
}

ExtensionManager::HostCompatibility displayedCompatibility(
    const ExtensionManager& manager, const QList<Extension>& registry_extensions, const Extension& extension) {
  const bool registry_owned = std::ranges::any_of(
      registry_extensions, [&](const Extension& candidate) { return candidate.id == extension.id; });
  if (!registry_owned) {
    const auto installed = manager.installedExtensions();
    const auto record = installed.constFind(extension.id);
    if (record != installed.cend()) {
      return manager.hostCompatibility(*record);
    }
  }
  return manager.hostCompatibility(extension);
}

}  // namespace

MarketplaceWindow::MarketplaceWindow(const QUrl& registry_url, QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  download_mgr_ = new DownloadManager(this);
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = new ExtensionManager(
      download_mgr_, PlatformUtils::extensionsDir(), PlatformUtils::pendingDir(), /*sink*/ {}, this);
  registry_url_ = registry_url;
  // applyPendingUninstalls/applyPendingInstalls already ran in ExtensionManager::initComponents().
  finishConstruction(nullptr);
}

MarketplaceWindow::MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  registry_url_ = registry_url;
  finishConstruction(nullptr);
}

MarketplaceWindow::MarketplaceWindow(
    ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
    QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  initial_snapshot_provided_ = true;
  registry_url_ = registry_url;
  finishConstruction(&installed);
}

void MarketplaceWindow::finishConstruction(const QMap<QString, InstalledExtension>* installed) {
  opened_at_ = QDateTime::currentDateTimeUtc();
  setupUi();
  setupSignals();
  if (installed != nullptr) {
    ext_mgr_->setInstalledExtensions(*installed);
  }
  updateDiagnosticsButton();
  showLatestDiagnostic();
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::~MarketplaceWindow() {
  // The manager keeps whatever confirmation was registered last; disarm ours so a
  // pending local install cannot put its question to a destroyed window. A no-op
  // once another window took the registration over.
  //
  // The guard is not defensive padding: at shutdown the manager is destroyed
  // first. It belongs to a service MainWindow owns as a member, and a member dies
  // before ~QWidget deletes the child widgets this window is one of — so nothing
  // is left to disarm, and reaching for it is a use-after-free.
  if (ext_mgr_) {
    ext_mgr_->clearReplaceConfirmation(this);
  }
  delete ui_;
}

// ─── UI Setup ────────────────────────────────────────────────────────────────

void MarketplaceWindow::setupUi() {
  // Canonical chrome: build the .ui onto a child body under PJ::Dialog's title
  // bar (its content area already owns a zero-margin layout).
  setDialogTitle(tr("PlotJuggler Marketplace"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
  content_widget_ = body;  // exposed via contentWidget() for embedding

  ui_->update_all_btn_->setFixedWidth(90);
  ui_->update_all_btn_->setEnabled(false);

  // Pin the toolbar-row pill height to the Install/Update button post-QSS
  // rendered size so the strip reads as one row. QPushButton grows via the QSS
  // `padding: ${space_comfortable}` which doesn't feed sizeHint on QStyleSheet
  // widgets (padding without a border), so the actual height is only known
  // after the first layout pass — deferred to the next event-loop tick.
  QMetaObject::invokeMethod(
      this,
      [this]() {
        const int actual_h = ui_->install_local_btn_->height();
        for (CheckButton* pill :
             {ui_->filter_data_loader_, ui_->filter_data_streamer_, ui_->filter_parser_, ui_->filter_toolbox_}) {
          pill->setFixedHeight(actual_h);
        }
      },
      Qt::QueuedConnection);

  // The marketplace doesn't link pj_app_core, so it can't pipe icons
  // through LoadSvg's recolor. Pick the theme-appropriate variant
  // directly from the resource bundle.
  const bool dark_theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString() !=
                          QStringLiteral("light");
  ui_->settings_btn_->setIcon(QIcon(
      dark_theme ? QStringLiteral(":/resources/svg/settings_cog_dark.svg")
                 : QStringLiteral(":/resources/svg/settings_cog_light.svg")));
  // The canonical Search provides the (self-retinting) magnifying glass and a
  // themed clear "x"; it sits on the toolbar surface, so use the standalone tone.
  ui_->search_edit_->setVariant(Search::Variant::kStandalone);

  // Scroll-area background comes from the central stylesheet
  // (#scroll_area_ rule binds it to ${dark_background}).

  connect(ui_->search_edit_, &Search::textChanged, this, &MarketplaceWindow::onSearchChanged);
  for (CheckButton* toggle :
       {ui_->filter_installed_, ui_->filter_data_loader_, ui_->filter_data_streamer_, ui_->filter_parser_,
        ui_->filter_toolbox_}) {
    connect(toggle, &CheckButton::toggled, this, &MarketplaceWindow::onFilterToggled);
  }
  connect(ui_->settings_btn_, &QPushButton::clicked, this, &MarketplaceWindow::pluginPreferencesRequested);
  connect(ui_->install_local_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onInstallLocalClicked);
  connect(ui_->update_all_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onUpdateAllClicked);
  connect(ui_->diagnostics_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onDiagnosticsClicked);

  // Plugin table: fixed columns (Name/Installed/Marketplace versions) sized to
  // their content, Description stretches to take the rest. Rows are read-only and
  // whole-row selectable; the Marketplace-version cell is highlighted when it is
  // a newer version than the installed one. All actions — install/update/
  // uninstall and enable/disable — live in the details footer, not in the cells.
  auto* table = ui_->plugin_table_;
  table->setColumnCount(kColCount);
  table->setHorizontalHeaderLabels({tr("Name"), tr("Category"), tr("Installed"), tr("Marketplace"), tr("Description")});
  table->verticalHeader()->setVisible(false);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  // Grid + alternating rows give clear visual separation between plugins; a
  // minimum row height keeps short-text rows from looking cramped.
  table->setShowGrid(true);
  table->setWordWrap(true);
  table->setAlternatingRowColors(true);
  // Sorting is on so the header click reorders rows. We restrict click-to-sort
  // to Name and Category — clicks on other columns are intercepted below and
  // reset back to the previous Name/Category sort. rebuildTable() toggles
  // sortingEnabled around the row insertion.
  table->setSortingEnabled(true);
  table->sortByColumn(kColCategory, Qt::AscendingOrder);
  table->verticalHeader()->setMinimumSectionSize(kMinRowHeight);
  auto* header = table->horizontalHeader();
  header->setSectionResizeMode(kColName, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColCategory, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColInstalledVersion, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColMarketplaceVersion, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColDescription, QHeaderView::Stretch);
  // Name, Category, Installed and Marketplace are sortable; a click on
  // Description (long free-form text) is intercepted and reverts to the last
  // valid sort state (default: Name ascending).
  connect(header, &QHeaderView::sortIndicatorChanged, this, [this, header](int section, Qt::SortOrder order) {
    if (section == kColName || section == kColCategory || section == kColInstalledVersion ||
        section == kColMarketplaceVersion) {
      last_sort_column_ = section;
      last_sort_order_ = order;
      return;
    }
    QSignalBlocker blocker(header);
    ui_->plugin_table_->sortByColumn(last_sort_column_, last_sort_order_);
  });

  // Delegate that lights up updatable rows in the "update" tone (the per-item
  // setBackground path is defeated by the app-wide QTableView::item QSS). The
  // The per-item fill colour is set in rebuildTable (Destructive pink for an
  // update, Emphasis amber for an incompatible plugin), painted at reduced
  // alpha as a subtle tint. Standard body ink reads cleanly on either.
  table->setItemDelegate(new MarketplaceCellDelegate(theme::text(theme::appTheme()), table));

  // Selecting a row updates the bottom "Details" footer with that plugin.
  connect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
    const QModelIndexList rows = ui_->plugin_table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
      return;
    }
    QTableWidgetItem* name = ui_->plugin_table_->item(rows.first().row(), kColName);
    footer_ext_id_ = (name != nullptr) ? name->data(Qt::UserRole).toString() : QString{};
    updateDetailFooter();
  });

  // Footer enable/disable toggle (the standard palette ToggleSwitch — Accent
  // "on" track). Hidden for non-installed plugins; acts on the plugin currently
  // shown in the footer; the change persists and takes effect on next restart.
  ui_->detail_enable_toggle_->setVisible(false);
  connect(ui_->detail_enable_toggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
    if (footer_ext_id_.isEmpty()) {
      return;
    }
    ext_mgr_->setEnabled(footer_ext_id_, checked);
    installations_changed_ = true;
    // Clear any sticky error first (e.g. a "Failed to load registry" from an
    // offline start): setStatus() suppresses a non-error message while an error
    // is latched, so without this the toggle's confirmation would never show and
    // the stale error would stay on screen. Every other user action clears it too.
    clearStickyStatus();
    setStatus(
        (checked ? tr("Extension %1 will be enabled after restart") : tr("Extension %1 will be disabled after restart"))
            .arg(footer_ext_id_));
    rebuildTable();  // keep the table's Enabled column in sync
  });

  // Floor + initial size: wide enough for the four fixed columns plus a roomy
  // Description, without the two-pane minimum the old master-detail view needed.
  setMinimumSize(900, 520);
  resize(1080, 600);

  // Canonical overlay pill scrollbars for the extension list / detail scroll
  // areas just built (their ranges update live as the registry loads).
  attachPillScrollbars(this);
}

// ─── Signal wiring ───────────────────────────────────────────────────────────

void MarketplaceWindow::setupSignals() {
  // A local archive whose id is already installed needs a decision, and the
  // manager cannot take it: this is UI policy. Replacement is staged, so the
  // wording promises "after restart" rather than an immediate swap. Registered
  // against `this` because the manager outlives this window and asks the question
  // asynchronously: a window closed mid-extraction must not be dialogged.
  ext_mgr_->setReplaceConfirmation(
      this, [this](const QString& id, const QString& installed_version, const QString& archive_version) {
        const QString text = (installed_version == archive_version)
                                 ? tr("\"%1\" is already installed at version %2, the same version the archive "
                                      "carries.\n\nReplace it? The replacement is applied the next time PlotJuggler "
                                      "starts.")
                                       .arg(id, installed_version)
                                 : tr("\"%1\" is already installed at version %2. The archive carries version "
                                      "%3.\n\nReplace it? The replacement is applied the next time PlotJuggler "
                                      "starts.")
                                       .arg(id, installed_version, archive_version);
        const int choice = MessageBox::question(
            this, tr("Replace installed extension?"), text,
            {{tr("Replace"), MessageBox::kPrimaryRole}, {tr("Cancel"), MessageBox::kCancelRole}});
        return choice == 0;
      });

  // RegistryManager
  connect(registry_mgr_, &RegistryManager::fetchStarted, this, [this]() { setInfoStatus("Loading registry..."); });

  connect(registry_mgr_, &RegistryManager::fetchFinished, this, [this](bool success) {
    if (!success) {
      setStatus("Failed to load registry", true);
      showInvalidRegistryDialog();
      return;
    }
    // A successful refresh is a strong "things are working" signal; let it
    // override any old sticky error so progress messages aren't suppressed.
    clearStickyStatus();
    registry_extensions_ = registry_mgr_->compatibleExtensions(PlatformUtils::currentPlatform());
    rebuildExtensionList();
    applyFilters();
    setInfoStatus("Ready — " + QString::number(extensions_.size()) + " extensions loaded");
  });

  connect(ext_mgr_, &ExtensionManager::installPendingRestart, this, [this](const QString& id) {
    // Staging finishes the active install just like installFinished does; clear
    // the busy marker so the card flips to "Needs Restart" (not a stuck
    // "Installing" badge) and processInstallQueue() can advance to the next item.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    installations_changed_ = true;
    local_install_path_.clear();
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    ++restart_pending_count_;
    refreshAfterInstalledChange();
    setStatus(QString("Extension %1 staged — will be active after restart").arg(id));
    processInstallQueue();
    maybeShowRestartRequiredDialog();
    // Staging is how every successful update ends, so this — not installFinished
    // — is the handler that closes a typical mixed batch. Must stay after
    // processInstallQueue(): a dispatched next item is what makes both reports
    // below correctly suppress themselves mid-batch.
    maybeShowBatchSummary();
  });

  connect(ext_mgr_, &ExtensionManager::installUnchanged, this, [this](const QString& id) {
    // A terminal outcome like installFinished, so the same bookkeeping closes the
    // operation — except installations_changed_, which stays as it was: nothing
    // reached the extensions dir, so there is nothing for the host to reload.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    ui_->progress_bar_->setVisible(false);
    local_install_path_.clear();
    status_error_sticky_ = false;
    refreshAfterInstalledChange();
    const QString version = ext_mgr_->installedVersion(id);
    setStatus(
        version.isEmpty()
            ? QString("%1 is already installed — the archive is identical, nothing changed").arg(id)
            : QString("%1 v%2 is already installed — the archive is identical, nothing changed").arg(id, version));
    processInstallQueue();
    maybeShowRestartRequiredDialog();
    maybeShowBatchSummary();
  });

  connect(ext_mgr_, &ExtensionManager::uninstallPendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    ++restart_pending_count_;
    refreshAfterInstalledChange();
    setStatus(QString("Extension %1 staged — will be uninstalled after restart").arg(id));
    maybeShowRestartRequiredDialog();
  });

  connect(ext_mgr_, &ExtensionManager::downgradePendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    ++restart_pending_count_;
    refreshAfterInstalledChange();
    setStatus(QString("Extension %1 will revert to its bundled version after restart").arg(id));
    maybeShowRestartRequiredDialog();
  });

  connect(registry_mgr_, &RegistryManager::fetchError, this, [this](const QString& error) {
    // Keep the concrete reason so the invalid-registry dialog (raised from
    // fetchFinished(false), which carries no message) can show which entry failed.
    last_registry_error_ = error;
    setStatus("Registry error: " + error, true);
  });

  // ExtensionManager
  connect(ext_mgr_, &ExtensionManager::installStarted, this, [this](const QString& id) {
    active_install_id_ = id;
    // A new install/update is starting, so a previous item's sticky error no
    // longer applies — clear it before showInstallProgress() so the live
    // "Installing…" status is not suppressed. Individual clicks clear the sticky
    // themselves, but an Update All batch dispatches each item straight from
    // processInstallQueue(), so without this a failed item would freeze the red
    // error over the next item's moving progress bar.
    clearStickyStatus();
    ui_->progress_bar_->setValue(0);
    ui_->progress_bar_->setRange(0, 100);
    ui_->progress_bar_->setVisible(true);
    showInstallProgress();
    rebuildTable();  // repaint so the active card shows the "Installing" badge
  });

  connect(ext_mgr_, &ExtensionManager::installProgress, this, [this](const QString& /*id*/, int percent) {
    ui_->progress_bar_->setValue(percent);
  });

  // Post-download phases (verifying, extracting) do not report byte-level
  // progress, so we flip the bar to indeterminate/busy mode and update the
  // status label with the current phase.
  connect(
      ext_mgr_, &ExtensionManager::installPhase, this, [this](const QString& /*id*/, DownloadManager::WorkPhase phase) {
        ui_->progress_bar_->setRange(0, 0);
        QString verb;
        switch (phase) {
          case DownloadManager::WorkPhase::Verifying:
            verb = u"Verifying"_s;
            break;
          case DownloadManager::WorkPhase::Extracting:
            verb = u"Extracting"_s;
            break;
        }
        showInstallProgress(verb);
      });

  connect(ext_mgr_, &ExtensionManager::installFinished, this, [this](const QString& id, bool success) {
    // Only clear the busy marker if this is the finish of the install we
    // actually started. A failure from a call that never reached
    // installStarted (rejected by an ExtensionManager guard, e.g.
    // unsupported platform) also emits installFinished with success=false;
    // in that case active_install_id_ still points at the install that IS
    // in flight and must stay set until it completes.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    ui_->progress_bar_->setVisible(false);
    if (success) {
      installations_changed_ = true;
    }
    refreshAfterInstalledChange();
    const bool was_sideload = !local_install_path_.isEmpty();
    local_install_path_.clear();
    if (success) {
      status_error_sticky_ = false;
      setStatus(installedStatusText(id, was_sideload));
    } else {
      // Count every failure of this run so the end-of-batch summary can report
      // it. installFinished(false) fires for both a genuine mid-download failure
      // and an op rejected before installStarted, which is exactly the set we
      // want to surface. On failure the status was already set by installError —
      // do not overwrite it here.
      ++batch_failed_count_;
    }
    processInstallQueue();
    // A batch whose LAST item finishes without staging (fresh install, or a
    // failed item) must still surface the dialog for the items that DID stage.
    maybeShowRestartRequiredDialog();
    // …and, once everything settles, a summary if anything failed, so the last
    // item's "Installed X" never hides a mid-batch failure.
    maybeShowBatchSummary();
  });

  connect(ext_mgr_, &ExtensionManager::installError, this, [this](const QString& /*id*/, const QString& error) {
    ui_->progress_bar_->setVisible(false);
    setStatus("Installation failed: " + error, true);
    // Queue advance lives in installFinished only — installError + installFinished both
    // fire from emitInstallFailure, so advancing here would double-pop the queue.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallFinished, this, [this](const QString& id, bool success) {
    if (success) {
      status_error_sticky_ = false;
      installations_changed_ = true;
      // Read the name BEFORE the repaint: uninstalling a local-only extension
      // drops its row from extensions_, leaving nothing to look the name up in.
      QString name = id;
      for (const auto& ext : extensions_) {
        if (ext.id == id) {
          name = ext.name;
          break;
        }
      }
      refreshAfterInstalledChange();
      setStatus("Uninstalled " + name);
    }
    // On failure the status was already set by uninstallError — do not overwrite it.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallError, this, [this](const QString& /*id*/, const QString& error) {
    setStatus("Uninstall failed: " + error, true);
  });

  connect(
      ext_mgr_, &ExtensionManager::diagnosticReported, this,
      [this](const QString& /*id*/, const QString& message, bool is_error) {
        updateDiagnosticsButton();
        // Seen live while the window is open — mark it surfaced so a later
        // re-open doesn't show it again as if it were new.
        ext_mgr_->markDiagnosticsSurfaced();
        if (is_error) {
          setStatus("Marketplace diagnostic: " + message, true);
        }
      });
}

// ─── Table Population ─────────────────────────────────────────

void MarketplaceWindow::rebuildTable(bool preserve_scroll) {
  auto* table = ui_->plugin_table_;
  const int saved_scroll = table->verticalScrollBar()->value();

  // Silence selection changes while tearing down + repopulating (setRowCount(0)
  // clears the selection); the deliberate selectRow() at the end fires exactly
  // one itemSelectionChanged that refreshes the footer. Also switch sorting off
  // while inserting rows — otherwise QTableWidget would resort after each row
  // (O(n²) inserts, and the intermediate order breaks the row-index bookkeeping
  // above).
  table->blockSignals(true);
  const bool sorting_was_enabled = table->isSortingEnabled();
  table->setSortingEnabled(false);
  table->setRowCount(0);

  // Marketplace-cell highlight fills (subtle tints over the cell surface): the
  // Destructive pink for an available update, the Emphasis amber for an
  // incompatible plugin — the same amber the footer's incompatibility notice
  // uses.
  const auto fw = theme::appTheme();
  QColor update_fill = theme::destructive(theme::Destructive::Nominal, fw);
  update_fill.setAlphaF(0.22);
  QColor incompatible_fill = theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, fw);
  incompatible_fill.setAlphaF(0.22);

  const auto installed = ext_mgr_->installedExtensions();
  for (const Extension& ext : filtered_) {
    const int row = table->rowCount();
    table->insertRow(row);

    const bool is_installed = installed.contains(ext.id);
    const bool has_update = ext_mgr_->hasUpdate(ext);
    const auto compat = displayedCompatibility(*ext_mgr_, registry_extensions_, ext);

    // Name, with the full description as tooltip. Plain QTableWidgetItem — the
    // Name column sorts by case-insensitive text.
    auto* name_item = new QTableWidgetItem(ext.name);
    name_item->setToolTip(ext.description);
    name_item->setData(Qt::UserRole, ext.id);
    table->setItem(row, kColName, name_item);

    // Category — the registry `category` value verbatim (data_loader,
    // data_stream, message_parser, toolbox). CategoryItem breaks category ties
    // alphabetically by the plugin name it stores in kCategoryNameKeyRole, so
    // the default sort reads as "grouped by category, alphabetical within".
    auto* category_item = new CategoryItem(ext.category);
    category_item->setToolTip(ext.category);
    category_item->setData(kCategoryNameKeyRole, ext.name);
    table->setItem(row, kColCategory, category_item);

    // Installed version (an em dash when not installed), centered. Asked of the
    // manager rather than read off the snapshot: a core plugin the startup seed
    // refreshed cannot be re-scanned to its new version within this process.
    auto* installed_item = new VersionItem(is_installed ? ext_mgr_->installedVersion(ext.id) : u"\u2014"_s);
    installed_item->setTextAlignment(Qt::AlignCenter);
    table->setItem(row, kColInstalledVersion, installed_item);

    // Marketplace (registry) version \u2014 the version an update would move to. Only
    // THIS cell is highlighted (MarketplaceCellDelegate paints it), not the whole
    // row. Incompatible wins over update: an incompatible plugin's cell is
    // amber (matching the footer notice) even when it also has an update;
    // otherwise an available update paints it pink.
    auto* market_item = new VersionItem(ext.version);
    market_item->setTextAlignment(Qt::AlignCenter);
    if (!compat.ok) {
      market_item->setData(kHighlightColorRole, incompatible_fill);
      market_item->setToolTip(compat.reason);
    } else if (has_update) {
      market_item->setData(kHighlightColorRole, update_fill);
      market_item->setToolTip(tr("Update available: v%1").arg(ext.version));
    }
    // Reduced-features note (compatible, but the full-feature floor exceeds
    // this build's SDK): tooltip only — an info fact, below the amber tier,
    // not worth a tint of its own.
    if (compat.ok && !compat.completeness_note.isEmpty()) {
      const QString existing = market_item->toolTip();
      market_item->setToolTip(
          existing.isEmpty() ? compat.completeness_note : existing + u"\n"_s + compat.completeness_note);
    }
    table->setItem(row, kColMarketplaceVersion, market_item);

    // Description: full text, wraps within the stretched column. No tooltip —
    // the cell already renders the whole string, so a tooltip repeating it
    // adds nothing and covers the row on hover.
    auto* desc_item = new QTableWidgetItem(ext.description);
    table->setItem(row, kColDescription, desc_item);
  }

  // Re-enable sorting AFTER all rows are populated and sort by the last
  // user-chosen Name/Category column so the visible order is deterministic
  // across rebuilds. resizeRowsToContents follows so heights match the sorted
  // rows.
  if (sorting_was_enabled) {
    table->setSortingEnabled(true);
    table->sortByColumn(last_sort_column_, last_sort_order_);
  }

  table->resizeRowsToContents();
  // resizeRowsToContents sizes to text; lift any row below the readability floor.
  for (int row = 0; row < table->rowCount(); ++row) {
    if (table->rowHeight(row) < kMinRowHeight) {
      table->setRowHeight(row, kMinRowHeight);
    }
  }

  table->blockSignals(false);

  // Restore the selection by ext id (preserved across rebuilds) so the details
  // footer stays on the same plugin through install/update repaints; fall back
  // to the first row. selectRow() fires itemSelectionChanged → updateDetailFooter().
  if (table->rowCount() > 0) {
    int target = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
      if (table->item(row, kColName)->data(Qt::UserRole).toString() == footer_ext_id_) {
        target = row;
        break;
      }
    }
    table->selectRow(target);
  } else {
    footer_ext_id_.clear();
    updateDetailFooter();
  }

  // Enable "Update All" if ANY loaded extension has an update — not just the
  // filtered subset shown — so the button's reach matches its action. A staged
  // update keeps its old installed version until restart (hasUpdate stays true),
  // so exclude already-pending extensions to avoid re-staging what is queued.
  // Incompatible updates are excluded: their new version needs a newer host, so
  // install() would reject them — Update All must not appear to offer what it
  // can't do (it disables entirely when every pending update is incompatible).
  bool any_updatable = false;
  for (const Extension& ext : extensions_) {
    if (ext_mgr_->hasUpdate(ext) && ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->hasPendingInstall(ext.id) &&
        !ext_mgr_->hasPendingUninstall(ext.id)) {
      any_updatable = true;
      break;
    }
  }
  // Stay disabled while anything is in flight, not just while the queue is
  // non-empty: processInstallQueue() pops the last item before its install
  // finishes, so update_queue_ empties while active_install_id_ is still
  // downloading (not yet staged, so it still counts as updatable). Without the
  // active_install_id_ check the button would re-enable mid-batch and a click
  // would re-dispatch the in-flight update.
  ui_->update_all_btn_->setEnabled(any_updatable && update_queue_.isEmpty() && active_install_id_.isEmpty());

  if (preserve_scroll) {
    QTimer::singleShot(
        0, this, [this, saved_scroll]() { ui_->plugin_table_->verticalScrollBar()->setValue(saved_scroll); });
  }
}

// ─── Details footer ───────────────────────────────────────────

void MarketplaceWindow::updateDetailFooter() {
  // Tear down the previous action row (widgets + the leading stretch).
  QLayoutItem* old_item = nullptr;
  while ((old_item = ui_->detail_buttons_layout->takeAt(0)) != nullptr) {
    if (QWidget* old_widget = old_item->widget()) {
      // deleteLater (not delete) because this rebuild can be triggered from a
      // footer button's own clicked() slot (e.g. Downgrade) — destroying the
      // signal's emitter mid-emission would crash. But takeAt only unmanages the
      // widget from the layout; it stays a visible child at its old geometry
      // until the deferred delete fires, and a nested event loop (the "Restart
      // required" modal that a downgrade opens) leaves it painted underneath the
      // freshly-added badge — the two overlap. hide() it now so it disappears
      // immediately while its destruction stays safely deferred.
      old_widget->hide();
      old_widget->deleteLater();
    }
    delete old_item;
  }

  const Extension* ext = nullptr;
  for (const Extension& e : extensions_) {
    if (e.id == footer_ext_id_) {
      ext = &e;
      break;
    }
  }
  if (ext == nullptr) {
    ui_->detail_text_->clear();
    ui_->detail_title_->clear();
    ui_->detail_enable_toggle_->setVisible(false);
    return;
  }

  const auto installed = ext_mgr_->installedExtensions();
  // Empty when not installed; see installedVersion for why the
  // snapshot's own version can lag a seed-refreshed core plugin.
  const QString installed_version = ext_mgr_->installedVersion(ext->id);
  const auto esc = [](const QString& s) { return s.toHtmlEscaped(); };

  // Title (plugin name) — a real header-row label so the enable toggle can sit
  // at its height. The green toggle is shown only for installed plugins.
  ui_->detail_title_->setText(ext->name);
  {
    const bool title_installed = installed.contains(ext->id);
    ui_->detail_enable_toggle_->setVisible(title_installed);
    QSignalBlocker block(ui_->detail_enable_toggle_);
    ui_->detail_enable_toggle_->setChecked(title_installed && ext_mgr_->isEnabled(ext->id), /*animate=*/false);
  }

  QString html;

  // When the selected plugin is incompatible with this host, lead the footer with
  // a prominent notice carrying the reason, in the palette's Emphasis (amber)
  // tone — the same amber the Marketplace-version cell uses. Emphasis is a fill
  // family (no legible text ink), so tint the notice's background rather than its
  // text and keep the standard body ink on top. When the plugin is ALSO outdated
  // (an installed version with a newer — but incompatible — registry version), the
  // notice states both facts: the update exists but is blocked, and why (R6).
  if (const auto compat = displayedCompatibility(*ext_mgr_, registry_extensions_, *ext); !compat.ok) {
    QColor tint = theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, theme::appTheme());
    tint.setAlphaF(0.30);
    const QString message =
        ext_mgr_->hasUpdate(*ext)
            ? tr("⚠ Update to v%1 available, but blocked — %2").arg(esc(ext->version), esc(compat.reason))
            : tr("⚠ Incompatible — %1").arg(esc(compat.reason));
    html +=
        u"<p style='margin:0 0 6px 0; padding:3px 6px; font-weight:700; "
        u"background-color:rgba(%1,%2,%3,%4);'>%5</p>"_s.arg(tint.red())
            .arg(tint.green())
            .arg(tint.blue())
            .arg(tint.alphaF())
            .arg(message);
  } else if (!compat.completeness_note.isEmpty()) {
    // Info tier, deliberately quieter than the amber block above: the plugin
    // runs here, it just has optional features waiting on a newer build.
    html += u"<p style='margin:0 0 6px 0; padding:3px 6px;'>ℹ %1</p>"_s.arg(esc(compat.completeness_note));
  }

  // Metadata line (publisher/author • category • license • requires PJ •
  // installed).
  QStringList meta;
  if (!ext->publisher.isEmpty()) {
    meta << esc(ext->publisher);
  } else if (!ext->author.isEmpty()) {
    meta << esc(ext->author);
  }
  if (!ext->category.isEmpty()) {
    meta << esc(ext->category);
  }
  if (!ext->license.isEmpty()) {
    meta << esc(ext->license);
  }
  if (!ext->min_plotjuggler_version.isEmpty()) {
    meta << u"requires PJ %1+"_s.arg(esc(ext->min_plotjuggler_version));
  }
  if (!installed_version.isEmpty()) {
    meta << u"installed: v%1"_s.arg(esc(installed_version));
  }
  if (!meta.isEmpty()) {
    html += u"<p style='margin:0 0 6px 0;'>%1</p>"_s.arg(meta.join(u"  •  "_s));
  }

  // Description.
  if (!ext->description.isEmpty()) {
    html += u"<p style='margin:0 0 6px 0;'>%1</p>"_s.arg(esc(ext->description));
  }

  // Changelog: newest version first, matching the marketplace spec's examples.
  // The map is keyed by version string, so its natural order is lexicographic
  // ("1.10.0" before "1.9.0"); sort the keys by semver instead.
  if (!ext->changelog.isEmpty()) {
    QStringList versions = ext->changelog.keys();
    std::sort(versions.begin(), versions.end(), [](const QString& a, const QString& b) {
      return comparePluginVersions(a.toStdString(), b.toStdString()) > 0;
    });
    html += u"<p style='margin:0 0 2px 0;'><b>Changelog</b></p><ul style='margin:0 0 0 -20px;'>"_s;
    for (const QString& version : versions) {
      html += u"<li><b>%1</b> — %2</li>"_s.arg(esc(version), esc(ext->changelog.value(version)));
    }
    html += u"</ul>"_s;
  }

  ui_->detail_text_->setHtml(html);

  // ── Action row (same order as the old detail panel): primary action ·
  //    Uninstall/Downgrade · stretch · Visit Website. ──
  const QString ext_id = ext->id;
  const bool is_installed = installed.contains(ext_id);
  const bool has_update = ext_mgr_->hasUpdate(*ext);
  const bool has_newer_local = ext_mgr_->hasNewerInstalledVersion(*ext);
  const bool needs_restart = ext_mgr_->hasPendingInstall(ext_id) || ext_mgr_->hasPendingUninstall(ext_id);
  const bool in_update_queue =
      std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext_id; });
  const bool installing = ext_id == active_install_id_ || pending_clicks_.contains(ext_id) || in_update_queue;
  const bool is_bundled = ext_mgr_->isBundled(ext_id);

  // Leading stretch pushes the whole cluster to the right edge.
  ui_->detail_buttons_layout->addStretch();

  // Primary action / status: colour-coded per state via the #extButton*/#extBadge*
  // QSS rules. Uninstall/Downgrade/Visit Website below stay standard buttons.
  // Every footer button carries mpFooterButton so the stylesheet gives them one
  // shared geometry: a min-width floor plus the snug vertical padding that sets
  // their height. Pinned in QSS rather than via setFixedWidth because these are
  // QSS-styled buttons, and QStyleSheetStyle governs their geometry — a
  // widget-level fixed width is ignored. min-width is a floor, not a clamp, so a
  // long label (Downgrade names the version) still grows to its natural width
  // while keeping the same height as its neighbours.
  const auto mark_footer_button = [](QPushButton* button) { button->setProperty("mpFooterButton", true); };
  auto* action = new QPushButton;
  mark_footer_button(action);
  if (installing) {
    action->setText(tr("Installing"));
    action->setObjectName("extBadgeInstalling");
    action->setEnabled(false);
  } else if (needs_restart) {
    action->setText(tr("Needs Restart"));
    action->setObjectName("extBadgeNeedsRestart");
    action->setEnabled(false);
  } else if (has_update) {
    action->setText(tr("Update"));
    action->setObjectName("extButtonUpdate");
    connect(action, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
  } else if (has_newer_local) {
    action->setText(tr("Local newer"));
    action->setObjectName("extBadgeLocalNewer");
    action->setEnabled(false);
  } else if (is_installed) {
    action->setText(tr("Installed"));
    action->setObjectName("extBadgeInstalled");
    action->setEnabled(false);
  } else {
    action->setText(tr("Install"));
    action->setObjectName("extButtonInstall");
    connect(action, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
  }
  // An incompatible plugin can't be installed/updated: disable the actionable
  // primary button (Install/Update — the state badges are already disabled) and
  // surface the reason on hover. install() would refuse it anyway; this stops the
  // click before it starts.
  if (action->isEnabled()) {
    if (const auto compat = ext_mgr_->hostCompatibility(*ext); !compat.ok) {
      action->setEnabled(false);
      action->setToolTip(compat.reason);
    }
  }
  ui_->detail_buttons_layout->addWidget(action);

  // Uninstall / Downgrade-to-bundled (installed, not mid-operation), with the
  // same bundled logic the old detail dialog used. Standard button style too.
  if (is_installed && !installing && !needs_restart) {
    const QString bundled_version = ext_mgr_->bundledVersion(ext_id);
    const int installed_vs_bundled =
        is_bundled ? comparePluginVersions(installed_version.toStdString(), bundled_version.toStdString()) : 0;
    if (is_bundled && installed_vs_bundled <= 0) {
      // Core plugin at its bundled version: shown but locked.
      auto* uninstall = new QPushButton(tr("Uninstall"));
      mark_footer_button(uninstall);
      uninstall->setEnabled(false);
      uninstall->setToolTip(tr("This extension ships with the application and cannot be uninstalled"));
      ui_->detail_buttons_layout->addWidget(uninstall);
    } else if (is_bundled) {
      // Core plugin updated above bundled: offer revert-to-bundled.
      auto* downgrade = new QPushButton(tr("Downgrade to bundled v%1").arg(bundled_version));
      mark_footer_button(downgrade);
      downgrade->setToolTip(tr("Reverts to the bundled version v%1 on the next launch").arg(bundled_version));
      connect(downgrade, &QPushButton::clicked, this, [this, ext_id]() {
        clearStickyStatus();
        ext_mgr_->downgradeToBundled(ext_id);
      });
      ui_->detail_buttons_layout->addWidget(downgrade);
    } else {
      auto* uninstall = new QPushButton(tr("Uninstall"));
      mark_footer_button(uninstall);
      connect(uninstall, &QPushButton::clicked, this, [this, ext_id]() { onUninstallButtonClicked(ext_id); });
      ui_->detail_buttons_layout->addWidget(uninstall);
    }
  }

  // Visit Website / repository (right edge).
  const QString url = !ext->website.isEmpty() ? ext->website : ext->repository;
  if (!url.isEmpty()) {
    auto* web = new QPushButton(tr("Visit Website"));
    mark_footer_button(web);
    connect(web, &QPushButton::clicked, this, [url]() { QDesktopServices::openUrl(QUrl(url)); });
    ui_->detail_buttons_layout->addWidget(web);
  }
}

// ─── Filtering ────────────────────────────────────────────────────────────────

bool MarketplaceWindow::rebuildExtensionList() {
  QStringList previous_ids;
  previous_ids.reserve(extensions_.size());
  for (const Extension& ext : extensions_) {
    previous_ids << ext.id;
  }

  extensions_ = registry_extensions_;

  // An installed id the registry does not list has no row to reuse, so build one
  // from what the plugin declares about itself. The registry-sourced fields
  // (author, license, website, changelog, platforms…) stay empty; every consumer
  // guards them with isEmpty(), and the footer's primary action is disabled for an
  // installed extension anyway, so no install path can be driven off this row.
  const auto installed = ext_mgr_->installedExtensions();
  for (auto it = installed.cbegin(); it != installed.cend(); ++it) {
    if (std::any_of(registry_extensions_.cbegin(), registry_extensions_.cend(), [&](const Extension& ext) {
          return ext.id == it.key();
        })) {
      continue;
    }
    const InstalledExtension& record = it.value();
    Extension local;
    local.id = record.id;
    local.name = record.name;
    local.description = record.description;
    local.category = record.category;
    // Use installedVersion(), not record.version: the scan snapshot can be stale
    // when the seed refreshed a bundled plugin to a newer version than the scan
    // saw (installedVersion() reports what the seed wrote). Reading record.version
    // here makes hasNewerInstalledVersion() compare the seeded version against the
    // stale one and render a bogus "Local newer" badge plus an outdated version.
    local.version = ext_mgr_->installedVersion(record.id);
    local.min_plotjuggler_version = record.min_plotjuggler_version;
    extensions_.append(local);
  }

  // An extension whose uninstall is staged has already left installedExtensions()
  // but is still on disk and still running, so it needs a row to carry its "Needs
  // Restart" state. A registry-listed one already has one; a local-only one would
  // vanish from the table with nothing to say the removal is still pending.
  const auto staged_uninstalls = ext_mgr_->stagedUninstalls();
  for (auto it = staged_uninstalls.cbegin(); it != staged_uninstalls.cend(); ++it) {
    if (std::any_of(
            extensions_.cbegin(), extensions_.cend(), [&](const Extension& ext) { return ext.id == it.key(); })) {
      continue;
    }
    const InstalledExtension& record = it.value();
    Extension staged;
    staged.id = record.id;
    staged.name = record.name;
    staged.description = record.description;
    staged.category = record.category;
    staged.version = record.version;
    staged.min_plotjuggler_version = record.min_plotjuggler_version;
    extensions_.append(staged);
  }

  QStringList current_ids;
  current_ids.reserve(extensions_.size());
  for (const Extension& ext : extensions_) {
    current_ids << ext.id;
  }
  return current_ids != previous_ids;
}

void MarketplaceWindow::refreshAfterInstalledChange() {
  if (rebuildExtensionList()) {
    applyFilters();
  } else {
    rebuildTable();
  }
}

void MarketplaceWindow::applyFilters() {
  // Trim before matching: leading/trailing whitespace is not meaningful in a
  // search term, and an untrimmed space makes contains() miss every plugin
  // whose name/description doesn't embed that exact space (a stray space →
  // empty list).
  const QString search = ui_->search_edit_->text().trimmed().toLower();

  // Category values are the strings the published registry actually ships in each
  // extension's "category" field, NOT the button labels and NOT the vocabulary in
  // the marketplace spec doc (§5.2 lists data_streamer/parser/bundle; the live
  // registry uses data_stream/message_parser and ships no bundles). A value that
  // does not appear in the registry silently matches nothing, so these must be
  // checked against real registry data rather than the spec.
  QStringList active_categories;
  if (ui_->filter_data_loader_->isChecked()) {
    active_categories << u"data_loader"_s;
  }
  if (ui_->filter_data_streamer_->isChecked()) {
    active_categories << u"data_stream"_s;
  }
  if (ui_->filter_parser_->isChecked()) {
    active_categories << u"message_parser"_s;
  }
  if (ui_->filter_toolbox_->isChecked()) {
    active_categories << u"toolbox"_s;
  }
  const bool installed_only = ui_->filter_installed_->isChecked();

  // The categories the four toggles can represent. A plugin whose category is
  // one of these obeys its toggle; a plugin whose category falls outside this
  // set (empty, or an unmodeled/future value) has no toggle to govern it.
  static const QStringList kToggleableCategories = {
      u"data_loader"_s, u"data_stream"_s, u"message_parser"_s, u"toolbox"_s};

  filtered_.clear();
  for (const auto& ext : extensions_) {
    // Category checkboxes are additive: a toggleable category is shown only
    // while its toggle is checked (all four checked = every such category
    // visible; all four unchecked = none). A category outside the toggleable
    // set is never hidden here — it belongs to no toggle, so filtering it out
    // would make it permanently unreachable (a registry plugin with no category
    // could never be found to install, an installed one never uninstalled).
    if (kToggleableCategories.contains(ext.category) && !active_categories.contains(ext.category)) {
      continue;
    }
    if (installed_only && !ext_mgr_->isInstalled(ext.id)) {
      continue;
    }
    // Version incompatibility does NOT hide a row: the entry stays listed with its
    // reason, and the install action is disabled with that reason on hover. Platform
    // is a separate axis and is already excluded upstream — the registry list comes
    // from compatibleExtensions(currentPlatform()), and rows synthesized for installed
    // extensions carry no platforms map, so nothing here can be foreign-platform.
    if (!search.isEmpty()) {
      bool match = ext.name.toLower().contains(search) || ext.description.toLower().contains(search);
      if (!match) {
        for (const auto& tag : ext.tags) {
          if (tag.toLower().contains(search)) {
            match = true;
            break;
          }
        }
      }
      if (!match) {
        continue;
      }
    }
    filtered_.append(ext);
  }

  rebuildTable(/*preserve_scroll=*/false);
  setInfoStatus(QString::number(filtered_.size()) + " of " + QString::number(extensions_.size()) + " extensions shown");
}

void MarketplaceWindow::setStatus(const QString& msg, bool is_error) {
  if (!is_error && status_error_sticky_) {
    return;
  }
  status_error_sticky_ = is_error;
  ui_->status_label_->setText(msg);
  // The error tone is keyed off objectName via the
  // QLabel#marketplaceStatusError rule in resources/stylesheet_*.qss.
  // Clearing the objectName restores the inherited default text style.
  ui_->status_label_->setObjectName(is_error ? u"marketplaceStatusError"_s : QString{});
  ui_->status_label_->style()->unpolish(ui_->status_label_);
  ui_->status_label_->style()->polish(ui_->status_label_);
}

void MarketplaceWindow::clearStickyStatus() {
  status_error_sticky_ = false;
}

QString MarketplaceWindow::queueSuffix() const {
  const int queued = queuedCount();
  QString suffix;
  if (queued > 0) {
    suffix += u"  ·  "_s + QString::number(queued) + u" queued"_s;
  }
  // Surface failures live too, so a mid-batch failure is visible while the rest
  // of the queue is still running, not only in the end-of-batch summary.
  if (batch_failed_count_ > 0) {
    suffix += u"  ·  "_s + QString::number(batch_failed_count_) + u" failed"_s;
  }
  return suffix;
}

void MarketplaceWindow::setInfoStatus(const QString& msg) {
  if (isInstallBusy()) {
    // An install owns the status line; keep it showing the live progress rather
    // than letting a filter/refresh/registry-load message desync it from the
    // still-moving progress bar.
    showInstallProgress();
    return;
  }
  setStatus(msg);
}

void MarketplaceWindow::showInstallProgress(const QString& verb) {
  if (!isInstallBusy()) {
    return;
  }
  QString name;
  if (active_install_id_.isEmpty()) {
    name = QFileInfo(local_install_path_).fileName();  // no id until the manifest is read
  } else {
    name = active_install_id_;
    for (const auto& ext : extensions_) {
      if (ext.id == active_install_id_) {
        name = ext.name;
        break;
      }
    }
  }
  setStatus(verb + u" "_s + name + u"…"_s + queueSuffix());
}

void MarketplaceWindow::showLatestDiagnostic() {
  // Surface a diagnostic on window open ONLY if one arrived that hasn't been
  // shown yet (e.g. a staged-promotion failure that happened at startup while
  // no window was open). Re-showing the latest unconditionally on every re-open
  // resurrected a stale error the user had already moved past.
  if (!ext_mgr_->hasUnsurfacedDiagnostics()) {
    return;
  }
  const ExtensionDiagnostic& diagnostic = ext_mgr_->diagnostics().back();
  ext_mgr_->markDiagnosticsSurfaced();
  setStatus("Marketplace diagnostic: " + diagnostic.message, diagnostic.is_error);
}

void MarketplaceWindow::updateDiagnosticsButton() {
  // Only what happened since this panel opened. The manager keeps diagnostics for
  // the life of the application, so counting all of them made a reopened panel
  // announce failures the user had already dealt with — the same staleness
  // showLatestDiagnostic() guards against for the status line.
  const QList<ExtensionDiagnostic> diagnostics = ext_mgr_->diagnostics();
  const int count = static_cast<int>(std::count_if(
      diagnostics.cbegin(), diagnostics.cend(),
      [this](const ExtensionDiagnostic& diagnostic) { return diagnostic.timestamp >= opened_at_; }));
  ui_->diagnostics_btn_->setVisible(count > 0);
  ui_->diagnostics_btn_->setText(count > 1 ? QString("Details (%1)").arg(count) : "Details");
}

// ─── Slots ────────────────────────────────────────────────────────────────────

void MarketplaceWindow::onSearchChanged(const QString& /*text*/) {
  // Filtering is a user action like Refresh/Install: clear a sticky error so the
  // "K of N shown" count applyFilters() emits isn't suppressed by setStatus().
  clearStickyStatus();
  applyFilters();
}
void MarketplaceWindow::onFilterToggled() {
  clearStickyStatus();
  applyFilters();
}

void MarketplaceWindow::onInstallLocalClicked() {
  const QString path =
      FileDialog::getOpenFileName(this, tr("Install plugin from local ZIP"), QString(), tr("Plugin package (*.zip)"));
  if (path.isEmpty()) {
    return;  // cancelled
  }
  clearStickyStatus();
  // Canonical form, so the same archive picked through a relative path or a
  // symlink counts as one file for the dedupe below.
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  const QString zip_path = QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
  // If anything is already installing, queue this sideload behind it instead of
  // dispatching straight into ExtensionManager (which would reject it with
  // "already in progress"). processInstallQueue() drains it when the manager
  // frees up. Mirrors how a card click is queued via pending_clicks_.
  if (isInstallBusy()) {
    // Picking a file that is already running or already waiting is treated as
    // picking nothing at all: silently dropped, never queued behind itself.
    // Installing the same archive twice is a REPLACE of the id the first pass
    // just installed, so it would stage the plugin and demand a restart for no
    // gain.
    if (zip_path == local_install_path_ || pending_local_zips_.contains(zip_path)) {
      return;
    }
    pending_local_zips_.append(zip_path);
    showInstallProgress();  // keep the active op visible; reflect the new queue depth
    rebuildTable();
    return;
  }
  startLocalInstall(zip_path);
}

void MarketplaceWindow::startLocalInstall(QString zip_path) {
  local_install_path_ = zip_path;
  showInstallProgress();  // installStarted only arrives after the manifest read
  ext_mgr_->installFromLocalZip(zip_path);
}

QString MarketplaceWindow::installedStatusText(const QString& id, bool from_file) const {
  // A sideloaded id is by definition absent from the registry list, so its name and
  // version have to come from the installed snapshot instead.
  //
  // No restart is promised here: this runs on installFinished, which a sideload
  // only reaches on the fresh-install branch — the one that writes straight to the
  // extensions dir and is picked up by the host's catalog reload, exactly like a
  // fresh registry install. Replacing an installed id is staged instead and
  // reports through installPendingRestart, which owns the restart wording; an
  // archive identical to the installed tree installs nothing and reports through
  // installUnchanged, which owns its own wording for the same reason.
  if (from_file) {
    const QString version = ext_mgr_->installedVersion(id);
    return version.isEmpty() ? QString("Installed %1 from file").arg(id)
                             : QString("Installed %1 v%2 from file").arg(id, version);
  }
  for (const auto& ext : extensions_) {
    if (ext.id == id) {
      return "Installed " + ext.name + " v" + ext.version;
    }
  }
  return "Installed " + id;
}

void MarketplaceWindow::showEvent(QShowEvent* event) {
  activateEmbedded();
  Dialog::showEvent(event);
}

void MarketplaceWindow::activateEmbedded() {
  if (ext_mgr_ == nullptr) {
    return;
  }
  // Re-stamped here rather than only at construction: the host keeps this
  // controller alive across a close, so reopening the panel runs through here
  // and not through the constructor. Stamped before the refresh below, whose
  // own diagnostics belong to this activation.
  opened_at_ = QDateTime::currentDateTimeUtc();

  bool state_changed = false;
  if (initial_snapshot_provided_) {
    initial_snapshot_provided_ = false;
    state_changed = true;
  } else {
    const auto before = ext_mgr_->installedExtensions();
    ext_mgr_->refreshInstalledFromDisk();
    if (!installedStatesEqual(ext_mgr_->installedExtensions(), before)) {
      installations_changed_ = true;
      state_changed = true;
    }
  }
  // Composing is unconditional even though repainting is not: this is the only
  // entry point guaranteed to run, so a session whose registry never loads would
  // otherwise never build the local-only rows and would show an empty list.
  if (rebuildExtensionList()) {
    applyFilters();
  } else if (state_changed) {
    rebuildTable();
  }
  updateDiagnosticsButton();
  showLatestDiagnostic();
}

void MarketplaceWindow::setRegistryUrl(const QUrl& registry_url) {
  if (registry_url == registry_url_) {
    return;
  }
  registry_url_ = registry_url;
  // Pointing at another registry is a user action, so clear a sticky error from
  // the previous one — otherwise setStatus() would swallow this fetch's own
  // progress and the list would appear to change under a stale failure message.
  clearStickyStatus();
  registry_mgr_->fetchRegistry(registry_url_);
}

void MarketplaceWindow::onActionButtonClicked(const QString& ext_id) {
  // If another install/update is already in flight — a registry op OR a local
  // sideload — queue this click and let processInstallQueue() dispatch it when
  // the current one completes. Otherwise ExtensionManager::install() would
  // reject with "Install of X is already in progress" — its single-install-at-
  // a-time model is intentional, we just hide it behind a queue at the UI layer.
  if (isInstallBusy()) {
    const bool in_update_queue =
        std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext_id; });
    if (ext_id == active_install_id_ || pending_clicks_.contains(ext_id) || in_update_queue) {
      return;  // deduplicate — running, queued by a click, or already in the Update All batch
    }
    pending_clicks_.append(ext_id);
    showInstallProgress();  // keep the active install visible; reflect the new queue depth
    rebuildTable();         // repaint so the queued card shows the "Installing" badge
    return;
  }

  // Resolve against extensions_ (all loaded), not filtered_: a queued click
  // dispatched by processInstallQueue() must still be found even if the user
  // changed the search/category filter and it is no longer in the visible set.
  for (const auto& ext : extensions_) {
    if (ext.id != ext_id) {
      continue;
    }
    clearStickyStatus();
    if (ext_mgr_->hasUpdate(ext)) {
      ext_mgr_->update(ext);
    } else if (ext_mgr_->hasNewerInstalledVersion(ext)) {
      setStatus("Installed version is newer than registry version", true);
    } else if (!ext_mgr_->isInstalled(ext.id)) {
      ext_mgr_->install(ext);
    }
    return;
  }
}

void MarketplaceWindow::onUninstallButtonClicked(const QString& ext_id) {
  clearStickyStatus();
  ext_mgr_->uninstall(ext_id);
}

void MarketplaceWindow::onUpdateAllClicked() {
  clearStickyStatus();
  update_queue_.clear();
  // Iterate every loaded extension, not just the currently filtered/searched
  // subset: "Update All" means all updatable extensions, regardless of the
  // active category filter or search term. Skip extensions already staged for
  // restart: a staged update leaves the installed version unchanged (so
  // hasUpdate() stays true) but re-queuing it would just re-download and
  // re-stage the same payload.
  for (const auto& ext : extensions_) {
    // Skip an update already in flight or queued by an individual click: the
    // one currently downloading (active_install_id_) has not staged yet, so
    // hasPendingInstall() is still false — re-queuing it would dispatch update()
    // a second time once it stages and be rejected with "already staged". This
    // mirrors the dedup in onActionButtonClicked.
    if (ext.id == active_install_id_ || pending_clicks_.contains(ext.id)) {
      continue;
    }
    // Skip incompatible updates — install() would reject them (see the enable
    // guard in rebuildTable, which keeps the button in step with this queue).
    if (ext_mgr_->hasUpdate(ext) && ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->hasPendingInstall(ext.id) &&
        !ext_mgr_->hasPendingUninstall(ext.id)) {
      update_queue_.append(ext);
    }
  }
  if (update_queue_.isEmpty()) {
    return;
  }
  ui_->update_all_btn_->setEnabled(false);
  setStatus("Updating " + QString::number(update_queue_.size()) + " extensions...");
  rebuildTable();  // repaint so all queued cards show the "Installing" badge
  processInstallQueue();
}

void MarketplaceWindow::onDiagnosticsClicked() {
  Dialog dlg(this);
  dlg.setDialogTitle(tr("Marketplace Diagnostics"));
  dlg.resize(640, 360);

  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  auto* text = new QPlainTextEdit(body);
  text->setReadOnly(true);

  QStringList lines;
  for (const ExtensionDiagnostic& diagnostic : ext_mgr_->diagnostics()) {
    const QString level = diagnostic.is_error ? "ERROR" : "INFO";
    const QString id = diagnostic.id.isEmpty() ? "-" : diagnostic.id;
    lines.append(QString("[%1] %2 %3: %4")
                     .arg(diagnostic.timestamp.toLocalTime().toString(Qt::ISODate), level, id, diagnostic.message));
  }
  text->setPlainText(lines.isEmpty() ? "No diagnostics." : lines.join('\n'));
  layout->addWidget(text);

  auto* close_row = new QHBoxLayout;
  close_row->addStretch();
  auto* close_button = new QPushButton(tr("Close"), body);
  close_row->addWidget(close_button);
  connect(close_button, &QPushButton::clicked, &dlg, &QDialog::reject);
  layout->addLayout(close_row);
  dlg.contentLayout()->addWidget(body);
  dlg.exec();
}

void MarketplaceWindow::processInstallQueue() {
  // Wait until the current install/update finishes before dispatching the next
  // one — ExtensionManager only runs one at a time. A local sideload sets no
  // active_install_id_, so isInstallBusy() (not the id alone) is the gate.
  if (isInstallBusy()) {
    return;
  }
  // Individual button clicks (pending_clicks_) run ahead of Update All
  // (update_queue_) so an explicit user click on a card is not stuck
  // behind a bulk-update batch that was already in flight.
  if (!pending_clicks_.isEmpty()) {
    const QString next_id = pending_clicks_.takeFirst();
    onActionButtonClicked(next_id);
    // If the dispatch didn't actually start an install (e.g. the extension is
    // already installed or is "local newer" by now), no installFinished will
    // fire to advance the queue — keep draining so one dead entry can't stall
    // the rest.
    if (!isInstallBusy()) {
      processInstallQueue();
    }
    return;
  }
  // Queued "Install local…" sideloads run after explicit card clicks and ahead
  // of the bulk Update All batch.
  if (!pending_local_zips_.isEmpty()) {
    startLocalInstall(pending_local_zips_.takeFirst());
    return;
  }
  if (!update_queue_.isEmpty()) {
    ext_mgr_->update(update_queue_.takeFirst());
  }
}

void MarketplaceWindow::maybeShowRestartRequiredDialog() {
  // Wait for the whole batch: while anything is active or queued — including a
  // local-ZIP sideload, which runs id-less until its manifest is read — the
  // next completion handler calls back here, so the dialog fires exactly once
  // when everything settles.
  if (restart_pending_count_ == 0 || restart_dialog_open_ || !installBatchSettled()) {
    return;
  }
  const int staged = std::exchange(restart_pending_count_, 0);
  const QString text = staged == 1
                           ? tr("An extension change is staged. Restart PlotJuggler to apply it.")
                           : tr("%1 extension changes are staged. Restart PlotJuggler to apply them.").arg(staged);
  // Parent to the visible host: in the app this window is a hidden controller
  // whose content widget is embedded elsewhere, so parenting to `this` would
  // center the modal on hidden stale geometry.
  QWidget* host = content_widget_ != nullptr ? content_widget_->window() : this;
  restart_dialog_open_ = true;
  MessageBox::information(host, tr("Restart required"), text);
  restart_dialog_open_ = false;
  // Anything that staged inside the modal's nested event loop shows now.
  maybeShowRestartRequiredDialog();
}

void MarketplaceWindow::showInvalidRegistryDialog() {
  if (registry_error_dialog_open_) {
    return;
  }
  // Parent to the visible host, like maybeShowRestartRequiredDialog(): this
  // window is a hidden controller whose content is embedded elsewhere.
  QWidget* host = content_widget_ != nullptr ? content_widget_->window() : this;
  const QString detail = last_registry_error_.isEmpty() ? QString() : tr("\n\nDetails: %1").arg(last_registry_error_);
  registry_error_dialog_open_ = true;
  // Generic consequence-only wording: the paragraph never claims a specific cause
  // (it fires on a parse error AND on a network failure), so the concrete reason
  // lives in the Details line instead. This stays correct for any future failure
  // kind without needing a new message per cause.
  MessageBox::information(
      host, tr("Could not load marketplace registry"),
      tr("The marketplace registry could not be loaded. Only the extensions already installed on this "
         "machine are shown; new extensions and updates are unavailable until it loads correctly.") +
          detail);
  registry_error_dialog_open_ = false;
}

void MarketplaceWindow::maybeShowBatchSummary() {
  // Only once the whole run has settled, and only if something failed: otherwise
  // the normal per-item "Installed X" / restart dialog already tells the story.
  if (batch_failed_count_ == 0 || !installBatchSettled()) {
    return;
  }
  const int failed = std::exchange(batch_failed_count_, 0);
  // Sticky (is_error=true) so it isn't overwritten by a trailing non-error
  // status, and so it reads as the outcome it is: not everything succeeded.
  // Point at Diagnostics, where the per-item errors are recorded.
  const QString summary = failed == 1 ? tr("Finished with 1 failure — see Diagnostics for details.")
                                      : tr("Finished with %1 failures — see Diagnostics for details.").arg(failed);
  setStatus(summary, /*is_error=*/true);
}

}  // namespace PJ
