#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pj_base/span.hpp"

namespace PJ {

/// Growable byte buffer. Growing reallocates and invalidates data() pointers.
class RawBuffer {
 public:
  RawBuffer() = default;

  explicit RawBuffer(std::size_t initial_capacity);

  void reserve(std::size_t capacity);

  void append(const void* data, std::size_t size);

  void resize(std::size_t new_size);

  /// Keeps the capacity.
  void clear();

  [[nodiscard]] const uint8_t* data() const noexcept;

  [[nodiscard]] uint8_t* mutableData() noexcept;

  [[nodiscard]] std::size_t size() const noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept;

  [[nodiscard]] bool empty() const noexcept;

 private:
  std::vector<uint8_t> data_;
};

/// Owning packed validity bitmap (Arrow-compatible LSB-first layout).
class BitVector {
 public:
  BitVector() = default;

  [[nodiscard]] static constexpr std::size_t bytesForBits(std::size_t num_bits) noexcept {
    return (num_bits + 7) / 8;
  }

  void initValid(std::size_t num_bits);

  /// Grows to `num_bits`; added bytes are zero (null), so set each new bit explicitly.
  void ensureSize(std::size_t num_bits);

  /// Unchecked: the bit must lie inside the size set by initValid()/ensureSize(). Same for setNull() and isValid().
  void setValid(std::size_t bit_index);

  void setNull(std::size_t bit_index);

  [[nodiscard]] bool isValid(std::size_t bit_index) const;

  [[nodiscard]] std::size_t countNulls(std::size_t num_bits) const;

  void assignBytes(Span<const uint8_t> bytes, std::size_t bit_count);

  void clear();

  /// Views this vector's bytes: invalid after any change.
  [[nodiscard]] PJ::BitSpan bitSpan() const noexcept;

  [[nodiscard]] const uint8_t* data() const noexcept;

  [[nodiscard]] uint8_t* mutableData() noexcept;

  [[nodiscard]] std::size_t sizeBytes() const noexcept;

  [[nodiscard]] std::size_t sizeBits() const noexcept;

  [[nodiscard]] bool empty() const noexcept;

 private:
  std::vector<uint8_t> bytes_;
  std::size_t bit_count_ = 0;
};

}  // namespace PJ
