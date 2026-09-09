// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtest/gtest.h>

#include <string_view>

namespace pj_app_test {

// Shell scenarios exercise widget behavior without requiring a working OpenGL context.
class GuiTestEnvironment : public ::testing::Environment {
 public:
  void SetUp() override;
};

// Every registered environment runs before each ctest case. Scope custom setup
// to its suite so unrelated sources cannot overwrite that case's settings or theme.
[[nodiscard]] bool isTestSuiteSelected(std::string_view suite_name);

}  // namespace pj_app_test
