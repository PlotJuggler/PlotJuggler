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

  /// Gives back the capacity past size(): for a buffer that grew by appends and will not grow again.
  void shrinkToFit();

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

  /// setNull() over [begin, end), whole bytes at a time. Same bounds rule.
  void setNullRange(std::size_t begin, std::size_t end) {
    fillRange(begin, end, false);
  }

  /// setValid() over [begin, end), whole bytes at a time. Same bounds rule.
  void setValidRange(std::size_t begin, std::size_t end) {
    fillRange(begin, end, true);
  }

  /// Gives back the capacity past the bits held.
  void shrinkToFit();

  // Inline: per-cell loops in other translation units call it.
  [[nodiscard]] bool isValid(std::size_t bit_index) const {
    return (bytes_[bit_index / 8] & (1u << (bit_index % 8))) != 0;
  }

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
  void fillRange(std::size_t begin, std::size_t end, bool valid);

  std::vector<uint8_t> bytes_;
  std::size_t bit_count_ = 0;
};

}  // namespace PJ
