// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/line_transport.hpp"

#include <atomic>
#include <iosfwd>
#include <string_view>

namespace tessera::harness {

// `tessera headless` without --script: builds the built-in windowless fixture host and serves the
// line protocol on `transport` until the input closes or `stop` is set (may be null). One
// RuntimeLoop tick runs per loop iteration on a manual clock, so no simulation state reads the
// wall clock; only the bounded readiness wait uses real time. Returns 0 on a clean end, 2 when
// the host cannot be built.
[[nodiscard]] int run_headless_serve(LineTransport& transport, std::string_view output_directory,
                                     bool diagnostics_enabled, const std::atomic<bool>* stop,
                                     std::ostream& err);

} // namespace tessera::harness
