// Copyright 2025 Davide Faconti
// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "rosx_introspection/contrib/SmallVector.h"

namespace nanocdr
{

template <typename T>
class Span
{
public:
  Span(T* data, size_t size) : data_(data), size_(size)
  {
  }

  // if std::is_const_v<T> is true, add the constructor from
  // std::vector<std::remove_const_t<T>>
  template <typename U, typename Allocator,
            typename = std::enable_if_t<std::is_const_v<U>>>
  Span(const std::vector<U, Allocator>& vec) : data_(vec.data()), size_(vec.size())
  {
  }

  // if std::is_const_v<T> is false, add the constructor from std::vector<T>
  template <typename U, typename Allocator,
            typename = std::enable_if_t<!std::is_const_v<U>>>
  Span(std::vector<U, Allocator>& vec) : data_(vec.data()), size_(vec.size())
  {
  }

  T* data() const
  {
    return data_;
  }
  size_t size() const
  {
    return size_;
  }

  T& operator[](size_t index) const
  {
    return data_[index];
  }

  T* begin() const
  {
    return data_;
  }
  T* end() const
  {
    return data_ + size_;
  }

  bool empty() const
  {
    return size_ == 0;
  }

  void trim_front(size_t n)
  {
    data_ += n;
    size_ -= n;
  }

  void trim_back(size_t n)
  {
    size_ -= n;
  }

private:
  T* data_ = nullptr;
  size_t size_ = 0;
};

using Buffer = Span<uint8_t>;
using ConstBuffer = Span<const uint8_t>;

//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------

// CDR Header related types and constants
enum class CdrVersion : uint8_t
{
  DDS_CDR = 1,
  XCDRv1 = 2,
  XCDRv2 = 3
};

enum class EncodingFlag : uint8_t
{
  PLAIN_CDR = 0x0,
  PL_CDR = 0x2,
  PLAIN_CDR2 = 0x6,
  DELIMIT_CDR2 = 0x8,
  PL_CDR2 = 0xa
};

enum class Endianness : uint8_t
{
  CDR_BIG_ENDIAN = 0x00,
  CDR_LITTLE_ENDIAN = 0x01
};

Endianness getCurrentEndianness();

template <typename T>
inline void swapEndianness(T& val);

struct CdrHeader
{
  Endianness endianness = Endianness::CDR_LITTLE_ENDIAN;
  EncodingFlag encoding = EncodingFlag::PLAIN_CDR;
  CdrVersion version = CdrVersion::DDS_CDR;
};

//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------

/**
 * @brief TypeDefinition is a template class that defines the encoding and decoding of a
 * type.
 *
 * Example usage. Given a struct:
 * @code
 * struct MyType {
 *   int a;
 *   float b;
 * };
 * @endcode
 *
 * You should define a specialization of TypeDefinition for your type:
 * @code
 * namespace nanocdr {
 * template <> struct TypeDefinition<MyType> {
 *   template <class Operator>
 *   void operator()(MyType& obj, Operator& op) {
 *     op(obj.a);
 *     op(obj.b);
 *   }
 * };
 * }
 * @endcode
 */
template <class Type>
struct TypeDefinition
{
  TypeDefinition() = delete;

  template <class Operator>
  void operator()(Type& obj, Operator& op);
};

//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------
/**
 * @brief Decoder is a class that decodes data from a buffer.
 */
class Decoder
{
public:
  Decoder(ConstBuffer buffer, CdrVersion default_cdr = CdrVersion::DDS_CDR,
          bool align_from_message_start = false);

  const CdrHeader& header() const
  {
    return header_;
  }

  /**
   * @brief Decode a single value from the buffer.
   *
   * @tparam T The type of the value to decode.
   * @param out The value to decode into.
   */
  template <typename T>
  void decode(T& out);

  // specializations for std::vector
  template <typename T, typename Allocator>
  void decode(std::vector<T, Allocator>& out);

  // specializations for std::array
  template <typename T, size_t N>
  void decode(std::array<T, N>& out);

  // specializations for std::string
  void decode(std::string& out);

  /// Check if an optional member is present in the CDR stream.
  /// Consumes the presence indicator from the buffer.
  bool hasMember()
  {
    if (header_.version == CdrVersion::XCDRv1 || header_.version == CdrVersion::DDS_CDR)
    {
      // XCDRv1/DDS_CDR: 2 bytes member ID + 2 bytes size.
      // The 4-byte PL_CDR member header itself must be 4-byte aligned relative
      // to the CDR origin (a uint16 decode would only align to 2).
      const size_t pad = alignment(4);
      if (pad > 0)
      {
        buffer_.trim_front(pad);
      }
      uint16_t member_id = 0;
      uint16_t size = 0;
      decode(member_id);
      decode(size);
      if (size == 0)
      {
        return false;
      }
      if (buffer_.size() < size)
      {
        throw std::runtime_error("Decode: optional member size exceeds remaining buffer");
      }
      member_scopes_.push_back(MemberScope{buffer_.data(), buffer_.data() + size});
      return true;
    }
    else
    {
      // XCDR2 PLAIN_CDR2: 1 byte boolean is_present
      bool is_present = false;
      decode(is_present);
      return is_present;
    }
  }

  // Move forward the pointer of the buffer
  void jump(size_t offset)
  {
    buffer_.trim_front(offset);
  }

  /// Get a view to the current buffer (bytes left to decode)
  ConstBuffer currentBuffer() const
  {
    return buffer_;
  }

private:
  ConstBuffer buffer_;
  const uint8_t* origin_ = nullptr;
  CdrHeader header_;

  struct MemberScope
  {
    const uint8_t* origin = nullptr;
    const uint8_t* end = nullptr;
  };

  llvm_vecsmall::SmallVector<MemberScope, 8> member_scopes_;

  size_t alignment(size_t data_size)
  {
    while (!member_scopes_.empty() && buffer_.data() >= member_scopes_.back().end)
    {
      member_scopes_.pop_back();
    }

    data_size = (data_size == 8) ? align64_ : data_size;
    const auto* origin = member_scopes_.empty() ? origin_ : member_scopes_.back().origin;
    return (data_size - ((buffer_.data() - origin) % data_size)) & (data_size - 1);
  }
  size_t align64_ = 8;
};

//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------

/**
 * @brief Encoder is a class that encodes data into a buffer.
 */
class Encoder
{
public:
  // Use this constructor if you want the encoder to use its own internal buffer.
  // You can copy it later using encodedBuffer()
  Encoder(CdrHeader header) : Encoder(header, default_storage_)
  {
  }

  // Use this constructor if you alredy have a buffer to encode into
  Encoder(CdrHeader header, std::vector<uint8_t>& storage);

  const CdrHeader& header() const
  {
    return header_;
  }

  /**
   * @brief Encode a single value into the buffer.
   *
   * @tparam T The type of the value to encode.
   * @param in The value to encode.
   */
  template <typename T>
  void encode(const T& in);

  // specializations for std::string
  void encode(const std::string& in);

  // specializations for std::vector
  template <typename T, typename Allocator>
  void encode(const std::vector<T, Allocator>& in);

  // specialization for std::array
  template <typename T, size_t N>
  void encode(const std::array<T, N>& in);

  // Get a view to the current buffer (bytes already encoded)
  ConstBuffer encodedBuffer() const
  {
    return ConstBuffer(storage_->data(), storage_->size());
  }

private:
  CdrHeader header_;
  std::vector<uint8_t> default_storage_;
  std::vector<uint8_t>* storage_ = nullptr;
  const uint8_t* origin_ = nullptr;
  size_t align64_ = 8;

  size_t alignment(size_t data_size) const
  {
    data_size = (data_size == 8) ? align64_ : data_size;
    return (data_size - ((storage_->size() - 4) % data_size)) & (data_size - 1);
  }
};

//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------
// Implementation
//----------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------

template <typename T>
constexpr bool is_arithmetic()
{
  return std::is_arithmetic_v<T> || std::is_same_v<T, std::byte> ||
         std::is_same_v<T, char>;
}

template <typename T, class = void>
struct is_type_defined : std::false_type
{
};

template <typename T>
struct is_type_defined<T, decltype(TypeDefinition<T>(), void())> : std::true_type
{
};

template <typename T>
constexpr bool is_type_defined_v()
{
  return is_type_defined<T>::value;
}

inline Endianness getCurrentEndianness()
{
  union
  {
    uint8_t u8;
    uint16_t u16 = 0x0100;
  } endian_test = {};
  endian_test.u16 = 0x0100;
  return endian_test.u8 == 0x01 ? Endianness::CDR_BIG_ENDIAN :
                                  Endianness::CDR_LITTLE_ENDIAN;
}

template <typename T>
inline void swapEndianness(T& val)
{
  static_assert(is_arithmetic<T>(), "swapEndianness: T must be an arithmetic type");
  if constexpr (sizeof(T) == 2)
  {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(&val);
    std::swap(ptr[0], ptr[1]);
  }
  else if constexpr (sizeof(T) == 4)
  {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(&val);
    std::swap(ptr[0], ptr[3]);
    std::swap(ptr[1], ptr[2]);
  }
  else if constexpr (sizeof(T) == 8)
  {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(&val);
    std::swap(ptr[0], ptr[7]);
    std::swap(ptr[1], ptr[6]);
    std::swap(ptr[2], ptr[5]);
    std::swap(ptr[3], ptr[4]);
  }
}

inline Decoder::Decoder(ConstBuffer buffer, CdrVersion default_cdr,
                       bool align_from_message_start)
  : buffer_(buffer), origin_(buffer.data() + (align_from_message_start ? 0 : 4))
{
  const auto* ptr = buffer_.data();
  uint8_t dummy = ptr[0];
  if (dummy != 0)
  {
    throw std::runtime_error("Invalid CDR header: expected first byte to be 0");
  }

  const uint8_t encapsulation = ptr[1];
  header_.endianness = static_cast<Endianness>(encapsulation & 0x1);
  header_.encoding =
      static_cast<EncodingFlag>(encapsulation & static_cast<uint8_t>(~0x1));
  header_.version = default_cdr;

  switch (header_.encoding)
  {
    case EncodingFlag::PLAIN_CDR2:
    case EncodingFlag::DELIMIT_CDR2:
    case EncodingFlag::PL_CDR2:
      if (CdrVersion::XCDRv1 <= header_.version)
      {
        header_.version = CdrVersion::XCDRv2;
      }
      else
      {
        throw std::runtime_error("Unexpected encoding received.");
      }
      break;
    case EncodingFlag::PL_CDR:
      if (CdrVersion::XCDRv1 <= header_.version)
      {
        header_.version = CdrVersion::XCDRv1;
      }
      else
      {
        throw std::runtime_error("Unexpected encoding received.");
      }
      break;
    case EncodingFlag::PLAIN_CDR:
      if (CdrVersion::XCDRv1 <= header_.version)
      {
        header_.version = CdrVersion::XCDRv1;
      }
      break;
    default:
      throw std::runtime_error("Unexpected encoding received.");
  }
  align64_ = (header_.version == CdrVersion::XCDRv2) ? 4 : 8;

  // Note: origin_ determines alignment calculation:
  // - XCDR2 standard / FastDDS / ROS2: aligns from byte 4 (after CDR header) [default, align_from_message_start=false]
  // - Legacy CDR / RTI DDS Micro: aligns from byte 0 (message start) [align_from_message_start=true]
  // Modern XCDR2 specification clarifies byte 4 as correct; byte 0 remains for backward compatibility.

  buffer_.trim_front(4);  // Remove the header from the buffer
}

template <typename T, typename Allocator>
inline void Decoder::decode(std::vector<T, Allocator>& out)
{
  uint32_t len = 0;
  decode(len);
  out.resize(len);
  for (uint32_t i = 0; i < len; i++)
  {
    decode(out[i]);
  }
}

template <typename T, size_t N>
inline void Decoder::decode(std::array<T, N>& out)
{
  for (uint32_t i = 0; i < out.size(); i++)
  {
    decode(out[i]);
  }
}

inline void Decoder::decode(std::string& out)
{
  uint32_t len = 0;
  decode(len);
  if (buffer_.size() < len)
  {
    throw std::runtime_error("Decode: not enough data to decode (string). Size: " +
                             std::to_string(len));
  }

  auto* str_ptr = buffer_.data();
  buffer_.trim_front(len);

  // check if last character is a null terminator
  auto str_len = len;
  if (len > 0 && str_ptr[len - 1] == '\0')
  {
    str_len--;
  }

  out.resize(str_len);
  if (str_len > 0)
  {
    memcpy(out.data(), str_ptr, str_len);
  }
}

template <typename T>
inline void Decoder::decode(T& out)
{
  static_assert(is_arithmetic<T>() || is_type_defined_v<T>(), "decode: T must be an "
                                                              "arithmetic type or a "
                                                              "defined type");

  if constexpr (is_arithmetic<T>())
  {
    const size_t align = alignment(sizeof(T));
    if (align > 0)
    {
      buffer_.trim_front(align);
    }

    if (buffer_.size() < sizeof(T))
    {
      throw std::runtime_error("Decode: not enough data to decode");
    }
    memcpy(&out, buffer_.data(), sizeof(T));

    if constexpr (sizeof(T) >= 2)
    {
      if (header_.endianness != getCurrentEndianness())
      {
        swapEndianness(out);
      }
    }
    buffer_.trim_front(sizeof(T));
    return;
  }
  if constexpr (is_type_defined_v<T>())
  {
    auto op = [this](auto& obj) { decode(obj); };
    TypeDefinition<T>().operator()(out, op);
  }
}

inline Encoder::Encoder(CdrHeader header, std::vector<uint8_t>& storage)
  : header_(header), storage_(&storage)
{
  storage_->clear();
  storage_->reserve(1024);
  storage_->resize(4);
  align64_ = (header_.version == CdrVersion::XCDRv2) ? 4 : 8;
  origin_ = storage_->data() + 4;

  storage[0] = 0;  // First byte is always 0 in CDR
  storage[1] =
      static_cast<uint8_t>(header.encoding) | static_cast<uint8_t>(header.endianness);
  storage[2] = 0;  // Options (usually 0)
  storage[3] = 0;  // Reserved (usually 0)
}

template <typename T>
inline void Encoder::encode(const T& in)
{
  static_assert(is_arithmetic<T>() || is_type_defined_v<T>(), "encode: T must be an "
                                                              "arithmetic type or a "
                                                              "defined type");
  if constexpr (is_arithmetic<T>())
  {
    if constexpr (sizeof(T) >= 2)
    {
      const auto align = alignment(sizeof(T));
      if (align > 0)
      {
        storage_->resize(storage_->size() + align);
      }
    }
    const auto prev_size = storage_->size();
    storage_->resize(prev_size + sizeof(T));
    if constexpr (sizeof(T) >= 2)
    {
      if (header_.endianness != getCurrentEndianness())
      {
        T tmp = in;
        swapEndianness(tmp);
        memcpy(storage_->data() + prev_size, &tmp, sizeof(T));
        return;
      }
    }
    memcpy(storage_->data() + prev_size, &in, sizeof(T));
    return;
  }
  if constexpr (is_type_defined_v<T>())
  {
    auto op = [this](auto& obj) { encode(obj); };
    TypeDefinition<T>().operator()(const_cast<T&>(in), op);
  }
}

inline void Encoder::encode(const std::string& in)
{
  const uint32_t str_len = in.size();
  encode(str_len);
  const auto prev_size = storage_->size();
  storage_->resize(prev_size + str_len);
  memcpy(storage_->data() + prev_size, in.data(), str_len);
}

template <typename T, typename Allocator>
inline void Encoder::encode(const std::vector<T, Allocator>& in)
{
  const uint32_t len = in.size();
  encode(len);
  for (const auto& item : in)
  {
    encode(item);
  }
}

template <typename T, size_t N>
inline void Encoder::encode(const std::array<T, N>& in)
{
  for (const auto& item : in)
  {
    encode(item);
  }
}

}  // namespace nanocdr
