#include "rosx_introspection/deserializer.hpp"

#include "rosx_introspection/contrib/nanocdr.hpp"

namespace RosMsgParser {

Variant ROS_Deserializer::deserialize(BuiltinType type) {
  switch (type) {
    case BOOL:
      return deserialize<bool>();
    case CHAR:
      return deserialize<char>();
    case BYTE:
    case UINT8:
      return deserialize<uint8_t>();
    case UINT16:
      return deserialize<uint16_t>();
    case UINT32:
      return deserialize<uint32_t>();
    case UINT64:
      return deserialize<uint64_t>();

    case INT8:
      return deserialize<int8_t>();
    case INT16:
      return deserialize<int16_t>();
    case INT32:
      return deserialize<int32_t>();
    case INT64:
      return deserialize<int64_t>();

    case FLOAT32:
      return deserialize<float>();
    case FLOAT64:
      return deserialize<double>();

    case DURATION:
    case TIME: {
      RosMsgParser::Time tmp;
      tmp.sec = deserialize<uint32_t>();
      tmp.nsec = deserialize<uint32_t>();
      return tmp;
    }

    default:
      throw std::runtime_error("ROS_Deserializer: type not recognized");
  }

  return {};
}

void ROS_Deserializer::deserializeString(std::string& dst) {
  uint32_t string_size = deserialize<uint32_t>();

  if (string_size > _bytes_left) {
    throw std::runtime_error("Buffer overrun in ROS_Deserializer::deserializeString");
  }

  if (string_size == 0) {
    dst = {};
    return;
  }

  const char* buffer_ptr = reinterpret_cast<const char*>(_ptr);
  dst.assign(buffer_ptr, string_size);

  _ptr += string_size;
  _bytes_left -= string_size;
}

uint32_t ROS_Deserializer::deserializeUInt32() {
  return deserialize<uint32_t>();
}

Span<const uint8_t> ROS_Deserializer::deserializeByteSequence() {
  uint32_t vect_size = deserialize<uint32_t>();
  if (vect_size > _bytes_left) {
    throw std::runtime_error(
        "Buffer overrun in "
        "ROS_Deserializer::deserializeByteSequence");
  }
  if (vect_size == 0) {
    return {};
  }
  Span<const uint8_t> out(_ptr, vect_size);
  jump(vect_size);
  return out;
}

const uint8_t* ROS_Deserializer::getCurrentPtr() const {
  return _ptr;
}

void ROS_Deserializer::jump(size_t bytes) {
  if (bytes > _bytes_left) {
    throw std::runtime_error("Buffer overrun");
  }
  _ptr += bytes;
  _bytes_left -= bytes;
}

void ROS_Deserializer::reset() {
  _ptr = _buffer.data();
  _bytes_left = _buffer.size();
}

// ----------------------------------------------

template <typename T>
static T Deserialize(nanocdr::Decoder& decoder) {
  T tmp;
  decoder.decode(tmp);
  return tmp;
}

Variant NanoCDR_Deserializer::deserialize(BuiltinType type) {
  switch (type) {
    case BOOL:
      return Deserialize<bool>(*_cdr_decoder);
    case CHAR:
      return Deserialize<char>(*_cdr_decoder);
    case BYTE:
    case UINT8:
      return Deserialize<uint8_t>(*_cdr_decoder);
    case UINT16:
      return Deserialize<uint16_t>(*_cdr_decoder);
    case UINT32:
      return Deserialize<uint32_t>(*_cdr_decoder);
    case UINT64:
      return Deserialize<uint64_t>(*_cdr_decoder);

    case INT8:
      return Deserialize<int8_t>(*_cdr_decoder);
    case INT16:
      return Deserialize<int16_t>(*_cdr_decoder);
    case INT32:
      return Deserialize<int32_t>(*_cdr_decoder);
    case INT64:
      return Deserialize<int64_t>(*_cdr_decoder);

    case FLOAT32:
      return Deserialize<float>(*_cdr_decoder);
    case FLOAT64:
      return Deserialize<double>(*_cdr_decoder);

    case DURATION:
    case TIME: {
      RosMsgParser::Time tmp;
      tmp.sec = Deserialize<uint32_t>(*_cdr_decoder);
      tmp.nsec = Deserialize<uint32_t>(*_cdr_decoder);
      return tmp;
    }

    default:
      throw std::runtime_error("NanoCDR_Deserializer: type not recognized");
  }

  return {};
}

void NanoCDR_Deserializer::deserializeString(std::string& dst) {
  _cdr_decoder->decode(dst);
}

uint32_t NanoCDR_Deserializer::deserializeUInt32() {
  return Deserialize<uint32_t>(*_cdr_decoder);
}

Span<const uint8_t> NanoCDR_Deserializer::deserializeByteSequence() {
  uint32_t seqLength = 0;
  _cdr_decoder->decode(seqLength);

  if (seqLength == 0) {
    return {};
  }

  const auto* ptr = _cdr_decoder->currentBuffer().data();
  _cdr_decoder->jump(seqLength);
  return {reinterpret_cast<const uint8_t*>(ptr), seqLength};
}

const uint8_t* NanoCDR_Deserializer::getCurrentPtr() const {
  return reinterpret_cast<const uint8_t*>(_cdr_decoder->currentBuffer().data());
}

void NanoCDR_Deserializer::jump(size_t bytes) {
  _cdr_decoder->jump(bytes);
}

void NanoCDR_Deserializer::reset() {
  nanocdr::ConstBuffer nano_buffer(_buffer.data(), _buffer.size());
  _cdr_decoder.emplace(nano_buffer, nanocdr::CdrVersion::DDS_CDR, _align_from_start);
}

}  // namespace RosMsgParser
