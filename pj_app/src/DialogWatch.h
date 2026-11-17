#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>

namespace PJ::dialog_presenter {

/// Rejects one dialog as the user's Cancel would; a no-op once it completed, however late it is called.
using DialogCancelFn = std::function<void()>;

/// Called with the dialog's closer before any nested event loop runs, then with an empty functor once it completed;
/// never when no dialog opens. For a caller that may withdraw the question (an agent's stopped call): the closer
/// reaches only that dialog, nothing else on screen.
using DialogWatch = std::function<void(DialogCancelFn close)>;

}  // namespace PJ::dialog_presenter
