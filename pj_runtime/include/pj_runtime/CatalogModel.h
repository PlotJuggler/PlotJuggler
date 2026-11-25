#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>
#include <chrono>
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

// The canonical object type in an ObjectTopicDescriptor's metadata_json; kNone when invalid or missing.
[[nodiscard]] sdk::BuiltinObjectType objectTypeFromMetadata(std::string_view metadata_json);

// The wire format of a parser-backed object topic's payloads, "<encoding>:<schema>"
// (from the metadata's payload_encoding / payload_schema), or "" for a topic that
// stores canonical serialized objects. Two same-name topics are only mergeable
// when their formats match: the merged bytes are decoded by ONE topic's parser.
[[nodiscard]] std::string objectPayloadFormatFromMetadata(std::string_view metadata_json);

// A single value-per-timestamp series. `logical_type` is the schema-declared primitive (narrow integer widths survive
// though storage widens them); it decides the consumer families (isPlottablePrimitive / isDiscretePrimitive).
struct ScalarFieldPayload {
  QString field_name;
  QString field_path;
  TopicId topic_id = 0;
  std::size_t column_index = 0;
  PrimitiveType logical_type = PrimitiveType::kUnspecified;
  // Float column whose every retained value is whole (JSON integers parsed as double): also offered as a discrete
  // state series.
  bool whole_numbers_only = false;

  friend bool operator==(const ScalarFieldPayload&, const ScalarFieldPayload&) = default;
};

// Capability filters, not a type partition: integers and bool are both plottable and discrete; floats are
// plottable-only, strings discrete-only. Exhaustive switches with no default, so a new PrimitiveType fails to compile
// instead of landing in neither family.
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

// A finite label set the State Transitions strip can display. Floats are excluded: a continuous value would emit one
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

// Which consumer family a saved series identity may (re)bind to (resolveCurveKey / descriptorForPath). The overlap is
// deliberate: a strip series saved on a string field rebinds onto an integer field at the same path after a reload,
// while a plot curve never binds a string.
enum class SeriesCapability : uint8_t {
  kPlottable,  // numeric curves a plot can draw (floats, integers, bool)
  kDiscrete,   // state series the strip can display (strings, integers, bool, whole-number floats)
  kAny,        // every scalar field: identity lookups (which topic a row is), never what a view can draw
};

struct ObjectTopicPayload {
  ObjectTopicId object_topic_id;
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
  QString metadata_json;

  friend bool operator==(const ObjectTopicPayload&, const ObjectTopicPayload&) = default;
};

// A topic a streaming source can stream but has not subscribed: no data and no storage id yet. Shown as a paused
// placeholder, superseded by the real entry when data arrives. `classification` (kNone = scalar-bearing, else the
// builtin object type) routes drops and marks the always-active infrastructure tier (TF / CameraInfo).
struct AdvertisedTopicPayload {
  sdk::BuiltinObjectType classification = sdk::BuiltinObjectType::kNone;

  friend bool operator==(const AdvertisedTopicPayload&, const AdvertisedTopicPayload&) = default;
};

// One catalog entry. An AdvertisedTopicPayload entry has no storage id: neither a scalar field nor an object topic.
struct CatalogItem {
  QString key;  // Opaque catalog key, not a display path.
  QString dataset_name;
  QString topic_name;
  DatasetId dataset_id;
  std::variant<ScalarFieldPayload, ObjectTopicPayload, AdvertisedTopicPayload> payload;

  friend bool operator==(const CatalogItem&, const CatalogItem&) = default;
};

// Canonical object types a Scene2DDockWidget can display (images, video frames, overlays, depth): the one place the
// tree views ask to mark a topic droppable into a 2D view. Keep in sync with Scene2DDockWidget::setImageTopic.
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

// An advertised, unsubscribed topic: its name and a-priori classification (kNone = scalar-bearing).
struct AdvertisedTopic {
  QString topic_name;
  sdk::BuiltinObjectType classification = sdk::BuiltinObjectType::kNone;
};

// Storage facts behind one topic for the curve-tree tooltip. `schema_name`: the scalar topic's registered schema
// (empty when none) or the builtin object type. `count`: rows or entries; `live`: an ingest is in flight, so the count
// moves.
struct TopicStats {
  QString topic_name;
  QString schema_name;
  quint64 count = 0;
  bool live = false;
};

// Qt-side facade over the session's topics and curves for the views. GUI thread.
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
  /// has it. A non-empty `dataset_label` keeps only the dataset so labelled (datasetByLabel), none for an unknown one.
  /// Walks the catalog in place, without the copy and sort of `items()`.
  [[nodiscard]] std::vector<CatalogItem> objectTopicsNamed(
      const QString& topic_name, const QString& dataset_label) const;

  /// The object-topic item of `topic_id`, or nullopt when the catalog has none.
  /// Walks the catalog in place, without the copy and sort of `items()`.
  [[nodiscard]] std::optional<CatalogItem> objectTopicItem(ObjectTopicId topic_id) const;

  /// Catalog keys of the object topics a preview (an ephemeral on-demand recipe) materializes. items() keeps
  /// them, since the panel that made a preview resolves and attaches its topic through the catalog; only the
  /// user-facing trees leave them out, by asking here.
  [[nodiscard]] QSet<QString> ephemeralPreviewKeys() const;

  /// The scalar field `key` names in `dataset` (`markerSeriesField`'s naming): a plottable one, or with `strings_too` a
  /// string one too (an on-demand input reads strings, a marker rule never). Memoized per `strings_too` until the
  /// items change; several matches resolve to the first in `items()` order. GUI thread only.
  [[nodiscard]] std::optional<ScalarFieldPayload> findScalarForMarkerKey(
      DatasetId dataset, const std::string& key, bool strings_too = false) const;

  /// Dataset owning `topic_name`, or 0 when unknown or present in several datasets.
  /// Multiple fields of one topic still identify the same dataset.
  [[nodiscard]] DatasetId datasetForTopic(const QString& topic_name) const;
  [[nodiscard]] bool isEmpty() const noexcept;
  [[nodiscard]] std::optional<CatalogItem> itemDescriptor(const QString& key) const;
  /// The name a plugin addresses the item `key` by (a curve-tree drop onto a plugin panel): a scalar field's
  /// `sdk::markerSeriesKey` (strings included; the plugin decides what it can read), a paused topic's name, or an
  /// object topic's name, qualified by its dataset label when another dataset has that topic too. nullopt for an
  /// unknown key. GUI thread only.
  [[nodiscard]] std::optional<std::string> pluginDropName(const QString& key) const;
  /// The key under which `dataset` lists the object topic `topic`, or nullopt when the catalog does not list it. A
  /// hash lookup: how a caller holding topic ids finds their rows without walking `items()`.
  [[nodiscard]] std::optional<QString> objectTopicKey(DatasetId dataset, ObjectTopicId topic) const;
  /// The same lookup for column `column` of the DataEngine topic `topic`.
  [[nodiscard]] std::optional<QString> scalarColumnKey(DatasetId dataset, TopicId topic, std::size_t column) const;

  // Zero-order hold at display-axis `display_seconds` for the curve `key`; null rows are skipped, as the plot tracker
  // does. nullopt: unknown key, not a scalar field, or no sample at or before. Reads under the engine lock.
  [[nodiscard]] std::optional<double> scalarValueAt(const QString& key, double display_seconds) const;

  // A scalar field or a scalar-shaped (kNone) placeholder: the value column shows "-" for one with no sample yet,
  // blank for other rows.
  [[nodiscard]] bool isScalarKey(const QString& key) const;

  // A string field (subset of isScalarKey): read through stringValueAt.
  [[nodiscard]] bool isStringKey(const QString& key) const;

  // A field the State Transitions strip can display (see isDiscreteField); superset of isStringKey.
  [[nodiscard]] bool isDiscreteKey(const QString& key) const;

  // The string sibling of scalarValueAt.
  [[nodiscard]] std::optional<QString> stringValueAt(const QString& key, double display_seconds) const;

  // nullopt for an unknown key, an advertised placeholder or no session. Takes the engine/store locks: hover rate only.
  [[nodiscard]] std::optional<TopicStats> topicStats(const QString& key) const;

  std::vector<CurveDescriptor> curves() const;
  // The descriptor for `key` when its field satisfies `capability`. kPlottable is the plot gate (strings have no
  // curve descriptor); the strip passes kDiscrete.
  [[nodiscard]] std::optional<CurveDescriptor> curveDescriptor(
      const QString& key, SeriesCapability capability = SeriesCapability::kPlottable) const;

  // (id, label), dataset id ascending: the datasets the tree lists (those with items).
  [[nodiscard]] std::vector<std::pair<DatasetId, QString>> datasets() const;
  [[nodiscard]] std::vector<DatasetId> datasetIds() const;

  // The datastore's raw DatasetInfo::source_name: no display name, no "name (2)" ordinal. Layout identity
  // resolution falls back to it when a saved DatasetId was reminted.
  [[nodiscard]] std::optional<QString> datasetSourceName(DatasetId dataset_id) const;
  /// The dataset's label (DataEngine::datasetLabel): the name the tree, the plugin catalog snapshot and every
  /// "label:topic/field" qualifier use. A session name for replies a caller may send back, never for persistence.
  [[nodiscard]] std::optional<QString> datasetLabel(DatasetId dataset_id) const;
  /// The dataset labelled `label` (DataEngine::datasetByLabel).
  [[nodiscard]] std::optional<DatasetId> datasetByLabel(const QString& label) const;

  // Resolves a saved curve identity (topic+field, optionally an exact DatasetId, a raw source label, a full path) to
  // the live key, never guessing by load order. The one resolver for layout/undo restore (PendingDisplayBinder) and
  // clipboard paste (PlotWidget):
  //   * Qualified: resolveDatasetIdentity names a dataset and the field binds there only (never a sibling's). When the
  //     exact id failed and a path is present, the path outranks the source label: bind when exactly one path sibling
  //     provides the series.
  //   * Unqualified: bind only when topic+field is unique across datasets.
  // nullopt under ambiguity, a missing series or no session. `capability`: a plot curve never binds a string field,
  // a strip series never a float.
  [[nodiscard]] std::optional<QString> resolveCurveKey(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path, const QString& topic,
      const QString& field, SeriesCapability capability = SeriesCapability::kPlottable) const;
  // The same resolution for an identity held as one value (a parsed layout entry).
  [[nodiscard]] std::optional<QString> resolveCurveKey(
      const SavedSeriesPath& path, SeriesCapability capability = SeriesCapability::kPlottable) const;

  // SessionManager::resolveDatasetIdentity behind the catalog facade, so binding code needs no FileLoader. Empty
  // without a session.
  [[nodiscard]] DatasetIdentityResolution resolveDatasetIdentity(
      DatasetId saved_id, const QString& saved_source, const QString& saved_path = {}) const;
  // Empty without a session or a file-backed path.
  [[nodiscard]] QString datasetSourcePath(DatasetId dataset_id) const;
  // An owned copy of SessionManager::datasetMetadata; nullopt when none.
  [[nodiscard]] std::optional<DatasetMetadata> datasetMetadata(DatasetId dataset_id) const;

  // A topic+field path within one dataset: lets a layout rebind across similar datasets whose per-load keys differ.
  [[nodiscard]] std::optional<CurveDescriptor> descriptorForPath(
      DatasetId dataset_id, const QString& topic, const QString& field,
      SeriesCapability capability = SeriesCapability::kPlottable) const;

  // tombstone=true hides every dataset id from later rebuilds while its data lingers in the engine; pass false when
  // the caller also erases the data.
  void clearAll(bool tombstone = true);
  void removeItems(const std::vector<QString>& keys);
  void removeCurves(const std::vector<QString>& keys);

  // tombstone=true hides the id from later rebuilds (the data stays: reload swap, speculative and rollback callers;
  // undo via restoreDataset); pass false when the caller also erases the data. Emits cleared() if the catalog empties,
  // else one itemsRemoved; false when the dataset had no items. Media eviction is the caller's job.
  bool removeDataset(DatasetId dataset_id, bool tombstone = true);

  // Drops the tombstones and rebuilds, so a layout load can show removed curves again.
  void resetRemovalState();

  // Restores one dataset hidden by removeDataset().
  void restoreDataset(DatasetId dataset_id);

  // Relabels the dataset (DataEngine::setDatasetLabel): a DataSource names its dataset instead of the file's basename,
  // a merge names its anchor. Made unique like any label; empty returns to the source name. The label lives in the
  // engine, so it survives rebuilds, tombstones and restores. DatasetInfo::source_name is untouched, so dataset reuse
  // (by source_name) is unaffected.
  void setDatasetDisplayName(DatasetId dataset_id, const QString& display_name);

  // The complete advertised set of a streaming dataset: each topic is a data-less paused placeholder unless a real
  // entry of that name exists. Creates no storage. GUI thread (producers marshal). Emits itemsAdded/itemsRemoved.
  void setAdvertisedTopics(DatasetId dataset_id, const std::vector<AdvertisedTopic>& topics);

  void clearAdvertisedTopics(DatasetId dataset_id);

  // Set by StreamingSourceManager for a source with kCapabilityPerTopicPause. Without it the source sends everything,
  // so a topic missing from TopicDemandTracker's active set is not a pause: consumers gate on this first.
  void setPerTopicPauseCapable(DatasetId dataset_id, bool capable);
  [[nodiscard]] bool isPerTopicPauseCapable(DatasetId dataset_id) const;

  // Rebuilds only when the content fingerprint moved (most ingest batches only add rows). A created or retired topic
  // always moves it, so it also serves a caller that may have changed no topics. Synchronous: a caller reads the
  // updated catalog on return.
  void rebuildIfChanged();

  /// The least time between two rebuilds that a commit triggers while a file ingest is open (see the samplesIngested
  /// connection in the constructor); the gap is also at least four times the last rebuild's cost.
  void setMinIngestRebuildGap(std::chrono::milliseconds gap);

 public slots:
  void rebuildFromDatastore();

 signals:
  // Once per rebuild, so views sort once.
  void itemsAdded(const std::vector<CatalogItem>& items);
  void itemAdded(const CatalogItem& item);
  // One emission per removal operation (dataset, multi-key trash, keys gone on a rebuild).
  void itemsRemoved(const QStringList& keys);
  void cleared();
  // Same key, different item (a float field turning discrete, a reload retyping a column): consumers resolving keys by
  // capability re-resolve.
  void itemsChanged(const QStringList& keys);

 private:
  // The rebuild body; rebuildFromDatastore() also refreshes the fingerprint, so the gate stays in sync.
  void rebuildNow();

  // Engine structure (datasets, topics, column counts, retired columns, object topics, dataset labels) plus tombstones:
  // equal fingerprints mean an identical rebuild.
  [[nodiscard]] std::uint64_t catalogFingerprint() const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
