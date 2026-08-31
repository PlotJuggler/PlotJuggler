#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file sha256.hpp
 * @brief Small streaming SHA-256 implementation shared by marketplace admission.
 */

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"

namespace PJ {

/// Incremental SHA-256 state. Instances are single-use after `finishHex()`.
class Sha256 {
 public:
  /// Add arbitrary bytes to the digest.
  void update(const uint8_t* data, size_t size) noexcept {
    for (size_t index = 0; index < size; ++index) {
      buffer_[buffer_size_++] = data[index];
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_.data());
        bit_count_ += 512;
        buffer_size_ = 0;
      }
    }
  }

  /// Add string bytes without encoding transformation.
  void update(std::string_view bytes) noexcept {
    update(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  }

  /// Finalize and return the lower-case 64-character digest.
  [[nodiscard]] std::string finishHex() noexcept {
    const uint64_t total_bits = bit_count_ + static_cast<uint64_t>(buffer_size_) * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      while (buffer_size_ < buffer_.size()) {
        buffer_[buffer_size_++] = 0;
      }
      transform(buffer_.data());
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56U) {
      buffer_[buffer_size_++] = 0;
    }
    for (size_t index = 0; index < 8U; ++index) {
      buffer_[63U - index] = static_cast<uint8_t>(total_bits >> (index * 8U));
    }
    transform(buffer_.data());

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint32_t word : state_) {
      output << std::setw(8) << word;
    }
    return output.str();
  }

 private:
  static constexpr std::array<uint32_t, 64> kRoundConstants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
  };

  static uint32_t choose(uint32_t first, uint32_t second, uint32_t third) noexcept {
    return (first & second) ^ (~first & third);
  }

  static uint32_t majority(uint32_t first, uint32_t second, uint32_t third) noexcept {
    return (first & second) ^ (first & third) ^ (second & third);
  }

  static uint32_t bigSigma0(uint32_t value) noexcept {
    return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
  }

  static uint32_t bigSigma1(uint32_t value) noexcept {
    return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
  }

  static uint32_t smallSigma0(uint32_t value) noexcept {
    return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
  }

  static uint32_t smallSigma1(uint32_t value) noexcept {
    return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
  }

  void transform(const uint8_t* block) noexcept {
    std::array<uint32_t, 64> words{};
    for (size_t index = 0; index < 16U; ++index) {
      const size_t offset = index * 4U;
      words[index] = (static_cast<uint32_t>(block[offset]) << 24U) |
                     (static_cast<uint32_t>(block[offset + 1U]) << 16U) |
                     (static_cast<uint32_t>(block[offset + 2U]) << 8U) | static_cast<uint32_t>(block[offset + 3U]);
    }
    for (size_t index = 16U; index < words.size(); ++index) {
      words[index] =
          smallSigma1(words[index - 2U]) + words[index - 7U] + smallSigma0(words[index - 15U]) + words[index - 16U];
    }

    uint32_t first = state_[0];
    uint32_t second = state_[1];
    uint32_t third = state_[2];
    uint32_t fourth = state_[3];
    uint32_t fifth = state_[4];
    uint32_t sixth = state_[5];
    uint32_t seventh = state_[6];
    uint32_t eighth = state_[7];
    for (size_t index = 0; index < words.size(); ++index) {
      const uint32_t temporary1 =
          eighth + bigSigma1(fifth) + choose(fifth, sixth, seventh) + kRoundConstants[index] + words[index];
      const uint32_t temporary2 = bigSigma0(first) + majority(first, second, third);
      eighth = seventh;
      seventh = sixth;
      sixth = fifth;
      fifth = fourth + temporary1;
      fourth = third;
      third = second;
      second = first;
      first = temporary1 + temporary2;
    }
    state_[0] += first;
    state_[1] += second;
    state_[2] += third;
    state_[3] += fourth;
    state_[4] += fifth;
    state_[5] += sixth;
    state_[6] += seventh;
    state_[7] += eighth;
  }

  std::array<uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<uint8_t, 64> buffer_{};
  size_t buffer_size_ = 0;
  uint64_t bit_count_ = 0;
};

/// Hash in-memory bytes and return lower-case hexadecimal SHA-256.
[[nodiscard]] inline std::string sha256Hex(std::string_view bytes) noexcept {
  Sha256 hash;
  hash.update(bytes);
  return hash.finishHex();
}

/// Stream and hash one regular file without loading it into memory.
[[nodiscard]] inline Expected<std::string> sha256File(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return unexpected("cannot open file for SHA-256: " + path.string());
  }
  Sha256 hash;
  std::array<char, 64U * 1024U> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hash.update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(count));
    }
  }
  if (!input.eof()) {
    return unexpected("failed while reading file for SHA-256: " + path.string());
  }
  return hash.finishHex();
}

}  // namespace PJ
