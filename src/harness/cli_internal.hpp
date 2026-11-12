// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/automation/app_config.hpp"
#include "tessera/harness/cli_frame.hpp"
#include "tessera/harness/latency_audit.hpp"
#include "tessera/harness/script.hpp"
#include "tessera/harness/session.hpp"

#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace tessera::harness::detail {

// The script lane of the CLI (`cli_script.cpp`), split out of `cli.cpp` so argument parsing and
// script execution stay under the file-size limit. Not public API.

void print_script_error(std::ostream& err, std::string_view command, const ScriptError& error);

[[nodiscard]] bool wants_diagnostics(const Script& script);

[[nodiscard]] std::optional<std::string> read_script_file(std::string_view path);

// Runs one parsed script against the built-in fixture or an app manifest. Returns the documented
// section 3 exit code: 0 pass, 1 run failure, 2 configuration/artifact failure.
[[nodiscard]] int run_script(const Script& script, std::string_view command,
                             std::string_view output_directory, const std::string& report_path,
                             bool diagnostics_enabled, const std::optional<LatencyProfile>& latency,
                             SessionClock& audit_clock, const automation::AppManifest* app,
                             const std::optional<CliFrameRequest>& frame_request, std::ostream& out,
                             std::ostream& err);

} // namespace tessera::harness::detail
