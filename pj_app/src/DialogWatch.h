#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>

namespace PJ::dialog_presenter {

/// Closes one open dialog as a rejection (the user's Cancel). A no-op once that dialog completed,
/// however late it is called.
using DialogCancelFn = std::function<void()>;

/// Told when a dialog is up and when it is gone: called with the functor that closes it once it is
/// open (before any nested event loop runs), then with an empty functor when it completed. Never
/// called when no dialog opens. For whoever must be able to withdraw the question (an AI agent's
/// stopped call): the closer is that dialog's own, so nothing else on screen is ever touched.
using DialogWatch = std::function<void(DialogCancelFn close)>;

}  // namespace PJ::dialog_presenter
