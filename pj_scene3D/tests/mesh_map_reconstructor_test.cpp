// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// MeshMapReconstructor against a fake timeline: forward replay applies only the
// new updates; a rewind, another map or invalidate() asks for the map again;
// which stream's updates belong to the map (the sibling, or by geometry id) and
// that pairing staying across maps; another mesh's entries fetched once; what a
// replay reports as refused and lost; the dirty spans of one advance().

#include "pj_scene3d_core/mesh_map_reconstructor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace pj::scene3d;
using PJ::sdk::MeshMapUpdate;
using Datatype = PJ::sdk::PointField::Datatype;
using Target = PJ::sdk::MeshChannel::Target;

constexpr uint64_t kSibling = 1;
constexpr uint64_t kOther = 2;

// Byte storage outliving the spans handed out.
std::vector<std::shared_ptr<std::vector<uint8_t>>>& blobs() {
  static std::vector<std::shared_ptr<std::vector<uint8_t>>> storage;
  return storage;
}

template <typename T>
PJ::Span<const uint8_t> keep(const std::vector<T>& values) {
  auto blob = std::make_shared<std::vector<uint8_t>>(values.size() * sizeof(T));
  std::memcpy(blob->data(), values.data(), blob->size());
  blobs().push_back(blob);
  return {blob->data(), blob->size()};
}

// Four faces around a centre vertex, a float `cost` face channel of zeros.
MeshMapState makeState(const std::string& geometry_id) {
  PJ::sdk::MeshMap mesh;
  mesh.frame_id = "map";
  mesh.geometry_id = geometry_id;
  mesh.vertex_count = 5;
  mesh.positions = keep(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0});
  mesh.face_count = 4;
  mesh.indices = keep(std::vector<uint32_t>{0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1});
  PJ::sdk::MeshChannel cost;
  cost.name = "cost";
  cost.target = Target::kFace;
  cost.datatype = Datatype::kFloat32;
  cost.data = keep(std::vector<float>{0, 0, 0, 0});
  mesh.channels = {cost};
  return decodeMeshMap(mesh);
}

// A sparse update writing `value` to `face` of `cost`.
MeshMapUpdate makeUpdate(uint32_t face, float value, const std::string& geometry_id) {
  PJ::sdk::MeshChannelPatch patch;
  patch.channel.name = "cost";
  patch.channel.target = Target::kFace;
  patch.channel.datatype = Datatype::kFloat32;
  patch.channel.data = keep(std::vector<float>{value});
  patch.indices = keep(std::vector<uint32_t>{face});
  MeshMapUpdate update;
  update.geometry_id = geometry_id;
  update.patches.push_back(patch);
  return update;
}

// The injected timeline: entries over all streams, and how many were fetched.
struct Timeline {
  enum class Kind { kUpdate, kInvalid, kLost };
  struct Entry {
    PJ::Timestamp ts = 0;
    uint64_t stream = 0;
    uint64_t identity = 0;
    Kind kind = Kind::kUpdate;
    MeshMapUpdate update;
  };
  std::vector<Entry> entries;
  int fetched = 0;

  void add(PJ::Timestamp ts, uint64_t stream, MeshMapUpdate update) {
    entries.push_back({ts, stream, entries.size() + 1, Kind::kUpdate, std::move(update)});
  }
  void addBroken(PJ::Timestamp ts, uint64_t stream, Kind kind) {
    entries.push_back({ts, stream, entries.size() + 1, kind, {}});
  }
  [[nodiscard]] MeshMapReconstructor::UpdatesProvider provider() {
    return [this](PJ::Timestamp lo, PJ::Timestamp hi, MeshMapReconstructor::Sink& sink) {
      std::vector<const Entry*> window;
      for (const Entry& entry : entries) {
        if (entry.ts > lo && entry.ts <= hi) {
          window.push_back(&entry);
        }
      }
      std::stable_sort(window.begin(), window.end(), [](const Entry* a, const Entry* b) { return a->ts < b->ts; });
      for (const Entry* entry : window) {
        if (!sink.wants(entry->stream, entry->identity)) {
          continue;
        }
        ++fetched;
        if (entry->kind == Kind::kLost) {
          sink.lost(entry->stream);
        } else if (entry->kind == Kind::kInvalid) {
          sink.invalid(entry->stream, entry->identity, "broken");
        } else {
          sink.apply(entry->stream, entry->identity, entry->update);
        }
      }
    };
  }
};

struct Rig {
  MeshMapReconstructor reconstructor;
  Timeline timeline;

  Rig() {
    const std::vector<MeshMapReconstructor::Stream> streams = {
        {.key = kSibling, .sibling = true}, {.key = kOther, .sibling = false}};
    reconstructor.setStreams(streams);
  }
  [[nodiscard]] double cost(uint32_t face) const {
    return reconstructor.state()->find(Target::kFace, "cost")->values[face];
  }
  MeshMapReplay advance(PJ::Timestamp t) {
    return reconstructor.advance(t, timeline.provider());
  }
};

TEST(MeshMapReconstructorTest, AppliesOnlyTheNewUpdatesGoingForward) {
  Rig rig;
  rig.timeline.add(100, kSibling, makeUpdate(0, 1.0F, "A"));  // stamped with the map
  rig.timeline.add(120, kSibling, makeUpdate(1, 2.0F, "A"));
  rig.timeline.add(180, kSibling, makeUpdate(2, 3.0F, "A"));
  rig.timeline.add(90, kSibling, makeUpdate(3, 9.0F, "A"));  // before the map: never applies

  EXPECT_TRUE(rig.reconstructor.needsBase(7, 150));
  rig.reconstructor.rebase(7, 100, makeState("A"));
  EXPECT_FALSE(rig.reconstructor.needsBase(7, 150));
  EXPECT_EQ(rig.advance(150).applied, 2);
  EXPECT_DOUBLE_EQ(rig.cost(0), 1.0);
  EXPECT_DOUBLE_EQ(rig.cost(1), 2.0);
  EXPECT_DOUBLE_EQ(rig.cost(2), 0.0);
  EXPECT_DOUBLE_EQ(rig.cost(3), 0.0);
  EXPECT_EQ(rig.reconstructor.appliedUntil(), 150);

  EXPECT_EQ(rig.advance(200).applied, 1);
  EXPECT_EQ(rig.timeline.fetched, 3) << "the second advance read only its own window";
  EXPECT_DOUBLE_EQ(rig.cost(2), 3.0);
  EXPECT_EQ(rig.advance(200).applied, 0) << "not ahead: a no-op";
}

TEST(MeshMapReconstructorTest, ARewindAnotherMapOrInvalidateAsksForTheMap) {
  Rig rig;
  rig.reconstructor.rebase(7, 100, makeState("A"));
  (void)rig.advance(200);
  EXPECT_FALSE(rig.reconstructor.needsBase(7, 200));
  EXPECT_TRUE(rig.reconstructor.needsBase(7, 150)) << "before what was applied";
  EXPECT_TRUE(rig.reconstructor.needsBase(8, 300)) << "another map entry";
  rig.reconstructor.invalidate();
  EXPECT_TRUE(rig.reconstructor.needsBase(7, 300));
  rig.reconstructor.rebase(7, 100, makeState("A"));
  EXPECT_FALSE(rig.reconstructor.needsBase(7, 300));
  rig.reconstructor.clear();
  EXPECT_EQ(rig.reconstructor.state(), nullptr);
  EXPECT_TRUE(rig.reconstructor.needsBase(7, 300));
  EXPECT_EQ(rig.advance(300).applied, 0) << "no state: nothing to apply to";
}

TEST(MeshMapReconstructorTest, TheSiblingAppliesAnyUpdateAnotherStreamOnlyByGeometryId) {
  Rig rig;
  rig.timeline.add(110, kSibling, makeUpdate(0, 1.0F, ""));
  rig.timeline.add(120, kOther, makeUpdate(1, 2.0F, ""));   // no id on a non-sibling: not ours
  rig.timeline.add(130, kOther, makeUpdate(2, 3.0F, "B"));  // another mesh
  rig.reconstructor.rebase(7, 100, makeState("A"));
  EXPECT_TRUE(rig.reconstructor.paired(kSibling));
  EXPECT_FALSE(rig.reconstructor.paired(kOther));

  MeshMapReplay replay = rig.advance(150);
  EXPECT_EQ(replay.applied, 1);
  EXPECT_EQ(replay.refused, 0) << "another stream's updates are not this map's trouble";
  EXPECT_DOUBLE_EQ(rig.cost(1), 0.0);
  EXPECT_DOUBLE_EQ(rig.cost(2), 0.0);
  EXPECT_FALSE(rig.reconstructor.paired(kOther));

  rig.timeline.add(160, kOther, makeUpdate(3, 4.0F, "A"));
  EXPECT_EQ(rig.advance(200).applied, 1);
  EXPECT_DOUBLE_EQ(rig.cost(3), 4.0);
  EXPECT_TRUE(rig.reconstructor.paired(kOther));

  // The pairing outlives the map; reset() ends it.
  rig.reconstructor.rebase(8, 300, makeState("C"));
  EXPECT_TRUE(rig.reconstructor.paired(kOther));
  rig.reconstructor.reset();
  EXPECT_FALSE(rig.reconstructor.paired(kOther));
  EXPECT_FALSE(rig.reconstructor.paired(kSibling)) << "the streams went with it";
}

TEST(MeshMapReconstructorTest, AnotherMeshsEntriesAreFetchedOnce) {
  Rig rig;
  for (PJ::Timestamp ts = 110; ts < 200; ts += 10) {
    rig.timeline.add(ts, kOther, makeUpdate(0, 5.0F, "B"));
  }
  rig.timeline.addBroken(195, kOther, Timeline::Kind::kInvalid);
  rig.timeline.add(150, kSibling, makeUpdate(1, 1.0F, "A"));
  rig.reconstructor.rebase(7, 100, makeState("A"));
  EXPECT_EQ(rig.advance(200).applied, 1);
  EXPECT_EQ(rig.timeline.fetched, 11);
  EXPECT_EQ(rig.reconstructor.foreignEntryCount(), 10u);

  // A rewind replays the window: only the sibling's update is read again.
  rig.reconstructor.rebase(7, 100, makeState("A"));
  EXPECT_EQ(rig.advance(200).applied, 1);
  EXPECT_EQ(rig.timeline.fetched, 12);
  EXPECT_FALSE(rig.reconstructor.wants(kOther, 1));
  EXPECT_TRUE(rig.reconstructor.wants(kSibling, 11));

  // "Not this geometry" holds only while the geometry does.
  rig.reconstructor.rebase(8, 100, makeState("B"));
  EXPECT_EQ(rig.reconstructor.foreignEntryCount(), 0u);
  const MeshMapReplay replay = rig.advance(200);
  EXPECT_EQ(replay.applied, 9);
  EXPECT_EQ(replay.refused, 1) << "the sibling's update still names A";
  EXPECT_DOUBLE_EQ(rig.cost(0), 5.0);
}

TEST(MeshMapReconstructorTest, ReportsWhatASiblingCouldNotApply) {
  Rig rig;
  rig.timeline.add(110, kSibling, makeUpdate(0, 1.0F, "B"));  // names another geometry
  rig.timeline.addBroken(120, kSibling, Timeline::Kind::kInvalid);
  rig.timeline.addBroken(130, kSibling, Timeline::Kind::kLost);
  rig.timeline.addBroken(140, kOther, Timeline::Kind::kLost);  // not paired: not ours to report
  rig.timeline.add(150, kSibling, makeUpdate(9, 1.0F, "A"));   // a face the map does not have
  rig.reconstructor.rebase(7, 100, makeState("A"));
  const MeshMapReplay replay = rig.advance(200);
  EXPECT_EQ(replay.applied, 1);
  EXPECT_EQ(replay.refused, 2);
  EXPECT_EQ(replay.decode_error, "broken");
  EXPECT_EQ(replay.lost, 1);
  EXPECT_EQ(replay.skipped_indices, 1u);
  EXPECT_DOUBLE_EQ(rig.cost(0), 0.0);
}

TEST(MeshMapReconstructorTest, AChangedStreamSetKeepsPairingsAndStartsOver) {
  Rig rig;
  rig.timeline.add(110, kOther, makeUpdate(0, 1.0F, "A"));
  rig.reconstructor.rebase(7, 100, makeState("A"));
  (void)rig.advance(200);
  ASSERT_TRUE(rig.reconstructor.paired(kOther));

  const std::vector<MeshMapReconstructor::Stream> same = {
      {.key = kSibling, .sibling = true}, {.key = kOther, .sibling = false}};
  rig.reconstructor.setStreams(same);
  EXPECT_FALSE(rig.reconstructor.needsBase(7, 200)) << "the same set changes nothing";

  const std::vector<MeshMapReconstructor::Stream> more = {
      {.key = kSibling, .sibling = true}, {.key = kOther, .sibling = false}, {.key = 3, .sibling = false}};
  rig.reconstructor.setStreams(more);
  EXPECT_TRUE(rig.reconstructor.needsBase(7, 200)) << "the new stream may hold updates already passed";
  EXPECT_TRUE(rig.reconstructor.paired(kOther));
  EXPECT_FALSE(rig.reconstructor.paired(3));
}

TEST(MeshMapReconstructorTest, DirtySpansCoverOneAdvance) {
  Rig rig;
  rig.timeline.add(110, kSibling, makeUpdate(1, 1.0F, "A"));
  rig.timeline.add(120, kSibling, makeUpdate(2, 1.0F, "A"));
  rig.timeline.add(160, kSibling, makeUpdate(3, 1.0F, "A"));
  rig.reconstructor.rebase(7, 100, makeState("A"));
  const auto dirty = [&rig] { return rig.reconstructor.state()->find(Target::kFace, "cost")->dirty; };
  (void)rig.advance(150);
  EXPECT_EQ(dirty(), (ElementRange{.begin = 1, .end = 3}));
  (void)rig.advance(200);
  EXPECT_EQ(dirty(), (ElementRange{.begin = 3, .end = 4}));
  (void)rig.advance(300);
  EXPECT_TRUE(dirty().empty());
}

}  // namespace
