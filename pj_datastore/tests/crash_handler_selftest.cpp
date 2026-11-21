// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <csignal>

namespace {

// On Linux and macOS every pj_add_test_runner runner installs backward-cpp's handler
// (cmake/test_mains/pj_test_crash_handler.cpp), so a crashing test prints a symbolized stack trace instead of ctest's
// bare "SegFault". Sanitizer lanes and Windows leave it out (cmake/PjTests.cmake says why).
TEST(CrashHandlerDeathTest, ACrashPrintsAStackTrace) {
#if !defined(PJ_TEST_CRASH_HANDLER)
  GTEST_SKIP() << "no crash handler in this lane (sanitizers, Windows)";
#else
  EXPECT_DEATH(std::raise(SIGSEGV), "Stack trace");
#endif
}

}  // namespace
