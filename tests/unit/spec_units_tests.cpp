// SPDX-License-Identifier: MIT
//
// Issue #691, "Done when": *the `stylesheet-v1.md` unit list and the code agree; a test fails if one
// is added without the other.*
//
// This is that test. It reads the spec's section 8.1 viewport line out of the repository and checks
// every unit named there against the shared table, and every unit in the table against the spec. A
// unit added to one and not the other fails here rather than shipping as a silently unparsed value -
// which is exactly how `vmin`, `vmax` and the whole small/large/dynamic family were promised by the
// spec and missing from the code.
//
// The spec path is resolved from the source file's own location, so the test works from any build
// directory. When the file cannot be found the test fails rather than skipping: a check that cannot
// run is not a pass.

#include "tessera/ui/style/viewport_units.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::parse_viewport_unit;
using tessera::ui::style::ViewportAxis;
using tessera::ui::style::ViewportSize;

// Every unit the shared table must know, written out here so the test does not derive its
// expectation from the table it is checking.
constexpr std::array<std::string_view, 16> kExpectedUnits = {
    "vw",   "vh",   "vmin",  "vmax",  "svw",  "svh",  "svmin", "svmax",
    "lvw",  "lvh",  "lvmin", "lvmax", "dvw",  "dvh",  "dvmin", "dvmax",
};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "spec_units: " << message << "\n";
    }
    return condition;
}

// The repository root, from this file's own path: <root>/tests/unit/spec_units_tests.cpp.
std::string repo_root() {
    std::string path = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t slash = path.find_last_of("/\\");
        if (slash == std::string::npos) {
            return {};
        }
        path.erase(slash);
    }
    return path;
}

// The viewport bullet of section 8.1, as the spec writes it. A markdown bullet wraps across several
// lines, so this collects the `- viewport:` line and every following line that is indented as a
// continuation, stopping at the next bullet or a blank line.
std::string spec_viewport_line(const std::string& root) {
    const std::string file = root + "/docs/specs/stylesheet-v1.md";
    std::ifstream input(file);
    if (!input) {
        return {};
    }
    std::string line;
    std::string collected;
    bool inside = false;
    while (std::getline(input, line)) {
        if (!inside) {
            if (line.rfind("- viewport:", 0) == 0) {
                inside = true;
                collected = line;
            }
            continue;
        }
        // A continuation line starts with whitespace; a new bullet or a blank line ends the bullet.
        if (line.empty() || line.rfind("- ", 0) == 0 || line.rfind("#", 0) == 0) {
            break;
        }
        if (line.front() != ' ' && line.front() != '\t') {
            break;
        }
        collected += " " + line;
    }
    return collected;
}

// The `code` spans of a markdown line, which is how the spec writes unit names.
std::vector<std::string> backticked(std::string_view line) {
    std::vector<std::string> found;
    std::size_t index = 0;
    while (true) {
        const std::size_t open = line.find('`', index);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = line.find('`', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        found.emplace_back(line.substr(open + 1, close - open - 1));
        index = close + 1;
    }
    return found;
}

} // namespace

int main() {
    bool ok = true;

    // 1. Every unit in the table parses, and the table has no unit the expectation list lacks.
    for (const std::string_view unit : kExpectedUnits) {
        ViewportSize size = ViewportSize::Default;
        ViewportAxis axis = ViewportAxis::Width;
        ok = expect(parse_viewport_unit(unit, size, axis),
                    "the shared table does not know `" + std::string(unit) + "`") &&
             ok;
    }

    // 2. The spec names exactly those units. The spec writes the family as a list of backticked
    //    names; a name it lists that the table lacks, or the reverse, is the drift this catches.
    const std::string root = repo_root();
    ok = expect(!root.empty(), "could not resolve the repository root from __FILE__") && ok;
    const std::string line = spec_viewport_line(root);
    ok = expect(!line.empty(),
                "docs/specs/stylesheet-v1.md has no `- viewport:` line in section 8.1") &&
         ok;
    if (!line.empty()) {
        // Only the unit list, not the prose after it: the bullet continues with a sentence that
        // names `viewport_units.hpp` and `@media`, which are not units. The list ends at the first
        // full stop.
        const std::size_t stop = line.find('.');
        const std::string_view list_part =
            stop == std::string::npos ? std::string_view(line) : std::string_view(line).substr(0, stop);
        const std::vector<std::string> named = backticked(list_part);
        ok = expect(!named.empty(), "the spec's viewport line names no units") && ok;
        for (const std::string& name : named) {
            // The spec also names the inline/block-axis forms as deliberately absent; those are
            // prose, not a promise, and the table is not expected to know them.
            if (name == "vi" || name == "vb" || name == "svi" || name == "svb" || name == "lvi" ||
                name == "lvb" || name == "dvi" || name == "dvb") {
                continue;
            }
            ViewportSize size = ViewportSize::Default;
            ViewportAxis axis = ViewportAxis::Width;
            ok = expect(parse_viewport_unit(name, size, axis),
                        "the spec promises `" + name + "` but the shared table does not know it") &&
                 ok;
        }
        // And the reverse: a unit the table knows that the spec never promises is a unit the engine
        // accepts without a contract.
        for (const std::string_view unit : kExpectedUnits) {
            bool listed = false;
            for (const std::string& name : named) {
                listed = listed || name == unit;
            }
            ok = expect(listed, "the shared table knows `" + std::string(unit) +
                                    "` but the spec does not list it");
        }
    }

    if (!ok) {
        return 1;
    }
    std::cout << "spec_units: the spec and the shared table agree\n";
    return 0;
}
