#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "pj_runtime/SavedSeriesPath.h"

namespace PJ::layout_xml {

// Canonical layout-file extension, leading dot included. Double extension
// so the files read as XML to editors/tools while staying identifiable as
// PJ4 layouts. Single source of truth for the dialog filter, the dialog's
// default suffix, and ensureLayoutExtension below.
inline constexpr char kLayoutExtension[] = ".pj4.xml";

// Returns `path` with kLayoutExtension appended, but only when it carries
// no extension at all (QFileInfo::suffix() empty). A path the user already
// typed an extension for — including ".pj4.xml" — is returned unchanged.
// Backstops the Save dialog's defaultSuffix so a bare typed name always
// lands as a .pj4.xml file regardless of platform dialog quirks. Empty in,
// empty out.
[[nodiscard]] QString ensureLayoutExtension(const QString& path);

// Returns true when serialized browser-layout bytes still contain a
// page-lifetime upload identity or its MEMFS backing path. Both generic and
// source-bound browser downloads must reject such bytes before handing them to
// the browser; native layout files are not subject to this policy.
[[nodiscard]] bool containsEphemeralBrowserPath(const QByteArray& serialized);

// Per-dataset Source Timeline state nested under one replayable file. One file
// may fan out into several DatasetIds; source_index is the stable position in
// that file's fan-out order (so two fan-out members with identical display
// names can't swap offsets) and source_name disambiguates ordinary unique names.
struct DataSourceDatasetRef {
  QString source_name;
  int source_index = -1;
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  // Schema v3 wrote SessionManager::displayOffset() (per-source alignment +
  // global reference); v4 writes sourceDisplayOffset() only. Set true for a v3
  // read so the apply path subtracts the current global reference — see
  // DataSourceRef::display_offset_includes_global_reference.
  bool display_offset_includes_global_reference = false;
  int timeline_order = -1;
};

/// Binds each saved <fileInfo>/<dataset> fan-out child to a live candidate
/// dataset, consuming each candidate at most once. Returns one DatasetId per
/// saved child (dataset ids as raw std::uint32_t, this header's convention),
/// aligned by index (0 = unmatched: missing or ambiguous — a
/// surviving sibling must never inherit an offset that cannot be proven its
/// own). Policy: a name shared by SEVERAL saved children binds by
/// source_index ONLY, and only while the same-named fan-out shape is
/// unchanged (saved count == candidate count for that name); a name-unique
/// child matches by name first, falling back to its source_index (which must
/// agree with the saved name when one is present). `source_name_of` supplies
/// each candidate's live raw source name.
[[nodiscard]] std::vector<std::uint32_t> matchFanoutDatasets(
    const QList<DataSourceDatasetRef>& saved, const std::vector<std::uint32_t>& candidates,
    const std::function<QString(std::uint32_t)>& source_name_of);

// Data-source reference extracted from <previouslyLoaded_Datafiles>.
// serialized_path preserves the filename attribute exactly as written so a
// browser source-bound layout can match it to a newly selected upload without
// inventing a filesystem base directory. resolved_path is the desktop-ready
// form: relative filesystem paths are anchored at the layout directory, while
// pj-upload:// identities are preserved literally. Empty resolved_path means
// no replayable source was found in the layout.
struct DataSourceRef {
  QString serialized_path;
  QString resolved_path;
  QString prefix;
  // Optional browser-authored fingerprint of the selected source bytes. Only
  // canonical lower-case SHA-256 is accepted on read; native layouts omit it.
  // Browser replay consults it when two logical sources share a basename.
  QString content_sha256;
  // The layout gave a content_sha256 that is not canonical, so content_sha256
  // stayed empty. A downloaded layout refuses such a source: it promised a
  // fingerprint that nothing can be checked against.
  bool content_sha256_malformed = false;
  QString plugin_id;           // Empty when the layout had no <plugin> child.
  QString plugin_config_json;  // Empty when the layout had no <plugin> child.
  // Stable plugin identity from <plugin manifest_id="...">. New layouts write
  // BOTH the display name (ID — what old readers keep using) and this manifest
  // id; the loader matches by manifest id first, falling back to the display
  // name. Empty for layouts written before the attribute existed.
  QString plugin_manifest_id;
  // Provider-generic source record from an optional <materialize> child of
  // <fileInfo> (a sibling of <plugin>): the provider plugin able to re-obtain
  // this source (provider attribute), that provider's durable identity for it
  // (identity attribute), and the full canonical descriptor JSON (the CDATA
  // payload, preserved byte-exact — the bytes are a cross-repo identity
  // contract and must never be re-serialized). All three are empty when the
  // layout carries no <materialize> child (every pre-existing layout).
  QString materialize_provider;
  QString materialize_identity;
  QString materialize_descriptor_json;
  // Source Timeline state, re-bound by source path on reload (DatasetIds are
  // re-minted each session, so the file path is the only stable identity).
  // display_offset_ns is the per-source display shift (display = raw - offset);
  // has_display_offset is false when the layout predates this attribute, so the
  // reloaded dataset keeps its natural zero offset. timeline_order is the bar's
  // top-to-bottom slot in the timeline (-1 when absent → fall back to load order).
  // These flat fields mirror the first <dataset> child (legacy single-track view
  // for older readers); the per-dataset fan-out state lives in `datasets`.
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  // Schema v3 wrote SessionManager::displayOffset(), which included the global
  // relative-time reference. Schema v4 writes sourceDisplayOffset() only. The
  // apply path subtracts its current global reference for this read-only
  // migration, keeping total placement equal to the v3 value without
  // double-applying it. Set true only for a v3 (or older) read.
  bool display_offset_includes_global_reference = false;
  int timeline_order = -1;
  // Schema-v4 additive extension: one entry per dataset the file fanned out
  // into. Empty means an older (≤v3) layout whose single legacy timeline state
  // lives in the flat fields above.
  QList<DataSourceDatasetRef> datasets;
};

// True when `ref` carries a provider <materialize> record (any of the three
// materialize_* fields non-empty). This is the ONE predicate both halves of
// the layout-restore classification key off — the import batch claims these
// sources and the plain reload loop skips them — so it lives here, next to
// the fields, where the two sides cannot diverge.
[[nodiscard]] inline bool hasMaterializeRecord(const DataSourceRef& ref) {
  return !ref.materialize_provider.isEmpty() || !ref.materialize_identity.isEmpty() ||
         !ref.materialize_descriptor_json.isEmpty();
}

// Reads every <previouslyLoaded_Datafiles>/<fileInfo> and its optional
// <plugin> child, one DataSourceRef per file in document order. Each
// serialized_path is the raw filename attribute. resolved_path is absolute for
// filesystem paths (relatives are anchored at `layout_dir`) and literal for a
// pj-upload:// identity.
// fileInfo entries with no filename are skipped. Returns an empty list when
// the wrapper element is absent or holds no usable fileInfo. Multiple entries
// support multi-file sessions; single-file layouts yield a one-element list.
[[nodiscard]] QList<DataSourceRef> extractDataSource(const QDomDocument& doc, const QDir& layout_dir);

// Where a downloaded layout's <fileInfo filename> lives: `filename` (a raw
// serialized_path) resolved against `layout_url`, the http(s) URL the layout
// was fetched from. A relative filename is a path, never URL syntax ('#', '?'
// and '%' stay part of the name); an absolute http(s) URL is used as is.
// Returns an empty URL, describing why in `*error` (may be null), for an empty
// filename, a local machine path (absolute POSIX, drive letter, UNC, any
// backslash), another scheme (file:, pj-upload:, ...), a reference that names
// a directory, or a `layout_url` that is not http(s): a web-hosted layout never
// points the app at local files. Browser caveat: a WASM fetch reports the
// REQUESTED URL even after a redirect, so a layout served through a redirect to
// another directory resolves against the requested one.
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

// Source Timeline view chrome persisted as the <source_timeline> element: pure
// view state, independent of the per-source offsets/order (those round-trip via
// DataSourceRef). Each field is optional so a layout that omits an attribute — or
// predates it — leaves that aspect of the widget untouched on restore.
struct SourceTimelineViewState {
  std::optional<double> zoom;            // pixels-per-ns (Ctrl+wheel zoom); only > 0 is valid
  std::optional<qint64> scroll_left_ns;  // display-ns at the viewport's left edge
  std::optional<int> scroll_top_px;      // vertical viewport offset (px); only >= 0 is valid
  std::optional<int> name_column_width;  // left name-column width (px); only > 0 is valid
  std::optional<bool> snap;              // edge-snap-while-dragging toggle
};

// Serialize / parse the <source_timeline> element. write builds a fresh element
// on `doc`, emitting only the set fields (zoom at 17 sig-figs so the ~1e-7
// pixels-per-ns round-trips exactly). read pulls the attributes back, leaving a
// field nullopt when its attribute is absent or malformed (zoom/width also
// require a positive value). Pure Qt-DOM, no widget — the host applies the parsed
// state to the widget, keeping the encode/decode unit-testable.
[[nodiscard]] QDomElement writeSourceTimelineViewState(QDomDocument& doc, const SourceTimelineViewState& state);
[[nodiscard]] SourceTimelineViewState readSourceTimelineViewState(const QDomElement& element);

// True iff both inputs identify the same source. Filesystem paths compare as
// the same on-disk file; pj-upload:// and pj-stream:// identities compare literally. Used by the
// layout-load data-source replay to decide whether the currently
// loaded source is the one the layout references (and re-load is a
// no-op) or a different file (and the user should be prompted).
// Files compare via QFileInfo::canonicalFilePath, so relative-vs-absolute,
// trailing-slash, and symlink variations all collapse correctly. Browser upload
// identities are deliberately not normalized: their tokens are opaque
// and session-scoped.
// Empty inputs are treated as "not the same".
[[nodiscard]] bool isSamePath(const QString& a, const QString& b);

// Stable identity of a series: topic + field path, optionally qualified by an
// exact live DatasetId, a portable raw source label, and a full file path. The
// qualifiers stop same-shaped datasets from being interchanged during undo,
// while unqualified legacy layouts still rebind when the structural path is
// globally unique. This replaces the engine's opaque per-load catalog key
// (CurveDescriptor::name); topic and field stay separate because PJ4 field
// paths can themselves contain '/'. The explicit constructor keeps existing
// two-argument brace-initializations compiling.
struct SeriesPath : SavedSeriesPath {
  SeriesPath(
      QString topic_in = {}, QString field_in = {}, std::uint32_t dataset_id_in = 0, QString dataset_source_in = {},
      QString dataset_path_in = {})
      : SavedSeriesPath{
            std::move(topic_in), std::move(field_in), dataset_id_in, std::move(dataset_source_in),
            std::move(dataset_path_in)} {}

  // Human-readable form for missing-curve lists: "topic/field".
  [[nodiscard]] QString display() const;
};

// Read one curve's SeriesPath off a <curve> element, honoring the range-checked
// dataset-id parse (an out-of-range or non-numeric id decays to 0/unqualified).
// These are the single reader for curve attributes so hand-built SeriesPath call
// sites (e.g. PendingDisplayBinder) cannot drift from the range-check.
//   * readTimeSeriesPath: the plain topic/field + dataset_* qualifiers.
//   * readXyXPath / readXyYPath: the x_/y_-prefixed XY-axis qualifiers.
// nullopt when the element lacks the relevant topic attribute (no stable identity).
[[nodiscard]] std::optional<SeriesPath> readTimeSeriesPath(const QDomElement& curve);
[[nodiscard]] std::optional<SeriesPath> readXyXPath(const QDomElement& curve);
[[nodiscard]] std::optional<SeriesPath> readXyYPath(const QDomElement& curve);

// Resolves a stable SeriesPath to a concrete catalog key within the target
// dataset, or std::nullopt when that dataset has no matching topic+field.
using SeriesKeyResolver = std::function<std::optional<QString>(const SeriesPath&)>;

// Collects the distinct SeriesPaths referenced by <curve> children of <plot>
// elements: time-series curves carry topic/field attributes; XY curves carry
// x_topic/x_field and y_topic/y_field. Order-preserving, de-duplicated.
[[nodiscard]] QList<SeriesPath> extractSeriesPaths(const QDomDocument& doc);

// Rewrites each <curve>'s concrete key attributes (name for time-series;
// curve_x/curve_y for XY) to the target dataset's keys, resolving each curve's
// stable topic/field via `resolve`. A curve whose path(s) don't resolve has
// those key attributes cleared and its path(s) returned in the (de-duplicated)
// result so the caller can prompt/strip. In place.
[[nodiscard]] QList<SeriesPath> rebindCurveKeys(QDomDocument& doc, const SeriesKeyResolver& resolve);

// Converts a saved workspace document into a portable generic layout by removing
// the exact dataset hints: dataset_id / dataset_source / dataset_path (and their
// x_/y_ XY variants) from plotted TS/XY curves, input_dataset_* from
// data-processor <processor> inputs, dataset_* from <transform> inputs, marker
// <generator>s, hidden <marker_scope> rows and scene-dock <layer> /
// <config_topic> elements, source_dataset_* from <robot_model> / <trail>, and
// dataset_* from <state_transitions> <series> rows. The
// topic+field (or topic+type; a generator's input series) identity remains and
// binds at apply time only when unique in the loaded data — the scene loaders
// implement the same unique-only fallback as plots and processors. An on-demand
// <input> name keeps its qualifier, the alias its script reads; its `topic` or
// `upstream` says what it binds. Undo snapshots and source-bound layouts keep
// their qualifiers.
void removeDatasetQualifiersForGenericLayout(QDomDocument& doc);

// Maps a serialized DatasetId to its FileLoader-known full source path (empty for
// a non-file/unknown dataset). Supplied to stampDatasetSourcePaths so the XML
// layer needs no FileLoader dependency of its own.
using DatasetPathLookup = std::function<QString(std::uint32_t)>;

// Adds the FileLoader-known full-path companion (`*_dataset_path`) next to every
// persisted `*_dataset_id` on plotted TS/XY curves, data-processor <processor> and
// <transform> inputs, marker <generator>s and hidden <marker_scope> rows. A path the
// element already carries is overwritten. Scene docks and State Transitions rows
// are NOT visited: they serialize the session's absolute path themselves, and a
// file save makes it layout-relative with relativizeDatasetSourcePaths. A dataset
// the lookup can't place keeps its id/source qualifier unchanged.
void stampDatasetSourcePaths(QDomDocument& doc, const DatasetPathLookup& lookup);

// Makes a document safe to persist outside the live session. A non-zero numeric
// id WITHOUT a full-path qualifier is only a volatile in-session hint; strip it
// so a later session cannot accept a coincidentally reminted id merely because a
// basename-like source label also matches. Path-qualified ids and zero-valued
// local-scene ids are left intact; the retained source label may still rebind
// when unique. Scoped to the elements stampDatasetSourcePaths stamps; scene ids
// survive so restored scene layers keep binding.
void removeUnvalidatedDatasetIds(QDomDocument& doc);

// Resolves relative `*_dataset_path` qualifiers against the layout file's
// directory before any plot/processor/scene restore consumes them, so a
// source-bound layout moves together with its data. Absolute qualifiers (every
// undo snapshot's, and a file's paths outside its folder) are only cleaned.
void resolveDatasetSourcePaths(QDomDocument& doc, const QDir& layout_dir);

// Rewrites every non-empty dataset source-path qualifier in place. Covers the
// five qualifier shapes used by plots, processors, transforms, and scene docks:
// dataset_path, x_dataset_path, y_dataset_path, input_dataset_path, and
// source_dataset_path. The callback receives the serialized value and must
// return the desired replacement (returning it unchanged is a no-op). This is a
// pure XML pass: it performs no filesystem access and changes no schema.
using DatasetPathRemapper = std::function<QString(const QString&)>;
void remapDatasetSourcePaths(QDomDocument& doc, const DatasetPathRemapper& remap);

// Save-side inverse of resolveDatasetSourcePaths: hands every ABSOLUTE filesystem
// qualifier of the five shapes remapDatasetSourcePaths visits to `relocate` and stores
// its result. Relative values and opaque identities (containing "://") are skipped, so
// a non-idempotent relocator is safe. Works by value, not by DatasetId, so it also
// reaches the qualifiers widgets serialize themselves (scene docks, <robot_model> /
// <trail>, <state_transitions> rows).
void relativizeDatasetSourcePaths(QDomDocument& doc, const DatasetPathRemapper& relocate);

// Sets `<prefix>dataset_path` to identity_for_dataset[id] on every element of the
// document whose `<prefix>dataset_id` is a key of the map, for all five qualifier
// shapes — scene layers, config topics and robot models included, which
// stampDatasetSourcePaths skips. How a save names data that has no file, such as a
// live stream (`pj-stream://<id>`).
void stampDatasetIdentities(QDomDocument& doc, const QHash<std::uint32_t, QString>& identity_for_dataset);

// The load-side inverse for streams: every qualifier whose `<prefix>dataset_path`
// is a key of the map becomes the exact live DatasetId (its source and path
// attributes dropped), and each <curve>, <layer> and <config_topic> so bound
// becomes a pending intent, because a stream's topics arrive only after the
// layout applied. A bound payload of a <layer> (a robot model, a trail) makes
// that layer the pending intent, and gives a layer with no qualifier of its own
// the payload's `dataset_id`, which its pending demand names. Other qualifiers
// are left untouched. Run it after
// removeUnvalidatedDatasetIds, which would strip the path-less ids it writes.
void bindStreamQualifiers(QDomDocument& doc, const QHash<QString, std::uint32_t>& dataset_for_identity);

// Rewrites each <previouslyLoaded_Datafiles>/<fileInfo> filename attribute
// through `remap`, visiting exactly the elements extractDataSource yields
// (non-empty filename, in document order) — the element-selection rule lives
// once, next to the reader. The callback receives the SERIALIZED attribute
// value; returning it unchanged is a no-op. Pure XML pass.
void remapFileInfoFilenames(QDomDocument& doc, const DatasetPathRemapper& remap);

// Removes every <curve> left without any usable key after rebindCurveKeys
// (empty name and empty curve_x/curve_y). Two-pass so the live node list
// isn't invalidated mid-iteration.
void stripUnresolvedCurves(QDomDocument& doc);

// Annotates every <plot>'s <range> with an explicit x_basis marker when it lacks
// one, so the widget loader never has to infer the X coordinate meaning from plot
// mode. A read-side migration: it changes NO numeric range values. An XY plot's
// range becomes x_basis="value"; a time-series range becomes x_basis="absolute"
// because PJ4 has always persisted time-axis ranges in absolute seconds (the v3
// writer already did, per PR #248). Idempotent — a range that already carries
// x_basis is left untouched. Call BEFORE any widget restore consumes the document.
void normalizePlotRangeBasis(QDomDocument& doc);

}  // namespace PJ::layout_xml
