// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/buffer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace PJ {
namespace {
TEST(RawBufferTest, DefaultConstructEmpty) {
  RawBuffer buf;
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0u);
}

TEST(RawBufferTest, ConstructWithCapacity) {
  constexpr std::size_t kCap = 256;
  RawBuffer buf(kCap);
  EXPECT_TRUE(buf.empty());
  EXPECT_GE(buf.capacity(), kCap);
}

TEST(RawBufferTest, AppendData) {
  RawBuffer buf;
  const std::array<uint8_t, 4> payload = {0xDE, 0xAD, 0xBE, 0xEF};
  buf.append(payload.data(), payload.size());

  EXPECT_EQ(buf.size(), 4u);
  EXPECT_FALSE(buf.empty());
  EXPECT_EQ(buf.data()[0], 0xDE);
  EXPECT_EQ(buf.data()[1], 0xAD);
  EXPECT_EQ(buf.data()[2], 0xBE);
  EXPECT_EQ(buf.data()[3], 0xEF);
}

TEST(RawBufferTest, AppendMultipleTimes) {
  RawBuffer buf;
  const std::array<uint8_t, 3> first = {1, 2, 3};
  const std::array<uint8_t, 5> second = {4, 5, 6, 7, 8};

  buf.append(first.data(), first.size());
  buf.append(second.data(), second.size());

  EXPECT_EQ(buf.size(), 8u);
  for (uint8_t i = 0; i < 8; ++i) {
    EXPECT_EQ(buf.data()[i], i + 1);
  }
}

TEST(RawBufferTest, Resize) {
  RawBuffer buf;
  buf.resize(16);
  EXPECT_EQ(buf.size(), 16u);
  EXPECT_FALSE(buf.empty());
}

TEST(RawBufferTest, Clear) {
  RawBuffer buf;
  const std::array<uint8_t, 4> payload = {1, 2, 3, 4};
  buf.append(payload.data(), payload.size());
  EXPECT_FALSE(buf.empty());

  buf.clear();
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0u);
}

TEST(RawBufferTest, Reserve) {
  RawBuffer buf;
  buf.reserve(1000);
  EXPECT_GE(buf.capacity(), 1000u);
  EXPECT_EQ(buf.size(), 0u);
  EXPECT_TRUE(buf.empty());
}

TEST(BitVectorTest, BytesForBits) {
  EXPECT_EQ(BitVector::bytesForBits(0), 0u);
  EXPECT_EQ(BitVector::bytesForBits(1), 1u);
  EXPECT_EQ(BitVector::bytesForBits(7), 1u);
  EXPECT_EQ(BitVector::bytesForBits(8), 1u);
  EXPECT_EQ(BitVector::bytesForBits(9), 2u);
  EXPECT_EQ(BitVector::bytesForBits(16), 2u);
  EXPECT_EQ(BitVector::bytesForBits(17), 3u);
}

TEST(BitVectorTest, InitAllValid) {
  BitVector bits;
  bits.initValid(16);

  EXPECT_EQ(bits.sizeBytes(), 2u);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_TRUE(bits.isValid(i)) << "bit " << i << " should be valid after init";
  }
}

TEST(BitVectorTest, SetNull) {
  BitVector bits;
  bits.initValid(16);

  bits.setNull(5);
  EXPECT_FALSE(bits.isValid(5));

  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 5) {
      continue;
    }
    EXPECT_TRUE(bits.isValid(i)) << "bit " << i << " should still be valid";
  }
}

TEST(BitVectorTest, SetValidAfterNull) {
  BitVector bits;
  bits.initValid(16);

  bits.setNull(5);
  EXPECT_FALSE(bits.isValid(5));

  bits.setValid(5);
  EXPECT_TRUE(bits.isValid(5));
}

TEST(BitVectorTest, CountNulls) {
  BitVector bits;
  bits.initValid(16);

  bits.setNull(3);
  bits.setNull(7);
  bits.setNull(15);

  EXPECT_EQ(bits.countNulls(16), 3u);
}

TEST(BitVectorTest, ByteBoundary) {
  BitVector bits;
  bits.initValid(16);

  // Bit 7 is the last bit in byte 0; bit 8 is the first bit in byte 1.
  bits.setNull(7);
  bits.setNull(8);

  EXPECT_FALSE(bits.isValid(7));
  EXPECT_FALSE(bits.isValid(8));

  EXPECT_TRUE(bits.isValid(6));
  EXPECT_TRUE(bits.isValid(9));
}

TEST(BitVectorTest, CountNullsWithNoNulls) {
  BitVector bits;
  bits.initValid(32);
  EXPECT_EQ(bits.countNulls(32), 0u);
}

TEST(BitVectorTest, BitSpanView) {
  BitVector bits;
  bits.initValid(8);
  bits.setNull(1);
  bits.setNull(6);

  const BitSpan view = bits.bitSpan();
  EXPECT_EQ(view.sizeBits(), 8u);
  EXPECT_TRUE(view.test(0));
  EXPECT_FALSE(view.test(1));
  EXPECT_FALSE(view.test(6));
}

}  // namespace
}  // namespace PJ
