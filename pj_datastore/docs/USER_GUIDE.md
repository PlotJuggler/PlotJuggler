# pj_datastore User Guide

How to use pj_datastore to read and write time-series data. This guide is for plugin developers (DataSource, MessageParser, Toolbox) and AI agents implementing plugins.

Plugins interact with the datastore through host-provided views — never through engine classes directly. The three views are:

- **SourceWriteHostView** — for DataSource plugins (file importers, streamers)
- **ParserWriteHostView** — for MessageParser plugins (decoders)
- **ToolboxHostView** — for Toolbox plugins (read + write + catalog)

All are defined in `pj_base/sdk/plugin_data_api.hpp` (the `pj_base` headers come from the `plotjuggler_sdk` Conan package).

---

## 1. Data Model

- **Dataset**: One data source (one file, one network connection). Created automatically by the host.
- **Topic**: A named data stream within a dataset. Has typed columns sharing one timestamp column. Example: a ROS topic `/imu/data` with columns `angular_velocity.x`, `angular_velocity.y`, etc.
- **Field**: A typed column within a topic. Types: `float32`, `float64`, `int32`, `int64`, `uint64`, `bool`, `string`.
- **Timestamp**: `int64_t` nanoseconds since Unix epoch. Always absolute — never subtract a base time during ingestion.

### ValueRef — Preserve Native Types

```cpp
using ValueRef = std::variant<NullValue, TypedNull,
    float, double,
    int8_t, int16_t, int32_t, int64_t,
    uint8_t, uint16_t, uint32_t, uint64_t,
    bool, std::string_view>;
// TypedNull is a null carrying an explicit column type — see §6.
```

**Never cast int64 or uint64 to double.** Values larger than 2^53 lose precision. Push native types directly:

```cpp
// WRONG — loses precision for large integers
fields.push_back({"counter", static_cast<double>(value)});

// CORRECT — preserves full precision
fields.push_back({"counter", value});  // value is int64_t
```

---

## 2. Writing Data

DataSource and Toolbox write hosts name the target topic on each write. A
MessageParser write host is already bound to one topic by the host, so parser
calls omit the topic argument and write into that bound topic.

### Step 1: Get a topic handle

DataSource and Toolbox plugins create or resolve topics explicitly:

```cpp
auto topic = writeHost().ensureTopic("sensor/imu");
if (!topic) { /* handle error */ }
```

MessageParser plugins do not call `ensureTopic()` on the parser write host.
They only create fields inside the already-bound topic:

```cpp
auto field = writeHost().ensureField("temperature", PJ::PrimitiveType::kFloat64);
if (!field) { /* handle error */ }
```

### Step 2 (optional): Pre-register fields for the bound-write fast path

For DataSource and Toolbox writers:

```cpp
writeHost().ensureField(*topic, "accel.x", PJ::PrimitiveType::kFloat64);
writeHost().ensureField(*topic, "accel.y", PJ::PrimitiveType::kFloat64);
writeHost().ensureField(*topic, "accel.z", PJ::PrimitiveType::kFloat64);
writeHost().ensureField(*topic, "label",   PJ::PrimitiveType::kString);
```

Pre-registration is **optional**. If you skip it, fields are auto-created on
first non-null write via `appendRecord()`. Pre-registering is recommended when
the schema is known upfront because it enables the faster `appendBoundRecord()`
path and avoids mid-stream chunk sealing.

### Step 3: Append records

**By name** (flexible, resolves names each call). DataSource and Toolbox
writers pass the topic:

```cpp
std::vector<PJ::sdk::NamedFieldValue> fields = {
    {"accel.x", 9.81},
    {"accel.y", 0.0},
    {"accel.z", -0.05},
    {"label", std::string_view("moving")},
};
auto status = writeHost().appendRecord(*topic, timestamp_ns, PJ::Span(fields));
```

MessageParser writers omit the topic:

```cpp
std::vector<PJ::sdk::NamedFieldValue> fields = {
    {"temperature", 23.5},
    {"humidity", 61.0},
};
auto status = writeHost().appendRecord(timestamp_ns, PJ::Span(fields));
```

**By handle** (pre-resolved, faster for high-rate data). DataSource and
Toolbox writers pass the topic:

```cpp
auto fx = writeHost().ensureField(*topic, "accel.x", PJ::PrimitiveType::kFloat64);
auto fy = writeHost().ensureField(*topic, "accel.y", PJ::PrimitiveType::kFloat64);
// ... resolve all fields once ...

std::vector<PJ::sdk::BoundFieldValue> bound = {
    {*fx, 9.81},
    {*fy, 0.0},
};
writeHost().appendBoundRecord(*topic, timestamp_ns, PJ::Span(bound));
```

Parser writers use the field handles from their bound topic:

```cpp
auto temp = writeHost().ensureField("temperature", PJ::PrimitiveType::kFloat64);
std::vector<PJ::sdk::BoundFieldValue> bound = {{*temp, 23.5}};
writeHost().appendBoundRecord(timestamp_ns, PJ::Span(bound));
```

### Sparse Records

Not every field needs data on every row. Fields omitted from `appendRecord()` are automatically null-filled. This is the correct way to handle sparse data:

```cpp
// Row 1: only accel.x has data
fields = {{"accel.x", 1.0}};
writeHost().appendRecord(*topic, t1, PJ::Span(fields));
// accel.y, accel.z, label are null for this row

// Row 2: all fields have data
fields = {{"accel.x", 2.0}, {"accel.y", 3.0}, {"accel.z", 4.0}};
writeHost().appendRecord(*topic, t2, PJ::Span(fields));
```

### NamedFieldValue.name is std::string

The `name` field in `NamedFieldValue` is `std::string` (not `string_view`). You can safely use temporary string expressions:

```cpp
fields.push_back({prefix + "/" + key, value});  // safe — name is owned
```

### Bulk Arrow Import

For high-throughput imports or parser-shaped payloads that already have Arrow
data, use the Arrow C Data Interface (`ArrowArrayStream`). The byte-based
`appendArrowIpc` slot was removed in ABI v4.

DataSource and Toolbox writers pass the destination topic:

```cpp
PJ::sdk::ArrowStreamHolder stream(buildMyStream());
auto status = writeHost().appendArrowStream(*topic, std::move(stream), "timestamp");
```

Parser writers omit the topic because the parser host is bound to one topic:

```cpp
PJ::sdk::ArrowStreamHolder stream(buildMyPayloadStream());
auto status = writeHost().appendArrowStream(std::move(stream), "timestamp");
```

`ArrowStreamHolder` is an RAII wrapper that auto-releases the stream; the
`std::move` overload disarms it on success. See `pj_base/sdk/arrow.hpp` for
the holder + stream-builder helpers.

---

## 3. Timestamps

- Type: `int64_t` (nanoseconds since Unix epoch)
- May arrive **out of order** within a topic (multi-publisher recordings): every row is retained, rows are sorted per chunk at seal, and row queries return timestamp order
- Convert from seconds: `auto ts = static_cast<int64_t>(epoch_seconds * 1e9);`
- **Never subtract a base time** — display-time subtraction belongs in the UI layer

---

## 4. Delegated Ingest (Streaming Sources)

Streaming sources that receive pre-encoded messages (e.g., ROS CDR, Protobuf) use delegated ingest. The host routes raw bytes to the appropriate MessageParser plugin.

```cpp
// In onStart(): bind a parser for each topic/encoding
auto binding = runtimeHost().ensureParserBinding({
    .topic_name = "/camera/image",
    .parser_encoding = "cdr",
    .type_name = "sensor_msgs/msg/Image",
    .schema = PJ::Span<const uint8_t>(schema_data, schema_size),
    .parser_config_json = config_json,
});

// In onPoll(): push incoming messages (the host invokes the fetcher per policy)
runtimeHost().pushMessage(
    *binding, timestamp_ns, [bytes = payload]() -> std::vector<uint8_t> { return bytes; });
```

- `parser_encoding` is the wire format (e.g., `"cdr"`, `"json"`, `"protobuf"`), not the schema format
- Cache binding handles — don't re-resolve on every message
- `onPoll()` must not block — drain buffered data and return immediately

---

## 5. Reading Data

### Series Reads

Use `SeriesReader` for application-level access to one numeric/bool field as a
time series. It is created once from a topic and column index, then behaves like
a virtual vector of `(timestamp, value)` samples. Null physical rows are skipped:
if the field has no value at a row timestamp, that row is not a series sample.

```cpp
auto series_or = reader.series(topic_id, col_index);
if (!series_or) { /* handle invalid topic/column/type */ }

const PJ::SeriesReader series = *series_or;
auto bounds = series.bounds();
auto latest = series.sampleAtOrBeforeTime(now_ns);

series.samples(PJ::Range<PJ::Timestamp>{.min = t0, .max = t1})
    .forEach([](const PJ::SeriesSample& sample) {
      double value = sample.value;
      PJ::Timestamp ts = sample.timestamp;
    });
```

Series APIs are the default for plotting and other field-level consumers.
Row-level APIs below intentionally expose physical rows and nulls.

### Range Query

```cpp
auto reader = engine.createReader();
auto cursor = reader.rangeQuery({.topic_id = topic_id, .t_min = 0, .t_max = INT64_MAX});
if (!cursor) { /* handle error */ }

cursor->forEach([](const PJ::SampleRow& row) {
    double x = row.chunk->readNumericAsDouble(0, row.row_index);
    int64_t ts = row.chunk->readTimestamp(row.row_index);
});
```

### Latest-At Query

```cpp
auto sample = reader.latestAt({.topic_id = topic_id, .t = now_ns});
if (sample && *sample) {
    double val = (*sample)->chunk->readNumericAsDouble(0, (*sample)->row_index);
}
```

### Read Methods

| Method | Returns | Null behavior |
|--------|---------|---------------|
| `readNumericAsDouble(col, row)` | `double` | Returns 0.0 for nulls — **caller must check isNull()** |
| `readColumnAsDoubles(col, span, row_start)` | batch into span | **Returns NaN for null positions** (safe) |
| `readNumericAsInt64(col, row)` | `int64_t` | Returns 0 for nulls |
| `readNumericAsUint64(col, row)` | `uint64_t` | Returns 0 for nulls |
| `readString(col, row)` | `string_view` | Points into chunk memory — don't outlive the chunk |
| `readBool(col, row)` | `bool` | Returns false for nulls |
| `isNull(col, row)` | `bool` | Explicit null check |

### Schema Evolution and Column Bounds

Early chunks may have fewer columns than later ones (if columns were added via array expansion between chunks). Always check bounds:

```cpp
if (col_index < row.chunk->columns.size()) {
    double val = row.chunk->readNumericAsDouble(col_index, row.row_index);
}
```

---

## 6. Common Pitfalls

### Column addition auto-seals the current chunk

When `appendRecord()` encounters a new field after rows have been written,
the datastore seals the current chunk and adds the column to a fresh one.
Earlier rows (in sealed chunks) have no value for the new column — readers
treat absent columns as null.

You do NOT need to pre-register all fields before writing. Fields may appear
at any time. Pre-registration with `ensureField()` is still recommended when
the schema is known upfront, as it avoids mid-stream chunk sealing.

For schema-based parsers (ROS, Protobuf) where a field's type is known but
the value is null, use `TypedNull{type}` instead of `kNull` to create the
column immediately.

### Don't cast int64/uint64 to double

Values above 2^53 lose precision. Push the native type via ValueRef.

### Timestamps are nanoseconds, not seconds

`auto ts = static_cast<int64_t>(epoch_seconds * 1e9);` — don't forget the `* 1e9`.

### readNumericAsDouble doesn't check nulls

For single-value reads, check `isNull(col, row)` first. For batch reads, use `readColumnAsDoubles()` which returns NaN for nulls.

### string_view lifetime

`readString()` returns a `string_view` pointing into the chunk's dictionary-encoded memory. Don't store it beyond the chunk's lifetime.

### Sparse data: pre-register, let null-fill work

Don't skip rows for topics that have no data at a given timestamp. Instead, write a record with only the fields that have data. The engine null-fills the rest.

---

## 7. Minimal Examples

### File Importer (CSV pattern)

```cpp
class MyImporter : public PJ::FileSourceBase {
  uint64_t extraCapabilities() const override { return PJ::kCapabilityDirectIngest; }

  PJ::Status importData() override {
    auto topic = writeHost().ensureTopic("my_data");
    if (!topic) return PJ::unexpected(topic.error());

    // Pre-register ALL fields
    for (const auto& col_name : column_names) {
      writeHost().ensureField(*topic, col_name, PJ::PrimitiveType::kFloat64);
    }

    // Write rows
    for (const auto& row : parsed_rows) {
      std::vector<PJ::sdk::NamedFieldValue> fields;
      for (size_t i = 0; i < row.values.size(); i++) {
        fields.push_back({column_names[i], row.values[i]});
      }
      writeHost().appendRecord(*topic, row.timestamp_ns, PJ::Span(fields));
    }
    return PJ::okStatus();
  }
};
```

### Streaming Source (Delegated Ingest)

```cpp
class MyStreamer : public PJ::StreamSourceBase {
  uint64_t extraCapabilities() const override { return PJ::kCapabilityDelegatedIngest; }

  PJ::Status onStart() override {
    // Connect, discover topics, create parser bindings
    binding_ = *runtimeHost().ensureParserBinding({
        .topic_name = "/data",
        .parser_encoding = "json",
        .type_name = "MyMessage",
        .schema = {},
    });
    return PJ::okStatus();
  }

  PJ::Status onPoll() override {
    // Drain buffered messages (must not block!)
    while (auto msg = dequeue()) {
      runtimeHost().pushMessage(
          binding_, msg->timestamp_ns, [bytes = msg->payload]() -> std::vector<uint8_t> { return bytes; });
    }
    return PJ::okStatus();
  }

  void onStop() override { /* close connections */ }
};
```
