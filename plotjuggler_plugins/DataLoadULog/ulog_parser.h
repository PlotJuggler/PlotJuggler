#pragma once

#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <string.h>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>

#include <string_view>
#include <functional>

#include "PlotJuggler/plotdata.h"

typedef std::string_view StringView;

// Helper functions for std::string_view compatibility (C++17)
inline bool startsWith(std::string_view sv, std::string_view prefix)
{
  return sv.size() >= prefix.size() && sv.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(std::string_view sv, std::string_view suffix)
{
  return sv.size() >= suffix.size() &&
         sv.compare(sv.size() - suffix.size(), suffix.size(), suffix) == 0;
}

class ULogParser
{
public:
  struct DataStream
  {
    const char* _data;
    const size_t _length;
    size_t offset;

    DataStream(char* data, size_t len) : _data(data), _length(len), offset(0)
    {
    }

    void read(char* dst, size_t len)
    {
      // Truncated logs are common (recording interrupted): never read past the
      // end of the buffer. Callers must check operator bool() after reading.
      const size_t available = (offset < _length) ? (_length - offset) : 0;
      const size_t to_read = (len < available) ? len : available;
      memcpy(dst, &_data[offset], to_read);
      offset += len;
    }

    operator bool()
    {
      return offset < _length;
    }
  };

  enum FormatType
  {
    UINT8,
    UINT16,
    UINT32,
    UINT64,
    INT8,
    INT16,
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    BOOL,
    CHAR,
    OTHER
  };

  struct Field
  {
    Field() : array_size(1)
    {
    }
    FormatType type;
    std::string field_name;
    std::string other_type_ID;
    int array_size;
  };

  struct Parameter
  {
    std::string name;
    union
    {
      int32_t val_int;
      float val_real;
    } value;
    FormatType val_type;

    bool readFromBuffer(const char* message);
  };

  struct Format
  {
    Format() : padding(0), timestamp_idx(-1)
    {
    }
    std::string name;
    std::vector<Field> fields;
    int padding;
    int timestamp_idx;
  };

  struct MessageLog
  {
    char level;
    uint64_t timestamp;
    std::string msg;
  };

  // Flattened "decode program" for a message layout, built once per Format.
  // The walk order mirrors parseSimpleDataMessage exactly (field order, array
  // expansion, nested formats, padding), so the n-th OP_VAL corresponds to the
  // n-th flattened plot series of a subscription.
  enum OpKind : uint8_t
  {
    OP_TS = 0,   // top-level uint64 timestamp field (convert to seconds)
    OP_TS_SKIP,  // timestamp of a nested format: advance the pointer only
    OP_VAL,      // scalar value, appended to the value buffer
    OP_PAD       // padding bytes: advance the pointer only
  };

  struct DecodeOp
  {
    uint8_t kind;        // OpKind
    uint8_t value_type;  // FormatType, valid when kind == OP_VAL
    int32_t pad_bytes;   // valid when kind == OP_PAD
  };

  struct DecodePlan
  {
    std::vector<DecodeOp> ops;
    size_t value_count = 0;  // number of OP_VAL ops == flattened series count
    bool has_timestamp = false;
  };

  struct Subscription
  {
    Subscription() : msg_id(0), multi_id(0), format(nullptr)
    {
    }

    uint16_t msg_id;
    uint8_t multi_id;
    std::string message_name;
    const Format* format;

    // Direct-output mode: one PlotData pointer per flattened (non padding) field.
    // Created lazily on the first DATA message of this subscription. Pointers are
    // used instead of iterators because unordered_map rehashing invalidates
    // iterators but never references/pointers to elements.
    std::vector<PJ::PlotData*> plot_series;
    uint64_t sample_count = 0;
  };

  struct Timeseries
  {
    std::vector<std::optional<uint64_t>> timestamps;
    std::vector<std::pair<std::string, std::vector<double>>> data;
  };

public:
  // Optional progress callback, invoked with (offset, total) as the file is parsed.
  // Returning false aborts parsing with a std::runtime_error (user cancellation).
  using ProgressCallback = std::function<bool(size_t offset, size_t total)>;

  ULogParser(DataStream& datastream, ProgressCallback progress_cb = nullptr);

  /// Fast path: parsed points are pushed directly into plot_data, skipping the
  /// intermediate Timeseries storage and the second conversion pass.
  ULogParser(DataStream& datastream, PJ::PlotDataMapRef& plot_data,
             ProgressCallback progress_cb = nullptr);

  /// Direct mode switches to the multi-threaded pipeline for data sections
  /// larger than this threshold (default 8 MB). Setting it to SIZE_MAX forces
  /// the serial path (used by tests for comparison).
  static void setParallelThreshold(size_t bytes);

  const std::map<std::string, Timeseries>& getTimeseriesMap() const;

  /// Smallest message timestamp (in seconds) seen while parsing; used as the x
  /// coordinate for the single-point "_parameters" series.
  double getMinMessageTime() const
  {
    return _min_msg_time;
  }

  const std::vector<Parameter>& getParameters() const;

  const std::map<std::string, std::string>& getInfo() const;

  const std::vector<MessageLog>& getLogs() const;

private:
  void parse(DataStream& datastream, ProgressCallback progress_cb);

  /// Message loop over the data section (serial; used by both modes).
  void parseDataSectionSerial(DataStream& datastream, ProgressCallback progress_cb);

  bool readFileHeader(DataStream& datastream);

  bool readFileDefinitions(DataStream& datastream);

  bool readFormat(DataStream& datastream, uint16_t msg_size);

  bool readFlagBits(DataStream& datastream, uint16_t msg_size);

  bool readInfo(DataStream& datastream, uint16_t msg_size);

  bool readParameter(DataStream& datastream, uint16_t msg_size);

  bool readSubscription(DataStream& datastream, uint16_t msg_size);

  size_t fieldsCount(const Format& format) const;

  Timeseries createTimeseries(const Format* format);

  uint64_t _file_start_time;

  std::vector<Parameter> _parameters;

  std::vector<uint8_t> _read_buffer;

  size_t _data_section_start = 0;  ///< first ADD_LOGGED_MSG message

  int64_t _read_until_file_position = 1ULL << 60;  ///< read limit if log contains appended data

  std::set<std::string> _overridden_params;

  std::map<std::string, Format> _formats;

  std::map<std::string, std::string> _info;

  std::map<uint16_t, Subscription> _subscriptions;

  std::map<std::string, Timeseries> _timeseries;

  std::vector<StringView> splitString(const StringView& strToSplit, char delimeter);

  std::set<std::string> _message_name_with_multi_id;

  std::vector<MessageLog> _message_logs;

  void parseDataMessage(const Subscription& sub, const char* message);

  const char* parseSimpleDataMessage(Timeseries& timeseries, const Format* format,
                                     const char* message, size_t* index,
                                     bool read_timestamp = true);

  // ---- direct-output mode helpers ----
  void parseDataMessageDirect(Subscription& sub, const char* message);

  void createPlotSeries(const Format* format, const std::string& prefix,
                        PJ::PlotGroup::Ptr group, std::vector<PJ::PlotData*>& out);

  // ---- decode plans (shared by serial and parallel direct paths) ----
  const DecodePlan& getDecodePlan(const Format& format);

  void buildDecodePlanRec(const Format& format, bool nested, DecodePlan& plan);

  static void decodePlan(const DecodePlan& plan, const char* message,
                         std::vector<double>& values, double* msg_time);

  // Multi-threaded direct-mode pipeline: single-threaded scan that cuts the
  // data section into chunks (collecting subscription snapshots), parallel
  // chunk decoding into flat buffers, then parallel per-series deque fill.
  // Throws std::runtime_error on user cancellation, like the serial path.
  void parseParallel(const char* data, size_t length, size_t data_start,
                     const ProgressCallback& progress_cb);

  static size_t _parallel_threshold;  ///< direct mode uses the serial path below this size

  std::unordered_map<const Format*, DecodePlan> _plans;

  PJ::PlotDataMapRef* _plot_data = nullptr;

  double _min_msg_time = std::numeric_limits<double>::max();

  // Reused per DATA message in direct mode: flattened values in decode order
  // (n-th entry belongs to the n-th plot series); x is applied once the
  // message timestamp is known (it may appear anywhere in the layout).
  std::vector<double> _msg_values;
};
