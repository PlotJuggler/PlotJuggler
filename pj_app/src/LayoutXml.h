#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_runtime/SavedSeriesPath.h"

namespace PJ::layout_xml {

// Leading dot included. The double extension keeps the files XML to editors while marking them as PJ4 layouts.
inline constexpr char kLayoutExtension[] = ".pj4.xml";

// Appends kLayoutExtension only to a path with no extension at all; empty stays empty. Backstops the Save
// dialog's defaultSuffix against platform dialog quirks.
[[nodiscard]] QString ensureLayoutExtension(const QString& path);

// True when serialized browser-layout bytes still hold a page-lifetime upload identity or its MEMFS path; every
// browser layout download refuses such bytes (they name nothing outside this page).
[[nodiscard]] bool containsEphemeralBrowserPath(const QByteArray& serialized);

// Source Timeline state of one dataset a file fanned out into. source_index is its position in the fan-out order,
// so two members with the same display name cannot swap offsets; -1 when absent.
struct DataSourceDatasetRef {
  QString source_name;
  int source_index = -1;
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  // See DataSourceRef::display_offset_includes_global_reference.
  bool display_offset_includes_global_reference = false;
  int timeline_order = -1;
};

/// Binds each saved <fileInfo>/<dataset> child to a live candidate, each candidate used at most once. Returns one
/// DatasetId per saved child, aligned by index; 0 = missing or ambiguous, so a sibling never inherits an offset that
/// is not provably its own. A name shared by several saved children binds by source_index only, and only while the
/// saved and live counts of that name agree; a unique name matches by name, else by source_index (whose live name
/// must equal the saved one when saved). `source_name_of` gives a candidate's live raw source name.
[[nodiscard]] std::vector<std::uint32_t> matchFanoutDatasets(
    const QList<DataSourceDatasetRef>& saved, const std::vector<std::uint32_t>& candidates,
    const std::function<QString(std::uint32_t)>& source_name_of);

// A file a source's plugin read besides its data file (e.g. a dictionary picked in its dialog): a <sideFile> child
// of <fileInfo>. `filename` is the exact string the saved plugin configuration holds, relative to the data file's
// directory; browser replay asks for a file of that basename and rewrites every configuration string equal to it.
// Fingerprint rules are DataSourceRef::content_sha256's.
struct SideFileRef {
  QString filename = {};
  QString content_sha256 = {};
  bool content_sha256_malformed = false;
};

// A staged browser file: its sanitized name and content fingerprint (empty while unknown).
struct StagedFileInfo {
  QString name = {};
  QString content_sha256 = {};
};

struct LogicalSideFiles {
  std::string config = {};
  QList<SideFileRef> side_files = {};
};

// Prepares a browser source's plugin configuration for a source-bound save. Every configuration string other than
// `data_path` that `staged_file` knows is a side file: replaced by its sanitized name (or the first free
// `pj4-side/<n>/<name>` when that name collides with a configuration string or `data_logical_path`) and listed with
// its fingerprint; one file picked twice shares one entry. A string equal to the data file's directory becomes
// empty (the staging folder means nothing outside this page). Fails, setting `error`, when a side file has no
// fingerprint. A non-object configuration comes back verbatim. The data file's own path is left to
// rewriteReplayFilepaths.
[[nodiscard]] std::optional<LogicalSideFiles> logicalBrowserSideFiles(
    std::string_view config, const QString& data_path, const QString& data_logical_path,
    const std::function<std::optional<StagedFileInfo>(const QString&)>& staged_file, QString* error);

// One <previouslyLoaded_Datafiles>/<fileInfo>. serialized_path is the filename attribute verbatim, so browser
// replay can match it to a new upload without inventing a base directory; resolved_path anchors a relative path
// at the layout directory and keeps a pj-upload:// identity literal.
struct DataSourceRef {
  QString serialized_path;
  QString resolved_path;
  QString prefix;
  // Browser-written fingerprint of the source bytes; only canonical lower-case SHA-256 is read. Browser replay uses
  // it when two sources share a basename.
  QString content_sha256;
  // A non-canonical content_sha256 was given (content_sha256 stays empty); a downloaded layout refuses the source.
  bool content_sha256_malformed = false;
  QString plugin_id;           // Empty when the layout had no <plugin> child.
  QString plugin_config_json;  // Empty when the layout had no <plugin> child.
  // <plugin manifest_id>, written next to the display name (ID, which older readers use); the loader matches by
  // manifest id first, then display name. May be empty.
  QString plugin_manifest_id;
  // Optional <materialize> child of <fileInfo>: the provider plugin that can re-obtain this source, its durable
  // identity there, and the canonical descriptor JSON (CDATA). The descriptor bytes are a cross-repo identity
  // contract: keep them byte-exact, never re-serialize. All empty without the child.
  QString materialize_provider;
  QString materialize_identity;
  QString materialize_descriptor_json;
  // Source Timeline state, re-bound by source path (DatasetIds are re-minted each session). display = raw -
  // display_offset_ns; without has_display_offset the dataset keeps a zero offset. timeline_order is the bar's
  // top-to-bottom slot, -1 = load order. These flat fields mirror the first <dataset> child for older readers;
  // per-dataset state is in `datasets`.
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  // pj4_version < 4 saved SessionManager::displayOffset(), which includes the global relative-time reference;
  // v4+ saves sourceDisplayOffset(). True for an older read: the apply path subtracts the current global reference.
  bool display_offset_includes_global_reference = false;
  int timeline_order = -1;
  // One entry per fan-out dataset; empty for a pre-v4 layout, whose state is only in the flat fields.
  QList<DataSourceDatasetRef> datasets;
  // Browser-saved side files, in document order; empty for every other layout.
  QList<SideFileRef> side_files;
};

// The one predicate both halves of layout-restore classification use: the import batch claims these sources and
// the plain reload loop skips them, so they must not diverge.
[[nodiscard]] inline bool hasMaterializeRecord(const DataSourceRef& ref) {
  return !ref.materialize_provider.isEmpty() || !ref.materialize_identity.isEmpty() ||
         !ref.materialize_descriptor_json.isEmpty();
}

// One DataSourceRef per <fileInfo> with a filename, in document order; relative paths anchor at `layout_dir`.
[[nodiscard]] QList<DataSourceRef> extractDataSource(const QDomDocument& doc, const QDir& layout_dir);

// A downloaded layout's <fileInfo filename> (raw serialized_path) resolved against `layout_url`. A relative
// filename is a path, not URL syntax ('#', '?', '%' are part of the name); an absolute http(s) URL is kept. Empty
// URL, reason in `*error` (may be null), for an empty filename, a local machine path (absolute POSIX, drive
// letter, UNC, any backslash), another scheme, a directory, or a non-http(s) `layout_url`: a web layout never
// reaches local files. A WASM fetch reports the requested URL even after a redirect, so a layout behind a redirect
// to another directory resolves against the requested one.
[[nodiscard]] QUrl resolveFileSourceUrl(const QString& filename, const QUrl& layout_url, QString* error);

// The pj4_version of a layout carrying <previouslyLoaded_StreamingSources>, and the newest version this build
// reads. writeStreamingSources stamps it on the root; every other layout keeps the version below it, so an older
// build warns about exactly the layouts whose stream records it would drop.
inline constexpr int kStreamingLayoutSchemaVersion = 5;

// `pj-stream://<id>`: the *_dataset_path qualifier of everything bound to the stream record `stream_id`. It names
// the record, not a session: each replay starts fresh sessions under fresh DatasetIds and maps them back through
// it. Opaque, like a pj-upload:// identity: never resolved as a path, compared literally by isSamePath.
[[nodiscard]] QString streamSourceIdentity(const QString& stream_id);

// One <previouslyLoaded_StreamingSources>/<streamInfo>: a streaming plugin started with a saved configuration.
// `id` is unique within the layout and exists only to bind qualifiers to the right record when several streams
// share a plugin; it is not a connection-reuse key. plugin_config_json is the saved TEMPLATE, byte-exact: it may
// name deployment variables (pj_runtime's expandSourceConfigVariables), and a re-save must write it back
// unexpanded. Plugin identity follows <fileInfo>/<plugin>: manifest id first, display name as the fallback.
struct StreamingSourceRef {
  QString id;
  QString source_name;  // display name of the stream; may be empty
  QString plugin_id;
  QString plugin_manifest_id;
  QString plugin_config_json;
  // Source Timeline state from an optional <dataset> child; defaults (no offset, order -1) when absent.
  DataSourceDatasetRef dataset;
};

// Reads every <streamInfo>, in document order; an absent block is an empty list without error. One invalid record
// (no id, a repeated id, no <plugin> or a second one, a plugin with neither ID nor manifest_id, an empty
// configuration) or a second block rejects everything: the result is empty and `*error` (may be null) describes the
// first problem, so a layout never replays part of its stream set. A record's <dataset> offset is never read under
// the pre-v4 basis, whatever the root's pj4_version (hand-written presets often omit it).
[[nodiscard]] QList<StreamingSourceRef> extractStreamingSources(const QDomDocument& doc, QString* error);

// Appends <previouslyLoaded_StreamingSources> to the root and stamps kStreamingLayoutSchemaVersion; an empty list
// writes nothing and leaves the version alone. Each configuration is written as given (keep the template). Returns
// false, writing nothing and describing the first problem in `*error` (may be null), for anything
// extractStreamingSources would reject: an invalid record, or a document that already carries a block.
[[nodiscard]] bool writeStreamingSources(QDomDocument& doc, const QList<StreamingSourceRef>& sources, QString* error);

// True when the layout is source-bound (its binding is not "generic") and records a file or a streaming source.
// Such a layout carries plugin configuration, so the browser never retains it as a durable recipe.
[[nodiscard]] bool isSourceBoundLayout(const QDomDocument& doc);

// The <source_timeline> element (view state only; offsets and order are DataSourceRef's). An absent field leaves
// that aspect of the widget untouched on restore.
struct SourceTimelineViewState {
  std::optional<double> zoom;            // pixels per ns; only > 0 is valid
  std::optional<qint64> scroll_left_ns;  // display time at the viewport's left edge
  std::optional<int> scroll_top_px;      // only >= 0 is valid
  std::optional<int> name_column_width;  // px; only > 0 is valid
  std::optional<bool> snap;
};

// Write emits only the set fields; read leaves a field nullopt when its attribute is absent or invalid.
[[nodiscard]] QDomElement writeSourceTimelineViewState(QDomDocument& doc, const SourceTimelineViewState& state);
[[nodiscard]] SourceTimelineViewState readSourceTimelineViewState(const QDomElement& element);

// Filesystem paths compare by canonical file (relative/absolute and symlinks collapse); pj-upload:// and
// pj-stream:// identities are opaque and compare literally. An empty input is never the same.
[[nodiscard]] bool isSamePath(const QString& a, const QString& b);

// Stable identity of a series: topic + field, optionally qualified by a live DatasetId, a raw source label and a
// full file path, so same-shaped datasets are not interchanged; an unqualified path rebinds only when unique.
// Topic and field stay separate because a field path may contain '/'.
struct SeriesPath : SavedSeriesPath {
  SeriesPath(
      QString topic_in = {}, QString field_in = {}, std::uint32_t dataset_id_in = 0, QString dataset_source_in = {},
      QString dataset_path_in = {})
      : SavedSeriesPath{
            std::move(topic_in), std::move(field_in), dataset_id_in, std::move(dataset_source_in),
            std::move(dataset_path_in)} {}

  // "[source:]topic/field", for missing-curve lists.
  [[nodiscard]] QString display() const;
};

// The only readers of <curve> path attributes (plain, x_- and y_-prefixed): an out-of-range or non-numeric dataset
// id decays to 0 (unqualified). nullopt when the topic attribute is missing.
[[nodiscard]] std::optional<SeriesPath> readTimeSeriesPath(const QDomElement& curve);
[[nodiscard]] std::optional<SeriesPath> readXyXPath(const QDomElement& curve);
[[nodiscard]] std::optional<SeriesPath> readXyYPath(const QDomElement& curve);

// SeriesPath -> catalog key in the target dataset; nullopt when it has no such topic+field.
using SeriesKeyResolver = std::function<std::optional<QString>(const SeriesPath&)>;

// Distinct SeriesPaths of every <plot>/<curve> (time-series and both XY axes), in document order.
[[nodiscard]] QList<SeriesPath> extractSeriesPaths(const QDomDocument& doc);

// Rewrites each <curve>'s key attributes (name; curve_x/curve_y for XY) through `resolve`. An unresolved key is
// cleared and its path returned (de-duplicated) for the caller to prompt about or strip.
[[nodiscard]] QList<SeriesPath> rebindCurveKeys(QDomDocument& doc, const SeriesKeyResolver& resolve);

// A saved dataset and the file startup's explicit `-d` files give it.
struct DatasetFileMatch {
  enum class Rule : std::uint8_t {
    kPath,      ///< the saved path is that file
    kName,      ///< the only unclaimed file with the saved file name (or the path-less label)
    kPosition,  ///< the n-th dataset left over paired with the n-th file left over of the same format
    kAlias,     ///< a path-less label of a dataset matched by its path: follows it, not a dataset of its own
  };
  QString identity;  ///< the saved path, or the source label of a qualifier without one
  QString label;     ///< for messages: the saved source label, else the saved file name
  QString file;
  Rule rule = Rule::kPath;
};

// Maps the saved datasets of a layout to `files` (absolute, in command-line order), each file at most once:
//  1. a saved path that canonically is one of the files;
//  2. the only file still unclaimed with the saved file name (for a path-less label, the label);
//  3. per format (case-insensitive suffix), the saved paths and files still left, paired in order when their counts
//     are equal. A path-less label never pairs: it has no format and may name no file at all.
// Datasets are ordered by `source_order` (the layout's <fileInfo> paths, resolved), then by first appearance in the
// document. Opaque identities (pj-upload://, pj-stream://) never match. Run after resolveDatasetSourcePaths.
[[nodiscard]] QList<DatasetFileMatch> matchDatasetsToFiles(
    const QDomDocument& doc, const QStringList& files, const QStringList& source_order);

// Makes a generic layout: removes every dataset qualifier attribute (id, source, path; all five prefixes) from every
// element. What remains binds only when unique in the loaded data; every loader (plots, processors, scenes, Custom
// Widget wires, State Transitions) implements that unique-only fallback. An on-demand <input> name keeps its
// qualifier text: it is the alias its script reads.
//
// A qualifier whose identity is in `keep` keeps its binding: its path becomes the matched file, and a label the
// loader derived from the saved file name follows the new name (the resolver compares labels exactly). Its id is
// still removed.
void removeDatasetQualifiersForGenericLayout(QDomDocument& doc, const QList<DatasetFileMatch>& keep = {});

// A dataset a layout's qualifiers name: its saved path and source label, either possibly empty.
struct DatasetIdentity {
  QString path;
  QString source;
  // A browser upload or stream record: names a session's source, never a loaded file.
  [[nodiscard]] bool isOpaque() const;
  friend bool operator==(const DatasetIdentity&, const DatasetIdentity&) = default;
};

// The distinct datasets the layout's qualifiers name, in document order. The entries of one fan-out file share its
// path and differ by label, so each (path, label) pair is a dataset; a qualifier carrying only the path, or only the
// label, of another names that one.
[[nodiscard]] QList<DatasetIdentity> datasetIdentities(const QDomDocument& doc);

// Every topic the layout's elements read, under any qualifier shape, except the outputs of its own recipes (filters,
// transforms, on-demand recipes, marker rules).
[[nodiscard]] QSet<QString> referencedTopics(const QDomDocument& doc);

// A loaded dataset a layout is pinned to: `id`, `source` and `path` become the qualifier, `label` (the session's unique
// dataset label) names the copy.
struct DatasetPin {
  std::uint32_t id = 0;
  QString source;
  QString path;  ///< empty for a dataset with no file
  QString label;
};

// What planDatasetChoice knows of the loaded data (the shell fills it from CatalogModel).
struct LoadedDatasets {
  QList<DatasetPin> datasets;  ///< in load order
  // Calls `visit(dataset id, topic name)` for every topic of every loaded dataset.
  std::function<void(const std::function<void(std::uint32_t, const QString&)>& visit)> for_each_topic;
  // Whether `identity` names exactly one loaded dataset.
  std::function<bool(const DatasetIdentity& identity)> names_one;
};

// How a load binds a layout that may fit several loaded datasets. The rules and their reasons: pj_app ARCHITECTURE
// "One layout over several datasets".
struct DatasetChoice {
  enum class Kind : std::uint8_t {
    kKeep,  ///< apply the layout as it is
    kPin,   ///< pinLayoutToDataset(doc, datasets.front(), topics): one dataset fits best
    kAsk,   ///< `datasets` fit equally well: pinLayoutToDatasets on those picked
  };
  Kind kind = Kind::kKeep;
  QList<DatasetPin> datasets;
  QSet<QString> topics;  ///< kPin: the layout's topics datasets.front() carries
  QStringList blockers;  ///< kAsk: duplicationBlockers(doc)
};

// `saved`: the datasets the file named, when a load step (startup's explicit files) rewrote its qualifiers since;
// nullopt reads them from `doc`.
[[nodiscard]] DatasetChoice planDatasetChoice(
    const QDomDocument& doc, const std::optional<QList<DatasetIdentity>>& saved, const LoadedDatasets& loaded);

// What stops pinLayoutToDatasets from duplicating the layout across datasets, "<owner>/<id>" each: a transform (its id
// is also its script's class id, and unique per owner across datasets, so a copy cannot take another) and an on-demand
// recipe (its script reads its inputs and upstream outputs by name). Empty when the layout can be duplicated.
[[nodiscard]] QStringList duplicationBlockers(const QDomDocument& doc);

// Pins each element that reads one of `topics`, and each that reads data without naming a topic (a marker rule or
// scope, a recipe input), to `pin`; every other qualifier is removed, so those references bind where their topic is
// unique.
void pinLayoutToDataset(QDomDocument& doc, const DatasetPin& pin, const QSet<QString>& topics);

// The colour the session remembers for a <curve> that saved none, if any.
using RememberedCurveColor = std::function<std::optional<QColor>(const QDomElement& curve)>;

// Pins every element that reads data to `pins.front()`, replacing any qualifier it had and giving one to those
// without (a generic layout). With several pins, each <Tab> becomes one copy per pin, side by side, named
// "<tab> [<label>]", and each filter and dataset-bound marker rule one copy per pin, each copy pinned to its dataset
// and keeping its output name or rule id (both are per dataset). The tab copies past the first are the user's
// (plot_xml::prepareTabCopy) and take tab, area and plot ids new to the document. Before copying, a curve with no
// saved colour gets the one its plot would give it (plot_xml::fillMissingCurveColors, `remembered` first), so every
// copy shows it in the same colour. Several pins need duplicationBlockers(doc) to be empty.
void pinLayoutToDatasets(QDomDocument& doc, const QList<DatasetPin>& pins, const RememberedCurveColor& remembered = {});

// DatasetId -> its FileLoader full source path; empty for a non-file or unknown dataset.
using DatasetPathLookup = std::function<QString(std::uint32_t)>;

// Writes `*_dataset_path` next to every `*_dataset_id` of curves, processor and transform inputs, marker
// <generator>s, <marker_scope> rows and Custom Widget inputs, overwriting an existing one. Scene docks and State
// Transitions rows are not visited: they write the absolute path themselves. An id the lookup cannot place is left
// as is.
void stampDatasetSourcePaths(QDomDocument& doc, const DatasetPathLookup& lookup);

// Strips every non-zero dataset id without a path qualifier, on the elements stampDatasetSourcePaths visits: such an
// id is an in-session hint, and a later session could accept a reminted id whose source label happens to match.
// 0 is a local-scene sentinel and stays; scene ids are not visited, so restored scene layers keep binding.
void removeUnvalidatedDatasetIds(QDomDocument& doc);

// Anchors relative `*_dataset_path` qualifiers at the layout directory (so a source-bound layout moves with its
// data); absolute ones are only cleaned. Run before any restore reads them.
void resolveDatasetSourcePaths(QDomDocument& doc, const QDir& layout_dir);

// Rewrites every non-empty dataset_path, x_dataset_path, y_dataset_path, input_dataset_path and
// source_dataset_path through `remap`. No filesystem access.
using DatasetPathRemapper = std::function<QString(const QString&)>;
void remapDatasetSourcePaths(QDomDocument& doc, const DatasetPathRemapper& remap);

// Save-side inverse of resolveDatasetSourcePaths: passes every absolute filesystem qualifier remapDatasetSourcePaths
// visits through `relocate`. Relative values and "://" identities are skipped, so `relocate` need not be
// idempotent. Works by value, so it also reaches the qualifiers widgets write themselves.
void relativizeDatasetSourcePaths(QDomDocument& doc, const DatasetPathRemapper& relocate);

// Sets `<prefix>dataset_path` to identity_for_dataset[id] wherever `<prefix>dataset_id` is a key, on every element
// (scene ones included): how a save names data with no file, such as a stream (`pj-stream://<id>`).
void stampDatasetIdentities(QDomDocument& doc, const QHash<std::uint32_t, QString>& identity_for_dataset);

// Load-side inverse for streams: a qualifier whose `<prefix>dataset_path` is a key becomes that live DatasetId
// (source and path dropped), and each <curve>, <layer> and <config_topic> so bound becomes a pending intent, since
// a stream's topics arrive after the layout applied. A bound <layer> payload (robot model, trail) makes the layer
// pending and gives an unqualified layer the payload's `dataset_id`, which its pending demand names. Run after
// removeUnvalidatedDatasetIds, which would strip the path-less ids written here.
void bindStreamQualifiers(QDomDocument& doc, const QHash<QString, std::uint32_t>& dataset_for_identity);

// Rewrites, through `remap`, the serialized filename of exactly the <fileInfo> elements extractDataSource yields.
void remapFileInfoFilenames(QDomDocument& doc, const DatasetPathRemapper& remap);

// After rebindCurveKeys: removes every <curve> that is not a pending intent and has neither a name nor both
// curve_x and curve_y.
void stripUnresolvedCurves(QDomDocument& doc);

// Gives every <plot>/<range> without x_basis one ("value" for XY, "absolute" for time series: time ranges are
// saved in absolute seconds), changing no numbers. Run before any widget restore reads the document.
void normalizePlotRangeBasis(QDomDocument& doc);

}  // namespace PJ::layout_xml
