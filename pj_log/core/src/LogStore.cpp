// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_log/LogStore.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace PJ::log_view {

namespace {

constexpr uint32_t kUnmapped = UINT32_MAX;

/// Merge order: time, then wire, then key within the wire.
bool before(const RowRef& a, const RowRef& b) {
  if (a.time != b.time) {
    return a.time < b.time;
  }
  if (a.wire != b.wire) {
    return a.wire < b.wire;
  }
  return a.key < b.key;
}

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Lower-cased exactly as containsCaseInsensitive() folds the haystack: std::tolower follows the C locale, which Qt
/// sets from the environment, and could fold UTF-8 bytes the haystack keeps.
std::string lowerNeedle(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), lowerAscii);
  return text;
}

/// The text a row shows and filters on: terminal escapes stripped.
std::string shownText(std::string_view message) {
  std::string text(message);
  stripTerminalEscapes(text);
  return text;
}

}  // namespace

// --- StoreChange ---

void StoreChange::then(const StoreChange& next) {
  if (reset || next.reset) {
    reset = true;
    return;
  }
  // `next` is expressed against the state after this change, where the unchanged rows are
  // [0, old_tail - dropped_front); its own unchanged range starts at its dropped_front.
  old_tail = std::min(old_tail, next.old_tail + dropped_front);
  dropped_front += next.dropped_front;
}

// --- LogStore ---

LogStore::LogStore() {
  strings_.emplace_back();  // id 0 = ""
}

uint32_t LogStore::intern(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const auto found = string_ids_.find(text);
  if (found != string_ids_.end()) {
    return found->second;
  }
  const auto id = static_cast<uint32_t>(strings_.size());
  strings_.emplace_back(text);
  string_ids_.emplace(strings_.back(), id);
  return id;
}

bool LogStore::hasSources() const {
  return std::ranges::any_of(wires_, [](const auto& pair) { return pair.second.any_source; });
}

bool LogStore::hasCategories() const {
  return std::ranges::any_of(wires_, [](const auto& pair) { return pair.second.any_category; });
}

const LogRowSource* LogStore::source(uint32_t wire) const {
  const auto found = wires_.find(wire);
  return found != wires_.end() ? found->second.source.get() : nullptr;
}

uint32_t LogStore::panelId(uint32_t wire, uint32_t id) const {
  const auto found = wires_.find(wire);
  if (found == wires_.end() || id >= found->second.ids.size() || found->second.ids[id] == kUnmapped) {
    return 0;
  }
  return found->second.ids[id];
}

void LogStore::mapIds(Wire& state, const LogRowSource& source, size_t index) {
  const uint32_t source_id = source.sourceId(index);
  const uint32_t category_id = source.categoryId(index);
  for (const uint32_t id : {source_id, category_id}) {
    if (id >= state.ids.size()) {
      state.ids.resize(id + 1, kUnmapped);
    }
    if (state.ids[id] == kUnmapped) {
      state.ids[id] = id == 0 ? 0 : intern(source.dictionaryText(id));
    }
  }
  state.any_source = state.any_source || source_id != 0;
  state.any_category = state.any_category || category_id != 0;
}

StoreChange LogStore::apply(uint32_t wire, Wire& state, const LogRowSource& source, std::vector<RowRef> fresh) {
  StoreChange change = StoreChange::none(refs_.size());
  // A source evicts its oldest rows, so the indexed rows it evicted are the wire's first ones in the index: everything
  // of the wire before its first row still held is gone (a late row can be older, but it is not indexed yet). The
  // scan starts at the wire's oldest indexed time, so other wires' older rows are not walked.
  size_t from = static_cast<size_t>(std::distance(
      refs_.begin(),
      std::lower_bound(refs_.begin(), refs_.end(), RowRef{.time = state.oldest, .wire = 0, .key = 0}, before)));
  size_t stop = from;
  while (stop < refs_.size() && (refs_[stop].wire != wire || !source.find(refs_[stop].key, 0).has_value())) {
    ++stop;
  }
  state.oldest = stop < refs_.size() ? refs_[stop].time : std::numeric_limits<double>::infinity();
  const auto mine = [wire](const RowRef& ref) { return ref.wire == wire; };
  if (from == 0) {
    // The leading run leaves from the front (a deque pops it without moving the rest).
    size_t lead = 0;
    while (lead < stop && mine(refs_[lead])) {
      ++lead;
    }
    refs_.erase(refs_.begin(), refs_.begin() + static_cast<std::ptrdiff_t>(lead));
    change.dropped_front = lead;
    stop -= lead;
  }
  size_t first = from;
  while (first < stop && !mine(refs_[first])) {
    ++first;
  }
  if (first < stop) {
    const auto end = refs_.begin() + static_cast<std::ptrdiff_t>(stop);
    refs_.erase(std::remove_if(refs_.begin() + static_cast<std::ptrdiff_t>(first), end, mine), end);
    change.old_tail = first + change.dropped_front;
  }
  if (!fresh.empty()) {
    std::ranges::sort(fresh, before);
    state.oldest = std::min(state.oldest, fresh.front().time);
    const auto at = std::lower_bound(refs_.begin(), refs_.end(), fresh.front(), before);
    const auto position = static_cast<size_t>(std::distance(refs_.begin(), at));
    change.old_tail = std::min(change.old_tail, position + change.dropped_front);
    if (position == refs_.size()) {
      refs_.insert(refs_.end(), fresh.begin(), fresh.end());
    } else {
      // Merge the new rows with the tail they interleave with; the head stays where it is.
      std::vector<RowRef> tail(at, refs_.end());
      refs_.erase(at, refs_.end());
      std::merge(tail.begin(), tail.end(), fresh.begin(), fresh.end(), std::back_inserter(refs_), before);
    }
  }
  return change;
}

StoreChange LogStore::update(uint32_t wire, std::shared_ptr<const LogRowSource> source) {
  Wire& state = wires_[wire];
  state.source = std::move(source);
  const LogRowSource& src = *state.source;
  StoreChange change;
  src.batch([&] {
    std::vector<RowRef> fresh;
    const std::vector<size_t> added = src.indicesAfter(state.newest);
    fresh.reserve(added.size());
    for (const size_t index : added) {
      const uint64_t key = src.key(index);
      fresh.push_back(RowRef{.time = src.time(index), .wire = wire, .key = key});
      state.newest = std::max(state.newest, key);
      mapIds(state, src, index);
    }
    change = apply(wire, state, src, std::move(fresh));
  });
  return change;
}

StoreChange LogStore::prune(uint32_t wire) {
  const auto found = wires_.find(wire);
  if (found == wires_.end() || !found->second.source) {
    return StoreChange::none(refs_.size());
  }
  const LogRowSource& src = *found->second.source;
  StoreChange change;
  src.batch([&] { change = apply(wire, found->second, src, {}); });
  return change;
}

StoreChange LogStore::removeWire(uint32_t wire) {
  StoreChange change = StoreChange::none(refs_.size());
  const auto first = std::ranges::find(refs_, wire, &RowRef::wire);
  change.old_tail = static_cast<size_t>(std::distance(refs_.begin(), first));
  refs_.erase(std::remove_if(first, refs_.end(), [wire](const RowRef& ref) { return ref.wire == wire; }), refs_.end());
  wires_.erase(wire);
  return change;
}

StoreChange LogStore::replaceWire(uint32_t wire, std::shared_ptr<const LogRowSource> source) {
  StoreChange change = removeWire(wire);
  change.then(update(wire, std::move(source)));
  return change;
}

LogRow LogStore::row(size_t index, bool with_message) const {
  const RowRef& ref = refs_[index];
  LogRow out;
  out.time = ref.time;
  out.wire = ref.wire;
  const auto found = wires_.find(ref.wire);
  if (found == wires_.end() || !found->second.source) {
    return out;
  }
  const LogRowSource& src = *found->second.source;
  src.batch([&] {
    const auto found_at = src.find(ref.key, SIZE_MAX);
    if (!found_at) {
      return;  // evicted since the last update
    }
    const size_t at = *found_at;
    out.present = true;
    out.level = src.level(at);
    out.stamp_ns = src.ownStamp(at);
    out.source = panelId(ref.wire, src.sourceId(at));
    out.category = panelId(ref.wire, src.categoryId(at));
    if (with_message) {
      src.messages(at, 1, [&out](size_t, std::string_view message) { out.message = shownText(message); });
    }
  });
  return out;
}

LogStore::RowKey LogStore::keyOf(size_t index) const {
  const RowRef& ref = refs_[index];
  const RowRef probe{.time = ref.time, .wire = ref.wire, .key = 0};
  const auto end = refs_.begin() + static_cast<std::ptrdiff_t>(index);
  const auto first = std::lower_bound(refs_.begin(), end, probe, before);
  return RowKey{ref.time, ref.wire, static_cast<size_t>(std::distance(first, end))};
}

std::optional<size_t> LogStore::indexOf(const RowKey& key) const {
  const RowRef probe{.time = key.time, .wire = key.wire, .key = 0};
  const auto first = std::lower_bound(refs_.begin(), refs_.end(), probe, before);
  const size_t index = static_cast<size_t>(std::distance(refs_.begin(), first)) + key.rank;
  if (index >= refs_.size() || refs_[index].time != key.time || refs_[index].wire != key.wire) {
    return std::nullopt;
  }
  return index;
}

// --- Filter ---

bool containsCaseInsensitive(std::string_view haystack, std::string_view lower_needle) {
  if (lower_needle.empty()) {
    return true;
  }
  const auto found = std::search(
      haystack.begin(), haystack.end(), lower_needle.begin(), lower_needle.end(),
      [](char a, char b) { return lowerAscii(a) == b; });
  return found != haystack.end();
}

CompiledFilter::CompiledFilter(const LogFilter& filter, const LogStore& store)
    : store_(store),
      levels_(filter.levels),
      source_needle_(lowerNeedle(filter.source)),
      category_needle_(lowerNeedle(filter.category)),
      needle_(lowerNeedle(filter.text)),
      hidden_wires_(filter.hidden_wires) {
  levels_[static_cast<size_t>(LogLevel::kUnknown)] = true;
  std::sort(hidden_wires_.begin(), hidden_wires_.end());
}

bool CompiledFilter::idPasses(std::vector<char>& passes, uint32_t id, const std::string& needle) const {
  if (needle.empty()) {
    return true;
  }
  if (id >= passes.size()) {
    passes.resize(id + 1, -1);
  }
  if (passes[id] < 0) {
    passes[id] = containsCaseInsensitive(store_.text(id), needle) ? 1 : 0;
  }
  return passes[id] == 1;
}

size_t CompiledFilter::collect(size_t from, size_t to, size_t budget_rows, std::vector<uint32_t>& out) const {
  const size_t stop = std::min(to, from + std::max<size_t>(budget_rows, 1));
  if (from >= stop) {
    return stop;
  }
  // A wire's rows in [from, stop) are consecutive rows of its source: read them in one batch per wire.
  std::map<uint32_t, std::vector<size_t>> by_wire;
  for (size_t i = from; i < stop; ++i) {
    by_wire[store_.ref(i).wire].push_back(i);
  }
  std::vector<char> pass(stop - from, 0);
  for (const auto& [wire, positions] : by_wire) {
    if (std::binary_search(hidden_wires_.begin(), hidden_wires_.end(), wire)) {
      continue;
    }
    const LogRowSource* src = store_.source(wire);
    if (src == nullptr) {
      continue;
    }
    src->batch([&] {
      size_t hint = 0;  // a wire's positions are its rows in order
      struct Candidate {
        size_t position;
        size_t at;  ///< its index in the source
      };
      std::vector<Candidate> candidates;  // positions passing everything but the text
      for (const size_t position : positions) {
        const auto found_at = src->find(store_.ref(position).key, hint);
        if (!found_at) {
          continue;
        }
        const size_t at = *found_at;
        hint = at + 1;
        const auto level = static_cast<size_t>(src->level(at));
        if (level < levels_.size() && !levels_[level]) {
          continue;
        }
        if ((!source_needle_.empty() &&
             !idPasses(source_passes_, store_.panelId(wire, src->sourceId(at)), source_needle_)) ||
            (!category_needle_.empty() &&
             !idPasses(category_passes_, store_.panelId(wire, src->categoryId(at)), category_needle_))) {
          continue;
        }
        candidates.push_back({position, at});
      }
      if (candidates.empty()) {
        return;
      }
      if (needle_.empty()) {
        for (const Candidate& candidate : candidates) {
          pass[candidate.position - from] = 1;
        }
        return;
      }
      std::ranges::sort(candidates, {}, &Candidate::at);  // one pass over the source, in its order
      const size_t first = candidates.front().at;
      size_t next = 0;
      src->messages(first, candidates.back().at - first + 1, [&](size_t index, std::string_view message) {
        if (next >= candidates.size() || candidates[next].at != index) {
          return;
        }
        const bool escaped = message.find('\x1b') != std::string_view::npos;
        if (escaped ? containsCaseInsensitive(shownText(message), needle_)
                    : containsCaseInsensitive(message, needle_)) {
          pass[candidates[next].position - from] = 1;
        }
        ++next;
      });
    });
  }
  for (size_t i = from; i < stop; ++i) {
    if (pass[i - from] != 0) {
      out.push_back(static_cast<uint32_t>(i));
    }
  }
  return stop;
}

size_t revealedCount(const LogStore& store, const std::vector<uint32_t>& filtered, double tracker) {
  const auto end = std::upper_bound(filtered.begin(), filtered.end(), tracker, [&store](double t, uint32_t index) {
    return t < store.ref(index).time;
  });
  return static_cast<size_t>(std::distance(filtered.begin(), end));
}

void stripTerminalEscapes(std::string& text) {
  constexpr char kEsc = '\x1b';
  size_t in = text.find(kEsc);
  if (in == std::string::npos) {
    return;
  }
  size_t out = in;
  const size_t size = text.size();
  while (in < size) {
    if (text[in] != kEsc) {
      text[out++] = text[in++];
      continue;
    }
    ++in;
    if (in >= size) {
      break;
    }
    if (text[in] == '[') {
      // CSI: parameter and intermediate bytes, then one final byte in 0x40..0x7E.
      ++in;
      while (in < size &&
             (static_cast<unsigned char>(text[in]) < 0x40 || static_cast<unsigned char>(text[in]) > 0x7E)) {
        ++in;
      }
      ++in;
    } else if (text[in] == ']') {
      // OSC: up to BEL or ESC backslash.
      ++in;
      while (in < size && text[in] != '\a' && !(text[in] == kEsc && in + 1 < size && text[in + 1] == '\\')) {
        ++in;
      }
      in += (in < size && text[in] == kEsc) ? 2 : 1;
    } else {
      // Any other: intermediate bytes in 0x20..0x2F, then one final byte (ESC ( B, ESC 7, ...).
      while (in < size && text[in] >= 0x20 && text[in] <= 0x2F) {
        ++in;
      }
      ++in;
    }
  }
  text.resize(out);
}

}  // namespace PJ::log_view
