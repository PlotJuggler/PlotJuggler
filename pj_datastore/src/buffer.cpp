// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/buffer.hpp"

#include <bit>
#include <cstring>

namespace PJ {

RawBuffer::RawBuffer(std::size_t initial_capacity) {
  data_.reserve(initial_capacity);
}

void RawBuffer::reserve(std::size_t capacity) {
  data_.reserve(capacity);
}

void RawBuffer::append(const void* data, std::size_t size) {
  const auto* begin = static_cast<const uint8_t*>(data);
  data_.insert(data_.end(), begin, begin + size);
}

void RawBuffer::resize(std::size_t new_size) {
  data_.resize(new_size);
}

void RawBuffer::clear() {
  data_.clear();
}

void RawBuffer::shrinkToFit() {
  if (data_.capacity() > data_.size()) {
    data_.shrink_to_fit();
  }
}

const uint8_t* RawBuffer::data() const noexcept {
  return data_.data();
}

uint8_t* RawBuffer::mutableData() noexcept {
  return data_.data();
}

std::size_t RawBuffer::size() const noexcept {
  return data_.size();
}

std::size_t RawBuffer::capacity() const noexcept {
  return data_.capacity();
}

bool RawBuffer::empty() const noexcept {
  return data_.empty();
}

void BitVector::initValid(std::size_t num_bits) {
  bit_count_ = num_bits;
  bytes_.resize(bytesForBits(num_bits));
  if (!bytes_.empty()) {
    std::memset(bytes_.data(), 0xFF, bytes_.size());
  }
}

void BitVector::ensureSize(std::size_t num_bits) {
  if (num_bits > bit_count_) {
    bit_count_ = num_bits;
  }
  const std::size_t needed = bytesForBits(num_bits);
  if (bytes_.size() < needed) {
    bytes_.resize(needed);
  }
}

void BitVector::setValid(std::size_t bit_index) {
  bytes_[bit_index / 8] |= static_cast<uint8_t>(1u << (bit_index % 8));
}

void BitVector::setNull(std::size_t bit_index) {
  bytes_[bit_index / 8] &= static_cast<uint8_t>(~(1u << (bit_index % 8)));
}

void BitVector::setNullRange(std::size_t begin, std::size_t end) {
  while (begin < end && begin % 8 != 0) {
    setNull(begin++);
  }
  const std::size_t whole_bytes = (end - begin) / 8;
  if (whole_bytes > 0) {
    std::memset(bytes_.data() + begin / 8, 0, whole_bytes);
    begin += whole_bytes * 8;
  }
  while (begin < end) {
    setNull(begin++);
  }
}

void BitVector::setValidRange(std::size_t begin, std::size_t end) {
  while (begin < end && begin % 8 != 0) {
    setValid(begin++);
  }
  const std::size_t whole_bytes = (end - begin) / 8;
  if (whole_bytes > 0) {
    std::memset(bytes_.data() + begin / 8, 0xFF, whole_bytes);
    begin += whole_bytes * 8;
  }
  while (begin < end) {
    setValid(begin++);
  }
}

bool BitVector::isValid(std::size_t bit_index) const {
  return (bytes_[bit_index / 8] & (1u << (bit_index % 8))) != 0;
}

std::size_t BitVector::countNulls(std::size_t num_bits) const {
  const std::size_t num_bytes = bytesForBits(num_bits);
  const uint8_t* ptr = bytes_.data();

  std::size_t total_set_bits = 0;

  const std::size_t full_bytes = num_bits / 8;
  for (std::size_t i = 0; i < full_bytes; ++i) {
    total_set_bits += static_cast<std::size_t>(std::popcount(ptr[i]));
  }

  const std::size_t remaining_bits = num_bits % 8;
  if (remaining_bits > 0 && num_bytes > 0) {
    const uint8_t mask = static_cast<uint8_t>((1u << remaining_bits) - 1u);
    total_set_bits += static_cast<std::size_t>(std::popcount(static_cast<uint8_t>(ptr[full_bytes] & mask)));
  }

  return num_bits - total_set_bits;
}

void BitVector::assignBytes(Span<const uint8_t> bytes, std::size_t bit_count) {
  bytes_.assign(bytes.begin(), bytes.end());
  bit_count_ = bit_count;
}

void BitVector::clear() {
  bytes_.clear();
  bit_count_ = 0;
}

void BitVector::shrinkToFit() {
  bytes_.resize(bytesForBits(bit_count_));
  if (bytes_.capacity() > bytes_.size()) {
    bytes_.shrink_to_fit();
  }
}

PJ::BitSpan BitVector::bitSpan() const noexcept {
  return PJ::BitSpan{PJ::Span<const uint8_t>(bytes_.data(), bytes_.size()), 0, bit_count_};
}

const uint8_t* BitVector::data() const noexcept {
  return bytes_.data();
}

uint8_t* BitVector::mutableData() noexcept {
  return bytes_.data();
}

std::size_t BitVector::sizeBytes() const noexcept {
  return bytes_.size();
}

std::size_t BitVector::sizeBits() const noexcept {
  return bit_count_;
}

bool BitVector::empty() const noexcept {
  return bit_count_ == 0;
}

}  // namespace PJ
