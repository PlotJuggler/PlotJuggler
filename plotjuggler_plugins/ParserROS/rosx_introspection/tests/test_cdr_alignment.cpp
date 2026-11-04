#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "rosx_introspection/contrib/nanocdr.hpp"
#include "rosx_introspection/deserializer.hpp"

using namespace RosMsgParser;

namespace {

// Helper to create a CDR message with 48 booleans followed by a double
// aligned according to the specified mode
std::vector<uint8_t> CreateTestMessage(bool align_from_start) {
  std::vector<uint8_t> buffer;

  // CDR header (4 bytes)
  buffer.push_back(0x00);  // Always 0
  buffer.push_back(0x01);  // Little-endian encoding
  buffer.push_back(0x00);  // Options
  buffer.push_back(0x00);  // Reserved

  // 48 booleans (48 bytes)
  // Set pattern: alternating true/false for easy verification
  for (int i = 0; i < 48; i++) {
    buffer.push_back(i % 2);
  }

  // At this point we're at byte 52 (4 header + 48 bools)

  if (align_from_start) {
    // RTI DDS Micro / Legacy CDR: align from byte 0
    // double needs 8-byte alignment, next 8-byte boundary from byte 0 is byte 56
    // Add 4 padding bytes (52 -> 56)
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x00);
    buffer.push_back(0x00);
  }
  // else: XCDR2 / FastDDS: align from byte 4
  // Position 52 is already 8-byte aligned from byte 4 (52-4=48, 48%8=0)
  // No padding needed

  // Double value: 123.456 in little-endian
  double test_value = 123.456;
  const uint8_t* value_bytes = reinterpret_cast<const uint8_t*>(&test_value);
  for (int i = 0; i < 8; i++) {
    buffer.push_back(value_bytes[i]);
  }

  return buffer;
}

}  // namespace

// Test XCDR2 standard alignment (from byte 4, no padding needed)
TEST(CDRAlignment, XCDR2StandardAlignment) {
  // Create message with XCDR2 alignment (no padding)
  auto buffer = CreateTestMessage(false);

  // Expected layout:
  // Bytes 0-3: CDR header
  // Bytes 4-51: 48 booleans
  // Bytes 52-59: double (no padding, already 8-byte aligned from byte 4)
  EXPECT_EQ(buffer.size(), 60u);  // 4 + 48 + 8

  // Create deserializer with byte 4 alignment (XCDR2 standard)
  NanoCDR_Deserializer deserializer;
  deserializer.setAlignFromMessageStart(false);  // Align from byte 4

  Span<const uint8_t> span(buffer.data(), buffer.size());
  deserializer.init(span);

  // Deserialize 48 booleans
  for (int i = 0; i < 48; i++) {
    auto value = deserializer.deserialize(BuiltinType::BOOL);
    EXPECT_EQ(value.convert<bool>(), (i % 2) == 1) << "Boolean " << i;
  }

  // Deserialize double - should work without "not enough data" error
  auto double_value = deserializer.deserialize(BuiltinType::FLOAT64);
  EXPECT_NEAR(double_value.convert<double>(), 123.456, 0.001);
}

// Test RTI DDS Micro / Legacy CDR alignment (from byte 0, needs padding)
TEST(CDRAlignment, RTILegacyAlignment) {
  // Create message with RTI alignment (4 bytes padding)
  auto buffer = CreateTestMessage(true);

  // Expected layout:
  // Bytes 0-3: CDR header
  // Bytes 4-51: 48 booleans
  // Bytes 52-55: 4 padding bytes (to reach 8-byte boundary from byte 0)
  // Bytes 56-63: double
  EXPECT_EQ(buffer.size(), 64u);  // 4 + 48 + 4 + 8

  // Create deserializer with byte 0 alignment (RTI/legacy)
  NanoCDR_Deserializer deserializer;
  deserializer.setAlignFromMessageStart(true);  // Align from byte 0

  Span<const uint8_t> span(buffer.data(), buffer.size());
  deserializer.init(span);

  // Deserialize 48 booleans
  for (int i = 0; i < 48; i++) {
    auto value = deserializer.deserialize(BuiltinType::BOOL);
    EXPECT_EQ(value.convert<bool>(), (i % 2) == 1) << "Boolean " << i;
  }

  // Deserialize double - should work with proper padding
  auto double_value = deserializer.deserialize(BuiltinType::FLOAT64);
  EXPECT_NEAR(double_value.convert<double>(), 123.456, 0.001);
}

// Test that wrong alignment mode causes issues
TEST(CDRAlignment, WrongAlignmentModeDetection) {
  // Create RTI-aligned message (with padding)
  auto buffer = CreateTestMessage(true);

  // Try to deserialize with XCDR2 mode (byte 4 alignment)
  // This should either fail or read wrong data
  NanoCDR_Deserializer deserializer;
  deserializer.setAlignFromMessageStart(false);  // Wrong alignment for this message!

  Span<const uint8_t> span(buffer.data(), buffer.size());
  deserializer.init(span);

  // Deserialize 48 booleans
  for (int i = 0; i < 48; i++) {
    auto value = deserializer.deserialize(BuiltinType::BOOL);
    EXPECT_EQ(value.convert<bool>(), (i % 2) == 1) << "Boolean " << i;
  }

  // Try to deserialize double - will read from wrong position
  // With byte 4 alignment, it expects double at byte 52, but message has padding
  // so the double is actually at byte 56. This will read 4 bytes of padding
  // followed by 4 bytes of the actual double value.
  auto double_value = deserializer.deserialize(BuiltinType::FLOAT64);

  // The value should NOT match the expected value (will be garbage or wrong)
  EXPECT_NE(double_value.convert<double>(), 123.456)
      << "Wrong alignment should produce incorrect value";
}

// Test the opposite - XCDR2 message with RTI alignment mode
TEST(CDRAlignment, WrongAlignmentModeXCDR2) {
  // Create XCDR2-aligned message (no padding)
  auto buffer = CreateTestMessage(false);

  // Try to deserialize with RTI mode (byte 0 alignment)
  NanoCDR_Deserializer deserializer;
  deserializer.setAlignFromMessageStart(true);  // Wrong alignment for this message!

  Span<const uint8_t> span(buffer.data(), buffer.size());
  deserializer.init(span);

  // Deserialize 48 booleans
  for (int i = 0; i < 48; i++) {
    auto value = deserializer.deserialize(BuiltinType::BOOL);
    EXPECT_EQ(value.convert<bool>(), (i % 2) == 1) << "Boolean " << i;
  }

  // Try to deserialize double - will try to align to byte 56 but data is at byte 52
  // This should either fail with "not enough data" or read past the end
  bool threw_exception = false;
  try {
    auto double_value = deserializer.deserialize(BuiltinType::FLOAT64);
    // If it didn't throw, the value should be wrong (reading past intended data)
    EXPECT_NE(double_value.convert<double>(), 123.456)
        << "Wrong alignment should produce incorrect value or throw";
  } catch (const std::exception& e) {
    threw_exception = true;
    // Expected: "not enough data to decode" or similar
    std::string error_msg = e.what();
    EXPECT_TRUE(error_msg.find("not enough data") != std::string::npos)
        << "Expected 'not enough data' error, got: " << error_msg;
  }

  EXPECT_TRUE(threw_exception)
      << "Expected exception when reading past end with wrong alignment";
}
