// SPDX-License-Identifier: MPL-2.0

// Compiled into every pj_add_test_runner runner (outside sanitizer lanes): a crashing test prints a symbolized stack
// trace to stderr, which ctest shows under --output-on-failure, instead of a bare "SegFault". A source of its own
// rather than a static library: the linker would drop the unreferenced global from an archive.

#include <backward.hpp>

namespace {
backward::SignalHandling g_crash_handler;
}  // namespace
