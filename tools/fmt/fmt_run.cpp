// SPDX-License-Identifier: MIT

#include "fmt_run.hpp"
#include "tessera/compiler/format/format.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace tessera::fmt_cli {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "usage: tessera fmt [--check] [paths...]\n"
    "\n"
    "  Formats .tsx files with the compiler-owned printer (ADR-061). A path is a file or a\n"
    "  directory (walked for .tsx, skipping node_modules and dot directories); with no path\n"
    "  the current directory is used.\n"
    "\n"
    "  --check   change nothing; list the files that would change and exit 1 if there are any\n"
    "\n"
    "  Exit status: 0 done or already canonical, 1 --check found differences, 2 a file was\n"
    "  refused (syntax error, unplaceable comment) or could not be read.\n";

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool skipped_directory(const fs::path& path) {
    const std::string name = path.filename().string();
    return name == "node_modules" ||
           (!name.empty() && name[0] == '.' && name != "." && name != "..");
}

void collect(const fs::path& root, std::vector<fs::path>& files, std::ostream& err, bool& failed) {
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        files.push_back(root);
        return;
    }
    if (!fs::is_directory(root, ec)) {
        err << root.string() << ": no such file or directory\n";
        failed = true;
        return;
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && skipped_directory(it->path())) {
            it.disable_recursion_pending();
        } else if (it->is_regular_file(ec) && it->path().extension() == ".tsx") {
            files.push_back(it->path());
        }
    }
}

} // namespace

int run_fmt(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    namespace format = tessera::compiler::format;
    bool check = false;
    std::vector<std::string> paths;
    for (const std::string& arg : args) {
        if (arg == "--check") {
            check = true;
        } else if (arg == "--help" || arg == "-h") {
            out << kUsage;
            return 0;
        } else if (arg.starts_with("--")) {
            err << "unknown option " << arg << "\n" << kUsage;
            return 2;
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.empty()) {
        paths.emplace_back(".");
    }
    std::vector<fs::path> files;
    bool failed = false;
    for (const std::string& path : paths) {
        collect(fs::path(path), files, err, failed);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    std::size_t differing = 0;
    for (const fs::path& path : files) {
        std::string text;
        if (!read_file(path, text)) {
            err << path.generic_string() << ": cannot read\n";
            failed = true;
            continue;
        }
        const format::FormatResult result = format::format_source(format::FileKind::Tsx, text);
        if (!result.ok()) {
            for (const format::FormatDiagnostic& d : result.diagnostics) {
                err << path.generic_string() << ":" << d.line << ":" << d.column << ": error["
                    << d.code << "]: " << d.message << "\n";
            }
            failed = true;
            continue;
        }
        if (!result.changed) {
            continue;
        }
        ++differing;
        if (check) {
            out << path.generic_string() << "\n";
            continue;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << result.text;
        if (!file) {
            err << path.generic_string() << ": cannot write\n";
            failed = true;
        }
    }
    if (check && differing > 0) {
        err << differing << " file(s) would be reformatted\n";
    }
    if (failed) {
        return 2;
    }
    return check && differing > 0 ? 1 : 0;
}

} // namespace tessera::fmt_cli
