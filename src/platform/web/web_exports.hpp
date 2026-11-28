// SPDX-License-Identifier: MIT

#pragma once

#include "web_host.hpp"

// The Emscripten-only seam of the web HAL: the `extern "C"` entry points the host page calls and
// the two JS imports the host calls back. Built only under Emscripten (see CMakeLists.txt);
// everything it forwards to is plain C++ covered by the host unit tests.

namespace tessera::hal::web {

// Callbacks bound to the page imports `tessera_js_request_frame` and `tessera_js_wake_after`.
[[nodiscard]] WebHostCallbacks js_callbacks();

// Registers the host the exported entry points forward to. The app owns the host; pass nullptr
// before destroying it.
void attach_global_host(WebHost* host);

} // namespace tessera::hal::web
