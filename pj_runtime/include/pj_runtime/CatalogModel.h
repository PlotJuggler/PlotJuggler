#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/type_tree.hpp"  // PrimitiveType
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SavedSeriesPath.h"
#include "pj_runtime/SessionManager.h"  // DatasetIdentityResolution (returned by value below)

namespace PJ {

// Parses the canonical object type from an ObjectTopicDescriptor::metadata_json
// blob. Invalid or missing metadata maps to sdk::BuiltinObjectType::kNone.
[[nodiscard]] sdk::BuiltinObjectType objectTypeFromMetadata(std::string_view metadata_json);

// The wire format of a parser-backed object topic's payloads, "<encoding>:<schema>"
// (from the metadata's payload_encoding / payload_schema), or "" for a topic that
// stores canonical serialized objects. Two same-name topics are only mergeable
// when their formats match: the merged bytes are decoded by ONE topic's parser.
[[nodiscard]] std::string objectPayloadFormatFromMetadata(std::string_view metadata_json);

// Scalar-field payload: a single value-per-timestamp series read from the data
// engine. `logical_type` is the column's schema-declared primitive
// (ColumnDescriptor::logical_type — narrow integer widths survive here even
// though storage widens them); it decides which consumer families the field
// belongs to — see isPlottablePrimitive / isDiscretePrimitive.
struct ScalarFieldPayload {
  QString field_name;
  QString field_path;
  TopicId topic_id = 0;
  std::size_t column_index = 0;
  PrimitiveType logical_type = PrimitiveType::kUnspecified;
  // Float columns: every value retained so far is a whole number
  // (TopicStorage::wholeNumbersOnly), e.g. JSON integers parsed as double. Such
  // a column is also offered as a discrete state series (isDiscreteField).
  bool whole_numbers_only = false;

  friend bool operator==(const ScalarFieldPayload&, const ScalarFieldPayload&) = default;
};

// Consumer-family predicates over a scalar column's schema-declared primitive.
// These are capability filters, NOT a type partition: integers and bool belong
// to BOTH families (plottable as numeric curves AND displayable as discrete
// state series); floats are plottable-only, strings discrete-only. Exhaustive
// switches with no default, so a new PrimitiveType fails to compile here
// instead of silently landing in neither family.
[[nodiscard]] constexpr bool isPlottablePrimitive(PrimitiveType type) noexcept {
  switch (type) {
    case PrimitiveType::kFloat32:
    case PrimitiveType::kFloat64:
    case PrimitiveType::kInt8:
    case PrimitiveType::kInt16:
    case PrimitiveType::kInt32:
    case PrimitiveType::kInt64:
    case PrimitiveType::kUint8:
    case PrimitiveType::kUint16:
    case PrimitiveType::kUint32:
    case PrimitiveType::kUint64:
    case PrimitiveType::kBool:
      return true;
    case PrimitiveType::kString:
    case PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

// Discrete = the values form a finite label set a state-series consumer (the
// State Transitions strip) can display: strings, every integer width, and
// bool. Floats are excluded by design — a continuous value would emit one
// state segment per sample.
[[nodiscard]] constexpr bool isDiscretePrimitive(PrimitiveType type) noexcept {
  switch (type) {
    case PrimitiveType::kString:
    case PrimitiveType::kInt8:
    case PrimitiveType::kInt16:
    case PrimitiveType::kInt32:
    case PrimitiveType::kInt64:
    case PrimitiveType::kUint8:
    case PrimitiveType::kUint16:
    case PrimitiveType::kUint32:
    case PrimitiveType::kUint64:
    case PrimitiveType::kBool:
      return true;
    case PrimitiveType::kFloat32:
    case PrimitiveType::kFloat64:
    case PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

// A scalar field the state strip can display: a discrete primitive, or a float
// column that has only ever held whole numbers.
[[nodiscard]] constexpr bool isDiscreteField(const ScalarFieldPayload& field) noexcept {
  return isDiscretePrimitive(field.logical_type) || field.whole_numbers_only;
}

// Which consumer family a saved series identity may (re)bind to — the resolver
// filter of resolveCurveKey / descriptorForPath. A capability selector over the
// predicates above, so the overlap is deliberate: a strip series saved against
// a string field rebinds fine onto an integer field at the same path after a
// reload (both are kDiscrete), while a plot curve can never bind a string.
enum class SeriesCapability : uint8_t {
  kPlottable,  // numeric curves a plot can draw (floats, integers, bool)
  kDiscrete,   // state series the strip can display (strings, integers, bool, whole-number floats)
};

// Object-topic payload: time-indexed canonical-object stream from the object
// store (e.g. images, depth, image annotations).
struct ObjectTopicPayload {
  ObjectTopicId object_topic_id;
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
  QString metadata_json;

  friend bool operator==(const ObjectTopicPayload&, const ObjectTopicPayload&) = default;
};

// Advertised-topic payload: a topic a streaming source knows it can stream but
// has NOT subscribed, so there is no data and no storage id yet. Surfaced as a
// "paused" placeholder in the catalog and superseded by a real
// ScalarField/ObjectTopic entry the moment data for the topic arrives.
// `classification` is the a-priori schema classification (kNone = scalar-bearing;
// else the builtin object type) used to route drop behavior and to mark the
// always-active infrastructure tier (TF / CameraInfo).
struct AdvertisedTopicPayload {
  sdk::BuiltinObjectType classification = sdk::BuiltinObjectType::kNone;

  friend bool operator==(const AdvertisedTopicPayload&, const AdvertisedTopicPayload&) = default;
};

// A single entry in the catalog. The variant payload statically separates
// scalar-field state from object-topic (and advertised-placeholder) state so
// consumers cannot accidentally read object fields off a scalar entry (or vice
// versa) — the previous flat struct had per-variant dead fields that bit-rotted
// silently. An AdvertisedTopicPayload entry has NO storage id (it is a
// data-less placeholder), so it is neither a scalar field nor an object topic.
struct CatalogItem {
  QString key;  // Opaque catalog key, not a display path.
  QString dataset_name;
  QString topic_name;
  DatasetId dataset_id;
  std::variant<ScalarFieldPayload, ObjectTopicPayload, AdvertisedTopicPayload> payload;

  friend bool operator==(const CatalogItem&, const CatalogItem&) = default;
};

// True for canonical object types the 2D scene module (pj_scene2D) can display
// in a Scene2DDockWidget: raw/compressed images, single video frames, image
// overlays, and depth images. Used by catalog/tree views to mark a topic as
// droppable into a 2D view (icon + drop routing), keeping the
// "what is scene2D-displayable" policy in one place instead of scattered
// per-type == checks. Kept in sync with Scene2DDockWidget::setImageTopic.
[[nodiscard]] inline bool isScene2DDisplayable(sdk::BuiltinObjectType object_type) noexcept {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
    case sdk::BuiltinObjectType::kVideoFrame:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kDepthImage:
      return true;
    default:
      return false;
  }
}

// Convenience accessors. Prefer these over std::get_if at call sites.
[[nodiscard]] inline bool isScalarField(const CatalogItem& item) noexcept {
  return std::holds_alternative<ScalarFieldPayload>(item.payload);
}
[[nodiscard]] inline bool isObjectTopic(const CatalogItem& item) noexcept {
  return std::holds_alternative<ObjectTopicPayload>(item.payload);
}
[[nodiscard]] inline const ScalarFieldPayload* asScalarField(const CatalogItem& item) noexcept {
  return std::get_if<ScalarFieldPayload>(&item.payload);
}
[[nodiscard]] inline const ObjectTopicPayload* asObjectTopic(const CatalogItem& item) noexcept {
  return std::get_if<ObjectTopicPayload>(&item.payload);
}
[[nodiscard]] inline bool isAdvertisedTopic(const CatalogItem& item) noexcept {
  return std::holds_alternative<AdvertisedTopicPayload>(item.payload);
}
[[nodiscard]] inline const AdvertisedTopicPayload* asAdvertisedTopic(const CatalogItem& item) noexcept {
  return std::get_if<AdvertisedTopicPayload>(&item.payload);
}
// An object type no dock shows but a plugin panel reads as a recipe input (a depth image's calibration): its catalog
// row starts a drag, and a dock leaves it out of a drop.
[[nodiscard]] constexpr bool isPanelOnlyObjectType(sdk::BuiltinObjectType type) noexcept {
  return type == sdk::BuiltinObjectType::kCameraInfo;
}

// One available-but-unsubscribed topic advertised by a streaming source. Carries
// only what the catalog needs to show a paused placeholder: the topic name and
// its a-priori classification (kNone = scalar-bearing; else the builtin object
// type). No storage is allocated; the placeholder is superseded the moment real
// data for the topic arrives.
struct AdvertisedTopic {
  QString topic_name;
  sdk::BuiltinObjectType classification = sdk::BuiltinObjectType::kNone;
};

// Storage facts behind one topic, read at hover time for the curve-tree
// tooltip. `schema_name` is the registered schema for a scalar topic (e.g.
// "sensor_msgs/msg/Imu", empty when the topic has none) or the builtin object
// type for an object topic. `count` is the physical row count (scalar) or the
// entry count (object); `live` marks a dataset with an ingest in flight, where
// the count is a moving number.
struct TopicStats {
  QString topic_name;
  QString schema_name;
  quint64 count = 0;
  bool live = false;
};

// Qt-side facade over the catalog of topics/curves known to the current
// session. Populated as data sources load; GUI views (CurveListPanel,
// catalog trees in widget families) subscribe to the add/remove signals.
class CatalogModel : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<CatalogModel>;

  explicit CatalogModel(SessionManager* session = nullptr, QObject* parent = nullptr);
  ~CatalogModel() override;

  CatalogModel(const CatalogModel&) = delete;
  CatalogModel& operator=(const CatalogModel&) = delete;

  std::vector<CatalogItem> items() const;
  /// Calls `visit` on every item in no particular order, without the copy and sort of `items()`. `visit` must not
  /// change the catalog.
  void forEachItem(const std::function<void(const CatalogItem&)>& visit) const;
  /// How many times `items()` and `forEachItem()` have walked the whole catalog: a test reads the difference around a
  /// call to pin that it did not walk it.
  [[nodiscard]] std::size_t wholeCatalogWalks() const noexcept;
  /// The object-topic items named `topic_name`, in `items()` order: one per dataset that
  /// has it. A non-empty `dataset_source` keeps only the dataset with that source NAME.
  /// Walks the catalog in place, without the copy and sort of `items()`.
  [[nodiscard]] std::vector<CatalogItem> objectTopicsNamed(
      const QString& topic_name, const QString& dataset_source) const;

  /// The object-topic item of `topic_id`, or nullopt when the catalog has none.
  /// Walks the catalog in place, without the copy and sort of `items()`.
  [[nodiscard]] std::optional<CatalogItem> objectTopicItem(ObjectTopicId topic_id) const;

  /// Catalog keys of the object topics a preview (an ephemeral on-demand recipe) materializes. items() keeps
  /// them, since the panel that made a preview resolves and attaches its topic through the catalog; only the
  /// user-facing trees leave them out, by asking here.
  [[nodiscard]] QSet<QString> ephemeralPreviewKeys() const;

  /// The scalar field `key` names in `dataset` (`markerSeriesField`'s naming), or
  /// nullopt: a plottable one, or with `strings_too` a string one as well (an
  /// on-demand input reads strings, a marker rule never). Walks the catalog in place,
  /// without the copy and sort `items()` makes, and remembers what it found, per
  /// `strings_too`, until the catalog's items next change (an on-demand recipe asks
  /// at every request). Several matches resolve to the first in `items()` order. GUI
  /// thread only.
  [[nodiscard]] std::optional<ScalarFieldPayload> findScalarForMarkerKey(
      DatasetId dataset, const std::string& key, bool strings_too = false) const;

  /// Dataset owning `topic_name`, or 0 when unknown or present in several datasets.
  /// Multiple fields of one topic still identify the same dataset.
  [[nodiscard]] DatasetId datasetForTopic(const QString& topic_name) const;
  // Fast alternative to items().empty(), which copies and sorts entries.
  [[nodiscard]] bool isEmpty() const noexcept;
  [[nodiscard]] std::optional<CatalogItem> itemDescriptor(const QString& key) const;
  /// The name a plugin addresses the item `key` by (a curve-tree drop onto a plugin panel): a scalar field's
  /// `sdk::markerSeriesKey` (strings included; the plugin decides what it can read), a paused topic's name, or an
  /// object topic's name, source-qualified when another dataset has that topic too. nullopt for an unknown key. GUI
  /// thread only.
  [[nodiscard]] std::optional<std::string> pluginDropName(const QString& key) const;
  /// The key under which `dataset` lists the object topic `topic`, or nullopt when the catalog does not list it. A
  /// hash lookup: how a caller holding topic ids finds their rows without walking `items()`.
  [[nodiscard]] std::optional<QString> objectTopicKey(DatasetId dataset, ObjectTopicId topic) const;
  /// The same lookup for column `column` of the DataEngine topic `topic`.
  [[nodiscard]] std::optional<QString> scalarColumnKey(DatasetId dataset, TopicId topic, std::size_t column) const;

  // Latest scalar value at or before display-axis time `display_seconds` for the
  // curve identified by catalog `key` (zero-order hold). Returns nullopt when the
  // key is unknown / not a scalar field, or no sample exists at or before that
  // time. Backs the curve-list "Value" column; reads committed DataEngine storage
  // under its lock via DataReader::latestAt, converting the display time to raw ns
  // through the curve's per-dataset display offset.
  [[nodiscard]] std::optional<double> scalarValueAt(const QString& key, double display_seconds) const;

  // True iff `key` names a scalar-field curve OR a scalar-shaped (kNone) advertised
  // placeholder (not an object topic / object-shaped placeholder / unknown key).
  // Cheap lookup with no CatalogItem copy — lets the value column choose "-" (a
  // scalar with no sample yet, real or not-yet-subscribed) vs blank (non-scalar
  // row) without itemDescriptor().
  [[nodiscard]] bool isScalarKey(const QString& key) const;

  // True iff `key` names a string-typed scalar field (a subset of isScalarKey).
  // The value column reads it via stringValueAt instead of scalarValueAt.
  [[nodiscard]] bool isStringKey(const QString& key) const;

  // True iff `key` names a scalar field the State Transitions strip can display
  // (string / integer / bool, or a float holding only whole numbers — see
  // isDiscreteField). Superset of isStringKey; overlaps the plottable family on
  // integers, bools and whole-number floats.
  [[nodiscard]] bool isDiscreteKey(const QString& key) const;

  // Latest string value at or before display-axis time `display_seconds` for the
  // string curve `key` (zero-order hold). nullopt when the key is unknown / not a
  // string field, or no sample exists at or before that time. The string sibling
  // of scalarValueAt; backs the "Value" column for string fields.
  [[nodiscard]] std::optional<QString> stringValueAt(const QString& key, double display_seconds) const;

  // Storage facts for the topic behind `key` (a scalar field's owning topic or
  // an object topic). nullopt for an unknown key, an advertised placeholder (no
  // storage yet), or no session. Reads under the engine/store locks; hover-rate
  // only.
  [[nodiscard]] std::optional<TopicStats> topicStats(const QString& key) const;

  std::vector<CurveDescriptor> curves() const;
  // The descriptor for `key` when its field satisfies `capability`; nullopt
  // otherwise. The kPlottable default is the plot-side gate ("string keys have
  // no curve descriptor"); the strip passes kDiscrete to build its row
  // descriptors through the same single constructor.
  [[nodiscard]] std::optional<CurveDescriptor> curveDescriptor(
      const QString& key, SeriesCapability capability = SeriesCapability::kPlottable) const;

  // Loaded datasets as (id, display name) pairs, ordered by load (dataset id
  // ascending). Derived from current catalog contents.
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> datasets() const;
  // The ids alone, ascending — the cheap form for callers that only need
  // membership (no names materialized).
  [[nodiscard]] std::vector<DatasetId> datasetIds() const;

  // The datastore's unmodified DatasetInfo::source_name for `dataset_id`.
  // Unlike datasets(), this is neither a user-facing display-name override nor a
  // duplicate-label ordinal ("name (2)"). Layout identity resolution uses the
  // raw value as its portable fallback when a saved DatasetId was reminted.
  // nullopt when there is no session or the id is unknown.
  [[nodiscard]] std::optional<QString> datasetSourceName(DatasetId dataset_id) const;

  // Resolves a saved curve identity — a topic+field path optionally qualified by
  // an exact DatasetId, a raw source label, and a full file path — to the live
  // concrete catalog key it should rebind to, never guessing by load order. This
  // is THE shared three-tier resolver for both layout/undo restore
  // (PendingDisplayBinder) and clipboard paste (PlotWidget):
  //   * Qualified (id/source/path present): run resolveDatasetIdentity; if it
  //     names a dataset, bind topic+field within THAT dataset only (never steal a
  //     same-named field from a sibling → nullopt if it doesn't have it). If the
  //     exact id failed but a full path is present, the physical path OUTRANKS the
  //     raw source label: bind when exactly one path-sibling provides the series,
  //     staying unresolved under ambiguity.
  //   * Unqualified (legacy/generic): bind only when topic+field is globally
  //     unique across datasets.
  // Returns nullopt when there is no session, when the intended dataset lacks the
  // series, or when the candidates are ambiguous. `capability` selects the
  // consumer family the identity may bind to (see SeriesCapability) — a plot
  // curve can never bind a string field, a strip series can never bind a float.
  [[nodiscard]] std::optional<QString> resolveCurveKey(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path, const QString& topic,
      const QString& field, SeriesCapability capability = SeriesCapability::kPlottable) const;
  // The same resolution for an identity held as one value (a parsed layout entry).
  [[nodiscard]] std::optional<QString> resolveCurveKey(
      const SavedSeriesPath& path, SeriesCapability capability = SeriesCapability::kPlottable) const;

  // Resolves the same persisted (id, raw-source, full-path) identity used by
  // scene widgets (delegates to SessionManager::resolveDatasetIdentity — see
  // there for resolution order and ambiguity semantics). Kept on the catalog
  // facade so plot/layout binding does not need a separate FileLoader
  // dependency. Returns an empty resolution when there is no session.
  [[nodiscard]] DatasetIdentityResolution resolveDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path = {}) const;
  // The normalized full source path registered for `dataset_id`, or empty when
  // there is no session or the dataset has no file-backed path.
  [[nodiscard]] QString datasetSourcePath(DatasetId dataset_id) const;
  // Owned snapshot of the loader-metadata document for `dataset_id`
  // (SessionManager::datasetMetadata), or nullopt when none / no session.
  // Read-only facade — the catalog stores nothing.
  [[nodiscard]] std::optional<DatasetMetadata> datasetMetadata(DatasetId dataset_id) const;

  // Resolves a stable topic+field path to the matching scalar field within a
  // specific dataset. Lets a layout rebind across similar datasets where the
  // opaque per-load key differs but the topic/field path is identical.
  // `capability` selects which consumer family may match (see SeriesCapability).
  [[nodiscard]] std::optional<CurveDescriptor> descriptorForPath(
      DatasetId dataset_id, const QString& topic, const QString& field,
      SeriesCapability capability = SeriesCapability::kPlottable) const;

  // Clears the whole catalog. With tombstone=true (default) every dataset id is recorded
  // so a later rebuildFromDatastore stays empty while data still lingers in the engine;
  // pass tombstone=false when the caller ALSO erases the engine/object data (a real
  // "delete all"), since nothing is left to hide.
  void clearAll(bool tombstone = true);
  void removeItems(const std::vector<QString>& keys);
  void removeCurves(const std::vector<QString>& keys);

  // Removes one dataset's items from the catalog. With tombstone=true (default) the id is
  // hidden from future rebuildFromDatastore (scalar data stays in the DataEngine — used by
  // the reload swap + speculative/rollback callers; undo via restoreDataset). Pass
  // tombstone=false when the caller is ALSO erasing the underlying engine/object data (a
  // real "Remove Dataset"): nothing is kept to hide, so no tombstone is recorded. Emits
  // cleared() if it empties the catalog, else one batched itemsRemoved; returns false
  // (no-op) if the dataset has no items. Pure catalog op — media eviction is the caller's job.
  bool removeDataset(DatasetId dataset_id, bool tombstone = true);

  // Discards soft-delete tombstones and rebuilds from the datastore so a
  // resurrection path (e.g. layout load) can re-expose previously removed curves.
  void resetRemovalState();

  // Restores one dataset hidden by removeDataset().
  void restoreDataset(DatasetId dataset_id);

  // Overrides the tree-root label of a dataset, replacing the file-derived
  // source_name shown in catalog/tree views (issue #98 — lets a DataSource name
  // its dataset instead of inheriting the opened file's basename). Keyed by the
  // stable DatasetId so the override survives rebuildFromDatastore and
  // clearAll/restoreDataset. The override is normalized ('/'→'_') and
  // participates in duplicate-label ordinal disambiguation, exactly like a
  // source_name-derived label. An empty string clears the override (falls back
  // to source_name). Does not touch the engine's immutable DatasetInfo, so
  // dataset-reuse matching (by source_name) is unaffected.
  void setDatasetDisplayName(DatasetId dataset_id, const QString& display_name);

  // Replace the full set of advertised (available-but-unsubscribed) topics for a
  // streaming dataset. Each appears in the catalog as a data-less "paused"
  // placeholder ONLY where no storage-backed entry for the same topic name exists
  // (real data wins). Declarative: the given list is the complete set for
  // `dataset_id`; topics dropped from it are removed. Zero storage cost — no
  // DataEngine/ObjectStore topic is created. Must be called on the model's
  // (GUI) thread; streaming producers marshal to it. Emits itemsAdded/itemsRemoved.
  void setAdvertisedTopics(DatasetId dataset_id, const std::vector<AdvertisedTopic>& topics);

  // Drop every advertised placeholder for a dataset (e.g. on source disconnect).
  void clearAdvertisedTopics(DatasetId dataset_id);

  // Marks whether `dataset_id`'s streaming source supports per-topic pause
  // (kCapabilityPerTopicPause) — set by StreamingSourceManager right after a
  // session starts, cleared on session teardown. A non-demand source (or a
  // file dataset, where this is never set) sends everything regardless of
  // display state, so there is no wire subscription for the UI to represent
  // as "unsubscribed"; consumers gate on this before reading a topic's
  // presence/absence in TopicDemandTracker's active set as a pause signal.
  void setPerTopicPauseCapable(DatasetId dataset_id, bool capable);
  [[nodiscard]] bool isPerTopicPauseCapable(DatasetId dataset_id) const;

  // The samplesIngested gate (connected to SessionManager::samplesIngested in place
  // of a direct rebuildFromDatastore): runs a full rebuild ONLY when the catalog's
  // content fingerprint changed since the last rebuild. During a load most ingest
  // batches add rows, not new datasets/topics/columns, so this skips the
  // (allocation-heavy) full rebuild on the common path while staying fully
  // synchronous. A created or retired topic always moves the fingerprint, so it
  // also serves a caller that may have changed no topics (an unchanged restore).
  void rebuildIfChanged();

 public slots:
  void rebuildFromDatastore();

 signals:
  // Batched companion to itemAdded(), emitted once per rebuild so views can
  // update/sort once even when many catalog entries appear at the same time.
  void itemsAdded(const std::vector<CatalogItem>& items);
  void itemAdded(const CatalogItem& item);
  // One emission per removal operation (whole dataset, multi-key trash, or keys
  // that vanished on a rebuild) so consumers react once, not per key.
  void itemsRemoved(const QStringList& keys);
  void cleared();
  // Keys that survived a rebuild with a different item (payload or labels): e.g.
  // a float field turning discrete on its first whole-number chunk, or a reload
  // retyping a column. The key is unchanged, so itemsAdded/itemsRemoved do not
  // fire for it; consumers that resolve keys by capability re-resolve here.
  void itemsChanged(const QStringList& keys);

 private:
  // The actual full rebuild body. rebuildFromDatastore() wraps this and refreshes
  // the cached fingerprint, so every rebuild (gated or explicit) leaves the gate
  // in sync.
  void rebuildNow();

  // Allocation-light, order-independent fingerprint of everything that affects
  // catalog output: engine structure (datasets, topics, per-topic column count,
  // object topics) plus the catalog's own tombstone/override state. Equal
  // fingerprints ⇒ a rebuild would produce identical output.
  [[nodiscard]] std::uint64_t catalogFingerprint() const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
