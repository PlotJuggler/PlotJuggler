// SPDX-License-Identifier: MPL-2.0
#include "gui_test_env.h"

#include "pj_plotting/PlotWidgetBase.h"

namespace pj_app_test {

void GuiTestEnvironment::SetUp() {
  PJ::PlotWidgetBase::setOpenGlDisabledOverride(true);
}

bool isTestSuiteSelected(std::string_view suite_name) {
  const auto* unit_test = ::testing::UnitTest::GetInstance();
  for (int i = 0; i < unit_test->total_test_suite_count(); ++i) {
    const auto* suite = unit_test->GetTestSuite(i);
    if (suite->should_run() && suite_name == suite->name()) {
      return true;
    }
  }
  return false;
}

namespace {
static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new GuiTestEnvironment);
}  // namespace

}  // namespace pj_app_test
