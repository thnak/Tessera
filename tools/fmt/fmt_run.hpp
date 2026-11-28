// SPDX-License-Identifier: MIT

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace tessera::fmt_cli {

// `tessera fmt [--check] [paths]`: `args` excludes the program name. Directories are walked for
// `.tsx` files; with no path the current directory is used. Exit codes: 0 formatted or already
// canonical, 1 `--check` found files that differ, 2 a file was refused or could not be read.
int run_fmt(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

} // namespace tessera::fmt_cli
