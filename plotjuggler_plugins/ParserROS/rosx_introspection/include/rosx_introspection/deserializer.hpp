#pragma once

// API adapted to FastCDR

#include <exception>
#include <optional>

#include "rosx_introspection/builtin_types.hpp"
#include "rosx_introspection/contrib/nanocdr.hpp"
#include "rosx_introspection/variant.hpp"

namespace RosMsgParser {

class Deserializer {
 public:
  virtual void init(Span<const uint8_t> buffer) {
    _buffer = buffer;
    reset();
  }

  virtual bool isROS2() const = 0;

  virtual ~Deserializer() = default;

  // move the memory pointer
  virtual void jump(size_t bytes) = 0;

  // deserialize the current pointer into a variant (not a string)
  [[nodiscard]] virtual Variant deserialize(BuiltinType type) = 0;

  [[nodiscard]] virtual Span<const uint8_t> deserializeByteSequence() = 0;

  // deserialize the current pointer into a string
  virtual void deserializeString(std::string& out) = 0;

  [[nodiscard]] virtual uint32_t deserializeUInt32() = 0;

  [[nodiscard]] virtual const uint8_t* getCurrentPtr() const = 0;

  [[nodiscard]] virtual size_t bytesLeft() const {
    return _buffer.size() - (getCurrentPtr() - _buffer.data());
  }

  /// Check if an optional member is present in the CDR stream.
  /// Default: always present (for ROS messages which don't have optional fields).
  [[nodiscard]] virtual bool hasOptionalMember() {
    return true;
  }

  // reset the pointer to beginning of buffer
  virtual void reset() = 0;

 protected:
  Span<const uint8_t> _buffer;
};

//-----------------------------------------------------------------

// Specialization od deserializer that works with ROS1
class ROS_Deserializer : public Deserializer {
 public:
  Variant deserialize(BuiltinType type) override;

  bool isROS2() const override {
    return false;
  }

  void deserializeString(std::string& dst) override;

  uint32_t deserializeUInt32() override;

  Span<const uint8_t> deserializeByteSequence() override;

  const uint8_t* getCurrentPtr() const override;

  void jump(size_t bytes) override;

  void reset() override;

 protected:
  const uint8_t* _ptr;
  size_t _bytes_left;

  template <typename T>
  T deserialize() {
    T out;
    if (sizeof(T) > _bytes_left) {
      throw std::runtime_error("Buffer overrun in Deserializer");
    }
    out = (*(reinterpret_cast<const T*>(_ptr)));
    _bytes_left -= sizeof(T);
    _ptr += sizeof(T);
    return out;
  }
};

//-----------------------------------------------------------------

// Specialization od deserializer that works with ROS2
// wrapping FastCDR
class NanoCDR_Deserializer : public Deserializer {
 public:
  // Set to true for RTI DDS Micro compatibility (align from byte 0 instead of byte 4)
  void setAlignFromMessageStart(bool enable) { _align_from_start = enable; }

  Variant deserialize(BuiltinType type) override;

  void deserializeString(std::string& dst) override;

  uint32_t deserializeUInt32() override;

  Span<const uint8_t> deserializeByteSequence() override;

  const uint8_t* getCurrentPtr() const override;

  void jump(size_t bytes) override;

  virtual void reset() override;

  bool isROS2() const override {
    return true;
  }

  bool hasOptionalMember() override {
    return _cdr_decoder->hasMember();
  }

 protected:
  std::optional<nanocdr::Decoder> _cdr_decoder;
  bool _align_from_start = false;
};

using ROS2_Deserializer = NanoCDR_Deserializer;

}  // namespace RosMsgParser
