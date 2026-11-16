// SPDX-License-Identifier: MIT

// The privacy pass of docs/specs/crash-reporting-v1.md section 5 ("redact user paths, window
// titles, clipboard, tokens, document content").
//
// Three rules, applied line by line so a value cannot drag its key out of shape:
//
// 1. Any user-home prefix (`C:\Users\name`, `/home/name`, `/Users/name`) becomes `<home>`, reusing
//    the logger's `redact_paths` so there is one definition of "a home path".
// 2. A path-like value (`C:\...`, `\\server\...`, `/...`) is reduced to its last component. The
//    crash writer already stores basenames; this is the second line of defence for a record that
//    was hand-edited, produced by an older build, or enriched with a path by a symbolizer.
// 3. An environment-assignment shape - `NAME=value` with an upper-case, underscore-heavy NAME that
//    is not one of the record's own keys - is dropped. A crash report captures metadata; it never
//    captures the environment, and a stray `getenv` value must not ride along.

#include "tessera/diagnostics/crash_report.hpp"
#include "tessera/diagnostics/log.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::diagnostics {

namespace {

constexpr std::string_view kOwnKeys[] = {
    "reason", "code", "thread", "address", "app",    "commit", "build",     "platform",
    "arch",   "os",   "gpu",    "config",  "module", "frame",  "unmatched", "truncated"};

bool is_own_key(std::string_view key) {
    for (const std::string_view own : kOwnKeys) {
        if (key == own) {
            return true;
        }
    }
    return false;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// A value that names a filesystem location rather than a plain word.
bool looks_absolute_path(std::string_view value) {
    if (value.size() >= 3 && value[1] == ':' && (value[2] == '\\' || value[2] == '/')) {
        return true; // C:\ or C:/
    }
    if (starts_with(value, "\\\\")) {
        return true; // UNC
    }
    return value.size() > 1 && value[0] == '/';
}

// An environment variable name: upper case, digits and underscores, at least one letter, no lower
// case. Deliberately conservative - `PATH`, `HOME`, `AWS_SECRET_ACCESS_KEY` - so a normal
// `key=value` record line is never mistaken for one.
bool looks_environment_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    bool has_letter = false;
    for (const char byte : name) {
        if (byte >= 'A' && byte <= 'Z') {
            has_letter = true;
            continue;
        }
        if (byte >= '0' && byte <= '9') {
            continue;
        }
        if (byte == '_') {
            continue;
        }
        return false;
    }
    return has_letter;
}

} // namespace

std::string redact_report(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t line_end = text.find('\n', cursor);
        const std::string_view raw =
            text.substr(cursor, line_end == std::string_view::npos ? std::string_view::npos
                                                                   : line_end - cursor);
        cursor = line_end == std::string_view::npos ? text.size() : line_end + 1;
        const std::size_t equals = raw.find('=');
        if (equals != std::string_view::npos && equals > 0 && !is_own_key(raw.substr(0, equals)) &&
            looks_environment_name(raw.substr(0, equals))) {
            out += "# env-dropped\n";
            continue;
        }
        std::string line = redact_paths(raw);
        if (equals != std::string_view::npos && equals + 1 < line.size()) {
            const std::string_view value = std::string_view(line).substr(equals + 1);
            if (looks_absolute_path(value)) {
                line = std::string(line.substr(0, equals + 1));
                line += redact_path(value);
            }
        }
        out += line;
        if (line_end != std::string_view::npos) {
            out += '\n';
        }
    }
    return out;
}

bool contains_unredacted(std::string_view text) {
    // Defined by the pass it gates: text is unredacted exactly when `redact_report` would change
    // it.
    //
    // The previous implementation was five heuristics that had drifted from `redact_report` in both
    // directions -- it reported clean a line carrying `<home>` plus a second home prefix that the
    // pass rewrites (the early-out skipped the whole line), and it had no way to notice a rule the
    // pass later gained. Re-running the pass makes agreement structural rather than maintained.
    //
    // Credentials are deliberately out of scope: `redact_report` does not pattern-match tokens,
    // because a guessed token format is a false-positive machine that would mangle legitimate
    // content. A bare token is therefore not "removable", and this gate must not claim otherwise;
    // keeping secrets out of breadcrumbs is the writer's job (crash-reporting-v1.md section 5).
    return redact_report(text) != text;
}

} // namespace tessera::diagnostics
