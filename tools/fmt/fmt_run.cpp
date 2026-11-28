// SPDX-License-Identifier: MIT

#include "fmt_run.hpp"
#include "tessera/compiler/format/edits.hpp"
#include "tessera/compiler/format/format.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>

namespace tessera::fmt_cli {
namespace {

namespace fs = std::filesystem;
namespace format = tessera::compiler::format;

constexpr std::string_view kUsage =
    "usage: tessera fmt [--check] [paths...]\n"
    "\n"
    "  Formats .tsx, .css and tessera.json files with the compiler-owned printer (ADR-061). A path "
    "is a file or a\n"
    "  directory (walked for .tsx, .css and tessera.json, skipping node_modules and dot "
    "directories); with no path\n"
    "  the current directory is used.\n"
    "\n"
    "  --check   change nothing; list the files that would change and exit 1 if there are any\n"
    "  --recover   format a .tsx file around the top-level items that do not parse, keeping each\n"
    "            broken item byte for byte (reported on stderr), instead of refusing the file\n"
    "  --edits-json FILE   print the minimal edit list for one file as JSON (UTF-16 offsets,\n"
    "            the form the studio format worker answers with); changes nothing\n"
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

// The options of the nearest tessera.json at or above the file's directory (ADR-061: `format`
// in tessera.json), defaults when there is none. Cached per directory.
format::FormatOptions options_for(const fs::path& file,
                                  std::map<fs::path, format::FormatOptions>& cache) {
    std::error_code ec;
    fs::path dir = fs::absolute(file, ec).parent_path();
    std::vector<fs::path> walked;
    format::FormatOptions found;
    for (; !dir.empty(); dir = dir.parent_path()) {
        if (const auto hit = cache.find(dir); hit != cache.end()) {
            found = hit->second;
            break;
        }
        walked.push_back(dir);
        std::string text;
        if (read_file(dir / "tessera.json", text)) {
            found = format::format_options_from_tessera_json(text);
            break;
        }
        if (dir == dir.parent_path()) {
            break;
        }
    }
    for (const fs::path& visited : walked) {
        cache[visited] = found;
    }
    return found;
}

format::FormatOptions with_recover(format::FormatOptions options, bool recover) {
    options.recover = recover;
    return options;
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
        } else if (it->is_regular_file(ec) &&
                   format::file_kind_for_path(it->path().generic_string()).has_value()) {
            files.push_back(it->path());
        }
    }
}

} // namespace

int run_fmt(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    bool check = false;
    const bool recover = std::find(args.begin(), args.end(), "--recover") != args.end();
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--edits-json") {
            std::string text;
            if (i + 1 >= args.size() || !read_file(fs::path(args[i + 1]), text)) {
                err << "--edits-json needs a readable file\n";
                return 2;
            }
            const fs::path target(args[i + 1]);
            std::map<fs::path, format::FormatOptions> cache;
            out << format::format_edits_json(format::file_kind_for_path(target.generic_string())
                                                 .value_or(format::FileKind::Tsx),
                                             text,
                                             with_recover(options_for(target, cache), recover))
                << "\n";
            return 0;
        }
        if (arg == "--check") {
            check = true;
        } else if (arg == "--recover") {
            continue; // read before the loop, so it also applies to --edits-json
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

    std::map<fs::path, format::FormatOptions> options_cache;
    std::size_t differing = 0;
    for (const fs::path& path : files) {
        std::string text;
        if (!read_file(path, text)) {
            err << path.generic_string() << ": cannot read\n";
            failed = true;
            continue;
        }
        const format::FormatResult result = format::format_source(
            format::file_kind_for_path(path.generic_string()).value_or(format::FileKind::Tsx), text,
            with_recover(options_for(path, options_cache), recover));
        if (!result.ok()) {
            for (const format::FormatDiagnostic& d : result.diagnostics) {
                err << path.generic_string() << ":" << d.line << ":" << d.column << ": error["
                    << d.code << "]: " << d.message << "\n";
            }
            failed = true;
            continue;
        }
        for (const format::FormatDiagnostic& d : result.skipped) {
            err << path.generic_string() << ":" << d.line << ":" << d.column << ": warning["
                << d.code << "]: " << d.message << "\n";
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
