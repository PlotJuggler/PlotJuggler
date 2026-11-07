#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_plugins/widget_types.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SavedSeriesPath.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/WireTarget.h"

namespace PJ {

/// What a wire that did not resolve yet waits for: the topic its saved identity names and, when the
/// dataset that identity names is loaded, that dataset. The shell holds a demand reference on the
/// topic for it (PendingDisplayBinder), so a demand-driven stream sends the data the wire needs.
struct PendingWireDemand {
  QString topic_name;
  std::optional<DatasetId> preferred_dataset;

  bool operator==(const PendingWireDemand&) const = default;
};

/// One cable: a series wired to a slot of a widget input. Pending (unresolved) wires keep their
/// identity and are retried when the catalog changes.
struct WidgetWire {
  QString input;  ///< stable input name, never a position
  std::uint32_t slot = 0;
  std::size_t input_index = 0;  ///< position of `input` in the widget's current inputs()
  WidgetInputKind kind = WidgetInputKind::kNumber;
  SavedSeriesPath identity;  ///< what the layout stores; `field` is empty for an object topic
  bool resolved = false;
  DatasetId dataset = 0;  ///< valid when resolved
  WireTarget target;      ///< valid when resolved; its offset is the one read at resolution (see currentTarget)
  QString key;            ///< catalog key of a resolved scalar wire
  std::string label;      ///< "topic/field", handed to the widget with every value
};

/// The wiring model of one Custom Widget panel: which series feeds which input slot, how a wire
/// is resolved against the catalog, and how it is persisted. No widget classes: the dock owns the
/// plugin instance and calls in here.
///
/// Inputs are matched by NAME. `setInputs` is the single reconciliation point (after create,
/// load_config and inputs_changed): a wire whose input survives is kept, the others are dropped.
/// Main-thread only, like the catalog it reads.
class WidgetBindings {
 public:
  /// Slot argument meaning "append a new slot" on a `multiple` input.
  static constexpr std::uint32_t kNewSlot = std::numeric_limits<std::uint32_t>::max();

  struct InputsDelta {
    bool inputs_changed = false;
    bool wires_changed = false;  ///< at least one wire was dropped
  };

  WidgetBindings(SessionManager* session, CatalogModel* catalog);

  /// Installs the widget's current inputs and drops the wires that no longer fit (input gone,
  /// kind changed, extra slot on a non-`multiple` input).
  InputsDelta setInputs(std::vector<WidgetInput> inputs);
  [[nodiscard]] const std::vector<WidgetInput>& inputs() const noexcept {
    return inputs_;
  }
  [[nodiscard]] std::optional<std::size_t> inputIndex(const QString& name) const;

  [[nodiscard]] const std::vector<WidgetWire>& wires() const noexcept {
    return wires_;
  }
  [[nodiscard]] const WidgetWire* findWire(const QString& input, std::uint32_t slot) const;

  /// What kind of input the catalog key can feed: a plottable scalar is a number, a string field
  /// is text, an object topic is an object. nullopt for anything else (advertised placeholder,
  /// unknown key).
  [[nodiscard]] std::optional<WidgetInputKind> kindOfKey(const QString& key) const;
  [[nodiscard]] bool canBind(std::size_t input_index, const QString& key) const;

  /// Input a dropped series of `kind` goes to: the first compatible input with no wire, else the
  /// first compatible `multiple` input (a new slot), else the first compatible one (its wire is
  /// replaced). Inputs in `exclude` are skipped; nullopt when none qualifies.
  [[nodiscard]] std::optional<std::size_t> pickInput(
      WidgetInputKind kind, const std::set<std::size_t>& exclude = {}) const;

  /// Wires catalog `key` to `slot` of the input (see kNewSlot). A normal input always uses slot 0
  /// and its previous wire is replaced. False when the key does not fit the input or does not
  /// resolve.
  bool bindKey(std::size_t input_index, std::uint32_t slot, const QString& key);
  bool bindObject(std::size_t input_index, std::uint32_t slot, ObjectTopicId topic);
  bool unbind(const QString& input, std::uint32_t slot);

  /// Retries every pending wire against the catalog. True when one resolved.
  bool retryPending();
  /// Re-resolves every resolved wire from its identity after the catalog lost or changed items:
  /// a wire whose series vanished becomes pending, one whose ids moved is retargeted. True when
  /// any wire changed state.
  bool revalidate();
  /// revalidate() and retryPending() in one pass over the wires (after the catalog changed items).
  /// A wire revalidation turns pending is not retried: it just failed against the same catalog.
  bool refresh();

  /// The wire's read target with the dataset's display offset read now, or nullopt when pending.
  /// Allocates nothing: it runs for every wire on every tick.
  [[nodiscard]] std::optional<WireTarget> currentTarget(const WidgetWire& wire) const;

  /// (dataset, topic) of every resolved wire, without duplicates: the demand the panel holds.
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> displayedTopics() const;
  /// The pending wires' claims, without duplicates (see PendingWireDemand).
  [[nodiscard]] std::vector<PendingWireDemand> pendingDemands() const;
  /// Whether a pending wire names `topic`. Unlike pendingDemands(), resolves no dataset.
  [[nodiscard]] bool hasPendingTopic(const QString& topic) const;
  /// Whether pendingDemands() is non-empty: a pending wire names a topic.
  [[nodiscard]] bool hasPendingWires() const;

  /// Appends one `<input>` element per wire to `parent`. Resolved wires are stamped with the live
  /// dataset id, source and full path; pending ones are written back verbatim.
  void save(QDomDocument& doc, QDomElement& parent) const;
  /// Replaces the wires with the `<input>` children of `element`, resolving what it can. Elements
  /// naming an input the widget does not have, or one of another kind, are dropped.
  void load(const QDomElement& element);

  void clearWires();

 private:
  /// Resolves `wire.identity` against the live catalog/store and fills the wire. False when it
  /// does not resolve (yet); the wire is then left pending.
  bool resolveFromIdentity(WidgetWire& wire) const;
  /// One wire of revalidate(): `wire` is resolved. True when it turned pending or was retargeted.
  bool revalidateWire(WidgetWire& wire) const;
  /// Stamps the identity (dataset id, source, path) and the live dataset of a wire that resolved;
  /// `fallback_name` is the source when the catalog names none.
  void setDatasetIdentity(WidgetWire& wire, DatasetId dataset_id, const QString& fallback_name) const;
  void fillScalar(WidgetWire& wire, const CurveDescriptor& curve) const;
  /// False when the topic is not in the store.
  bool fillObject(WidgetWire& wire, ObjectTopicId topic) const;
  /// Inserts or replaces the wire at (input, slot); a normal input always uses slot 0.
  void place(std::size_t input_index, std::uint32_t slot, WidgetWire wire);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  std::vector<WidgetInput> inputs_;
  std::vector<WidgetWire> wires_;
};

}  // namespace PJ
