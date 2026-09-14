// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <thread>

namespace PJ {
namespace {

/// Intentionally faulty; never "fix" these writes, because their data race proves TSan instrumentation is active.
void WriteRacyValue(volatile int* value, const std::atomic<bool>* start) {
  while (!start->load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Volatile preserves every non-atomic store without synchronizing the threads; the barrier escapes the address.
  asm volatile("" : : "r"(value) : "memory");
  for (int iteration = 0; iteration < 100000; ++iteration) {
    *value = iteration;
  }
}

}  // namespace
}  // namespace PJ

/// Runs an intentionally faulty race; never "fix" it, because a clean run must fail the CTest guard.
int main() {
  int value = 0;
  std::atomic<bool> start{false};
  std::thread first(PJ::WriteRacyValue, &value, &start);
  std::thread second(PJ::WriteRacyValue, &value, &start);
  // The gate encourages overlap without establishing an order between the racy stores.
  start.store(true, std::memory_order_release);
  first.join();
  second.join();

  // CTest passes only when TSan printed a race report; this clean exit rejects missing instrumentation.
  return 0;
}
