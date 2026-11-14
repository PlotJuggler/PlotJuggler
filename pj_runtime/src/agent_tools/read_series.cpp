// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Agent tool executors. Each is a pure function of (args, host views) and
// returns a ToolResult whose `content` the model reads. Every failure is
// returned as data (ok=false), never thrown.
//
// This file: read_series.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "pj_runtime/agent_tools/series_stats.hpp"
#include "tools_internal.hpp"

namespace PJ::agent_tools::detail {
namespace {

// Cap on a single tool response handed back to the model, so a wide catalog or
// a long series can't blow the context window. read_series coarsens to fit.
constexpr std::size_t kMaxResponseBytes = 16 * 1024;

// Materialize a numeric field into parallel timestamp + double columns,
// coercing any numeric column type to double. Returns false for a non-numeric
// column (string/bool) or an empty/mismatched read.
bool readSeriesDoubles(
    const PJ::sdk::MaterializedSeriesView& view, std::vector<std::int64_t>& out_ts, std::vector<double>& out_vals) {
  const std::size_t n = view.rowCount();
  auto ts = view.timestamps();
  if (n == 0 || ts.size() != n) {
    return false;
  }
  out_ts.assign(ts.begin(), ts.end());
  out_vals.resize(n);

  auto copy_from = [&](const auto* p) -> bool {
    if (p == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
      out_vals[i] = static_cast<double>(p[i]);
    }
    return true;
  };

  switch (view.type()) {
    case PJ::PrimitiveType::kFloat64:
      return copy_from(view.valuesAsFloat64());
    case PJ::PrimitiveType::kFloat32:
      return copy_from(view.valuesAsFloat32());
    case PJ::PrimitiveType::kInt8:
      return copy_from(view.valuesAsInt8());
    case PJ::PrimitiveType::kInt16:
      return copy_from(view.valuesAsInt16());
    case PJ::PrimitiveType::kInt32:
      return copy_from(view.valuesAsInt32());
    case PJ::PrimitiveType::kInt64:
      return copy_from(view.valuesAsInt64());
    case PJ::PrimitiveType::kUint8:
      return copy_from(view.valuesAsUint8());
    case PJ::PrimitiveType::kUint16:
      return copy_from(view.valuesAsUint16());
    case PJ::PrimitiveType::kUint32:
      return copy_from(view.valuesAsUint32());
    case PJ::PrimitiveType::kUint64:
      return copy_from(view.valuesAsUint64());
    default:
      return false;  // bool/string aren't plottable numeric series
  }
}

json statsToJson(const SeriesStats& s) {
  json out = {{"count", s.count}, {"duration_s", s.duration_s}, {"rate_hz", s.rate_hz}};
  // NaN/inf poison a straight sum, so min/max/mean/stddev are computed over
  // finite values only; when none exist there is nothing honest to report, so
  // they are omitted rather than serialized as NaN (which nlohmann turns into
  // `null` — indistinguishable from a field that was never populated).
  if (s.has_values) {
    out["min"] = s.min;
    out["max"] = s.max;
    out["min_at_s"] = s.min_at_s;
    out["max_at_s"] = s.max_at_s;
    out["mean"] = s.mean;
    out["stddev"] = s.stddev;
  } else {
    out["note"] = "no finite values";
  }
  // The spacing facts that count/mean/rate cannot carry: a dropout leaves all
  // three untouched. The keys are self-describing on purpose — the tool's
  // schema description says nothing about them, so they cost prefix tokens in
  // no turn and appear exactly when a series is read.
  if (s.has_gap) {
    out["max_gap_s"] = s.max_gap_s;
    out["max_gap_at_s"] = s.max_gap_at_s;
  }
  if (s.invalid > 0) {
    out["invalid"] = s.invalid;
    out["invalid_fraction"] = static_cast<double>(s.invalid) / static_cast<double>(s.count);
  }
  return out;
}

// Cap on how many series one call may read. Stats are small and fixed-size per
// series, so this is not about the response cap — it is about not turning a
// single tool call into an unbounded scan of the whole dataset.
constexpr std::size_t kMaxBatchPaths = 32;

// Buckets are the expensive, variable payload — several of them share ONE
// response cap (see batchBuckets), so the batch stays smaller than the flat
// per-path cap above: past this many series sharing the cap coarsens every
// one of them past usefulness rather than saving a round trip.
constexpr std::size_t kMaxBucketsBatchPaths = 8;

// Hard cap on samples mode 'raw' returns per series: raw exists so the model
// can see actual values once it has narrowed to a handful of interesting
// ones, not to ship a whole recording through the tool loop one call at a
// time.
constexpr std::size_t kMaxRawSamples = 200;

// Batch cap for mode 'raw' — tighter than buckets' own, since each series
// here can carry up to kMaxRawSamples individual numbers rather than a
// bounded bucket count, so a handful of series already approaches the
// response cap.
constexpr std::size_t kMaxRawBatchPaths = 4;

// Best-effort absolute-ns -> display-seconds conversion for one series,
// preferring the dataset-scoped host call (immune to the "ambiguous topic
// between datasets" failure mode the plain topic-scoped one has) and falling
// back to the topic-scoped one when the host does not offer the former.
// Empty when neither is bound or neither succeeds — never guessed.
std::optional<double> toDisplaySeconds(
    ToolContext& ctx, const std::optional<PJ::sdk::DataSourceHandle>& source, const std::string& topic,
    std::int64_t absolute_ns) {
  if (source) {
    if (auto display_s = ctx.playback.toDisplayTimeForSource(*source, absolute_ns)) {
      return *display_s;
    }
  }
  if (auto display_s = ctx.playback.toDisplayTime(topic, absolute_ns)) {
    return *display_s;
  }
  return std::nullopt;
}

// The same for a series just resolved against `catalog`.
std::optional<double> toDisplaySeconds(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const SeriesRead& r, std::int64_t absolute_ns) {
  return toDisplaySeconds(ctx, dataSourceHandleFor(catalog, r.dataset), r.topic, absolute_ns);
}

// The display window [t_start_s, t_end_s] (playback's axis, plot_tab zoom's, stats' own t_start_display_s) as raw-ns
// bounds: floor at the start, ceil at the end, an absent bound left at the int64 limit. The series' own first sample is
// the only anchor available for the ns<->display conversion (see toDisplaySeconds): the affine offset it yields —
// display(anchor) minus anchor in seconds — translates the bounds. Empty when no conversion is available.
struct NsWindow {
  std::int64_t first = std::numeric_limits<std::int64_t>::min();
  std::int64_t last = std::numeric_limits<std::int64_t>::max();
  double offset_s = 0.0;          // display(ts) = ts*1e-9 + offset_s
  double anchor_display_s = 0.0;  // display(anchor)
};

std::optional<NsWindow> nsWindow(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const SeriesRead& r, std::int64_t anchor_ns,
    const std::optional<double>& t_start_s, const std::optional<double>& t_end_s) {
  const std::optional<double> anchor_display = toDisplaySeconds(ctx, catalog, r, anchor_ns);
  if (!anchor_display) {
    return std::nullopt;
  }
  NsWindow window;
  window.anchor_display_s = *anchor_display;
  window.offset_s = *anchor_display - static_cast<double>(anchor_ns) * 1e-9;
  auto toAbsNs = [&](double display_s, bool round_up) {
    const double ns = (display_s - window.offset_s) * 1e9;
    return static_cast<std::int64_t>(round_up ? std::ceil(ns) : std::floor(ns));
  };
  if (t_start_s) {
    window.first = toAbsNs(*t_start_s, /*round_up=*/false);
  }
  if (t_end_s) {
    window.last = toAbsNs(*t_end_s, /*round_up=*/true);
  }
  return window;
}

// Trims r's samples to `window` before stats or buckets ever see them, so a windowed read's t_start_display_s and
// bucket 't' come out relative to the first sample INSIDE the window, never to t_start_s itself. r may hold the whole
// series or already little more than the window; an empty window reports the whole series' span, which runs from the
// window's anchor to series_last_ns.
//
// An empty result turns the entry into an error (r.ok becomes false) rather than silently reinterpreting the window as
// relative ns, which would quietly answer a different question than the one asked.
void cutToWindow(
    SeriesRead& r, const NsWindow& window, std::int64_t series_last_ns, const std::optional<double>& t_start_s,
    const std::optional<double>& t_end_s) {
  const auto lo_it = std::lower_bound(r.ts.begin(), r.ts.end(), window.first);
  const auto hi_it = std::upper_bound(r.ts.begin(), r.ts.end(), window.last);
  if (lo_it >= hi_it) {
    const double series_end_display = static_cast<double>(series_last_ns) * 1e-9 + window.offset_s;
    std::ostringstream msg;
    msg << "no samples in window; series spans display [" << window.anchor_display_s << ", " << series_end_display
        << "]";
    r.ok = false;
    r.error = msg.str();
    return;
  }

  // Trim by INDEX, not by copying through the iterators above: ts and vals
  // are reassigned in place below, and an iterator pair that aliases the
  // vector being assigned is not safe to feed straight to vector::assign.
  const auto lo = static_cast<std::size_t>(lo_it - r.ts.begin());
  const auto hi = static_cast<std::size_t>(hi_it - r.ts.begin());
  std::vector<std::int64_t> windowed_ts(
      r.ts.begin() + static_cast<std::ptrdiff_t>(lo), r.ts.begin() + static_cast<std::ptrdiff_t>(hi));
  std::vector<double> windowed_vals(
      r.vals.begin() + static_cast<std::ptrdiff_t>(lo), r.vals.begin() + static_cast<std::ptrdiff_t>(hi));
  r.ts = std::move(windowed_ts);
  r.vals = std::move(windowed_vals);
  r.display_offset_s = window.offset_s;

  r.window = {{"axis", "display"}};
  if (t_start_s) {
    r.window["t_start_s"] = *t_start_s;
  }
  if (t_end_s) {
    r.window["t_end_s"] = *t_end_s;
  }
}

// Narrows a successfully-read whole series to the display window when either bound was requested; a no-op otherwise.
// No conversion available turns the entry into an error, like an empty window (cutToWindow).
void applyDisplayWindow(
    SeriesRead& r, ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::optional<double>& t_start_s,
    const std::optional<double>& t_end_s) {
  if (!t_start_s && !t_end_s) {
    return;
  }
  const auto window = r.ts.empty() ? std::nullopt : nsWindow(ctx, catalog, r, r.ts.front(), t_start_s, t_end_s);
  if (!window) {
    r.ok = false;
    r.error = "cannot map the display window for this series";
    return;
  }
  cutToWindow(r, *window, r.ts.back(), t_start_s, t_end_s);
}

// Round to `sig_figs` significant decimal digits before serializing: the raw
// float32 sources rarely carry more precision than that, and a double's full
// ~17-digit decimal expansion is roughly twice the bytes for no information
// the model can use. Leaves 0 and non-finite values untouched (a NaN/inf
// still round-trips through nlohmann as JSON null via renderBucketArray's own
// isfinite guard, not through this helper).
double roundSignificant(double v, int sig_figs = 6) {
  if (!std::isfinite(v) || v == 0.0) {
    return v;
  }
  const double magnitude = std::pow(10.0, sig_figs - 1 - static_cast<int>(std::floor(std::log10(std::fabs(v)))));
  return std::round(v * magnitude) / magnitude;
}

// Bucketize and render as the columnar JSON object both readSeriesTool's
// 'buckets' mode (single-series and batched) and evaluate's optional bucket
// summary send back: parallel arrays ("n"/"min"/"max"/"mean") instead of one
// object per bucket, since every bucket used to repeat the same handful of
// keys — measured 69 bytes/bucket as objects vs. 16 as columns on a real
// response, which is the difference between ~240 and ~1000 buckets under the
// 16 KiB cap. "t0"/"dt" let the model reconstruct t_i = t0 + i*dt; an
// explicit "t" array is emitted only when some bucket's actual t_rel_s
// deviates from that reconstruction by more than a tiny tolerance — either
// bucketize's "one bucket per sample" path over uneven timestamps, or a
// bucket dropped entirely because its window held no samples at all (both
// break the even grid the t0/dt formula assumes). A bucket with no finite
// samples reports null min/max/mean but keeps its 0 in "n", so a window of
// missing data is visible rather than silently absent from the arrays.
json renderBucketArray(std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points) {
  auto buckets = bucketize(ts, vals, max_points);
  json out = json::object();
  const std::size_t n = buckets.size();
  const double t0 = n > 0 ? buckets.front().t_rel_s : 0.0;
  const double dt = n >= 2 ? (buckets.back().t_rel_s - t0) / static_cast<double>(n - 1) : 0.0;
  const double tol = 1e-6 * std::max(1.0, std::fabs(dt));

  bool irregular = false;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(buckets[i].t_rel_s - (t0 + static_cast<double>(i) * dt)) > tol) {
      irregular = true;
      break;
    }
  }

  json n_arr = json::array();
  json min_arr = json::array();
  json max_arr = json::array();
  json mean_arr = json::array();
  json invalid_arr = json::array();
  json t_arr = json::array();
  bool any_invalid = false;
  for (const auto& b : buckets) {
    n_arr.push_back(b.count);
    if (b.count > 0) {
      min_arr.push_back(roundSignificant(b.min));
      max_arr.push_back(roundSignificant(b.max));
      mean_arr.push_back(roundSignificant(b.mean));
    } else {
      min_arr.push_back(nullptr);
      max_arr.push_back(nullptr);
      mean_arr.push_back(nullptr);
    }
    invalid_arr.push_back(b.invalid);
    any_invalid = any_invalid || b.invalid > 0;
    if (irregular) {
      t_arr.push_back(b.t_rel_s);
    }
  }

  out["t0"] = t0;
  out["dt"] = dt;
  out["n"] = std::move(n_arr);
  out["min"] = std::move(min_arr);
  out["max"] = std::move(max_arr);
  out["mean"] = std::move(mean_arr);
  if (irregular) {
    out["t"] = std::move(t_arr);
  }
  if (any_invalid) {
    out["invalid"] = std::move(invalid_arr);
  }
  return out;
}

// Render a windowed, successfully-read series as mode 'raw' samples: once the
// model has narrowed to a handful of points, buckets can no longer show it
// the actual values, so this closes that last step -- but only inside a
// window the caller set, and only up to `sample_cap` of them (kMaxRawSamples,
// halved by the raw envelope's own shrink-to-fit when the batch is too big),
// so a raw call cannot become an unbounded dump of a whole recording. Times
// are seconds relative to the first RETURNED sample's OWN display time
// ("t0_display_s"), not to the window's t_start_s, mirroring how bucket "t0"
// is relative to the first bucket rather than to the window bound. Values
// are rounded the same way bucket min/max/mean are; a non-finite value comes
// back as JSON null rather than poisoning the array. A window holding more
// than `sample_cap` is truncated to its first `sample_cap` samples and
// flagged "truncated", with "window_samples" carrying the true count, so the
// model knows to narrow further rather than assume it saw everything.
json renderRawSamples(
    ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const SeriesRead& r, std::size_t sample_cap) {
  const std::size_t window_samples = r.ts.size();
  const std::size_t count = std::min(window_samples, sample_cap);
  const std::int64_t t0_ns = r.ts.front();
  // The window's already-computed offset (display(ts) = ts*1e-9 + offset)
  // applies to any ns in this series, so reuse it instead of re-running the
  // ns->display conversion for this first sample; absent a window, fall back
  // to the call this always used to make.
  const std::optional<double> t0_display =
      r.display_offset_s ? std::optional<double>(static_cast<double>(t0_ns) * 1e-9 + *r.display_offset_s)
                         : toDisplaySeconds(ctx, catalog, r, t0_ns);
  if (!t0_display) {
    return {{"series", r.path}, {"error", "cannot map the display time of the first sample for this series"}};
  }

  json t_arr = json::array();
  json v_arr = json::array();
  for (std::size_t i = 0; i < count; ++i) {
    t_arr.push_back(static_cast<double>(r.ts[i] - t0_ns) * 1e-9);
    const double v = r.vals[i];
    v_arr.push_back(std::isfinite(v) ? json(roundSignificant(v)) : json(nullptr));
  }

  json out = {
      {"series", r.path}, {"t0_display_s", *t0_display}, {"t", std::move(t_arr)}, {"v", std::move(v_arr)},
      {"count", count},
  };
  if (count < window_samples) {
    out["truncated"] = true;
    out["window_samples"] = window_samples;
  }
  return out;
}

// Shrink `max_points` by half, re-rendering via `build`, until the serialized
// payload fits `cap` or `max_points` bottoms out at 16 -- the halving loop
// withCoarsenedBuckets and batchBuckets both ran themselves, over a
// single-series render and a whole batch envelope respectively. Adds a
// "note" to the last rendering when it still does not fit at the floor.
json shrinkToFit(const std::function<json(std::size_t)>& build, std::size_t cap, std::size_t max_points) {
  for (;;) {
    json out = build(max_points);
    const std::string dumped = out.dump();
    if (dumped.size() <= cap || max_points <= 16) {
      if (dumped.size() > cap) {
        out["note"] = "coarsened to fit the response cap";
      }
      return out;
    }
    max_points /= 2;
  }
}

// Several series' buckets, sharing ONE response cap instead of each getting
// the single-series kMaxResponseBytes on its own: min(N, 4) times that cap,
// halved in lockstep across every series until the whole envelope fits (or
// each is down to 16 buckets — the same floor withCoarsenedBuckets uses). A
// bad path becomes an {"error"} entry beside the ones that worked, same as
// 'stats'. Stats do not depend on max_points, so they are computed once,
// outside the halving loop that only re-renders the bucket arrays.
json batchBuckets(ToolContext& ctx, const std::vector<SeriesRead>& reads, std::size_t max_points) {
  const std::size_t n = reads.size();
  const std::size_t cap = std::min(n, std::size_t{4}) * kMaxResponseBytes;

  std::vector<json> stats(n);
  std::size_t failed = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (reads[i].ok) {
      stats[i] = statsWithDisplayStart(computeStats(reads[i].ts, reads[i].vals), ctx, reads[i].topic, reads[i].flat);
    } else {
      ++failed;
    }
  }

  return shrinkToFit(
      [&](std::size_t points) {
        json read_arr = json::array();
        for (std::size_t i = 0; i < n; ++i) {
          const SeriesRead& r = reads[i];
          if (!r.ok) {
            read_arr.push_back({{"series", r.path}, {"error", r.error}});
            continue;
          }
          json entry = {{"series", r.path}, {"stats", stats[i]}, {"buckets", renderBucketArray(r.ts, r.vals, points)}};
          if (!r.window.is_null()) {
            entry["window"] = r.window;
          }
          read_arr.push_back(std::move(entry));
        }
        json out = {{"count", n}, {"read", read_arr}};
        if (failed != 0) {
          out["failed"] = failed;
        }
        return out;
      },
      cap, max_points);
}

// Every numeric field of one already-resolved topic, as read_series paths
// in catalog order -- what a topic-only path expands to. Qualified with
// "dataset:" using the same rule resolveSeriesPath's own qualifying does, so
// an expanded path is never ambiguous even when several loaded datasets
// share this topic name. Takes the caller's own datasetByTopicIndex result,
// same reasoning as resolveTopicPath.
std::vector<std::string> topicNumericFieldPaths(
    const PJ::sdk::CatalogSnapshot& catalog, std::uint32_t topic_index, const TopicDatasetMap& topic_dataset) {
  std::vector<std::string> out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const auto& topic = topics[topic_index];
  const std::string topic_name(PJ::sdk::toStringView(topic.name));
  const auto dataset_it = topic_dataset.find(topic_index);
  const std::string dataset = dataset_it == topic_dataset.end() ? std::string() : dataset_it->second;
  for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
    const std::size_t idx = topic.first_field + fi;
    if (idx >= fields.size()) {
      break;
    }
    if (!isSignalField(PJ::sdk::fromAbiType(fields[idx].type), PJ::sdk::toStringView(fields[idx].name))) {
      continue;
    }
    out.push_back(qualifyWithDataset(dataset, joinSeriesPath(topic_name, PJ::sdk::toStringView(fields[idx].name))));
  }
  return out;
}

// Numeric field NAMES of one topic (relative to the topic, not full paths),
// excluding header/*-style fields -- timestamps and sequence numbers are not
// signals, so they count toward neither "numeric_fields" nor "fields" in the
// "unread" disclosure below. Field-name analogue of topicNumericFieldPaths,
// in the same catalog order.
std::vector<std::string> topicNumericFieldNames(const PJ::sdk::CatalogSnapshot& catalog, std::uint32_t topic_index) {
  std::vector<std::string> out;
  auto topics = catalog.topics();
  auto fields = catalog.fields();
  const auto& topic = topics[topic_index];
  for (std::uint32_t fi = 0; fi < topic.field_count; ++fi) {
    const std::size_t idx = topic.first_field + fi;
    if (idx >= fields.size()) {
      break;
    }
    std::string_view name = PJ::sdk::toStringView(fields[idx].name);
    if (!isSignalField(PJ::sdk::fromAbiType(fields[idx].type), name)) {
      continue;
    }
    while (!name.empty() && name.front() == '/') {
      name.remove_prefix(1);
    }
    out.push_back(std::string(name));
  }
  return out;
}

// The field name a successful read actually covered, relative to its owning
// topic -- e.g. "channels[4]" out of a resolved path
// "/mavros/rc/out/channels[4]" -- recovered by stripping the same
// dataset-qualifier and topic prefix joinSeriesPath added, so it lines up
// with topicNumericFieldNames's entries below.
std::string fieldNameFromResolvedPath(const SeriesRead& r) {
  std::string path = r.path;
  if (!r.dataset.empty()) {
    const std::string prefix = r.dataset + ":";
    if (path.compare(0, prefix.size(), prefix) == 0) {
      path = path.substr(prefix.size());
    }
  }
  if (path.size() > r.topic.size() && path.compare(0, r.topic.size(), r.topic) == 0) {
    path = path.substr(r.topic.size());
  }
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  return path;
}

// Cap on how many unread field names the "unread" disclosure below names per
// topic -- fed back to the model and re-sent on every later round-trip of the
// turn, so an unbounded list would be paid for repeatedly. Same reasoning as
// kMaxCandidates.
constexpr std::size_t kMaxUnreadFields = 12;

// read_series discloses, per response, which topic(s) it read only PART of:
// grouped by owning topic, a topic where THIS call read fewer distinct
// numeric fields (excluding header/*) than it has gets a "read"/
// "numeric_fields" count and the unread field names, pointing at the
// bare-topic form that would have read all of them in one call (see the
// commit this shipped with: faults on unread fields went unnoticed because
// the model asked for only the first few of a wide array). Built once from
// the raw reads so every read_series mode attaches it the same way. Empty
// (no "unread" key at the call site) when every topic touched by this call
// was read whole.
json unreadDisclosure(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<SeriesRead>& reads,
    const TopicDatasetMap& topic_dataset) {
  struct Tally {
    std::uint32_t topic_index = 0;
    std::vector<std::string> read_fields;
  };
  std::map<std::string, Tally> by_topic;  // keyed by the bare topic name read_series echoes as "topic"

  for (const auto& r : reads) {
    if (!r.ok) {
      continue;
    }
    auto it = by_topic.find(r.topic);
    if (it == by_topic.end()) {
      const std::string lookup_name = qualifyWithDataset(r.dataset, r.topic);
      const TopicLookup topic = resolveTopicPath(catalog.topics(), catalog.dataSources(), lookup_name, topic_dataset);
      if (!topic.topic_index) {
        continue;  // a series we just read always has an owning topic; defensive only
      }
      it = by_topic.emplace(r.topic, Tally{*topic.topic_index, {}}).first;
    }
    const std::string field = fieldNameFromResolvedPath(r);
    if (std::find(it->second.read_fields.begin(), it->second.read_fields.end(), field) ==
        it->second.read_fields.end()) {
      it->second.read_fields.push_back(field);
    }
  }

  json out = json::object();
  for (const auto& [topic_key, tally] : by_topic) {
    const std::vector<std::string> counted = topicNumericFieldNames(catalog, tally.topic_index);
    std::vector<std::string> unread_names;
    std::size_t read_count = 0;
    for (const auto& f : counted) {
      if (std::find(tally.read_fields.begin(), tally.read_fields.end(), f) != tally.read_fields.end()) {
        ++read_count;
      } else {
        unread_names.push_back(f);
      }
    }
    if (unread_names.empty()) {
      continue;
    }
    json fields_json = json::array();
    for (std::size_t i = 0; i < unread_names.size() && i < kMaxUnreadFields; ++i) {
      fields_json.push_back(unread_names[i]);
    }
    if (unread_names.size() > kMaxUnreadFields) {
      fields_json.push_back("…");
    }
    out[topic_key] = {
        {"read", read_count},
        {"numeric_fields", counted.size()},
        {"fields", std::move(fields_json)},
        {"hint", "a bare topic path reads every field of the topic in one call"},
    };
  }
  return out;
}

// How many of a topic's OWN numeric fields one read_series call may expand
// into before the mode's degradation kicks in (see expandRequestedPaths) --
// the same limit the mode already enforces on the whole call, so a topic
// that fits comfortably never trips a second, tighter limit later.
std::size_t topicExpansionCap(const std::string& mode) {
  if (mode == "buckets") {
    return kMaxBucketsBatchPaths;
  }
  if (mode == "raw") {
    return kMaxRawBatchPaths;
  }
  return kMaxBatchPaths;
}

// One item read_series works on after path expansion. `forced` bypasses the
// normal lookup+read entirely: it is set for an ambiguous topic, a topic
// with no numeric fields, or (mode buckets/raw) a topic too big for this
// call's cap -- cases expandRequestedPaths has already turned into a final
// answer, rendered by forcedFailure exactly like any other per-path error.
struct PathTask {
  std::string path;
  bool forced = false;
  std::string forced_error;
};

// A forced PathTask rendered as a SeriesRead failure, so every read_series
// mode's existing per-entry error rendering (built for a normal failed
// lookup) handles a topic-expansion error the same way, with no
// special-casing needed in the mode branches below.
SeriesRead forcedFailure(const PathTask& task) {
  SeriesRead r;
  r.path = task.path;
  r.error = task.forced_error;
  return r;
}

// Expand each requested path: unchanged when it already names a series (a
// field path always wins over a topic-only reading of the same string),
// and also unchanged when it names neither a series nor a topic -- the
// existing "unknown series" error fires later exactly as it did before this
// feature. Only a path that fails as a series AND succeeds as a bare topic
// expands, into every numeric field of that topic, in catalog order. A
// field a topic expands to is dropped when the caller also named it
// explicitly elsewhere in the same call, so the two never duplicate each
// other -- but two explicit requests for the same series are NOT
// deduplicated against each other, unchanged from before this feature
// (RawBatchCappedAtFour and friends rely on a repeated path still costing a
// slot against the batch cap). `notes_out` collects the "read the first N,
// cap" note mode 'stats' surfaces when a topic alone has more numeric
// fields than the cap; 'buckets' and 'raw' never populate it -- they fail
// that one task instead (see topicExpansionCap).
std::vector<PathTask> expandRequestedPaths(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<std::string>& paths, const std::string& mode,
    std::vector<std::string>& notes_out, const TopicDatasetMap& topic_dataset) {
  std::vector<PathTask> out;
  out.reserve(paths.size());
  const std::size_t cap = topicExpansionCap(mode);

  // Resolve every requested path as a series exactly once -- reused below
  // both to seed explicit_canonical and, in the main loop, as `direct` --
  // rather than resolving each one twice (a pre-pass, then the loop).
  std::vector<SeriesLookup> direct_lookups;
  direct_lookups.reserve(paths.size());
  for (const auto& want : paths) {
    direct_lookups.push_back(resolveSeriesPath(catalog, want, topic_dataset));
  }

  // Canonical form of every path in THIS call that resolves directly as a
  // series, computed once up front (order-independent) so a topic expansion
  // can skip a field the caller also named explicitly, regardless of
  // whether that explicit path appears before or after the topic path.
  std::vector<std::string> explicit_canonical;
  for (const auto& lookup : direct_lookups) {
    if (lookup.resolved) {
      explicit_canonical.push_back(lookup.resolved->path);
    }
  }
  auto namedExplicitly = [&](const std::string& canon) {
    return std::find(explicit_canonical.begin(), explicit_canonical.end(), canon) != explicit_canonical.end();
  };
  std::vector<std::string> topic_expanded_seen;  // fields already produced by an earlier topic expansion this call

  for (std::size_t i = 0; i < paths.size(); ++i) {
    const std::string& want = paths[i];
    const SeriesLookup& direct = direct_lookups[i];
    if (direct.resolved) {
      PathTask task;
      task.path = want;
      out.push_back(std::move(task));
      continue;
    }

    const TopicLookup topic = resolveTopicPath(catalog.topics(), catalog.dataSources(), want, topic_dataset);
    if (topic.ambiguous) {
      SeriesLookup shim;
      shim.ambiguous = true;
      shim.candidates = topic.candidates;
      PathTask task;
      task.path = want;
      task.forced = true;
      task.forced_error = seriesLookupError(want, shim);
      out.push_back(std::move(task));
      continue;
    }
    if (!topic.topic_index) {
      // Not a topic either: pass through unchanged, same as before this
      // feature -- readOne will fail it with the usual "unknown series".
      PathTask task;
      task.path = want;
      out.push_back(std::move(task));
      continue;
    }

    std::vector<std::string> fields = topicNumericFieldPaths(catalog, *topic.topic_index, topic_dataset);
    const std::string topic_name(PJ::sdk::toStringView(catalog.topics()[*topic.topic_index].name));
    if (fields.empty()) {
      PathTask task;
      task.path = want;
      task.forced = true;
      task.forced_error = "topic has no numeric fields";
      out.push_back(std::move(task));
      continue;
    }
    if (fields.size() > cap) {
      if (mode == "stats") {
        notes_out.push_back(
            "topic " + topic_name + " has " + std::to_string(fields.size()) + " numeric fields; the first " +
            std::to_string(cap) + " were read (cap); name the fields to read the rest");
        fields.resize(cap);
      } else {
        PathTask task;
        task.path = want;
        task.forced = true;
        task.forced_error = "topic " + topic_name + " has " + std::to_string(fields.size()) +
                            " numeric fields, more than mode '" + mode + "' allows in one call (" +
                            std::to_string(cap) +
                            "); read it in stats first, then name the fields whose shape you want";
        out.push_back(std::move(task));
        continue;
      }
    }
    for (auto& f : fields) {
      if (namedExplicitly(f)) {
        continue;
      }
      if (std::find(topic_expanded_seen.begin(), topic_expanded_seen.end(), f) != topic_expanded_seen.end()) {
        continue;
      }
      topic_expanded_seen.push_back(f);
      PathTask task;
      task.path = std::move(f);
      out.push_back(std::move(task));
    }
  }
  return out;
}

// The batch-shaped {count, read, failed?, unread?, note?} envelope built
// from already-read series, regardless of how many there are -- render each
// into its entry JSON (a failed read becomes {"series", "error"};
// `render_entry` renders a successful one, and may itself add an "error" --
// raw's renderRawSamples does, when the display time cannot be mapped), and
// count the failures. `extra_note`, when non-empty, is stats' own extra: the
// topic-expansion-cap note. Split out from finishReadEnvelope below so raw's
// shrink-to-fit loop (which needs a plain json to measure) can call it
// directly, without going through the single-task unwrapping.
json buildReadEnvelope(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<SeriesRead>& reads, const TopicDatasetMap& topic_dataset,
    const std::function<json(const SeriesRead&)>& render_entry, const std::string& extra_note = {}) {
  json arr = json::array();
  std::size_t failed = 0;
  for (const auto& r : reads) {
    json entry = r.ok ? render_entry(r) : json{{"series", r.path}, {"error", r.error}};
    if (entry.contains("error")) {
      ++failed;
    }
    arr.push_back(std::move(entry));
  }
  json out = {{"count", arr.size()}, {"read", arr}};
  if (failed != 0) {
    out["failed"] = failed;
  }
  const json unread = unreadDisclosure(catalog, reads, topic_dataset);
  if (!unread.empty()) {
    out["unread"] = unread;
  }
  if (!extra_note.empty()) {
    out["note"] = extra_note;
  }
  return out;
}

// Shared tail of read_series's per-task modes (stats, buckets' single-path,
// raw): buildReadEnvelope, then finish either as the one task's own JSON (an
// "error" there becomes a ToolResult::failure) or the batch envelope as-is.
ToolResult finishReadEnvelope(
    const PJ::sdk::CatalogSnapshot& catalog, const std::vector<SeriesRead>& reads, const TopicDatasetMap& topic_dataset,
    const std::function<json(const SeriesRead&)>& render_entry, const std::string& extra_note = {}) {
  const json out = buildReadEnvelope(catalog, reads, topic_dataset, render_entry, extra_note);
  // A lone path keeps the shape it has always had, so nothing that worked
  // before starts reading differently. A whole-topic path that expanded to
  // several series never takes this branch (reads.size() > 1 then).
  if (reads.size() == 1) {
    json only = out["read"].front();
    if (only.contains("error")) {
      return ToolResult::failure(only["error"].get<std::string>());
    }
    if (out.contains("unread")) {
      only["unread"] = out["unread"];
    }
    return ToolResult::success(only.dump());
  }
  return ToolResult::success(out.dump());
}

}  // namespace

JoinForecast forecastJoin(
    const ToolContext& ctx, const PJ::sdk::CatalogSnapshot& catalog, const std::vector<std::string>& inputs) {
  JoinForecast out;
  std::vector<std::int64_t> common;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    auto lookup = resolveSeriesPath(catalog, inputs[i]);
    if (!lookup.resolved) {
      return out;
    }
    auto view = ctx.host.readSeries(lookup.resolved->handle);
    if (!view) {
      return out;
    }
    std::vector<std::int64_t> ts;
    std::vector<double> vals;
    if (!readSeriesDoubles(*view, ts, vals)) {
      return out;  // a non-numeric input fails later, on its own terms
    }
    const SeriesStats stats = computeStats(ts, vals);
    std::ostringstream rate;
    rate << inputs[i] << " (" << std::fixed << std::setprecision(1) << stats.rate_hz << " Hz, " << ts.size()
         << " samples)";
    out.rates.push_back(rate.str());
    out.smallest = (i == 0) ? ts.size() : std::min(out.smallest, ts.size());

    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    if (i == 0) {
      common = std::move(ts);
    } else {
      std::vector<std::int64_t> next;
      std::set_intersection(common.begin(), common.end(), ts.begin(), ts.end(), std::back_inserter(next));
      common = std::move(next);
    }
  }
  out.checked = true;
  out.shared = common.size();
  return out;
}

// Collect the requested paths from either a bare string or an array.
//
// The parameter is named `paths` rather than `series` on purpose. The object
// store holds point clouds, images and occupancy grids behind the same read
// view, and when this call learns to summarize those too, "series" would be the
// wrong word for what it takes. The model learns the name from the description,
// so renaming it later is more expensive than choosing it now. `series` still
// works, undocumented, so a conversation already in flight does not break.
std::vector<std::string> requestedPaths(const json& args, std::initializer_list<const char*> keys) {
  std::vector<std::string> out;
  for (const char* key : keys) {
    if (!args.contains(key)) {
      continue;
    }
    const json& v = args[key];
    if (v.is_string()) {
      out.push_back(canonicalSeriesPath(v.get<std::string>()));
    } else if (v.is_array()) {
      for (const auto& e : v) {
        if (e.is_string()) {
          out.push_back(canonicalSeriesPath(e.get<std::string>()));
        }
      }
    }
    if (!out.empty()) {
      break;
    }
  }
  return out;
}

// With a playback host bound, report where this series STARTS on the plot
// axis, so the model can turn bucket-relative times into seek/zoom targets:
// display time of a bucket = t_start_display_s + bucket.t. Best-effort — the
// conversion is frame-dependent (user-editable offsets), never an error here.
json statsWithDisplayStart(
    const SeriesStats& stats, ToolContext& ctx, const std::string& topic, const FlatRunSummary& flat) {
  json stats_json = statsToJson(stats);
  if (ctx.playback.valid() && stats.count > 0) {
    if (auto display_s = ctx.playback.toDisplayTime(topic, stats.t_start_ns)) {
      stats_json["t_start_display_s"] = *display_s;
    }
  }
  // Whole-series facts (see FlatRunSummary): a constant channel says so
  // instead of the flat span, which would just restate "the whole thing".
  // Each fact carries a fixed-text interpretation alongside it -- "constant"
  // reads as "stuck" to a model unless told it just means "never sampled
  // differently", and a bare flat span invites the same misreading in the
  // other direction (a change WITHIN the recording, not since the start).
  if (flat.constant) {
    stats_json["constant"] = true;
    stats_json["constant_note"] =
        "held one value for the entire recording — an unused or unmapped output, not something that changed "
        "during it";
  } else if (flat.has_flat_span) {
    stats_json["flat_span_s"] = flat.flat_span_s;
    stats_json["flat_span_at_s"] = flat.flat_span_at_s;
    std::ostringstream note;
    note << std::fixed << std::setprecision(1);
    note << "held one value for " << flat.flat_span_s << " s starting at " << flat.flat_span_at_s
         << " s into the series";
    // Its own display time: flat_span_at_s counts from the series' first sample, while a windowed
    // read's t_start_display_s is the window's.
    if (ctx.playback.valid()) {
      if (auto display_s = ctx.playback.toDisplayTime(topic, flat.flat_span_start_ns)) {
        stats_json["flat_span_display_s"] = *display_s;
        note << " (display " << *display_s << " s)";
      }
    }
    note << " — a change within the recording";
    stats_json["flat_span_note"] = note.str();
  }
  return stats_json;
}

namespace {

// Resolves `want` into r's path, topic and dataset, or explains in r.error why it does not name a series.
std::optional<PJ::sdk::FieldHandle> resolveInto(
    SeriesRead& r, const PJ::sdk::CatalogSnapshot& catalog, const std::string& want,
    const TopicDatasetMap& topic_dataset) {
  r.path = want;
  auto lookup = resolveSeriesPath(catalog, want, topic_dataset);
  if (!lookup.resolved) {
    r.error = seriesLookupError(want, lookup);
    return std::nullopt;
  }
  r.path = lookup.resolved->path;
  r.topic = lookup.resolved->topic;
  r.dataset = lookup.resolved->dataset;
  return lookup.resolved->handle;
}

}  // namespace

SeriesRead readOne(
    const PJ::sdk::CatalogSnapshot& catalog, ToolContext& ctx, const std::string& want,
    const TopicDatasetMap& topic_dataset) {
  SeriesRead r;
  const auto handle = resolveInto(r, catalog, want, topic_dataset);
  if (!handle) {
    return r;
  }
  auto view = ctx.host.readSeries(*handle);
  if (!view) {
    r.error = "read failed for '" + want + "': " + view.error();
    return r;
  }
  if (!readSeriesDoubles(*view, r.ts, r.vals)) {
    r.error = "series '" + want + "' is not a numeric time series";
    return r;
  }
  // Whole-series facts, computed here — before any t_start_s/t_end_s window
  // narrows r.ts/r.vals below — so "never changes" / "freezes for N s" stay
  // claims about the entire recording even under a windowed read.
  r.flat = flatRunSummary(r.ts, r.vals);
  r.ok = true;
  return r;
}

namespace {

// readOne narrowed to the display window [t_start_s, t_end_s], answering exactly what readOne's whole read cut to the
// window answers. With the host's read_window and scan_values, no Arrow array of the whole series is built: the
// whole-series facts (the flat runs, the first sample that anchors the window, the span an empty window reports) come
// from the cheaper value scan, and the samples from a read of the window only.
SeriesRead readOneWindowed(
    const PJ::sdk::CatalogSnapshot& catalog, ToolContext& ctx, const std::string& want,
    const TopicDatasetMap& topic_dataset, const std::optional<double>& t_start_s,
    const std::optional<double>& t_end_s) {
  if ((!t_start_s && !t_end_s) || !ctx.read_window || !ctx.scan_values) {
    SeriesRead r = readOne(catalog, ctx, want, topic_dataset);
    if (r.ok) {
      applyDisplayWindow(r, ctx, catalog, t_start_s, t_end_s);
    }
    return r;
  }
  SeriesRead r;
  const auto handle = resolveInto(r, catalog, want, topic_dataset);
  if (!handle) {
    return r;
  }
  FlatRunAccumulator runs;
  const auto scanned = ctx.scan_values(
      *handle, std::numeric_limits<std::int64_t>::min(),
      [&runs](std::span<const std::int64_t> ts, std::span<const double> values) { runs.addBlock(ts, values); });
  if (!scanned) {
    r.error = "read failed for '" + want + "': " + scanned.error();
    return r;
  }
  if (runs.samples() == 0) {
    r.error = "series '" + want + "' has no numeric samples";
    return r;
  }
  r.flat = runs.summary();
  const auto window = nsWindow(ctx, catalog, r, runs.firstNs(), t_start_s, t_end_s);
  if (!window) {
    r.error = "cannot map the display window for this series";
    return r;
  }
  // Capped at the scan's last sample, so samples a stream appends between the two reads stay out of the window and
  // the samples and the flat facts describe one snapshot (retention, GUI-thread only, cannot move in between).
  auto view = ctx.read_window(*handle, window->first, std::min(window->last, runs.lastNs()));
  if (!view) {
    r.error = "read failed for '" + want + "': " + view.error();
    return r;
  }
  // An empty read is a window outside the series, which cutToWindow reports.
  if (view->rowCount() > 0 && !readSeriesDoubles(*view, r.ts, r.vals)) {
    r.error = "series '" + want + "' is not a numeric time series";
    return r;
  }
  r.ok = true;
  cutToWindow(r, *window, runs.lastNs(), t_start_s, t_end_s);
  return r;
}

}  // namespace

// Coarsen until the serialized payload fits the response cap so spiky data
// stays representable without overrunning the model's context. Returns
// `base` with a "buckets" array attached (and a "note" when it still does not
// fit at the smallest allowed resolution). Shared by read_series's single-path
// 'buckets' mode and evaluate's optional bucket summary.
json withCoarsenedBuckets(
    json base, std::span<const std::int64_t> ts, std::span<const double> vals, std::size_t max_points) {
  return shrinkToFit(
      [&](std::size_t points) {
        base["buckets"] = renderBucketArray(ts, vals, points);
        return base;
      },
      kMaxResponseBytes, max_points);
}

ToolResult readSeriesTool(const json& args, ToolContext& ctx) {
  const std::vector<std::string> paths = requestedPaths(args);
  if (paths.empty()) {
    return ToolResult::failure(
        "read_series requires 'paths': one topic/field path, or an array of them to read several in "
        "a single call");
  }
  const std::string mode = args.value("mode", std::string("stats"));

  // Optional display-axis window, shared by every path in this call (not
  // per-path — one read_series call reads one moment in time). Validated
  // once here so a backwards range fails the whole call instead of quietly
  // producing an empty-window error on every entry.
  std::optional<double> t_start_s;
  std::optional<double> t_end_s;
  if (args.contains("t_start_s") && !args["t_start_s"].is_null()) {
    t_start_s = args["t_start_s"].get<double>();
  }
  if (args.contains("t_end_s") && !args["t_end_s"].is_null()) {
    t_end_s = args["t_end_s"].get<double>();
  }
  if (t_start_s && t_end_s && *t_end_s <= *t_start_s) {
    return ToolResult::failure("t_end_s must be greater than t_start_s");
  }

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  // Built once and threaded through every lookup this call makes (expansion,
  // resolution, the unread disclosure) instead of each one rebuilding it.
  const TopicDatasetMap topic_dataset = datasetByTopicIndex(*catalog);

  // A path naming a bare topic (no field) expands to every numeric field of
  // that topic here -- see expandRequestedPaths. This is where the caps
  // below start counting: a topic that names twelve fields costs twelve,
  // exactly as if the model had named them one by one.
  std::vector<std::string> topic_notes;
  const std::vector<PathTask> tasks = expandRequestedPaths(*catalog, paths, mode, topic_notes, topic_dataset);
  if (tasks.size() > kMaxBatchPaths) {
    return ToolResult::failure(
        "read_series takes at most " + std::to_string(kMaxBatchPaths) + " series per call, got " +
        std::to_string(tasks.size()) + " after expanding any whole-topic paths to their fields");
  }

  // readOne, then narrow to the window when one was requested — shared by
  // every mode below so 'stats' and 'buckets', single-path and batched, all
  // apply the same window the same way.
  auto readWindowed = [&](const std::string& want) {
    return readOneWindowed(*catalog, ctx, want, topic_dataset, t_start_s, t_end_s);
  };
  auto readTask = [&](const PathTask& task) { return task.forced ? forcedFailure(task) : readWindowed(task.path); };

  if (mode == "stats") {
    // One entry per requested path, each carrying its own error. A single typo
    // must not cost the whole round trip — which is the entire point of asking
    // for several at once.
    std::vector<SeriesRead> reads;
    reads.reserve(tasks.size());
    for (const auto& task : tasks) {
      reads.push_back(readTask(task));
    }
    std::string note;
    if (!topic_notes.empty()) {
      note = topic_notes.front();
      for (std::size_t i = 1; i < topic_notes.size(); ++i) {
        note += "; " + topic_notes[i];
      }
    }
    return finishReadEnvelope(
        *catalog, reads, topic_dataset,
        [&](const SeriesRead& r) {
          json entry = {
              {"series", r.path}, {"stats", statsWithDisplayStart(computeStats(r.ts, r.vals), ctx, r.topic, r.flat)}};
          if (!r.window.is_null()) {
            entry["window"] = r.window;
          }
          return entry;
        },
        note);
  }

  if (mode == "buckets") {
    // Several series' buckets share ONE response cap (batchBuckets), so the
    // batch is capped tighter than the flat kMaxBatchPaths above: past this
    // many, sharing the cap would coarsen every one of them past usefulness.
    if (tasks.size() > kMaxBucketsBatchPaths) {
      return ToolResult::failure(
          "mode 'buckets' takes at most " + std::to_string(kMaxBucketsBatchPaths) +
          " paths per call — they share one response cap. Batch 'stats' first, then ask for buckets on "
          "the series worth the shape.");
    }
    const std::size_t max_points = static_cast<std::size_t>(std::clamp(args.value("max_points", 200), 1, 500));
    // A lone path keeps the shape it has always had, at full single-series
    // resolution — the batch envelope (and its shared, smaller cap) only
    // kicks in once there is more than one series to fit together.
    if (tasks.size() == 1) {
      SeriesRead r = readTask(tasks.front());
      return finishReadEnvelope(*catalog, {r}, topic_dataset, [&](const SeriesRead& rr) {
        const json stats_json = statsWithDisplayStart(computeStats(rr.ts, rr.vals), ctx, rr.topic, rr.flat);
        json base = {{"series", rr.path}, {"stats", stats_json}};
        if (!rr.window.is_null()) {
          base["window"] = rr.window;
        }
        return withCoarsenedBuckets(std::move(base), rr.ts, rr.vals, max_points);
      });
    }
    std::vector<SeriesRead> reads;
    reads.reserve(tasks.size());
    for (const auto& task : tasks) {
      reads.push_back(readTask(task));
    }
    json out = batchBuckets(ctx, reads, max_points);
    const json unread = unreadDisclosure(*catalog, reads, topic_dataset);
    if (!unread.empty()) {
      out["unread"] = unread;
    }
    return ToolResult::success(out.dump());
  }

  if (mode == "raw") {
    // Raw returns actual values, not a summary, so an unbounded window would
    // let one call dump an entire recording — the window is mandatory rather
    // than optional-and-defaulting-to-everything the way it is for stats and
    // buckets.
    if (!t_start_s || !t_end_s) {
      return ToolResult::failure(
          "mode 'raw' needs t_start_s and t_end_s: it returns samples, so the window is not "
          "optional");
    }
    // Each series can carry up to kMaxRawSamples individual numbers here,
    // not a bounded bucket count, so the batch cap is tighter than buckets'.
    if (tasks.size() > kMaxRawBatchPaths) {
      return ToolResult::failure(
          "mode 'raw' takes at most " + std::to_string(kMaxRawBatchPaths) +
          " paths per call — each can return up "
          "to " +
          std::to_string(kMaxRawSamples) +
          " samples, so more would blow the response cap. Narrow with 'buckets' "
          "or 'stats' first.");
    }
    std::vector<SeriesRead> reads;
    reads.reserve(tasks.size());
    for (const auto& task : tasks) {
      reads.push_back(readTask(task));
    }
    // The 4-paths/200-samples caps above are a guess, not a measurement: a
    // realistic batch of dataset-qualified paths at 200 samples each can
    // still overrun kMaxResponseBytes. Shrink the per-series sample count in
    // lockstep, same as buckets' batch envelope, until the whole response
    // fits (or every series is down to 16 samples).
    const json out = shrinkToFit(
        [&](std::size_t sample_cap) {
          return buildReadEnvelope(*catalog, reads, topic_dataset, [&](const SeriesRead& r) {
            return renderRawSamples(ctx, *catalog, r, sample_cap);
          });
        },
        kMaxResponseBytes, kMaxRawSamples);
    // A lone path keeps the shape it has always had, same as 'stats' and
    // single-path 'buckets': the batch envelope only kicks in once there is
    // more than one series to fit together.
    if (reads.size() == 1) {
      json only = out["read"].front();
      if (only.contains("error")) {
        return ToolResult::failure(only["error"].get<std::string>());
      }
      if (out.contains("unread")) {
        only["unread"] = out["unread"];
      }
      if (out.contains("note")) {
        only["note"] = out["note"];
      }
      return ToolResult::success(only.dump());
    }
    return ToolResult::success(out.dump());
  }
  return ToolResult::failure("unknown mode '" + mode + "' (use 'stats', 'buckets' or 'raw')");
}

namespace {

// wait_for between two resumes: the series it watches and how far it has looked.
// Resolved once, at the call: a fresh catalog snapshot on every resume would copy
// the whole catalog many times a second. The handle stays valid while its dataset
// is loaded; once it is not, the read fails and so does the wait.
//
// "New" is by timestamp: a sample counts when it is later than the latest one seen,
// so one that arrives late with an older or equal stamp is not looked at, and one a
// stream's retention drops between two looks is missed.
struct SeriesWatch : std::enable_shared_from_this<SeriesWatch> {
  // The caller resumes far more often (its tick) than a threshold needs watching.
  static constexpr std::chrono::milliseconds kLookEvery{200};
  static constexpr std::int64_t kLast = std::numeric_limits<std::int64_t>::max();

  PJ::sdk::FieldHandle field;
  std::optional<PJ::sdk::DataSourceHandle> source;
  std::string path;
  std::string topic;
  std::string comparison;
  double threshold = 0.0;
  double timeout_s = 0.0;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point next_look;
  // The stamp of the newest sample already looked at: only later ones can end the wait.
  std::optional<std::int64_t> seen_ns;
  std::optional<double> last_value;

  [[nodiscard]] bool holds(double value) const {
    if (comparison == ">") {
      return value > threshold;
    }
    if (comparison == "<") {
      return value < threshold;
    }
    return comparison == ">=" ? value >= threshold : value <= threshold;
  }

  // The samples after seen_ns, oldest first, a null as NaN where the host can tell (scan_values).
  PJ::Status readNew(ToolContext& ctx, std::vector<std::int64_t>& ts, std::vector<double>& vals) const {
    constexpr std::int64_t kFirst = std::numeric_limits<std::int64_t>::min();
    if (seen_ns == kLast) {
      return PJ::okStatus();  // nothing can be later
    }
    const std::int64_t first = seen_ns ? *seen_ns + 1 : kFirst;
    if (ctx.scan_values) {
      return ctx.scan_values(field, first, [&](std::span<const std::int64_t> stamps, std::span<const double> values) {
        ts.insert(ts.end(), stamps.begin(), stamps.end());
        if (values.size() == stamps.size()) {
          vals.insert(vals.end(), values.begin(), values.end());
        } else if (!values.empty()) {
          vals.insert(vals.end(), stamps.size(), values.front());  // one value every sample of the block holds
        }
      });
    }
    auto view = ctx.read_window && seen_ns ? ctx.read_window(field, first, kLast) : ctx.host.readSeries(field);
    if (!view) {
      return PJ::unexpected(view.error());
    }
    if (view->rowCount() > 0 && !readSeriesDoubles(*view, ts, vals)) {
      return PJ::unexpected(std::string("it is not a numeric time series"));
    }
    // A windowed read also returns the sample before the window, and a whole read everything.
    const auto fresh = seen_ns ? std::upper_bound(ts.begin(), ts.end(), *seen_ns) - ts.begin() : 0;
    ts.erase(ts.begin(), ts.begin() + fresh);
    vals.erase(vals.begin(), vals.begin() + fresh);
    return PJ::okStatus();
  }

  ToolResult poll(ToolContext& ctx) {
    const auto again = [this] {
      return ToolResult::deferred([self = shared_from_this()](ToolContext& current) { return self->poll(current); });
    };
    const auto now = std::chrono::steady_clock::now();
    if (now < next_look) {
      return again();
    }
    next_look = now + kLookEvery;
    std::vector<std::int64_t> ts;
    std::vector<double> vals;
    if (auto read = readNew(ctx, ts, vals); !read) {
      return ToolResult::failure("series '" + path + "' can no longer be read: " + read.error());
    }
    const double waited_s = std::chrono::duration<double>(now - started).count();
    for (std::size_t i = 0; i < ts.size() && i < vals.size(); ++i) {
      if (!holds(vals[i])) {  // false for a NaN, so for a null, whatever the comparison
        continue;
      }
      json out = {{"series", path}, {"value", vals[i]}, {"waited_s", std::round(waited_s * 10.0) / 10.0}};
      if (const auto display_s = toDisplaySeconds(ctx, source, topic, ts[i])) {
        out["time_s"] = *display_s;  // the display axis, as playback and read_series windows take it
      }
      return ToolResult::success(out.dump());
    }
    if (!ts.empty()) {
      seen_ns = ts.back();
    }
    if (!vals.empty()) {
      last_value = vals.back();
    }
    // After the look, not before it: a sample found by the look that notices the timeout still counts.
    if (waited_s >= timeout_s) {
      std::ostringstream why;
      why << "no new sample of '" << path << "' was " << comparison << " " << threshold << " within " << timeout_s
          << " s";
      if (last_value) {
        why << " (the latest was " << *last_value << ")";
      } else {
        why << " (no new sample arrived)";
      }
      return ToolResult::failure(why.str());
    }
    return again();
  }
};

}  // namespace

ToolResult waitForTool(const json& args, ToolContext& ctx) {
  const std::vector<std::string> paths = requestedPaths(args, {"series"});
  if (paths.size() != 1) {
    return ToolResult::failure("'series' must be one topic/field path");
  }
  auto watch = std::make_shared<SeriesWatch>();
  watch->comparison = args.value("comparison", std::string());
  if (!isComparison(watch->comparison)) {
    return ToolResult::failure(kComparisonError);
  }
  const auto threshold = args.find("threshold");
  if (threshold == args.end() || !threshold->is_number()) {
    return ToolResult::failure("'threshold' must be a number");
  }
  watch->threshold = threshold->get<double>();
  constexpr double kDefaultTimeoutS = 60.0;
  constexpr double kMaxTimeoutS = 3600.0;
  const auto timeout = args.find("timeout_s");
  const double asked = timeout != args.end() && timeout->is_number() ? timeout->get<double>() : kDefaultTimeoutS;
  watch->timeout_s = std::isfinite(asked) ? std::clamp(asked, 0.0, kMaxTimeoutS) : kDefaultTimeoutS;

  auto catalog = ctx.host.catalogSnapshot();
  if (!catalog) {
    return ToolResult::failure("catalog unavailable: " + catalog.error());
  }
  SeriesRead resolved;
  const auto field = resolveInto(resolved, *catalog, paths.front(), datasetByTopicIndex(*catalog));
  if (!field) {
    return ToolResult::failure(resolved.error);
  }
  // A field with no numbers would be watched for nothing until the timeout.
  for (const PJ_field_info_t& info : catalog->fields()) {
    if (info.handle.topic.id == field->topic.id && info.handle.id == field->id &&
        !isSignalField(PJ::sdk::fromAbiType(info.type), PJ::sdk::toStringView(info.name))) {
      return ToolResult::failure("series '" + resolved.path + "' is not a numeric time series");
    }
  }
  watch->field = *field;
  watch->source = dataSourceHandleFor(*catalog, resolved.dataset);
  watch->path = resolved.path;
  watch->topic = resolved.topic;
  watch->started = std::chrono::steady_clock::now();
  watch->next_look = watch->started;
  // What is there now does not count: the wait is for a sample that arrives after the call. A window
  // at the end of time holds nothing but "the sample before it", the latest, without decoding the rest.
  constexpr std::int64_t kLast = SeriesWatch::kLast;
  auto latest = ctx.read_window ? ctx.read_window(watch->field, kLast, kLast) : ctx.host.readSeries(watch->field);
  if (!latest) {
    return ToolResult::failure("read failed for '" + watch->path + "': " + latest.error());
  }
  if (const auto stamps = latest->timestamps(); !stamps.empty()) {
    watch->seen_ns = stamps.back();
  }
  return ToolResult::deferred([watch](ToolContext& current) { return watch->poll(current); });
}

}  // namespace PJ::agent_tools::detail
