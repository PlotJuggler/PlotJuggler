// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Luau sandbox state shared by the filter, marker and object-script engines: the memory cap, the
// instruction-budget watchdog (with an optional cancel poll) and the sandboxed-VM factory.
// Private to src/ and header-only, so it ships with pj_scripting_core's sources (see conanfile.py).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>  // std::free / std::realloc
#include <functional>
#include <limits>

#include "lua.h"
#include "lualib.h"
#include "pj_scripting/sandbox.h"

namespace PJ::scripting::detail {

// Largest script source we will hand to luau_compile (the compiler runs on the host heap, outside
// the VM allocator/watchdog — bound it by source size).
constexpr std::size_t kMaxSourceBytes = 1u << 20;  // 1 MiB

struct BudgetState {
  std::size_t used = 0;
  std::size_t max_bytes = 0;
  std::int64_t fuel = 0;
  const std::function<bool()>* cancelled = nullptr;  ///< polled at every safepoint; null = never cancelled
  /// First reason the run must fail (cancel, fuel, a host-side ceiling), recorded even if the script's
  /// pcall swallowed the error, so the host checks it after every protected call. `armFuel` clears it.
  const char* trip = nullptr;
};

// Lua allocator with a hard peak-bytes ceiling, overflow-safe. ptr==null ⇒ new
// (osize is a type tag — ignore it); nsize==0 ⇒ free; else realloc. Returns null
// past the cap, which Luau surfaces as a catchable out-of-memory error.
inline void* budgetAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* b = static_cast<BudgetState*>(ud);
  if (nsize == 0) {
    std::free(ptr);
    if (ptr != nullptr) {
      b->used = (osize <= b->used) ? b->used - osize : 0;  // guard underflow
    }
    return nullptr;
  }
  const std::size_t base = (ptr == nullptr) ? b->used : ((osize <= b->used) ? b->used - osize : 0);
  if (nsize > b->max_bytes || base > b->max_bytes - nsize) {  // base+nsize>max, no overflow
    return nullptr;
  }
  void* np = std::realloc(ptr, nsize);
  if (np != nullptr) {
    b->used = base + nsize;
  }
  return np;
}

// Fails the run for `reason`. The first reason sticks and the host checks it after the protected
// call, so a script's pcall cannot turn the failure into a truncated success.
[[noreturn]] inline void tripRun(lua_State* L, BudgetState& budget, const char* reason) {
  if (budget.trip == nullptr) {
    budget.trip = reason;
  }
  luaL_error(L, "%s", budget.trip);
}

// Safepoint callback: polls the cancel check, then spends one fuel unit and aborts the script when
// exhausted. The error is catchable by a script `pcall`, so `trip` is the authoritative signal the
// host checks after every protected call. gc>=0 marks a GC interrupt — skip it.
inline void budgetInterrupt(lua_State* L, int gc) {
  if (gc >= 0) {
    return;
  }
  auto* b = static_cast<BudgetState*>(lua_callbacks(L)->userdata);
  if (b == nullptr) {
    return;
  }
  if (b->cancelled != nullptr && (*b->cancelled)()) {
    tripRun(L, *b, "script cancelled");
  }
  if (--b->fuel <= 0) {
    tripRun(L, *b, "script exceeded its instruction budget");
  }
}

inline void armFuel(BudgetState* b, std::uint64_t fuel) {
  b->trip = nullptr;
  constexpr std::uint64_t kCap = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  b->fuel = static_cast<std::int64_t>(fuel > kCap ? kCap : fuel);
}

// Create a sandboxed VM: curated stdlib, os/coroutine/debug/setfenv/getfenv
// dropped, the shared stdlib frozen read-only (luaL_sandbox), then a fresh
// per-VM writable global table layered on top (luaL_sandboxthread). Net effect: a
// filter can assign its own top-level globals for persistent state, but cannot
// mutate the stdlib or leak globals into a sibling filter's VM. Also installs a
// memory cap + watchdog. `budget` must outlive the returned state. The byte cap is
// left OPEN during trusted setup (openlibs/sandbox) and clamped to limits.mem_bytes
// only once setup is done, so a small cap can never make setup fail/abort.
inline lua_State* newSandboxedState(BudgetState* budget, const BudgetLimits& limits) {
  budget->used = 0;
  budget->max_bytes = std::numeric_limits<std::size_t>::max();  // no cap during setup
  armFuel(budget, limits.setup_fuel);
  lua_State* L = lua_newstate(budgetAlloc, budget);
  if (L == nullptr) {
    return nullptr;
  }
  luaL_openlibs(L);
  // Drop capabilities filters never need; setfenv/getfenv would let a script swap
  // its environment and escape the per-VM globals table back onto the frozen
  // stdlib (or another scope), defeating the sandbox isolation set up below.
  for (const char* lib : {"os", "coroutine", "debug", "setfenv", "getfenv"}) {
    lua_pushnil(L);
    lua_setglobal(L, lib);
  }
  luaL_sandbox(L);  // freeze the shared stdlib read-only
  // Own writable global table for this VM (frozen stdlib as read-only __index):
  // top-level globals the script assigns land here, stdlib reads fall through.
  // One module loads per VM, so safeenv stays on and imports keep the fast path.
  luaL_sandboxthread(L);
  lua_callbacks(L)->userdata = budget;
  lua_callbacks(L)->interrupt = budgetInterrupt;
  budget->max_bytes = limits.mem_bytes;  // cap now applies to all untrusted execution
  return L;
}

}  // namespace PJ::scripting::detail
