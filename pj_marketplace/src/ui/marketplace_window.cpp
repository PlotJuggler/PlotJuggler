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
#include "pj_widgets/SvgUtil.h"
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

// Item data on the Category cell carrying the row's plugin name: CategoryItem's tie-break within a category.
constexpr int kCategoryNameKeyRole = Qt::UserRole + 2;

// Sort key for the Category column: category text, then plugin name (both case-insensitive), so the default sort reads
// as grouped by category, alphabetical within. Descending flips both keys.
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

// Sort item for the two version columns: a plain item would sort the text lexicographically ("10.0.0" before
// "9.0.0"); comparePluginVersions gives the package ordering. The em-dash placeholder of a not-installed row sorts
// below every valid version.
class VersionItem : public QTableWidgetItem {
 public:
  using QTableWidgetItem::QTableWidgetItem;
  [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
    return comparePluginVersions(semver(text()), semver(other.text())) < 0;
  }

 private:
  // Maps the em-dash placeholder to an empty (invalid) version.
  static std::string semver(const QString& display) {
    return display == u"—"_s ? std::string{} : display.toStdString();
  }
};

// Row-height floor, so short rows still read as separate.
constexpr int kMinRowHeight = 44;

// Per-item fill for the Marketplace-version cell (an invalid QColor: none), read by MarketplaceCellDelegate:
// Destructive pink for an update, Emphasis amber for an incompatible plugin.
constexpr int kHighlightColorRole = Qt::UserRole + 1;

// Paints the Marketplace-version cell in a per-item fill: the app-wide QSS rule
// `QTableView::item { background-color: … }` overrides any per-item setBackground(), so the highlight is drawn here.
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
    // Highlighted, unselected: fill the background and draw the text ourselves (the base would let the QSS repaint over
    // the fill).
    painter->save();
    painter->fillRect(option.rect, fill);
    painter->setPen(fg_);
    const QRect text_rect = option.rect.adjusted(6, 0, -6, 0);
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
  // Queued: a host can only connect to registryManager() once the constructor has returned.
  QMetaObject::invokeMethod(this, [this]() { registry_mgr_->fetchRegistry(registry_url_); }, Qt::QueuedConnection);
}

MarketplaceWindow::~MarketplaceWindow() {
  // The manager keeps the confirmation registered last: disarm ours so a pending local install cannot question a
  // destroyed window (no-op once another window took the registration over). The guard is needed: at shutdown the
  // manager can be destroyed first (it belongs to a service MainWindow owns as a member, and members die before
  // ~QWidget deletes this child), and reaching for it would be a use-after-free.
  if (ext_mgr_) {
    ext_mgr_->clearReplaceConfirmation(this);
  }
  delete ui_;
}

void MarketplaceWindow::setupUi() {
  // Build the .ui onto a child body under PJ::Dialog's title bar.
  setDialogTitle(tr("PlotJuggler Marketplace"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
  content_widget_ = body;  // exposed via contentWidget() for embedding

  ui_->update_all_btn_->setFixedWidth(90);
  ui_->update_all_btn_->setEnabled(false);

  // Pin the filter pills' height to the Install-local button's post-QSS height so the strip reads as one row. QSS
  // padding does not feed sizeHint, so the height is known only after the first layout pass: deferred a tick.
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

  // A solid button: the cog takes the button's own label ink (on Neutral), not the theme's icon ink, which in the dark
  // theme is light on a light fill.
  ui_->settings_btn_->setIcon(tintedPixmap(
      loadSvg(u":/resources/svg/settings_cog_light.svg"_s, currentTheme()),
      theme::onFill(theme::Variant::Neutral, theme::State::Nominal, theme::appTheme())));
  // On the toolbar surface, so the standalone tone.
  ui_->search_edit_->setVariant(Search::Variant::kStandalone);

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

  // Fixed columns sized to their content, Description stretches. Rows are read-only and whole-row selectable; the
  // Marketplace-version cell is highlighted when it is an update. Every action lives in the details footer, not in
  // the cells.
  auto* table = ui_->plugin_table_;
  table->setColumnCount(kColCount);
  table->setHorizontalHeaderLabels({tr("Name"), tr("Category"), tr("Installed"), tr("Marketplace"), tr("Description")});
  table->verticalHeader()->setVisible(false);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setShowGrid(true);
  table->setWordWrap(true);
  table->setAlternatingRowColors(true);
  // Header-click sorting; rebuildTable() turns it off while inserting rows.
  table->setSortingEnabled(true);
  table->sortByColumn(kColCategory, Qt::AscendingOrder);
  table->verticalHeader()->setMinimumSectionSize(kMinRowHeight);
  auto* header = table->horizontalHeader();
  header->setSectionResizeMode(kColName, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColCategory, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColInstalledVersion, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColMarketplaceVersion, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColDescription, QHeaderView::Stretch);
  // A click on Description (long free text) is intercepted and reverts to the last sortable column and order.
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

  // Highlights the Marketplace-version cell (rebuildTable sets the fill; the per-item setBackground path is defeated
  // by the app-wide QTableView::item QSS), at reduced alpha so the standard body ink reads on either tint.
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

  // Footer enable/disable toggle: hidden for non-installed plugins, acts on the plugin shown in the footer; the choice
  // persists and takes effect at the next start.
  ui_->detail_enable_toggle_->setVisible(false);
  connect(ui_->detail_enable_toggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
    if (footer_ext_id_.isEmpty()) {
      return;
    }
    ext_mgr_->setEnabled(footer_ext_id_, checked);
    installations_changed_ = true;
    // Clear a sticky error first: setStatus() suppresses non-error text while one is latched, which would hide this
    // confirmation.
    clearStickyStatus();
    setStatus(
        (checked ? tr("Extension %1 will be enabled after restart") : tr("Extension %1 will be disabled after restart"))
            .arg(footer_ext_id_));
    rebuildTable();
  });

  // Floor and initial size: the four fixed columns plus a roomy Description.
  setMinimumSize(900, 520);
  resize(1080, 600);

  // Overlay pill scrollbars for the scroll areas just built.
  attachPillScrollbars(this);
}

void MarketplaceWindow::setupSignals() {
  // A local archive whose id is already installed needs a decision, which is UI policy the manager cannot take.
  // Replacement is staged, so the wording promises "after restart". Registered against `this` because the manager
  // outlives this window and asks asynchronously.
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

  connect(registry_mgr_, &RegistryManager::fetchStarted, this, [this]() { setInfoStatus(tr("Loading registry...")); });

  connect(registry_mgr_, &RegistryManager::fetchFinished, this, [this](bool success) {
    if (!success) {
      setStatus(tr("Failed to load registry"), true);
      showInvalidRegistryDialog();
      return;
    }
    // A successful refresh clears an old sticky error so progress messages are not suppressed.
    clearStickyStatus();
    registry_extensions_ = registry_mgr_->compatibleExtensions(PlatformUtils::currentPlatform());
    rebuildExtensionList();
    applyFilters();
    setInfoStatus(tr("Ready — %1 extensions loaded").arg(extensions_.size()));
  });

  connect(ext_mgr_, &ExtensionManager::installPendingRestart, this, [this](const QString& id) {
    // Staging ends the active install like installFinished: clear the busy marker so the row shows "Needs Restart" and
    // the queue can advance.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    installations_changed_ = true;
    local_install_path_.clear();
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    ++restart_pending_count_;
    refreshAfterInstalledChange();
    setStatus(tr("Extension %1 staged — will be active after restart").arg(id));
    processInstallQueue();
    maybeShowRestartRequiredDialog();
    // Staging is how every successful update ends, so this handler, not installFinished, closes a typical mixed batch.
    // Must stay after processInstallQueue(): a dispatched next item makes both reports below suppress themselves
    // mid-batch.
    maybeShowBatchSummary();
  });

  connect(ext_mgr_, &ExtensionManager::installUnchanged, this, [this](const QString& id) {
    // A terminal outcome like installFinished, except installations_changed_ stays as it was: nothing reached the
    // extensions dir, so the host has nothing to reload.
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
            ? tr("%1 is already installed — the archive is identical, nothing changed").arg(id)
            : tr("%1 v%2 is already installed — the archive is identical, nothing changed").arg(id, version));
    processInstallQueue();
    maybeShowRestartRequiredDialog();
    maybeShowBatchSummary();
  });

  connect(ext_mgr_, &ExtensionManager::uninstallPendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    ++restart_pending_count_;
    refreshAfterInstalledChange();
    setStatus(tr("Extension %1 staged — will be uninstalled after restart").arg(id));
    maybeShowRestartRequiredDialog();
  });

  connect(ext_mgr_, &ExtensionManager::downgradePendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    ++restart_pending_count_;
    refreshAfterInstalledChange();
    setStatus(tr("Extension %1 will revert to its bundled version after restart").arg(id));
    maybeShowRestartRequiredDialog();
  });

  connect(registry_mgr_, &RegistryManager::fetchError, this, [this](const QString& error) {
    // Keep the reason for the invalid-registry dialog (raised from fetchFinished(false), which carries no message).
    last_registry_error_ = error;
    setStatus(tr("Registry error: %1").arg(error), true);
  });

  connect(ext_mgr_, &ExtensionManager::installStarted, this, [this](const QString& id) {
    active_install_id_ = id;
    // A new operation starts: clear the previous item's sticky error before showInstallProgress(), or an Update All
    // batch (which dispatches items straight from processInstallQueue()) would freeze the red error over the next
    // item's progress.
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

  // Verifying and extracting report no byte progress: switch the bar to busy mode and name the phase.
  connect(
      ext_mgr_, &ExtensionManager::installPhase, this, [this](const QString& /*id*/, DownloadManager::WorkPhase phase) {
        ui_->progress_bar_->setRange(0, 0);
        QString verb;
        switch (phase) {
          case DownloadManager::WorkPhase::Verifying:
            verb = tr("Verifying");
            break;
          case DownloadManager::WorkPhase::Extracting:
            verb = tr("Extracting");
            break;
        }
        showInstallProgress(verb);
      });

  connect(ext_mgr_, &ExtensionManager::installFinished, this, [this](const QString& id, bool success) {
    // Clear the busy marker only for the install we started: a call rejected by an ExtensionManager guard before
    // installStarted also emits installFinished(false) while another install is still in flight.
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
      // Count every failure of this run for the end-of-batch summary (installFinished(false) fires for a mid-download
      // failure and for an op rejected before installStarted). The status was already set by installError.
      ++batch_failed_count_;
    }
    processInstallQueue();
    // A batch whose last item finishes without staging must still surface the dialog for the items that did stage.
    maybeShowRestartRequiredDialog();
    // Once everything settles, a failure summary: the last "Installed X" must not hide a mid-batch failure.
    maybeShowBatchSummary();
  });

  connect(ext_mgr_, &ExtensionManager::installError, this, [this](const QString& /*id*/, const QString& error) {
    ui_->progress_bar_->setVisible(false);
    setStatus(tr("Installation failed: %1").arg(error), true);
    // Queue advance lives in installFinished only: emitInstallFailure fires both signals, so advancing here would pop
    // twice.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallFinished, this, [this](const QString& id, bool success) {
    if (success) {
      status_error_sticky_ = false;
      installations_changed_ = true;
      // Read the name BEFORE the repaint: a local-only row is dropped from extensions_.
      QString name = id;
      for (const auto& ext : extensions_) {
        if (ext.id == id) {
          name = ext.name;
          break;
        }
      }
      refreshAfterInstalledChange();
      setStatus(tr("Uninstalled %1").arg(name));
    }
    // On failure the status was already set by uninstallError — do not overwrite it.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallError, this, [this](const QString& /*id*/, const QString& error) {
    setStatus(tr("Uninstall failed: %1").arg(error), true);
  });

  connect(
      ext_mgr_, &ExtensionManager::diagnosticReported, this,
      [this](const QString& /*id*/, const QString& message, bool is_error) {
        updateDiagnosticsButton();
        // Seen live: mark it surfaced so a re-open does not show it as new.
        ext_mgr_->markDiagnosticsSurfaced();
        if (is_error) {
          setStatus(tr("Marketplace diagnostic: %1").arg(message), true);
        }
      });
}

void MarketplaceWindow::rebuildTable(bool preserve_scroll) {
  auto* table = ui_->plugin_table_;
  const int saved_scroll = table->verticalScrollBar()->value();

  // Silence selection changes during the rebuild (setRowCount(0) clears the selection); the selectRow() at the end
  // fires one itemSelectionChanged that refreshes the footer. Sorting is off while inserting, or the table would
  // re-sort after each row (O(n²)).
  table->blockSignals(true);
  const bool sorting_was_enabled = table->isSortingEnabled();
  table->setSortingEnabled(false);
  table->setRowCount(0);

  // Marketplace-cell tints: Destructive pink for an update, Emphasis amber for an incompatible plugin (the amber of the
  // footer notice).
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

    // Name, with the description as tooltip.
    auto* name_item = new QTableWidgetItem(ext.name);
    name_item->setToolTip(ext.description);
    name_item->setData(Qt::UserRole, ext.id);
    table->setItem(row, kColName, name_item);

    // Category: the registry value verbatim; CategoryItem breaks ties by plugin name (kCategoryNameKeyRole).
    auto* category_item = new CategoryItem(ext.category);
    category_item->setToolTip(ext.category);
    category_item->setData(kCategoryNameKeyRole, ext.name);
    table->setItem(row, kColCategory, category_item);

    // Installed version (em dash when not installed). Asked of the manager, not the snapshot: a core plugin the startup
    // seed refreshed cannot be re-scanned to its new version within this process.
    auto* installed_item = new VersionItem(is_installed ? ext_mgr_->installedVersion(ext.id) : u"\u2014"_s);
    installed_item->setTextAlignment(Qt::AlignCenter);
    table->setItem(row, kColInstalledVersion, installed_item);

    // Marketplace version: the one an update moves to. Only this cell is highlighted; incompatible (amber) wins over
    // update (pink).
    auto* market_item = new VersionItem(ext.version);
    market_item->setTextAlignment(Qt::AlignCenter);
    if (!compat.ok) {
      market_item->setData(kHighlightColorRole, incompatible_fill);
      market_item->setToolTip(compat.reason);
    } else if (has_update) {
      market_item->setData(kHighlightColorRole, update_fill);
      market_item->setToolTip(tr("Update available: v%1").arg(ext.version));
    }
    // Reduced-features note (compatible, but the full-feature floor exceeds this build's SDK): tooltip only.
    if (compat.ok && !compat.completeness_note.isEmpty()) {
      const QString existing = market_item->toolTip();
      market_item->setToolTip(
          existing.isEmpty() ? compat.completeness_note : existing + u"\n"_s + compat.completeness_note);
    }
    table->setItem(row, kColMarketplaceVersion, market_item);

    // Description: the full text wraps in the stretched column; no tooltip, which would only repeat it.
    auto* desc_item = new QTableWidgetItem(ext.description);
    table->setItem(row, kColDescription, desc_item);
  }

  // Re-enable sorting after all rows are in, by the last user-chosen sortable column, so the order is deterministic
  // across rebuilds.
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

  // Restore the selection by ext id so the footer stays on the same plugin through repaints, else the first row.
  // selectRow() fires itemSelectionChanged, which runs updateDetailFooter().
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

  // Enable "Update All" if ANY loaded extension has an update, not just the filtered subset, excluding those already
  // staged (a staged update keeps its old installed version, so hasUpdate stays true) and incompatible ones
  // (install() would reject them).
  bool any_updatable = false;
  for (const Extension& ext : extensions_) {
    if (ext_mgr_->hasUpdate(ext) && ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->hasPendingInstall(ext.id) &&
        !ext_mgr_->hasPendingUninstall(ext.id)) {
      any_updatable = true;
      break;
    }
  }
  // Stay disabled while anything is in flight: processInstallQueue() pops the last item before its install finishes,
  // so update_queue_ empties while active_install_id_ is still downloading, and a click would re-dispatch it.
  ui_->update_all_btn_->setEnabled(any_updatable && update_queue_.isEmpty() && active_install_id_.isEmpty());

  if (preserve_scroll) {
    QTimer::singleShot(
        0, this, [this, saved_scroll]() { ui_->plugin_table_->verticalScrollBar()->setValue(saved_scroll); });
  }
}

void MarketplaceWindow::updateDetailFooter() {
  // Tear down the previous action row (widgets + the leading stretch).
  QLayoutItem* old_item = nullptr;
  while ((old_item = ui_->detail_buttons_layout->takeAt(0)) != nullptr) {
    if (QWidget* old_widget = old_item->widget()) {
      // deleteLater, not delete: this rebuild can run from a footer button's own clicked() slot (e.g. Downgrade).
      // takeAt only unmanages the widget, which stays visible until the deferred delete and would overlap the new
      // badge when a nested event loop (the restart modal) runs, so hide() it now.
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

  // Title (plugin name) as a header-row label so the enable toggle sits at its height; the toggle shows only for
  // installed plugins.
  ui_->detail_title_->setText(ext->name);
  {
    const bool title_installed = installed.contains(ext->id);
    ui_->detail_enable_toggle_->setVisible(title_installed);
    QSignalBlocker block(ui_->detail_enable_toggle_);
    ui_->detail_enable_toggle_->setChecked(title_installed && ext_mgr_->isEnabled(ext->id), /*animate=*/false);
  }

  QString html;

  // When the selected plugin is incompatible with this host, lead the footer with a notice carrying the reason, tinted
  // in the Emphasis (amber) tone like the Marketplace-version cell (a fill family: tint the background, keep the body
  // ink). If it is also outdated, the notice says the update exists but is blocked, and why.
  if (const auto compat = displayedCompatibility(*ext_mgr_, registry_extensions_, *ext); !compat.ok) {
    QColor tint = theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, theme::appTheme());
    tint.setAlphaF(0.30);
    const QString message =
        ext_mgr_->hasUpdate(*ext)
            ? tr("⚠ Update to v%1 available, but blocked — %2").arg(esc(ext->version), esc(compat.reason))
            : tr("⚠ Incompatible — %1").arg(esc(compat.reason));
    html +=
        u"<p style='margin:0 0 6px 0; padding:3px 6px; font-weight:600; "
        u"background-color:rgba(%1,%2,%3,%4);'>%5</p>"_s.arg(tint.red())
            .arg(tint.green())
            .arg(tint.blue())
            .arg(tint.alphaF())
            .arg(message);
  } else if (!compat.completeness_note.isEmpty()) {
    // Info tier, quieter than the amber block: the plugin runs here, optional features wait on a newer build.
    html += u"<p style='margin:0 0 6px 0; padding:3px 6px;'>ℹ %1</p>"_s.arg(esc(compat.completeness_note));
  }

  // Metadata line: publisher/author • category • license • requires PJ • installed.
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
    meta << tr("requires PJ %1+").arg(esc(ext->min_plotjuggler_version));
  }
  if (!installed_version.isEmpty()) {
    meta << tr("installed: v%1").arg(esc(installed_version));
  }
  if (!meta.isEmpty()) {
    html += u"<p style='margin:0 0 6px 0;'>%1</p>"_s.arg(meta.join(u"  •  "_s));
  }

  // Description.
  if (!ext->description.isEmpty()) {
    html += u"<p style='margin:0 0 6px 0;'>%1</p>"_s.arg(esc(ext->description));
  }

  // Changelog, newest first. The map is keyed by version string (lexicographic: "1.10.0" before "1.9.0"), so sort the
  // keys by semver.
  if (!ext->changelog.isEmpty()) {
    QStringList versions = ext->changelog.keys();
    std::sort(versions.begin(), versions.end(), [](const QString& a, const QString& b) {
      return comparePluginVersions(a.toStdString(), b.toStdString()) > 0;
    });
    html += u"<p style='margin:0 0 2px 0; font-weight:600;'>"_s + tr("Changelog").toHtmlEscaped() +
            u"</p><ul style='margin:0 0 0 -20px;'>"_s;
    for (const QString& version : versions) {
      html += u"<li><span style='font-weight:600;'>%1</span> — %2</li>"_s.arg(
          esc(version), esc(ext->changelog.value(version)));
    }
    html += u"</ul>"_s;
  }

  ui_->detail_text_->setHtml(html);

  // Action row: primary action · Uninstall/Downgrade · stretch · Visit Website.
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

  // Primary action/status, colour-coded by the #extButton*/#extBadge* QSS rules. Every footer button carries
  // mpFooterButton so the stylesheet gives them one min-width floor and vertical padding (QStyleSheetStyle ignores a
  // widget-level fixed width). The floor does not clamp: a long label (Downgrade names the version) still grows.
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
  // An incompatible plugin cannot be installed or updated: disable the actionable button and show the reason on hover
  // (install() would refuse it anyway).
  if (action->isEnabled()) {
    if (const auto compat = ext_mgr_->hostCompatibility(*ext); !compat.ok) {
      action->setEnabled(false);
      action->setToolTip(compat.reason);
    }
  }
  ui_->detail_buttons_layout->addWidget(action);

  // Uninstall / Downgrade-to-bundled (installed, not mid-operation).
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

bool MarketplaceWindow::rebuildExtensionList() {
  QStringList previous_ids;
  previous_ids.reserve(extensions_.size());
  for (const Extension& ext : extensions_) {
    previous_ids << ext.id;
  }

  extensions_ = registry_extensions_;

  // An installed id the registry does not list has no row: build one from what the plugin declares about itself.
  // Registry-sourced fields stay empty (consumers guard with isEmpty()), and the primary action is disabled for an
  // installed extension, so no install path runs off this row.
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
    // installedVersion(), not record.version: the scan snapshot can be stale when the startup seed refreshed a bundled
    // plugin, which would render a bogus "Local newer" badge.
    local.version = ext_mgr_->installedVersion(record.id);
    local.min_plotjuggler_version = record.min_plotjuggler_version;
    extensions_.append(local);
  }

  // An extension whose uninstall is staged has left installedExtensions() but is still on disk and running, so a
  // local-only one needs a row to carry its "Needs Restart" state.
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
  // Trim: a stray space would make contains() miss every plugin.
  const QString search = ui_->search_edit_->text().trimmed().toLower();

  // Category values are what the published registry ships in "category" (data_loader, data_stream, message_parser,
  // toolbox), not the button labels. A value the registry does not use silently matches nothing, so check them
  // against real registry data.
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

  // The categories the four toggles govern; any other category (empty, unmodeled) has no toggle.
  static const QStringList kToggleableCategories = {
      u"data_loader"_s, u"data_stream"_s, u"message_parser"_s, u"toolbox"_s};

  filtered_.clear();
  for (const auto& ext : extensions_) {
    // Category toggles are additive: a toggleable category shows only while its toggle is on. A category outside that
    // set is never hidden, or a registry plugin with no category could never be found to install.
    if (kToggleableCategories.contains(ext.category) && !active_categories.contains(ext.category)) {
      continue;
    }
    if (installed_only && !ext_mgr_->isInstalled(ext.id)) {
      continue;
    }
    // Version incompatibility does not hide a row: it stays listed with its reason and a disabled install action.
    // Platform is excluded upstream (compatibleExtensions(currentPlatform()); synthesized rows carry no platforms map).
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
  setInfoStatus(tr("%1 of %2 extensions shown").arg(filtered_.size()).arg(extensions_.size()));
}

void MarketplaceWindow::setStatus(const QString& msg, bool is_error) {
  if (!is_error && status_error_sticky_) {
    return;
  }
  status_error_sticky_ = is_error;
  ui_->status_label_->setText(msg);
  // The error tone keys off objectName (QLabel#marketplaceStatusError in the stylesheets); clearing it restores the
  // default style.
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
    suffix += tr("  ·  %1 queued").arg(queued);
  }
  // Failures show live, so a mid-batch failure is visible while the queue still runs.
  if (batch_failed_count_ > 0) {
    suffix += tr("  ·  %1 failed").arg(batch_failed_count_);
  }
  return suffix;
}

void MarketplaceWindow::setInfoStatus(const QString& msg) {
  if (isInstallBusy()) {
    // An install owns the status line: show its live progress rather than a message that would desync from the bar.
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
  // Surface a diagnostic on window open only if it has not been shown yet (e.g. a staged-promotion failure at startup
  // while no window was open).
  if (!ext_mgr_->hasUnsurfacedDiagnostics()) {
    return;
  }
  const ExtensionDiagnostic& diagnostic = ext_mgr_->diagnostics().back();
  ext_mgr_->markDiagnosticsSurfaced();
  setStatus(tr("Marketplace diagnostic: %1").arg(diagnostic.message), diagnostic.is_error);
}

void MarketplaceWindow::updateDiagnosticsButton() {
  // Only what happened since this panel opened: the manager keeps diagnostics for the life of the application, and a
  // reopened panel must not advertise failures the user already dealt with.
  const QList<ExtensionDiagnostic> diagnostics = ext_mgr_->diagnostics();
  const int count = static_cast<int>(std::count_if(
      diagnostics.cbegin(), diagnostics.cend(),
      [this](const ExtensionDiagnostic& diagnostic) { return diagnostic.timestamp >= opened_at_; }));
  ui_->diagnostics_btn_->setVisible(count > 0);
  ui_->diagnostics_btn_->setText(count > 1 ? tr("Details (%1)").arg(count) : tr("Details"));
}

void MarketplaceWindow::onSearchChanged(const QString& /*text*/) {
  // Filtering is a user action: clear a sticky error so the "K of N shown" text is not suppressed.
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
  // Canonical form, so the same archive reached by a relative path or a symlink is one file for the dedupe below.
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  const QString zip_path = QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
  // Something is already installing: queue behind it instead of hitting the manager's "already in progress" rejection;
  // processInstallQueue() drains it.
  if (isInstallBusy()) {
    // A file already running or waiting is dropped: installing the same archive twice is a replace of the id the first
    // pass just installed, which would stage it and demand a restart for no gain.
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
  // A sideloaded id is absent from the registry list, so name and version come from the installed snapshot. No restart
  // is promised: installFinished is reached only on the fresh-install branch (written straight to the extensions dir
  // and picked up by the host's catalog reload). A replacement reports through installPendingRestart, an identical
  // archive through installUnchanged.
  if (from_file) {
    const QString version = ext_mgr_->installedVersion(id);
    return version.isEmpty() ? tr("Installed %1 from file").arg(id) : tr("Installed %1 v%2 from file").arg(id, version);
  }
  for (const auto& ext : extensions_) {
    if (ext.id == id) {
      return tr("Installed %1 v%2").arg(ext.name, ext.version);
    }
  }
  return tr("Installed %1").arg(id);
}

void MarketplaceWindow::showEvent(QShowEvent* event) {
  activateEmbedded();
  Dialog::showEvent(event);
}

void MarketplaceWindow::activateEmbedded() {
  if (ext_mgr_ == nullptr) {
    return;
  }
  // Re-stamped here, not only at construction: the host keeps this controller alive across a close, so a reopen runs
  // through here. Stamped before the refresh, whose diagnostics belong to this activation.
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
  // Composing is unconditional even though repainting is not: this is the one entry point guaranteed to run, so a
  // session whose registry never loads still builds the local-only rows.
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
  // Another registry is a user action: clear the previous one's sticky error, or setStatus() would swallow this
  // fetch's progress.
  clearStickyStatus();
  registry_mgr_->fetchRegistry(registry_url_);
}

void MarketplaceWindow::onActionButtonClicked(const QString& ext_id) {
  // Another install or update (registry op or sideload) is in flight: queue this click, and processInstallQueue()
  // dispatches it later. The manager's one-install-at-a-time guard would otherwise reject it.
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

  // Resolve against extensions_, not filtered_: a queued click must still be found after the user changed the filter.
  for (const auto& ext : extensions_) {
    if (ext.id != ext_id) {
      continue;
    }
    clearStickyStatus();
    if (ext_mgr_->hasUpdate(ext)) {
      ext_mgr_->update(ext);
    } else if (ext_mgr_->hasNewerInstalledVersion(ext)) {
      setStatus(tr("Installed version is newer than registry version"), true);
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
  // Every loaded extension, not just the filtered subset. Skip ones staged for restart: a staged update leaves the
  // installed version unchanged (hasUpdate() stays true), so re-queuing would re-download the same payload.
  for (const auto& ext : extensions_) {
    // Skip one in flight or queued by a click: the one downloading has not staged yet (hasPendingInstall() is false),
    // so re-queuing it would call update() again once it stages and be rejected. Mirrors onActionButtonClicked.
    if (ext.id == active_install_id_ || pending_clicks_.contains(ext.id)) {
      continue;
    }
    // Skip incompatible updates: install() would reject them (rebuildTable keeps the button in step).
    if (ext_mgr_->hasUpdate(ext) && ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->hasPendingInstall(ext.id) &&
        !ext_mgr_->hasPendingUninstall(ext.id)) {
      update_queue_.append(ext);
    }
  }
  if (update_queue_.isEmpty()) {
    return;
  }
  ui_->update_all_btn_->setEnabled(false);
  setStatus(tr("Updating %1 extensions...").arg(update_queue_.size()));
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
    const QString level = diagnostic.is_error ? tr("ERROR") : tr("INFO");
    const QString id = diagnostic.id.isEmpty() ? "-" : diagnostic.id;
    lines.append(QString("[%1] %2 %3: %4")
                     .arg(diagnostic.timestamp.toLocalTime().toString(Qt::ISODate), level, id, diagnostic.message));
  }
  text->setPlainText(lines.isEmpty() ? tr("No diagnostics.") : lines.join('\n'));
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
  // One at a time, as ExtensionManager runs. A sideload sets no active_install_id_, so isInstallBusy() is the gate.
  if (isInstallBusy()) {
    return;
  }
  // Button clicks run ahead of Update All so an explicit click is not stuck behind a batch in flight.
  if (!pending_clicks_.isEmpty()) {
    const QString next_id = pending_clicks_.takeFirst();
    onActionButtonClicked(next_id);
    // If the dispatch started nothing (already installed, or "local newer" by now) no installFinished will advance the
    // queue: keep draining.
    if (!isInstallBusy()) {
      processInstallQueue();
    }
    return;
  }
  // Queued sideloads run after button clicks and ahead of the Update All batch.
  if (!pending_local_zips_.isEmpty()) {
    startLocalInstall(pending_local_zips_.takeFirst());
    return;
  }
  if (!update_queue_.isEmpty()) {
    ext_mgr_->update(update_queue_.takeFirst());
  }
}

void MarketplaceWindow::maybeShowRestartRequiredDialog() {
  // Wait for the whole batch (a sideload runs id-less until its manifest is read): the next completion handler calls
  // back here, so the dialog fires once when everything settles.
  if (restart_pending_count_ == 0 || restart_dialog_open_ || !installBatchSettled()) {
    return;
  }
  const int staged = std::exchange(restart_pending_count_, 0);
  const QString text = staged == 1
                           ? tr("An extension change is staged. Restart PlotJuggler to apply it.")
                           : tr("%1 extension changes are staged. Restart PlotJuggler to apply them.").arg(staged);
  // Parent to the visible host: this window is a hidden controller whose content is embedded elsewhere, so `this` would
  // center the modal on stale geometry.
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
  // Parent to the visible host, as in maybeShowRestartRequiredDialog().
  QWidget* host = content_widget_ != nullptr ? content_widget_->window() : this;
  const QString detail = last_registry_error_.isEmpty() ? QString() : tr("\n\nDetails: %1").arg(last_registry_error_);
  registry_error_dialog_open_ = true;
  // Consequence-only wording: it fires on parse and network failures alike, so the cause goes in the Details line.
  MessageBox::information(
      host, tr("Could not load marketplace registry"),
      tr("The marketplace registry could not be loaded. Only the extensions already installed on this "
         "machine are shown; new extensions and updates are unavailable until it loads correctly.") +
          detail);
  registry_error_dialog_open_ = false;
}

void MarketplaceWindow::maybeShowBatchSummary() {
  // Only once the run has settled, and only if something failed; otherwise the per-item status and restart dialog tell
  // the story.
  if (batch_failed_count_ == 0 || !installBatchSettled()) {
    return;
  }
  const int failed = std::exchange(batch_failed_count_, 0);
  // Sticky (error) so a trailing non-error status cannot overwrite it; the per-item errors are in Diagnostics.
  const QString summary = failed == 1 ? tr("Finished with 1 failure — see Diagnostics for details.")
                                      : tr("Finished with %1 failures — see Diagnostics for details.").arg(failed);
  setStatus(summary, /*is_error=*/true);
}

}  // namespace PJ
