// SPDX-License-Identifier: MIT
//
// The per-property status table covers the registry (issue #213). `docs/specs/stylesheet-property-
// status-v1.md` is the normative record of what each registered longhand does; this test keeps it
// honest in three ways:
//
//   1. Every longhand `PropertyRegistry::longhand_names()` reports has exactly one row.
//   2. Every row names a registered longhand (no stale row for a removed property).
//   3. Every `applied` row names a test in its evidence column, so a consumer cannot be claimed
//      without evidence.
//
// A property added to the registry without a recorded status therefore fails the test run, which is
// the enforcement behind "every registered property has a real effect". The doc is read from
// TESSERA_DOCS_DIR, so the check runs against the real file rather than a copy.

#include "tessera/ui/style/property_registry.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::PropertyRegistry;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "property_status: " << message << "\n";
    }
    return condition;
}

constexpr const char* kStatusDoc = TESSERA_DOCS_DIR "/specs/stylesheet-property-status-v1.md";

// One row of the status table: the property name and its status word.
struct StatusRow {
    std::string property;
    std::string status;
};

// Splits a markdown table line into its cells (the text between the pipes).
std::vector<std::string> split_cells(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    for (std::size_t i = 1; i < line.size(); ++i) {
        if (line[i] == '|') {
            cells.push_back(cell);
            cell.clear();
        } else {
            cell.push_back(line[i]);
        }
    }
    return cells;
}

// The status word of a cell, or empty when the cell is not a status.
std::string status_of(const std::string& cell) {
    for (const char* word : {"applied", "parsed only", "removed"}) {
        if (cell.find(word) != std::string::npos) {
            return word;
        }
    }
    return {};
}

// Parses the `| `property` | status | ... |` rows. The legend table in section 1 has a status word
// in its first cell, so a row only counts when the *second* cell is a status and the first cell is
// a single backticked property name.
std::vector<StatusRow> read_status_rows(bool& ok) {
    std::vector<StatusRow> rows;
    std::ifstream input(kStatusDoc);
    if (!input) {
        ok = false;
        return rows;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 4 || line.front() != '|') {
            continue;
        }
        const std::vector<std::string> cells = split_cells(line);
        if (cells.size() < 2) {
            continue;
        }
        const std::string status = status_of(cells[1]);
        if (status.empty()) {
            continue;
        }
        const std::string& first = cells[0];
        const std::size_t open = first.find('`');
        if (open == std::string::npos) {
            continue;
        }
        const std::size_t close = first.find('`', open + 1);
        if (close == std::string::npos) {
            continue;
        }
        const std::string property = first.substr(open + 1, close - open - 1);
        if (property.empty() || property.find(' ') != std::string::npos ||
            property.find('*') != std::string::npos) {
            continue;
        }
        rows.push_back(StatusRow{property, status});
    }
    ok = true;
    return rows;
}

bool status_table_covers_the_registry() {
    const PropertyRegistry registry;
    const std::vector<std::string_view> registered = registry.longhand_names();
    if (!expect(!registered.empty(), "the registry has longhands")) {
        return false;
    }

    bool read_ok = false;
    const std::vector<StatusRow> rows = read_status_rows(read_ok);
    if (!expect(read_ok, "the status doc is readable") || !expect(!rows.empty(), "it has rows")) {
        return false;
    }

    std::set<std::string> documented;
    for (const StatusRow& row : rows) {
        if (!expect(documented.insert(row.property).second,
                    "no duplicate row for " + row.property)) {
            return false;
        }
    }

    bool ok = true;
    for (const std::string_view name : registered) {
        const std::string property(name);
        if (!expect(documented.count(property) == 1,
                    "registered property has a status row: " + property)) {
            ok = false;
        }
    }
    for (const StatusRow& row : rows) {
        bool is_registered = false;
        for (const std::string_view name : registered) {
            if (name == row.property) {
                is_registered = true;
                break;
            }
        }
        if (!expect(is_registered, "row is a registered property: " + row.property)) {
            ok = false;
        }
    }
    return ok;
}

// Every `applied` row must name a test, so a consumer cannot be claimed without evidence. The
// evidence column is the last cell; a row that says `applied` and names no test is a doc bug.
bool applied_rows_name_a_test() {
    std::ifstream input(kStatusDoc);
    if (!expect(static_cast<bool>(input), "the status doc is readable")) {
        return false;
    }

    std::string line;
    bool ok = true;
    std::size_t applied_rows = 0;
    while (std::getline(input, line)) {
        if (line.size() < 4 || line.front() != '|') {
            continue;
        }
        const std::vector<std::string> cells = split_cells(line);
        if (cells.size() < 3 || status_of(cells[1]) != "applied") {
            continue;
        }
        ++applied_rows;
        if (!expect(cells.back().find('`') != std::string::npos,
                    "an applied row names a test: " + line)) {
            ok = false;
        }
    }
    return expect(applied_rows > 0, "the doc has applied rows") && ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = status_table_covers_the_registry() && ok;
    ok = applied_rows_name_a_test() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "property_status: ok\n";
    return 0;
}
