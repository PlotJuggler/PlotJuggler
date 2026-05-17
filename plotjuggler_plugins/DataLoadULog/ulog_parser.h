#pragma once

#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <string.h>
#include <cstdint>
#include <optional>

#include <string_view>

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
      if (len > _length - offset)
      {
        offset = _length;
        return;
      }
      memcpy(dst, &_data[offset], len);
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

  struct Subscription
  {
    Subscription() : msg_id(0), multi_id(0), format(nullptr)
    {
    }

    uint16_t msg_id;
    uint8_t multi_id;
    std::string message_name;
    const Format* format;
  };

  struct Timeseries
  {
    std::vector<std::optional<uint64_t>> timestamps;
    std::vector<std::pair<std::string, std::vector<double>>> data;
  };

public:
  ULogParser(DataStream& datastream);

  const std::map<std::string, Timeseries>& getTimeseriesMap() const;

  const std::vector<Parameter>& getParameters() const;

  const std::map<std::string, std::string>& getInfo() const;

  const std::vector<MessageLog>& getLogs() const;

private:
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

  std::streampos _data_section_start;  ///< first ADD_LOGGED_MSG message

  int64_t _read_until_file_position = 1ULL << 60;  ///< read limit if log contains appended data

  std::set<std::string> _overridden_params;

  std::map<std::string, Format> _formats;

  std::map<std::string, std::string> _info;

  std::map<uint16_t, Subscription> _subscriptions;

  std::map<std::string, Timeseries> _timeseries;

  std::vector<StringView> splitString(const StringView& strToSplit, char delimeter);

  std::set<std::string> _message_name_with_multi_id;

  std::vector<MessageLog> _message_logs;

  void parseDataMessage(const Subscription& sub, char* message);

  char* parseSimpleDataMessage(Timeseries& timeseries, const Format* format, char* message,
                               size_t* index, bool read_timestamp = true);
};
