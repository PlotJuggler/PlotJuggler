#include <QSettings>
#include <QMessageBox>
#include "protobuf_parser.h"
#include "fmt/core.h"

namespace gp = google::protobuf;

ProtobufParser::ProtobufParser(const std::string& topic_name, const std::string type_name,
                               const gp::FileDescriptorSet& descriptor_set, PlotDataMapRef& data)
  : MessageParser(topic_name, data), _proto_pool(&_proto_database)
{
  gp::FileDescriptorProto unused;

  for (int i = 0; i < descriptor_set.file_size(); ++i)
  {
    const auto& file = descriptor_set.file(i);
    if (!_proto_database.FindFileByName(file.name(), &unused))
    {
      if (!_proto_database.Add(file))
      {
        throw std::runtime_error(
            fmt::format("failed to add definition {} to protoDB", file.name()));
      }
    }
  }

  _msg_descriptor = _proto_pool.FindMessageTypeByName(type_name);

  if (_msg_descriptor == nullptr)
  {
    throw std::runtime_error("Cannot get message descriptor");
  }
}

bool ProtobufParser::parseMessage(const MessageRef serialized_msg, double& timestamp)
{
  const google::protobuf::Message* prototype_msg = _msg_factory.GetPrototype(_msg_descriptor);

  google::protobuf::Message* mutable_msg = prototype_msg->New();
  if (!mutable_msg->ParseFromArray(serialized_msg.data(), serialized_msg.size()))
  {
    return false;
  }

  std::function<void(const google::protobuf::Message&, const std::string&, const bool)> ParseImpl;

  if (useEmbeddedTimestamp())
  {
    // search the correct field the first time
    if (!_timestamp_field_index && _first_message)
    {
      _first_message = false;
      for (auto i = 0; i < _msg_descriptor->field_count(); ++i)
      {
        const auto& field = _msg_descriptor->field(i);
        if ("timestamp" == field->name() &&
            field->cpp_type() == gp::FieldDescriptor::CPPTYPE_DOUBLE)
        {
          _timestamp_field_index = i;
          break;
        }
      }
    }
    // if there is a field with the expected name, use that as timestamp
    if (_timestamp_field_index)
    {
      const gp::Reflection* ref_tmp = mutable_msg->GetReflection();
      const gp::Descriptor* desc_tmp = mutable_msg->GetDescriptor();
      timestamp = ref_tmp->GetDouble(*mutable_msg, desc_tmp->field(*_timestamp_field_index));
    }
  }

  ParseImpl = [&](const google::protobuf::Message& msg, const std::string& prefix,
                  const bool is_map) {
    const gp::Reflection* reflection = msg.GetReflection();
    const gp::Descriptor* descriptor = msg.GetDescriptor();

    for (int index = 0; index < descriptor->field_count(); index++)
    {
      auto field = descriptor->field(index);

      std::string key =
          prefix.empty() ? std::string(field->name()) : fmt::format("{}/{}", prefix, field->name());
      if (is_map)
      {
        // Map messages only have 2 fields: key and value. The key will be represented in
        // the series name so skip it, and don't uselessly append "value" to the series
        // name for the value.
        if (field->name() == "key")
        {
          continue;
        }
        else
        {
          key = prefix;
        }
      }

      std::string suffix;

      if (!field)
      {
        continue;
      }

      // Skip unset singular fields that track presence so missing data is not plotted as
      // default values.
      if (!field->is_repeated() && field->has_presence() && !reflection->HasField(msg, field))
      {
        continue;
      }

      unsigned count = 1;
      bool repeated = false;
      if (field->is_repeated())
      {
        count = reflection->FieldSize(msg, field);
        repeated = true;
      }

      if (repeated && count > maxArraySize())
      {
        if (clampLargeArray())
        {
          count = std::max(count, maxArraySize());
        }
        else
        {
          continue;
        }
      }

      for (unsigned index = 0; index < count; index++)
      {
        if (repeated)
        {
          suffix = fmt::format("[{}]", index);
        }

        bool is_double = true;
        double value = 0;
        switch (field->cpp_type())
        {
          case gp::FieldDescriptor::CPPTYPE_DOUBLE: {
            value = !repeated ? reflection->GetDouble(msg, field) :
                                reflection->GetRepeatedDouble(msg, field, index);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_FLOAT: {
            auto tmp = !repeated ? reflection->GetFloat(msg, field) :
                                   reflection->GetRepeatedFloat(msg, field, index);
            value = static_cast<double>(tmp);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_UINT32: {
            auto tmp = !repeated ? reflection->GetUInt32(msg, field) :
                                   reflection->GetRepeatedUInt32(msg, field, index);
            value = static_cast<double>(tmp);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_UINT64: {
            auto tmp = !repeated ? reflection->GetUInt64(msg, field) :
                                   reflection->GetRepeatedUInt64(msg, field, index);
            value = static_cast<double>(tmp);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_BOOL: {
            auto tmp = !repeated ? reflection->GetBool(msg, field) :
                                   reflection->GetRepeatedBool(msg, field, index);
            value = static_cast<double>(tmp);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_INT32: {
            auto tmp = !repeated ? reflection->GetInt32(msg, field) :
                                   reflection->GetRepeatedInt32(msg, field, index);
            value = static_cast<double>(tmp);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_INT64: {
            auto tmp = !repeated ? reflection->GetInt64(msg, field) :
                                   reflection->GetRepeatedInt64(msg, field, index);
            value = static_cast<double>(tmp);
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_ENUM: {
            auto tmp = !repeated ? reflection->GetEnum(msg, field) :
                                   reflection->GetRepeatedEnum(msg, field, index);

            auto& series = this->getStringSeries(key + suffix);
            series.pushBack({ timestamp, std::string(tmp->name()) });
            is_double = false;
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_STRING: {
            auto tmp = !repeated ? reflection->GetString(msg, field) :
                                   reflection->GetRepeatedString(msg, field, index);

            if (tmp.size() > 100)
            {
              // probably a blob, skip it
              continue;
            }
            auto& series = this->getStringSeries(key + suffix);
            series.pushBack({ timestamp, tmp });
            is_double = false;
          }
          break;
          case gp::FieldDescriptor::CPPTYPE_MESSAGE: {
// Fix macro issue in Windows
#pragma push_macro("GetMessage")
#undef GetMessage
            const auto& new_msg = repeated ? reflection->GetRepeatedMessage(msg, field, index) :
                                             reflection->GetMessage(msg, field);
#pragma pop_macro("GetMessage")
            if (field->is_map())
            {
              // A protobuf map looks just like a message but with a "key" and
              // "value" field, extract the key so we can set a useful suffix.
              const auto* map_descriptor = new_msg.GetDescriptor();
              const auto* map_reflection = new_msg.GetReflection();
              const auto* key_field = map_descriptor->FindFieldByName("key");
              switch (key_field->cpp_type())
              {
                // A map's key is a scalar type (except floats and bytes) or a string
                case gp::FieldDescriptor::CPPTYPE_STRING: {
                  suffix = fmt::format("/{}", map_reflection->GetString(new_msg, key_field));
                }
                break;
                case gp::FieldDescriptor::CPPTYPE_INT32: {
                  suffix = fmt::format("/{}", map_reflection->GetInt32(new_msg, key_field));
                }
                break;
                case gp::FieldDescriptor::CPPTYPE_INT64: {
                  suffix = fmt::format("/{}", map_reflection->GetInt64(new_msg, key_field));
                }
                break;
                case gp::FieldDescriptor::CPPTYPE_UINT32: {
                  suffix = fmt::format("/{}", map_reflection->GetUInt32(new_msg, key_field));
                }
                break;
                case gp::FieldDescriptor::CPPTYPE_UINT64: {
                  suffix = fmt::format("/{}", map_reflection->GetUInt64(new_msg, key_field));
                }
                break;
              }
            }
            ParseImpl(new_msg, key + suffix, field->is_map());

            is_double = false;
          }
          break;
        }

        if (is_double)
        {
          auto& series = this->getSeries(key + suffix);
          series.pushBack({ timestamp, value });
        }
      }
    }
  };

  // start recursion
  ParseImpl(*mutable_msg, _topic_name, false);

  delete mutable_msg;
  return true;
}
