// SPDX-License-Identifier: MIT

#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace tessera::harness {

// Version reported by `tessera --version`. The harness slice ships ahead of the project version
// so a script can pin the protocol it was written against.
inline constexpr std::string_view kHarnessCliVersion = "0.1.0";

[[nodiscard]] std::string_view cli_usage() noexcept;

// Runs one `tessera` invocation. `args` excludes argv[0]. Returns the documented section 3 exit
// codes: 0 pass/help/version, 1 assertion or audit failure, 2 invalid script/config. The skeleton
// parses the documented flags and rejects `--script` with a diagnostic until the script runner
// lands; it never touches a runtime.
[[nodiscard]] int run_cli(std::span<const std::string_view> args, std::ostream& out,
                          std::ostream& err);

} // namespace tessera::harness
