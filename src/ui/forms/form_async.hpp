// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/forms/form_model.hpp"

#include <functional>

namespace tessera::ui::forms::detail {

// Bridge coroutine: awaits the caller's async validator and reports the boolean result exactly once
// through `done`. It runs on the form's CompletionQueue, so `done` executes on the UI thread during
// a queue drain (never inline from the caller, never on a detached thread). A cancelled token
// prevents the resume, so `done` does not run for superseded validation.
runtime::Task<void> run_async_bridge(runtime::TaskContext context, AsyncValidateFn validator,
                                     FormValue value, std::function<void(bool)> done);

} // namespace tessera::ui::forms::detail
