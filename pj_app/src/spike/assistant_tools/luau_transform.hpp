// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// The Luau recipe text generators live in the shared derived_recipes library (also used by the
// Transform Editor); re-exported here so the assistant keeps its names.
#include "derived_recipes/recipes.hpp"

namespace assistant_agent {

using derived_recipes::buildOnDemandChunk;
using derived_recipes::luaStringEscape;

}  // namespace assistant_agent
