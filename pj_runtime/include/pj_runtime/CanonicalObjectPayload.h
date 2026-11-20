#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <optional>
#include <span>

#include "pj_base/buffer_anchor.hpp"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/expected.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/WireTarget.h"

namespace PJ {

class SessionManager;

/// Decodes `entries`, stored entries of `type` on `topic`, handing `sink` each entry with its object in order: through
/// the topic's parser under its per-topic lock when one is bound (`decodeObjectEntry`), else from the canonical bytes.
/// A log's own stamp is moved onto the dataset clock by the entry's `payload_stamp_shift`, as TF ingest moves TF stamps
/// (0 means unstamped and stays). An error for an entry whose decode fails. The parser binding is looked up once per
/// call: a reload rebinds it. Any thread.
void decodeStoredObjects(
    const SessionManager& session, ObjectTopicId topic, sdk::BuiltinObjectType type,
    std::span<const ResolvedObjectEntry> entries,
    const std::function<void(const ResolvedObjectEntry& entry, Expected<sdk::BuiltinObject> object)>& sink);

/// The bytes a Widget plugin receives for one resolved object entry of `target`, or the reason it has
/// none. An empty encoder hands the stored payload over as it is.
using ObjectPayloadEncoder =
    std::function<Expected<sdk::PayloadView>(const WireTarget& target, const ResolvedObjectEntry& entry)>;

/// An ObjectPayloadEncoder for the wires of a Custom Widget panel: the canonical bytes of an entry, the form
/// the `pj_base` builtin codecs read. With no parser bound to the topic the stored bytes already are canonical
/// (a source or toolbox pushed the object itself) and are handed over as they are, anchor included. With a
/// parser bound (the store holds the wire message, e.g. ROS CDR), the entry is decoded through it under its
/// per-topic lock (`decodeStoredObjects`) and re-serialized with `serializeBuiltinObject`, into a buffer the view
/// owns; so is a log whose stamp `decodeStoredObjects` shifts. An error when the decode or the serialization fails. The
/// parser binding is looked up per call: a reload rebinds it. Any thread. `session` must outlive the encoder.
[[nodiscard]] ObjectPayloadEncoder canonicalObjectEncoder(const SessionManager& session);

/// The entry `uid` of `target`'s topic with `payload` replaced by what a widget receives (`encoder`, or
/// the stored bytes when it is empty); nullopt when the entry was evicted, its lazy fetch failed or it
/// does not encode. May refetch a lazy payload, so call it outside any store lock.
[[nodiscard]] std::optional<ResolvedObjectEntry> resolveEncodedObject(
    const ObjectStore& objects, const WireTarget& target, SequentialUID uid, const ObjectPayloadEncoder& encoder);

}  // namespace PJ
