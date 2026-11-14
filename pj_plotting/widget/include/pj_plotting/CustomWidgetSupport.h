#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>

#include "pj_base/builtin/builtin_object.hpp"

namespace PJ {

/// What the app's Custom Widgets cover, as a dock needs it; handed down tab widget -> docker -> dock.
/// `available`: some Custom Widget type is installed (the placeholder's Custom Widget icon). `shows_only`:
/// the object types only a Custom Widget shows; a dock without a mounted widget leaves those out of a drop
/// and says to create one first. Empty `shows_only`: no such type.
struct CustomWidgetSupport {
  bool available = true;
  std::function<bool(sdk::BuiltinObjectType)> shows_only;
};

}  // namespace PJ
