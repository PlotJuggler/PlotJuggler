#include "ulog_parser.h"
#include "ulog_messages.h"

#include <iosfwd>
#include <sstream>
#include <iomanip>
#include <QDebug>

using ios = std::ios;

ULogParser::ULogParser(DataStream& datastream) : _file_start_time(0)
{
  bool ret = readFileHeader(datastream);

  if (!ret)
  {
    throw std::runtime_error("ULog: wrong header");
  }

  if (!readFileDefinitions(datastream))
  {
    throw std::runtime_error("ULog: error loading definitions");
  }

  datastream.offset = _data_section_start;

  while (datastream)
  {
    ulog_message_header_s message_header;
    datastream.read((char*)&message_header, ULOG_MSG_HEADER_LEN);

    if (!datastream)
    {
      break;
    }

    _read_buffer.reserve(message_header.msg_size + 1);
    char* message = (char*)_read_buffer.data();
    datastream.read(message, message_header.msg_size);

    if (!datastream)
    {
      break;
    }

    message[message_header.msg_size] = '\0';

    switch (message_header.msg_type)
    {
      case (int)ULogMessageType::ADD_LOGGED_MSG: {
        Subscription sub;

        sub.multi_id = *reinterpret_cast<uint8_t*>(message);
        sub.msg_id = *reinterpret_cast<uint16_t*>(message + 1);
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
          uint16_t msg_id = *reinterpret_cast<uint16_t*>(message);
          _subscriptions.erase(msg_id);
        }
        break;
      case (int)ULogMessageType::DATA: {
        uint16_t msg_id = *reinterpret_cast<uint16_t*>(message);
        message += 2;
        auto sub_it = _subscriptions.find(msg_id);
        if (sub_it == _subscriptions.end())
        {
          continue;
        }
        const Subscription& sub = sub_it->second;

        parseDataMessage(sub, message);
      }
      break;

      case (int)ULogMessageType::LOGGING: {
        MessageLog msg;
        msg.level = static_cast<char>(message[0]);
        message += sizeof(char);
        msg.timestamp = *reinterpret_cast<uint64_t*>(message);
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

void ULogParser::parseDataMessage(const ULogParser::Subscription& sub, char* message)
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

char* ULogParser::parseSimpleDataMessage(Timeseries& timeseries, const Format* format,
                                         char* message, size_t* index, bool read_timestamp)
{
  for (size_t i = 0; i <= format->fields.size(); i++)
  {
    if (format->timestamp_idx == static_cast<int>(i))
    {
      uint64_t time_val = *reinterpret_cast<uint64_t*>(message);
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
          value = static_cast<double>(*reinterpret_cast<uint8_t*>(message));
          message += 1;
        }
        break;
        case INT8: {
          value = static_cast<double>(*reinterpret_cast<int8_t*>(message));
          message += 1;
        }
        break;
        case UINT16: {
          value = static_cast<double>(*reinterpret_cast<uint16_t*>(message));
          message += 2;
        }
        break;
        case INT16: {
          value = static_cast<double>(*reinterpret_cast<int16_t*>(message));
          message += 2;
        }
        break;
        case UINT32: {
          value = static_cast<double>(*reinterpret_cast<uint32_t*>(message));
          message += 4;
        }
        break;
        case INT32: {
          value = static_cast<double>(*reinterpret_cast<int32_t*>(message));
          message += 4;
        }
        break;
        case UINT64: {
          value = static_cast<double>(*reinterpret_cast<uint64_t*>(message));
          message += 8;
        }
        break;
        case INT64: {
          value = static_cast<double>(*reinterpret_cast<int64_t*>(message));
          message += 8;
        }
        break;
        case FLOAT: {
          value = static_cast<double>(*reinterpret_cast<float*>(message));
          message += 4;
        }
        break;
        case DOUBLE: {
          value = (*reinterpret_cast<double*>(message));
          message += 8;
        }
        break;
        case CHAR: {
          value = static_cast<double>(*reinterpret_cast<char*>(message));
          message += 1;
        }
        break;
        case BOOL: {
          value = static_cast<double>(*reinterpret_cast<bool*>(message));
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
