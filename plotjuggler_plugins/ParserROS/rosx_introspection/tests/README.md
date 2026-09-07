# rosx_introspection Tests

## Building and Running Tests

The tests use Google Test framework. To build and run:

### Install GTest (Ubuntu/Debian)

```bash
sudo apt install libgtest-dev cmake
```

### Configure with Testing Enabled

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
```

### Run All Tests

```bash
cd build
ctest --output-on-failure
```

### Run Specific Test

```bash
# Run CDR alignment tests
./build/plotjuggler_plugins/ParserROS/rosx_introspection/test_cdr_alignment

# Run ROS field tests
./build/plotjuggler_plugins/ParserROS/rosx_introspection/test_ros_field
```

## Test Coverage

### test_ros_field.cpp
Tests the ROS field definition parsing, including various array syntax forms (scalar, fixed, unbounded, bounded).

### test_cdr_alignment.cpp
Tests CDR (Common Data Representation) deserialization with different alignment modes:

1. **XCDR2StandardAlignment**: Tests modern XCDR2 standard (aligns from byte 4 after CDR header)
2. **RTILegacyAlignment**: Tests RTI DDS Micro / legacy CDR (aligns from byte 0)
3. **WrongAlignmentModeDetection**: Verifies that using wrong alignment produces incorrect results
4. **WrongAlignmentModeXCDR2**: Verifies that mismatched alignment throws errors

The test case specifically covers the **48 booleans + double** scenario that caused issues with RTI DDS Micro files:
- XCDR2: double at byte 52 (no padding needed, 48 % 8 = 0 from byte 4)
- RTI: double at byte 56 (4 bytes padding to reach 8-byte boundary from byte 0)

## Why This Test Matters

This test validates the fix for [RTI DDS Micro compatibility issue](https://github.com/facontidavide/PlotJuggler/issues/XXXX):
- RTI DDS Micro aligns CDR data from byte 0 (message start)
- FastDDS/ROS2 aligns from byte 4 (after CDR header)
- Both interpretations exist due to ambiguity in the original CDR spec
- Modern XCDR2 clarifies byte 4 as correct

The test ensures PlotJuggler can correctly decode MCAP files from both alignment modes via the user-configurable setting in the MCAP load dialog.
