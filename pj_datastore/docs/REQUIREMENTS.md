# pj_datastore Requirements

## 1. Purpose

Columnar time-series storage engine for PlotJuggler. Decouples data storage from UI, plugin formats, and transport protocols. Provides a single, type-safe data layer that all plugins write to and all consumers read from.

## 2. Goals

- Memory-efficient columnar storage with adaptive per-column encoding (constant, frame-of-reference, dictionary, packed bools)
- Full type fidelity: float32, float64, int32, int64, uint64, bool, string — no lossy double-only representation
- Derived (computed) series via a transform DAG supporting both single-input/single-output (SISO) and multi-input/multi-output (MIMO) transforms
- Decoupled plugin API: plugins interact through host-provided interfaces, never touching engine internals
- Independent time domains for comparing data from multiple sources with different clocks

## 3. Use Cases

- **One-shot file import**: CSV, Parquet, ULog, MCAP — read entire file, write all data, done
- **Continuous streaming ingest**: ZMQ, MQTT, WebSocket — data arrives indefinitely, rolling buffer with retention
- **Delegated ingest**: Source plugins push raw bytes; host routes to appropriate parser plugin which writes parsed fields
- **Derived series**: User-defined transforms (derivative, moving average, quaternion-to-RPY, Lua scripts) compute new series from existing data
- **Time-range queries**: Retrieve all samples in a time window for plotting
- **Latest-at queries**: Retrieve the most recent sample at or before a given time (for real-time displays)
- **Rolling buffer with eviction**: Streaming sources need bounded memory; old data evicted outside a configurable retention window

## 4. Functional Requirements

### 4.1 Data Model

- Data is organized as: Dataset, Topic, Field (column)
- A Dataset represents one data source (e.g., one file, one network connection)
- A Topic represents one logical data stream (e.g., one sensor, one message type)
- Fields are typed columns within a topic, sharing the same timestamp column
- Schemas describe the structure of typed topics (struct with named, typed fields)

### 4.2 Type System

- Supported primitive types: float32, float64, int32, int64, uint64, bool, string
- Values represented as a type-safe variant (ValueRef) preserving native precision
- No implicit narrowing: int64 and uint64 values must not be silently cast to double

### 4.3 Ingest

- Row-at-a-time append: set fields by name or by pre-resolved handle, then commit the row
- Bulk columnar append: write arrays of timestamps + column data in one call
- Sparse records: fields not included in a row are automatically null-filled
- Fields may be introduced at any point during ingestion — not just before the first row. This is the expected behavior for variable-length sequences (ROS, Protobuf, DDS) and dynamic-schema formats (JSON). When a new field appears after rows exist, the engine seals the current chunk and continues with the expanded column set.
- Pre-registration with `ensureField()` is an optional optimization for the minority of sources with a fixed, fully-known schema. It enables the faster bound-write path (`appendBoundRecord`) and avoids mid-stream chunk sealing.
- Out-of-order timestamps within a topic are accepted **losslessly** (nanosecond resolution, absolute epoch time): real recordings interleave publishers whose embedded stamps disagree (e.g. a localizer future-dating its transforms). Rows are stable-sorted per chunk at seal time and queries serve results in timestamp order.
- Arrow IPC import: accept Arrow record batches for high-throughput bulk ingest

### 4.4 Storage

- Chunked columnar storage: data accumulated in mutable builders, sealed into immutable chunks at configurable row thresholds
- Per-column encoding selected at seal time based on data statistics (constant values, integer ranges, string cardinality, boolean density)
- Validity bitmaps (Arrow-compatible, LSB-first) track null values per column
- Null values in batch reads returned as NaN so consumers don't confuse null with zero

### 4.5 Schema Evolution

The column set of a topic evolves during ingestion. This is the common case, not the exception — most real-world data sources produce a column count that is unknown at startup and changes as messages arrive:

- **Variable-length sequences** are the norm in schema-based protocols (ROS, DDS, Protobuf, IDL, FlatBuffers). A `repeated float data` field is flattened to columns `data[0]`, `data[1]`, etc. The column count changes with every message that has a different sequence length. Nested messages containing sequences multiply the effect. Even a single ROS `sensor_msgs/PointCloud2` can produce hundreds of dynamically-sized columns.
- **Dynamic-schema formats** (JSON, MessagePack, CBOR) have no fixed schema at all. Each message may introduce new keys.
- **Fixed column count is the exception**, limited to formats where every field is a scalar with no sequences (e.g., a flat CSV, a Protobuf message with only scalar fields). The engine must not be designed around this minority case.

Requirements:

- New fields may appear at any point during ingestion. The engine seals the current chunk and continues with the expanded column set. Rows in earlier chunks have no value for the new column; readers treat absent columns as null.
- Field IDs are append-only and stable — existing handles are never invalidated by later column additions.
- Once a field is created with a given type, it keeps that type. A record value of another type is stored converted when the conversion is exact (e.g. an unsigned `0` into an int64 field, `3` into a float64 field); otherwise that field is null for the record, the rest of the record is still written, and the field is reported once through the ingest-warning handler. Explicitly re-registering a field (`ensureField()`) with a different type is an error.
- Schema changes are resolved between rows, never mid-row.
- Pre-registration with `ensureField()` is an optimization for the fixed-schema case, not a prerequisite for writing data.

### 4.6 Query

- Series queries: view one numeric/bool topic column as a time series of value-bearing samples
- Series sample count, bounds, and timestamp lookups always skip null physical rows
- Range queries: iterate all rows in a time window for a topic
- Latest-at queries: find the most recent row at or before a given timestamp
- Per-row and per-chunk-range iteration for physical-row consumers that need explicit null handling
- Read methods for each type: double, int64, uint64, bool, string, with explicit null checking

### 4.7 Derived Series

- Transform DAG: register transforms that take one or more input topics and produce one or more output topics
- SISO transforms: single input to single output, point-at-a-time sequential contract
- MIMO transforms: N inputs to M outputs, exact-timestamp inner join (all inputs must have matching timestamps)
- Incremental scheduling: only process new data since last run (watermark tracking)
- Batch recompute: clear output, reset transform state, replay full input history
- Topological ordering: transforms scheduled in dependency order, cycle detection on registration

### 4.8 Retention

- Configurable retention window (nanoseconds)
- Old chunks evicted when their maximum timestamp falls outside the retention window
- Eviction is per-topic, triggered externally (not automatic)

## 5. Non-Functional Requirements

- Pure C++20 with fmt + tsl::robin_map (no Qt dependency)
- Clean under AddressSanitizer (ASAN) in debug builds
- Deterministic chunk ordering: no internal threading, synchronous commit model
- Zero-copy string reads where possible (string_view into dictionary-encoded column memory)
- Builds with -Wall -Wextra -Werror

## 6. Plugin Contract

- Plugins interact through typed host views (SourceWriteHostView, ParserWriteHostView, ToolboxHostView)
- Plugins never instantiate or reference engine classes directly
- Error model: Expected for fallible operations, Status for success/failure — no exceptions cross the plugin ABI boundary
- Host manages flush and commit lifecycle — plugins don't call flush or commit directly

## 7. Deferred / Out of Scope

- RLE (run-length encoding) for repetitive numeric data
- Asynchronous plugin staging queue with background commit thread
- Schema version history tracking
- Persistence (save/load to disk)
- Advanced time-domain alignment (cross-source interpolation)
