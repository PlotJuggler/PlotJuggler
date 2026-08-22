#include "ulog_parser.h"
#include "ulog_messages.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iosfwd>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <thread>
#include <QDebug>

using ios = std::ios;

size_t ULogParser::_parallel_threshold = 8 * 1024 * 1024;

void ULogParser::setParallelThreshold(size_t bytes)
{
  _parallel_threshold = bytes;
}

ULogParser::ULogParser(DataStream& datastream, ProgressCallback progress_cb)
  : _file_start_time(0)
{
  parse(datastream, progress_cb);
}

ULogParser::ULogParser(DataStream& datastream, PJ::PlotDataMapRef& plot_data,
                       ProgressCallback progress_cb)
  : _file_start_time(0), _plot_data(&plot_data)
{
  if (!readFileHeader(datastream))
  {
    throw std::runtime_error("ULog: wrong header");
  }
  if (!readFileDefinitions(datastream))
  {
    throw std::runtime_error("ULog: error loading definitions");
  }

  const size_t data_bytes =
      (datastream._length > _data_section_start) ? (datastream._length - _data_section_start) : 0;
  const unsigned hw = std::thread::hardware_concurrency();
  if (data_bytes >= _parallel_threshold && hw >= 2)
  {
    parseParallel(datastream._data, datastream._length, _data_section_start, progress_cb);
  }
  else
  {
    datastream.offset = _data_section_start;
    parseDataSectionSerial(datastream, progress_cb);
  }
}

void ULogParser::parse(DataStream& datastream, ProgressCallback progress_cb)
{
#ifdef ULOG_BENCH_TRACE
  fprintf(stderr, "[trace] parse start\n");
#endif
  bool ret = readFileHeader(datastream);

  if (!ret)
  {
    throw std::runtime_error("ULog: wrong header");
  }
#ifdef ULOG_BENCH_TRACE
  fprintf(stderr, "[trace] header ok\n");
#endif

  if (!readFileDefinitions(datastream))
  {
    throw std::runtime_error("ULog: error loading definitions");
  }
#ifdef ULOG_BENCH_TRACE
  fprintf(stderr, "[trace] definitions ok, data section at %lld\n",
          (long long)_data_section_start);
#endif

  datastream.offset = _data_section_start;
  parseDataSectionSerial(datastream, progress_cb);
}

void ULogParser::parseDataSectionSerial(DataStream& datastream, ProgressCallback progress_cb)
{
  int last_reported_pct = -1;
#ifdef ULOG_BENCH_TRACE
  long long traced_msgs = 0;
#endif
  while (datastream)
  {
#ifdef ULOG_BENCH_TRACE
    if ((++traced_msgs % 100000) == 0)
    {
      fprintf(stderr, "[trace] %lld msgs, offset=%zu\n", traced_msgs, datastream.offset);
    }
#endif
    // Report parsing progress (and honor cancellation) once per percentage point.
    if (progress_cb)
    {
      const int pct = static_cast<int>(datastream.offset * 100 / datastream._length);
      if (pct != last_reported_pct)
      {
        last_reported_pct = pct;
        if (!progress_cb(datastream.offset, datastream._length))
        {
          throw std::runtime_error("ULog: import cancelled by user");
        }
      }
    }

    ulog_message_header_s message_header;
    datastream.read((char*)&message_header, ULOG_MSG_HEADER_LEN);
    if (!datastream)
    {
      break;  // truncated header at the end of the file: keep the data parsed so far
    }

    if (datastream.offset + message_header.msg_size > datastream._length)
    {
      break;  // truncated message payload: keep the data parsed so far
    }
#ifdef ULOG_BENCH_TRACE
    if (traced_msgs <= 32)
    {
      fprintf(stderr, "[trace] msg #%lld type=%c size=%u offset=%zu\n", traced_msgs,
              (char)message_header.msg_type, (unsigned)message_header.msg_size,
              datastream.offset);
    }
#endif

    // Zero-copy: every handler below is length-based, so the payload can be
    // parsed directly inside the (bounds-checked) file buffer.
    const char* message = datastream._data + datastream.offset;
    datastream.offset += message_header.msg_size;

    switch (message_header.msg_type)
    {
      case (int)ULogMessageType::ADD_LOGGED_MSG: {
        Subscription sub;

        sub.multi_id = *reinterpret_cast<const uint8_t*>(message);
        sub.msg_id = *reinterpret_cast<const uint16_t*>(message + 1);
        message += 3;
        sub.message_name.assign(message, message_header.msg_size - 3);

        const auto it = _formats.find(sub.message_name);
        if (it != _formats.end())
        {
          sub.format = &it->second;
        }
        _subscriptions.insert({ sub.msg_id, sub });

        if (sub.multi_id > 0)
        {
          _message_name_with_multi_id.insert(sub.message_name);
        }

        //            printf("ADD_LOGGED_MSG: %d %d %s\n", sub.msg_id, sub.multi_id,
        //            sub.message_name.c_str() ); std::cout << std::endl;
      }
      break;
      case (int)ULogMessageType::REMOVE_LOGGED_MSG:
        printf("REMOVE_LOGGED_MSG\n");
        {
          uint16_t msg_id = *reinterpret_cast<const uint16_t*>(message);
          _subscriptions.erase(msg_id);
        }
        break;
      case (int)ULogMessageType::DATA: {
        uint16_t msg_id = *reinterpret_cast<const uint16_t*>(message);
        message += 2;
        auto sub_it = _subscriptions.find(msg_id);
        if (sub_it == _subscriptions.end())
        {
          continue;
        }
        Subscription& sub = sub_it->second;

        if (_plot_data)
        {
          parseDataMessageDirect(sub, message);
        }
        else
        {
          parseDataMessage(sub, message);
        }
      }
      break;

      case (int)ULogMessageType::LOGGING: {
        MessageLog msg;
        msg.level = static_cast<char>(message[0]);
        message += sizeof(char);
        msg.timestamp = *reinterpret_cast<const uint64_t*>(message);
        message += sizeof(uint64_t);
        msg.msg.assign(message, message_header.msg_size - 9);
        // printf("LOG %c (%ld): %s\n", msg.level, msg.timestamp, msg.msg.c_str() );
        _message_logs.push_back(std::move(msg));
      }
      break;
      case (int)ULogMessageType::SYNC:  // printf("SYNC\n" );
        break;
      case (int)ULogMessageType::DROPOUT:  // printf("DROPOUT\n" );
        break;
      case (int)ULogMessageType::INFO:  // printf("INFO\n" );
        break;
      case (int)ULogMessageType::INFO_MULTIPLE:  // printf("INFO_MULTIPLE\n" );
        break;
      case (int)ULogMessageType::PARAMETER_DEFAULT:  // printf("PARAMETER_DEFAULT\n" );
        break;
      case (int)ULogMessageType::PARAMETER:
        Parameter new_param;
        new_param.readFromBuffer(message);
        bool found = false;
        for (auto& prev_param : _parameters)
        {
          if (prev_param.name == new_param.name)
          {
            prev_param = std::move(new_param);
            found = true;
            break;
          }
        }
        if (!found)
        {
          _parameters.push_back(new_param);
        }
        break;
    }
  }
}

void ULogParser::parseDataMessage(const ULogParser::Subscription& sub, const char* message)
{
  size_t other_fields_count = 0;
  std::string ts_name = sub.message_name;

  for (const auto& field : sub.format->fields)
  {
    if (field.type == OTHER)
    {
      other_fields_count++;
    }
  }

  if (_message_name_with_multi_id.count(ts_name) > 0)
  {
    char buff[16];
    sprintf(buff, ".%02d", sub.multi_id);
    ts_name += std::string(buff);
  }

  // get the timeseries or create if if it doesn't exist
  auto ts_it = _timeseries.find(ts_name);
  if (ts_it == _timeseries.end())
  {
    ts_it = _timeseries.insert({ ts_name, createTimeseries(sub.format) }).first;
  }
  Timeseries& timeseries = ts_it->second;

  size_t index = 0;
  parseSimpleDataMessage(timeseries, sub.format, message, &index);
}

const char* ULogParser::parseSimpleDataMessage(Timeseries& timeseries, const Format* format,
                                               const char* message, size_t* index,
                                               bool read_timestamp)
{
  for (size_t i = 0; i <= format->fields.size(); i++)
  {
    if (format->timestamp_idx == static_cast<int>(i))
    {
      uint64_t time_val = *reinterpret_cast<const uint64_t*>(message);
      message += sizeof(uint64_t);
      if (read_timestamp)
      {
        timeseries.timestamps.push_back(time_val);
      }
    }

    if (i == format->fields.size())
    {
      break;
    }

    const auto& field = format->fields[i];

    // skip _padding messages which are one byte in size
    if (startsWith(StringView(field.field_name), "_padding"))
    {
      message += field.array_size;
      continue;
    }

    for (int array_pos = 0; array_pos < field.array_size; array_pos++)
    {
      double value = 0;
      switch (field.type)
      {
        case UINT8: {
          value = static_cast<double>(*reinterpret_cast<const uint8_t*>(message));
          message += 1;
        }
        break;
        case INT8: {
          value = static_cast<double>(*reinterpret_cast<const int8_t*>(message));
          message += 1;
        }
        break;
        case UINT16: {
          value = static_cast<double>(*reinterpret_cast<const uint16_t*>(message));
          message += 2;
        }
        break;
        case INT16: {
          value = static_cast<double>(*reinterpret_cast<const int16_t*>(message));
          message += 2;
        }
        break;
        case UINT32: {
          value = static_cast<double>(*reinterpret_cast<const uint32_t*>(message));
          message += 4;
        }
        break;
        case INT32: {
          value = static_cast<double>(*reinterpret_cast<const int32_t*>(message));
          message += 4;
        }
        break;
        case UINT64: {
          value = static_cast<double>(*reinterpret_cast<const uint64_t*>(message));
          message += 8;
        }
        break;
        case INT64: {
          value = static_cast<double>(*reinterpret_cast<const int64_t*>(message));
          message += 8;
        }
        break;
        case FLOAT: {
          value = static_cast<double>(*reinterpret_cast<const float*>(message));
          message += 4;
        }
        break;
        case DOUBLE: {
          value = (*reinterpret_cast<const double*>(message));
          message += 8;
        }
        break;
        case CHAR: {
          value = static_cast<double>(*reinterpret_cast<const char*>(message));
          message += 1;
        }
        break;
        case BOOL: {
          value = static_cast<double>(*reinterpret_cast<const bool*>(message));
          message += 1;
        }
        break;
        case OTHER: {
          // recursion!!!
          auto child_format = _formats.at(field.other_type_ID);
          message = parseSimpleDataMessage(timeseries, &child_format, message, index, false);
        }
        break;

      }  // end switch

      if (field.type != OTHER)
      {
        timeseries.data[(*index)++].second.push_back(value);
      }
    }  // end for
  }

  if (read_timestamp && format->timestamp_idx < 0)
  {
    timeseries.timestamps.push_back(std::nullopt);
  }

  return message;
}

// ---------------------------------------------------------------------------
// Direct-output mode: points are pushed straight into the PlotDataMapRef,
// avoiding the intermediate Timeseries storage and a second conversion pass.
// ---------------------------------------------------------------------------

void ULogParser::createPlotSeries(const Format* format, const std::string& prefix,
                                  PJ::PlotGroup::Ptr group, std::vector<PJ::PlotData*>& out)
{
  for (const auto& field : format->fields)
  {
    // skip padding messages
    if (startsWith(StringView(field.field_name), "_padding"))
    {
      continue;
    }

    std::string new_prefix = prefix + "/" + field.field_name;
    for (int i = 0; i < field.array_size; i++)
    {
      std::string array_suffix = "";
      if (field.array_size > 1)
      {
        char buff[16];
        sprintf(buff, ".%02d", i);
        array_suffix = buff;
      }
      if (field.type != OTHER)
      {
        auto it = _plot_data->addNumeric(new_prefix + array_suffix, group);
        out.push_back(&it->second);
      }
      else
      {
        createPlotSeries(&this->_formats.at(field.other_type_ID), new_prefix + array_suffix,
                         group, out);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Decode plans: a Format is walked once into a flat op list; every DATA
// message of that format is then decoded by a tight loop with no field-name
// comparisons, no recursion and no re-walking of the layout.
// ---------------------------------------------------------------------------

void ULogParser::buildDecodePlanRec(const Format& format, bool nested, DecodePlan& plan)
{
  for (size_t i = 0; i <= format.fields.size(); i++)
  {
    if (format.timestamp_idx == static_cast<int>(i))
    {
      DecodeOp op{};
      op.kind = nested ? OP_TS_SKIP : OP_TS;
      if (!nested)
      {
        plan.has_timestamp = true;
      }
      plan.ops.push_back(op);
    }

    if (i == format.fields.size())
    {
      break;
    }

    const auto& field = format.fields[i];

    // skip _padding messages which are one byte in size
    if (startsWith(StringView(field.field_name), "_padding"))
    {
      DecodeOp op{};
      op.kind = OP_PAD;
      op.pad_bytes = field.array_size;
      plan.ops.push_back(op);
      continue;
    }

    for (int array_pos = 0; array_pos < field.array_size; array_pos++)
    {
      if (field.type != OTHER)
      {
        DecodeOp op{};
        op.kind = OP_VAL;
        op.value_type = static_cast<uint8_t>(field.type);
        plan.ops.push_back(op);
        plan.value_count++;
      }
      else
      {
        // recursion!!! (nested formats never store their own timestamp)
        buildDecodePlanRec(_formats.at(field.other_type_ID), true, plan);
      }
    }
  }
}

const ULogParser::DecodePlan& ULogParser::getDecodePlan(const Format& format)
{
  auto it = _plans.find(&format);
  if (it == _plans.end())
  {
    DecodePlan plan;
    buildDecodePlanRec(format, false, plan);
    it = _plans.emplace(&format, std::move(plan)).first;
  }
  return it->second;
}

void ULogParser::decodePlan(const DecodePlan& plan, const char* message,
                            std::vector<double>& values, double* msg_time)
{
  values.clear();
  values.reserve(plan.value_count);

  for (const DecodeOp& op : plan.ops)
  {
    switch (op.kind)
    {
      case OP_TS:
        *msg_time = static_cast<double>(*reinterpret_cast<const uint64_t*>(message)) * 0.000001;
        message += 8;
        break;
      case OP_TS_SKIP:
        message += 8;
        break;
      case OP_PAD:
        message += op.pad_bytes;
        break;
      case OP_VAL: {
        double value = 0;
        switch (op.value_type)
        {
          case UINT8:
            value = static_cast<double>(*reinterpret_cast<const uint8_t*>(message));
            message += 1;
            break;
          case INT8:
            value = static_cast<double>(*reinterpret_cast<const int8_t*>(message));
            message += 1;
            break;
          case UINT16:
            value = static_cast<double>(*reinterpret_cast<const uint16_t*>(message));
            message += 2;
            break;
          case INT16:
            value = static_cast<double>(*reinterpret_cast<const int16_t*>(message));
            message += 2;
            break;
          case UINT32:
            value = static_cast<double>(*reinterpret_cast<const uint32_t*>(message));
            message += 4;
            break;
          case INT32:
            value = static_cast<double>(*reinterpret_cast<const int32_t*>(message));
            message += 4;
            break;
          case UINT64:
            value = static_cast<double>(*reinterpret_cast<const uint64_t*>(message));
            message += 8;
            break;
          case INT64:
            value = static_cast<double>(*reinterpret_cast<const int64_t*>(message));
            message += 8;
            break;
          case FLOAT:
            value = static_cast<double>(*reinterpret_cast<const float*>(message));
            message += 4;
            break;
          case DOUBLE:
            value = *reinterpret_cast<const double*>(message);
            message += 8;
            break;
          case CHAR:
            value = static_cast<double>(*reinterpret_cast<const char*>(message));
            message += 1;
            break;
          case BOOL:
            value = static_cast<double>(*reinterpret_cast<const bool*>(message));
            message += 1;
            break;
          default:
            break;  // OTHER never appears as OP_VAL
        }
        values.push_back(value);
      }
      break;
    }
  }
}

void ULogParser::parseDataMessageDirect(Subscription& sub, const char* message)
{
  if (sub.plot_series.empty())
  {
    std::string ts_name = sub.message_name;

    if (_message_name_with_multi_id.count(ts_name) > 0)
    {
      char buff[16];
      sprintf(buff, ".%02d", sub.multi_id);
      ts_name += std::string(buff);
    }

    auto group = _plot_data->getOrCreateGroup(ts_name);
    createPlotSeries(sub.format, ts_name, group, sub.plot_series);
  }

  double msg_time = -1.0;
  decodePlan(getDecodePlan(*sub.format), message, _msg_values, &msg_time);

  if (msg_time < 0.0)
  {
    // message has no timestamp field: fall back to the sample index,
    // mirroring the previous timestamps[i].value_or(i) behavior
    msg_time = static_cast<double>(sub.sample_count) * 0.000001;
  }
  sub.sample_count++;

  _min_msg_time = std::min(_min_msg_time, msg_time);
  if (std::isfinite(msg_time))
  {
    // n-th decoded value belongs to the n-th flattened series
    for (size_t i = 0; i < _msg_values.size(); i++)
    {
      const double value = _msg_values[i];
      // same NaN/Inf filtering as TimeseriesBase::pushUnsorted, but appending
      // directly to the underlying deque: no virtual call per point
      if (std::isfinite(value))
      {
        sub.plot_series[i]->_points.emplace_back(msg_time, value);
      }
    }
  }
}

// ===========================================================================
// Multi-threaded direct-mode pipeline.
//
// ULog DATA messages are independent except for the subscription table
// (ADD/REMOVE_LOGGED_MSG) and the file-order requirement within each series,
// so the data section is processed in three phases:
//   1. scan  (1 thread): walk every message once, cut the section into
//      chunks at message boundaries, snapshot the active subscriptions per
//      chunk and eagerly create the plot series (same naming rules as the
//      serial path, at each subscription's first DATA message).
//   2. parse (N threads): each chunk is decoded into flat per-series point
//      buffers using the precompiled decode plans.
//   3. fill  (N threads): chunks are appended to the PlotData deques in file
//      order; series are independent, so the fill of one chunk is fanned out
//      across all worker threads.
// A pacing limit keeps at most a few parsed-but-unfilled chunks in memory.
// ===========================================================================

namespace
{
struct ParallelSlot
{
  std::string name;
  uint8_t multi_id = 0;
  const ULogParser::Format* format = nullptr;
  const ULogParser::DecodePlan* plan = nullptr;
  std::vector<PJ::PlotData*> plot_series;
  bool series_created = false;
};

struct ParallelChunk
{
  size_t start = 0;  // offset of the first message
  size_t end = 0;    // offset one past the last complete message
  std::vector<std::pair<uint16_t, int32_t>> active_snap;  // msg_id -> slot, at chunk start
  std::vector<uint64_t> count_snap;                       // per-slot samples at chunk start
  std::vector<uint64_t> chunk_counts;                     // per-slot DATA messages in chunk
  // per slot, per series: decoded points (chunk-local, file order)
  std::vector<std::vector<std::vector<PJ::PlotData::Point>>> bufs;
  std::vector<ULogParser::Parameter> params;
  std::vector<ULogParser::MessageLog> logs;
  double min_time = std::numeric_limits<double>::max();
};
}  // namespace

void ULogParser::parseParallel(const char* data, size_t length, size_t data_start,
                               const ProgressCallback& progress_cb)
{
  using Point = PJ::PlotData::Point;
  const bool trace = (getenv("ULOG_PAR_TRACE") != nullptr);
  const auto tr0 = std::chrono::steady_clock::now();
  auto tr_ms = [&]() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr0)
        .count();
  };

  const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
  const unsigned num_threads = std::min(hw, 8u);

  // ------------------------------------------------------------ phase 1: scan
  std::vector<ParallelSlot> pslots;
  std::map<std::pair<uint8_t, std::string>, int32_t> slot_by_key;
  std::unordered_map<uint16_t, int32_t> active;
  std::vector<uint64_t> slot_samples;

  const size_t data_total = length - data_start;
  const size_t target_chunk = std::max<size_t>(4ull << 20, data_total / (2 * num_threads));

  std::deque<ParallelChunk> chunks;  // deque: element addresses must stay stable
  size_t chunk_start = data_start;

  // Snapshot of the subscription state at the start of the current chunk.
  // Workers must begin from the chunk-start state (a subscription that is
  // removed inside the chunk was still active for the chunk's earlier part).
  std::vector<std::pair<uint16_t, int32_t>> start_active;
  std::vector<uint64_t> start_counts;
  std::vector<uint64_t> chunk_counts;  // DATA messages per slot, current chunk

  auto push_chunk = [&](size_t end) {
    ParallelChunk c;
    c.start = chunk_start;
    c.end = end;
    c.active_snap = start_active;
    c.count_snap = start_counts;
    c.chunk_counts = std::move(chunk_counts);
    chunks.push_back(std::move(c));
    chunk_start = end;
    start_active.assign(active.begin(), active.end());
    start_counts = slot_samples;
    chunk_counts.assign(slot_samples.size(), 0);
  };

  size_t pos = data_start;
  while (pos + ULOG_MSG_HEADER_LEN <= length)
  {
    uint16_t msg_size;
    memcpy(&msg_size, data + pos, 2);
    const uint8_t msg_type = static_cast<uint8_t>(data[pos + 2]);
    const char* payload = data + pos + 3;
    if (pos + 3 + msg_size > length)
    {
      break;  // truncated message: stop here, keep everything parsed so far
    }

    switch (msg_type)
    {
      case (int)ULogMessageType::ADD_LOGGED_MSG: {
        const uint8_t multi_id = static_cast<uint8_t>(payload[0]);
        uint16_t msg_id;
        memcpy(&msg_id, payload + 1, 2);
        std::string name(payload + 3, msg_size - 3);

        if (multi_id > 0)
        {
          _message_name_with_multi_id.insert(name);
        }

        auto key = std::make_pair(multi_id, name);
        auto sit = slot_by_key.find(key);
        int32_t slot_id;
        if (sit == slot_by_key.end())
        {
          slot_id = static_cast<int32_t>(pslots.size());
          ParallelSlot s;
          s.name = std::move(name);
          s.multi_id = multi_id;
          const auto fit = _formats.find(s.name);
          s.format = (fit != _formats.end()) ? &fit->second : nullptr;
          pslots.push_back(std::move(s));
          slot_samples.push_back(0);
          chunk_counts.push_back(0);
          slot_by_key.emplace(key, slot_id);
        }
        else
        {
          slot_id = sit->second;
        }
        active.emplace(msg_id, slot_id);  // std::map::insert semantics: first wins
      }
      break;

      case (int)ULogMessageType::REMOVE_LOGGED_MSG: {
        uint16_t msg_id;
        memcpy(&msg_id, payload, 2);
        active.erase(msg_id);
      }
      break;

      case (int)ULogMessageType::DATA: {
        uint16_t msg_id;
        memcpy(&msg_id, payload, 2);
        auto ait = active.find(msg_id);
        if (ait != active.end())
        {
          ParallelSlot& slot = pslots[ait->second];
          if (!slot.series_created && slot.format)
          {
            // Same naming rules as the serial path: the multi_id suffix is
            // decided at the first DATA message of the subscription.
            std::string ts_name = slot.name;
            if (_message_name_with_multi_id.count(ts_name) > 0)
            {
              char buff[16];
              sprintf(buff, ".%02d", slot.multi_id);
              ts_name += std::string(buff);
            }
            auto group = _plot_data->getOrCreateGroup(ts_name);
            createPlotSeries(slot.format, ts_name, group, slot.plot_series);
            slot.plan = &getDecodePlan(*slot.format);
            slot.series_created = true;
          }
          slot_samples[ait->second]++;
          chunk_counts[ait->second]++;
        }
      }
      break;

      default:
        break;
    }

    pos += 3 + msg_size;
    if (pos - chunk_start >= target_chunk && chunks.size() < 64)
    {
      push_chunk(pos);
    }
  }
  push_chunk(pos);  // final chunk (possibly empty)

  const size_t n_chunks = chunks.size();
  const size_t n_slots = pslots.size();
  for (auto& c : chunks)
  {
    c.bufs.resize(n_slots);
    for (size_t s = 0; s < n_slots; s++)
    {
      c.bufs[s].resize(pslots[s].plot_series.size());
    }
  }
  if (trace)
  {
    fprintf(stderr, "[par] scan: %.0f ms, chunks=%zu slots=%zu threads=%u\n", tr_ms(), n_chunks,
            n_slots, num_threads);
  }

  // Per-chunk point buffers are reserved just-in-time inside parse_chunk
  // (scan counted the DATA messages of every slot per chunk), so parsing never
  // triggers a geometric reallocation while keeping only the paced chunks big.

  // ----------------------------------------------------------- phase 2: parse
  std::atomic<bool> stop{ false };
  std::atomic<size_t> parsed_bytes{ 0 };
  std::atomic<size_t> filled_bytes{ 0 };

  std::mutex mtx;
  std::condition_variable cv;
  std::vector<char> parsed_flags(n_chunks, 0);
  size_t filled_idx = 0;  // chunks with index < filled_idx are fully filled

  std::atomic<size_t> next_chunk{ 0 };

  auto parse_chunk = [&](size_t ci) {
    ParallelChunk& c = chunks[ci];
    std::unordered_map<uint16_t, int32_t> local_active(c.active_snap.begin(),
                                                       c.active_snap.end());
    std::vector<uint64_t> local_counts = c.count_snap;
    std::vector<double> values;
    double local_min = std::numeric_limits<double>::max();

    // Exact DATA-message counts per slot are known from the scan phase:
    // reserve once so the decode loop below never reallocates.
    for (size_t s = 0; s < c.bufs.size(); s++)
    {
      const uint64_t msgs = (s < c.chunk_counts.size()) ? c.chunk_counts[s] : 0;
      if (msgs > 0)
      {
        for (auto& series_buf : c.bufs[s])
        {
          series_buf.reserve(static_cast<size_t>(msgs));
        }
      }
    }

    size_t p = c.start;
    size_t last_flush = p;
    size_t msg_counter = 0;

    while (p + ULOG_MSG_HEADER_LEN <= c.end)
    {
      if (((msg_counter++ & 0x3FF) == 0) && stop.load(std::memory_order_relaxed))
      {
        break;
      }

      uint16_t msg_size;
      memcpy(&msg_size, data + p, 2);
      const uint8_t msg_type = static_cast<uint8_t>(data[p + 2]);
      const char* payload = data + p + 3;

      switch (msg_type)
      {
        case (int)ULogMessageType::DATA: {
          uint16_t msg_id;
          memcpy(&msg_id, payload, 2);
          auto ait = local_active.find(msg_id);
          if (ait != local_active.end())
          {
            const int32_t slot_id = ait->second;
            ParallelSlot& slot = pslots[slot_id];
            if (slot.plan)
            {
              double msg_time = -1.0;
              decodePlan(*slot.plan, payload + 2, values, &msg_time);
              if (msg_time < 0.0)
              {
                // no timestamp field: fall back to the sample index
                msg_time = static_cast<double>(local_counts[slot_id]) * 0.000001;
              }
              local_min = std::min(local_min, msg_time);
              if (std::isfinite(msg_time))
              {
                auto& sbufs = c.bufs[slot_id];
                for (size_t i = 0; i < values.size(); i++)
                {
                  const double value = values[i];
                  if (std::isfinite(value))
                  {
                    sbufs[i].emplace_back(msg_time, value);
                  }
                }
              }
            }
            local_counts[slot_id]++;
          }
        }
        break;

        case (int)ULogMessageType::ADD_LOGGED_MSG: {
          const uint8_t multi_id = static_cast<uint8_t>(payload[0]);
          uint16_t msg_id;
          memcpy(&msg_id, payload + 1, 2);
          auto sit = slot_by_key.find(
              { multi_id, std::string(payload + 3, msg_size - 3) });
          if (sit != slot_by_key.end())
          {
            local_active.emplace(msg_id, sit->second);
            // the slot may have been created inside this chunk, after the
            // chunk-start snapshot was taken
            if (static_cast<size_t>(sit->second) >= local_counts.size())
            {
              local_counts.resize(sit->second + 1, 0);
            }
          }
        }
        break;

        case (int)ULogMessageType::REMOVE_LOGGED_MSG: {
          uint16_t msg_id;
          memcpy(&msg_id, payload, 2);
          local_active.erase(msg_id);
        }
        break;

        case (int)ULogMessageType::PARAMETER: {
          Parameter param;
          param.readFromBuffer(payload);
          c.params.push_back(std::move(param));
        }
        break;

        case (int)ULogMessageType::LOGGING: {
          MessageLog log;
          log.level = payload[0];
          memcpy(&log.timestamp, payload + 1, 8);
          log.msg.assign(payload + 9, msg_size - 9);
          c.logs.push_back(std::move(log));
        }
        break;

        default:
          break;
      }

      p += 3 + msg_size;
      if ((msg_counter & 0x3FF) == 0)
      {
        parsed_bytes.fetch_add(p - last_flush, std::memory_order_relaxed);
        last_flush = p;
      }
    }
    parsed_bytes.fetch_add(p - last_flush, std::memory_order_relaxed);
    c.min_time = local_min;

    {
      std::lock_guard<std::mutex> lk(mtx);
      parsed_flags[ci] = 1;
    }
    cv.notify_all();
  };

  std::vector<std::thread> parse_threads;
  parse_threads.reserve(num_threads);
  for (unsigned t = 0; t < num_threads; t++)
  {
    parse_threads.emplace_back([&]() {
      while (true)
      {
        const size_t ci = next_chunk.fetch_add(1);
        if (ci >= n_chunks || stop.load(std::memory_order_relaxed))
        {
          break;
        }
        {
          // pacing: cap the memory of parsed-but-unfilled chunks
          std::unique_lock<std::mutex> lk(mtx);
          cv.wait(lk, [&] { return stop.load() || ci < filled_idx + 3; });
        }
        if (stop.load(std::memory_order_relaxed))
        {
          break;
        }
        parse_chunk(ci);
      }
    });
  }

  // ------------------------------------------------------------- phase 3: fill
  std::atomic<size_t> fill_gen{ 0 };       // chunk in flight = fill_gen - 1
  std::atomic<size_t> fill_next_slot{ 0 };
  std::atomic<size_t> fill_finished{ 0 };  // helpers that completed current gen

  size_t n_helpers = std::max<size_t>(1, num_threads - 1);
  if (const char* fh = getenv("ULOG_PAR_HELPERS"))
  {
    n_helpers = std::max<size_t>(1, static_cast<size_t>(atoi(fh)));
  }
  std::atomic<bool> shutdown{ false };
  std::vector<std::thread> fill_threads;
  fill_threads.reserve(n_helpers);
  for (size_t t = 0; t < n_helpers; t++)
  {
    fill_threads.emplace_back([&]() {
      size_t my_gen = 0;
      while (true)
      {
        {
          std::unique_lock<std::mutex> lk(mtx);
          cv.wait(lk, [&] { return shutdown || fill_gen.load() != my_gen; });
        }
        if (shutdown && fill_gen.load() == my_gen)
        {
          break;
        }
        my_gen = fill_gen.load();
        const size_t ci = my_gen - 1;
        ParallelChunk& c = chunks[ci];

        while (true)
        {
          const size_t s = fill_next_slot.fetch_add(1);
          if (s >= n_slots || stop.load(std::memory_order_relaxed))
          {
            break;
          }
          auto& sbufs = c.bufs[s];
          auto& series = pslots[s].plot_series;
          for (size_t i = 0; i < series.size(); i++)
          {
            auto& dst = series[i]->_points;
            const auto& src = sbufs[i];
            dst.insert(dst.end(), src.begin(), src.end());
          }
        }
        fill_finished.fetch_add(1);
        cv.notify_all();
      }
    });
  }

  // ------------------------------------------- coordination, progress, cancel
  bool cancelled = false;
  auto report_progress = [&]() -> bool {
    if (!progress_cb)
    {
      return true;
    }
    const size_t done =
        parsed_bytes.load(std::memory_order_relaxed) + filled_bytes.load(std::memory_order_relaxed);
    const size_t half = (done / 2 < data_total) ? (done / 2) : data_total;
    return progress_cb(data_start + half, length);
  };

  for (size_t ci = 0; ci < n_chunks; ci++)
  {
    {
      std::unique_lock<std::mutex> lk(mtx);
      while (!parsed_flags[ci] && !stop.load())
      {
        if (!report_progress())
        {
          stop = true;
          cancelled = true;
        }
        cv.wait_for(lk, std::chrono::milliseconds(30));
      }
    }
    if (stop.load())
    {
      break;
    }

    {
      std::lock_guard<std::mutex> lk(mtx);
      fill_next_slot.store(0);
      fill_gen.store(ci + 1);
    }
    cv.notify_all();

    {
      std::unique_lock<std::mutex> lk(mtx);
      while (fill_finished.load() < n_helpers * (ci + 1) && !stop.load())
      {
        if (!report_progress())
        {
          stop = true;
          cancelled = true;
        }
        cv.wait_for(lk, std::chrono::milliseconds(30));
      }
    }

    ParallelChunk& c = chunks[ci];
    filled_bytes.fetch_add(c.end - c.start, std::memory_order_relaxed);
    c.bufs.clear();
    c.bufs.shrink_to_fit();
    if (trace)
    {
      fprintf(stderr, "[par] chunk %zu/%zu filled at %.0f ms (parsed=%zu)\n", ci + 1, n_chunks,
              tr_ms(), parsed_bytes.load(std::memory_order_relaxed));
    }
    {
      std::lock_guard<std::mutex> lk(mtx);
      filled_idx = ci + 1;
    }
    cv.notify_all();
    if (stop.load())
    {
      break;
    }
  }

  // ---------------------------------------------------------------- shutdown
  {
    std::lock_guard<std::mutex> lk(mtx);
    shutdown.store(true);
  }
  cv.notify_all();
  for (auto& th : fill_threads)
  {
    th.join();
  }
  for (auto& th : parse_threads)
  {
    th.join();
  }

  if (cancelled)
  {
    throw std::runtime_error("ULog: import cancelled by user");
  }

  // --------------------------------- merge chunk-local state in file order
  for (auto& c : chunks)
  {
    for (auto& param : c.params)
    {
      bool found = false;
      for (auto& prev_param : _parameters)
      {
        if (prev_param.name == param.name)
        {
          prev_param = std::move(param);
          found = true;
          break;
        }
      }
      if (!found)
      {
        _parameters.push_back(std::move(param));
      }
    }
    for (auto& log : c.logs)
    {
      _message_logs.push_back(std::move(log));
    }
    _min_msg_time = std::min(_min_msg_time, c.min_time);
  }
}

const std::map<std::string, ULogParser::Timeseries>& ULogParser::getTimeseriesMap() const
{
  return _timeseries;
}
const std::vector<ULogParser::Parameter>& ULogParser::getParameters() const
{
  return _parameters;
}

const std::map<std::string, std::string>& ULogParser::getInfo() const
{
  return _info;
}

const std::vector<ULogParser::MessageLog>& ULogParser::getLogs() const
{
  return _message_logs;
}

bool ULogParser::readSubscription(DataStream& datastream, uint16_t msg_size)
{
  _read_buffer.reserve(msg_size + 1);
  char* message = (char*)_read_buffer.data();

  datastream.read(message, msg_size);
  message[msg_size] = 0;

  if (!datastream)
  {
    return false;
  }

  return true;
}

size_t ULogParser::fieldsCount(const ULogParser::Format& format) const
{
  size_t count = 0;
  for (const auto& field : format.fields)
  {
    if (field.type == OTHER)
    {
      // recursion!
      count += fieldsCount(_formats.at(field.other_type_ID));
    }
    else
    {
      count += size_t(field.array_size);
    }
  }
  return count;
}

std::vector<StringView> ULogParser::splitString(const StringView& strToSplit, char delimeter)
{
  std::vector<StringView> splitted_strings;
  splitted_strings.reserve(4);

  size_t pos = 0;
  while (pos < strToSplit.size())
  {
    size_t new_pos = strToSplit.find_first_of(delimeter, pos);
    if (new_pos == std::string::npos)
    {
      new_pos = strToSplit.size();
    }
    StringView sv = { &strToSplit.data()[pos], new_pos - pos };
    splitted_strings.push_back(sv);
    pos = new_pos + 1;
  }
  return splitted_strings;
}

bool ULogParser::readFileHeader(DataStream& datastream)
{
  ulog_file_header_s msg_header;
  datastream.read((char*)&msg_header, sizeof(msg_header));

  if (!datastream)
  {
    return false;
  }

  _file_start_time = msg_header.timestamp;

  // verify it's an ULog file
  char magic[8];
  magic[0] = 'U';
  magic[1] = 'L';
  magic[2] = 'o';
  magic[3] = 'g';
  magic[4] = 0x01;
  magic[5] = 0x12;
  magic[6] = 0x35;
  return memcmp(magic, msg_header.magic, 7) == 0;
}

bool ULogParser::readFileDefinitions(DataStream& datastream)
{
  ulog_message_header_s message_header;

  while (true)
  {
    //    qDebug() <<"\noffset before" << datastream.offset;
    datastream.read((char*)&message_header, ULOG_MSG_HEADER_LEN);
    //    qDebug() <<"msg_size" << message_header.msg_size;
    //    qDebug() <<"type" << char(message_header.msg_type);
    //    qDebug() <<"offset after" << datastream.offset;

    if (!datastream)
    {
      return false;
    }

    switch (message_header.msg_type)
    {
      case (int)ULogMessageType::FLAG_BITS:
        if (!readFlagBits(datastream, message_header.msg_size))
        {
          return false;
        }
        break;

      case (int)ULogMessageType::FORMAT:
        if (!readFormat(datastream, message_header.msg_size))
        {
          return false;
        }

        break;

      case (int)ULogMessageType::PARAMETER:
        if (!readParameter(datastream, message_header.msg_size))
        {
          return false;
        }

        break;

      case (int)ULogMessageType::ADD_LOGGED_MSG: {
        _data_section_start = datastream.offset - ULOG_MSG_HEADER_LEN;
        return true;
      }

      case (int)ULogMessageType::INFO: {
        if (!readInfo(datastream, message_header.msg_size))
        {
          return false;
        }
      }
      break;
      case (int)ULogMessageType::INFO_MULTIPLE:      // skip
      case (int)ULogMessageType::PARAMETER_DEFAULT:  // skip
        datastream.offset += message_header.msg_size;
        break;

      default:
        printf("unknown log definition type %i, size %i (offset %i)\n",
               (int)message_header.msg_type, (int)message_header.msg_size, (int)datastream.offset);
        datastream.offset += message_header.msg_size;
        break;
    }
  }
  return true;
}

bool ULogParser::readFlagBits(DataStream& datastream, uint16_t msg_size)
{
  if (msg_size != 40)
  {
    printf("unsupported message length for FLAG_BITS message (%i)", msg_size);
    return false;
  }

  _read_buffer.reserve(msg_size);
  uint8_t* message = (uint8_t*)_read_buffer.data();
  datastream.read((char*)message, msg_size);

  // uint8_t *compat_flags = message;
  uint8_t* incompat_flags = message + 8;

  // handle & validate the flags
  bool contains_appended_data = incompat_flags[0] & ULOG_INCOMPAT_FLAG0_DATA_APPENDED_MASK;
  bool has_unknown_incompat_bits = false;

  if (incompat_flags[0] & ~0x1)
  {
    has_unknown_incompat_bits = true;
  }

  for (int i = 1; i < 8; ++i)
  {
    if (incompat_flags[i])
    {
      has_unknown_incompat_bits = true;
    }
  }

  if (has_unknown_incompat_bits)
  {
    printf("Log contains unknown incompat bits set. Refusing to parse");
    return false;
  }

  if (contains_appended_data)
  {
    uint64_t appended_offsets[3];
    memcpy(appended_offsets, message + 16, sizeof(appended_offsets));

    if (appended_offsets[0] > 0)
    {
      // the appended data is currently only used for hardfault dumps, so it's safe to
      // ignore it.
      //  LOG_INFO("Log contains appended data. Replay will ignore this data" );
      _read_until_file_position = appended_offsets[0];
    }
  }
  return true;
}

bool ULogParser::readFormat(DataStream& datastream, uint16_t msg_size)
{
  _read_buffer.reserve(msg_size + 1);
  char* buffer = (char*)_read_buffer.data();
  datastream.read(buffer, msg_size);
  buffer[msg_size] = 0;

  if (!datastream)
  {
    return false;
  }

  std::string str_format(buffer);
  size_t pos = str_format.find(':');

  if (pos == std::string::npos)
  {
    return false;
  }

  std::string name = str_format.substr(0, pos);
  std::string fields = str_format.substr(pos + 1);

  Format format;
  auto fields_split = splitString(fields, ';');
  format.fields.reserve(fields_split.size());
  for (auto field_section : fields_split)
  {
    auto raw_tokens = splitString(field_section, ' ');
    std::vector<StringView> field_pair;
    for (const auto& token : raw_tokens)
    {
      if (!token.empty())
      {
        field_pair.push_back(token);
      }
    }
    if (field_pair.size() < 2)
    {
      continue;
    }

    auto field_type = field_pair.at(0);
    auto field_name = field_pair.at(1);

    Field field;
    if (startsWith(field_type, "int8_t"))
    {
      field.type = INT8;
      field_type.remove_prefix(6);
    }
    else if (startsWith(field_type, "int16_t"))
    {
      field.type = INT16;
      field_type.remove_prefix(7);
    }
    else if (startsWith(field_type, "int32_t"))
    {
      field.type = INT32;
      field_type.remove_prefix(7);
    }
    else if (startsWith(field_type, "int64_t"))
    {
      field.type = INT64;
      field_type.remove_prefix(7);
    }
    else if (startsWith(field_type, "uint8_t"))
    {
      field.type = UINT8;
      field_type.remove_prefix(7);
    }
    else if (startsWith(field_type, "uint16_t"))
    {
      field.type = UINT16;
      field_type.remove_prefix(8);
    }
    else if (startsWith(field_type, "uint32_t"))
    {
      field.type = UINT32;
      field_type.remove_prefix(8);
    }
    else if (startsWith(field_type, "uint64_t"))
    {
      field.type = UINT64;
      field_type.remove_prefix(8);
    }
    else if (startsWith(field_type, "double"))
    {
      field.type = DOUBLE;
      field_type.remove_prefix(6);
    }
    else if (startsWith(field_type, "float"))
    {
      field.type = FLOAT;
      field_type.remove_prefix(5);
    }
    else if (startsWith(field_type, "bool"))
    {
      field.type = BOOL;
      field_type.remove_prefix(4);
    }
    else if (startsWith(field_type, "char"))
    {
      field.type = CHAR;
      field_type.remove_prefix(4);
    }
    else
    {
      field.type = OTHER;

      if (endsWith(field_type, "]"))
      {
        StringView helper = field_type;
        while (!endsWith(helper, "["))
        {
          helper.remove_suffix(1);
        }

        helper.remove_suffix(1);
        field.other_type_ID = std::string(helper);

        while (!startsWith(field_type, "["))
        {
          field_type.remove_prefix(1);
        }
      }
      else
      {
        field.other_type_ID = std::string(field_type);
      }
    }

    field.array_size = 1;

    if (field_type.size() > 0 && field_type[0] == '[')
    {
      field_type.remove_prefix(1);
      field.array_size = field_type[0] - '0';
      field_type.remove_prefix(1);

      while (field_type[0] != ']')
      {
        field.array_size = 10 * field.array_size + field_type[0] - '0';
        field_type.remove_prefix(1);
      }
    }

    if (field.type == UINT64 && field_name == StringView("timestamp"))
    {
      format.timestamp_idx = format.fields.size();
    }
    else
    {
      field.field_name = std::string(field_name);
      format.fields.push_back(field);
    }
  }

  format.name = name;
  _formats[name] = std::move(format);

  return true;
}

template <typename T>
std::string int_to_hex(T i)
{
  std::stringstream stream;
  stream << "0x" << std::setfill('0') << std::setw(sizeof(T) * 2) << std::hex << i;
  return stream.str();
}

bool ULogParser::readInfo(DataStream& datastream, uint16_t msg_size)
{
  _read_buffer.reserve(msg_size);
  uint8_t* message = (uint8_t*)_read_buffer.data();
  datastream.read((char*)message, msg_size);

  if (!datastream)
  {
    return false;
  }
  uint8_t key_len = message[0];
  message++;
  std::string raw_key((char*)message, key_len);
  message += key_len;
  std::string raw_value((char*)message, msg_size - key_len - 1);

  auto key_parts = splitString(raw_key, ' ');

  std::string key = std::string(key_parts[1]);
  std::string value;

  if (startsWith(key_parts[0], "char["))
  {
    value = raw_value;
  }
  else if (key_parts[0] == StringView("bool"))
  {
    bool val = *reinterpret_cast<const bool*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("uint8_t"))
  {
    uint8_t val = *reinterpret_cast<const uint8_t*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("int8_t"))
  {
    int8_t val = *reinterpret_cast<const int8_t*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("uint16_t"))
  {
    uint16_t val = *reinterpret_cast<const uint16_t*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("int16_t"))
  {
    int16_t val = *reinterpret_cast<const int16_t*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("uint32_t"))
  {
    uint32_t val = *reinterpret_cast<const uint32_t*>(raw_value.data());
    if (startsWith(key_parts[1], "ver_") && endsWith(key_parts[1], "_release"))
    {
      value = int_to_hex(val);
    }
    else
    {
      value = std::to_string(val);
    }
  }
  else if (key_parts[0] == StringView("int32_t"))
  {
    int32_t val = *reinterpret_cast<const int32_t*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("float"))
  {
    float val = *reinterpret_cast<const float*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("double"))
  {
    double val = *reinterpret_cast<const double*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("uint64_t"))
  {
    uint64_t val = *reinterpret_cast<const uint64_t*>(raw_value.data());
    value = std::to_string(val);
  }
  else if (key_parts[0] == StringView("int64_t"))
  {
    int64_t val = *reinterpret_cast<const int64_t*>(raw_value.data());
    value = std::to_string(val);
  }

  _info.insert({ key, value });
  return true;
}

bool ULogParser::readParameter(DataStream& datastream, uint16_t msg_size)
{
  _read_buffer.reserve(msg_size);
  char* message = (char*)_read_buffer.data();
  datastream.read((char*)message, msg_size);
  if (!datastream)
  {
    return false;
  }

  Parameter param;
  param.readFromBuffer(message);
  _parameters.push_back(param);
  return true;
}

ULogParser::Timeseries ULogParser::createTimeseries(const ULogParser::Format* format)
{
  std::function<void(const Format& format, const std::string& prefix)> appendVector;

  Timeseries timeseries;

  appendVector = [&appendVector, this, &timeseries](const Format& format,
                                                    const std::string& prefix) {
    for (const auto& field : format.fields)
    {
      // skip padding messages
      if (startsWith(StringView(field.field_name), "_padding"))
      {
        continue;
      }

      std::string new_prefix = prefix + "/" + field.field_name;
      for (int i = 0; i < field.array_size; i++)
      {
        std::string array_suffix = "";
        if (field.array_size > 1)
        {
          char buff[16];
          sprintf(buff, ".%02d", i);
          array_suffix = buff;
        }
        if (field.type != OTHER)
        {
          timeseries.data.push_back({ new_prefix + array_suffix, std::vector<double>() });
        }
        else
        {
          appendVector(this->_formats.at(field.other_type_ID), new_prefix + array_suffix);
        }
      }
    }
  };

  appendVector(*format, {});
  return timeseries;
}

bool ULogParser::Parameter::readFromBuffer(const char* message)
{
  const uint8_t key_len = static_cast<uint8_t>(message[0]);
  message++;
  std::string key((char*)message, key_len);

  const size_t pos = key.find(' ');
  if (pos == std::string::npos)
  {
    return false;
  }

  const std::string type = key.substr(0, pos);
  this->name = key.substr(pos + 1);
  message += key_len;

  if (type == "int32_t")
  {
    this->value.val_int = *reinterpret_cast<const int32_t*>(message);
    this->val_type = INT32;
  }
  else if (type == "float")
  {
    this->value.val_real = *reinterpret_cast<const float*>(message);
    this->val_type = FLOAT;
  }
  else
  {
    throw std::runtime_error("unknown parameter type");
  }
  return true;
}
