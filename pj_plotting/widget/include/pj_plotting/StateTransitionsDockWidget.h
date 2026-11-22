#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QWidget>
#include <memory>
#include <optional>
#include <vector>

#include "pj_plotting/StateTransitionsController.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_widgets/StateTransitionsView.h"

namespace PJ {

class CatalogModel;
class PlaybackEngine;
class SessionManager;

/// The plot-area State Transitions dock: a StateTransitionsView bound by a StateTransitionsController, as the
/// `IDataWidget` the dock system hosts. Kind string and XML payload tag: `state_transitions`.
///
/// Persistence: each row serializes its stable dataset/topic/field identity (the scene-dock pattern, not LayoutXml's
/// plot-curve passes) and is rebound on load through `CatalogModel::resolveCurveKey`. A `<series>` that cannot resolve
/// (its dataset is not loaded) is kept and re-serialized on the next save, so a layout round trip without the data
/// loses nothing; it is not rendered, and re-resolves as soon as the catalog surfaces its dataset (`itemsAdded`), the
/// strip's analog of PendingDisplayBinder's flush.
class StateTransitionsDockWidget : public QWidget, public IDataWidget {
  Q_OBJECT
 public:
  StateTransitionsDockWidget(
      SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback, QWidget* parent = nullptr);

  // --- IDataWidget ---
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;
  /// Accepts every dropped key that is a discrete series; true if any row was added. The DockWidget routes curve-list
  /// drops on a mounted strip (and the auto-create seed) through this.
  bool tryAcceptSeriesKeys(const QStringList& catalog_keys) override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  [[nodiscard]] bool xmlValidateState(const QDomElement& element) const override;
  /// One `topic` (or `topic/field`) per saved series whose dataset is not loaded.
  [[nodiscard]] QStringList unresolvedRestores() const override;

  [[nodiscard]] StateTransitionsView* view() noexcept {
    return view_;
  }
  [[nodiscard]] StateTransitionsController* controller() noexcept {
    return controller_.get();
  }

 signals:
  /// The widget's persisted state changed through a user gesture (series added or removed, view chrome committed). The
  /// shell debounces it into a workspace-undo snapshot, as for the scene docks.
  void workspaceChanged();

 private:
  /// One saved-but-unresolved series (dataset not loaded): the raw identity attributes, preserved verbatim across
  /// save/load.
  struct PendingSeries {
    QString dataset_source;
    QString dataset_path;
    qint64 dataset_id = 0;
    QString topic;
    QString field;
    bool visible = true;
  };

  /// A <state_transitions> element as xmlLoadState applies it.
  struct ParsedState {
    std::vector<PendingSeries> series;
    std::optional<double> zoom_px_per_ns;  ///< unset when absent, unparsable or not positive: the zoom and pan stay
    qint64 viewport_left_ns = 0;           ///< applied only with a zoom
    int viewport_top_px = 0;
  };
  /// The one validator, shared by xmlLoadState and xmlValidateState: reads every attribute through readers that report
  /// to an active XmlOptionsRecorder and refuses only another root tag. Pure: changes nothing.
  [[nodiscard]] static bool parseState(const QDomElement& element, ParsedState& parsed);

  /// Resolves one saved identity and materializes its row (restoring the eye state); false when the catalog cannot
  /// resolve it (yet).
  bool tryResolveSeries(const PendingSeries& saved);
  /// Drains pending_series_ entries that now resolve; connected to the catalog's itemsAdded so late-loading datasets
  /// bind their rows.
  void retryPendingSeries();
  /// Restores the saved zoom, pan and vertical offset (zoom first, so the pan maps under it).
  void applyViewChrome(const ParsedState& parsed);

  StateTransitionsView* view_ = nullptr;
  std::unique_ptr<StateTransitionsController> controller_;
  CatalogModel* catalog_ = nullptr;
  std::vector<PendingSeries> pending_series_;
};

}  // namespace PJ
