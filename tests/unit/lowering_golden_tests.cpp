// SPDX-License-Identifier: MIT

// Golden of lowered expressions: one JSON array of {source, ir} entries compared byte for byte.
// Regenerating the golden is deliberate, never automatic:
//   lowering_golden_tests --write tests/golden/lowering/expressions.json
// from the repository root, then review the diff (docs/design/jsx-ir-lowering.md section 8).

#include "lowering_test_util.hpp"
#include "tessera/compiler/ir/json_value.hpp"

#include <fstream>
#include <string>
#include <string_view>

#ifndef TESSERA_LOWERING_GOLDEN_DIR
#error "TESSERA_LOWERING_GOLDEN_DIR must be defined by the build"
#endif

namespace {

using namespace lowering_test;

const MapNames kNames{
    {"a", "state::a"},
    {"b", "state::b"},
    {"c", "state::c"},
    {"count", "state::count"},
    {"ok", "state::ok"},
    {"title", "state::title"},
    {"props.title", "prop::title"},
    {"item.row.id", "item.row.id"},
};

constexpr std::string_view kSources[] = {
    "0",
    "42",
    "2147483647",
    "-2147483648",
    "0.5",
    "1e3",
    "\"a\\nb\"",
    "true",
    "null",
    "count",
    "props.title",
    "item.row.id",
    "!ok",
    "-count",
    "+count",
    "a + b",
    "a === b",
    "a !== b",
    "a < b && c >= b",
    "c ? a : b",
    "`n=${count}!`",
    "`${a}${b}`",
    "min(a, b)",
    "max(min(a, b), 0)",
    "formatNumber(count, 2)",
};

std::string build_document() {
    ir::JsonValue document = ir::JsonValue::array();
    for (const std::string_view source : kSources) {
        const Lowered lowered = lower(source, &kNames);
        ir::JsonValue entry = ir::JsonValue::object();
        entry.set("source", ir::JsonValue::string(std::string(source)));
        if (const ir::Expr* expr = expr_of(lowered); expr != nullptr) {
            entry.set("ir", ir::expr_to_json(expr));
        }
        document.push(std::move(entry));
    }
    return ir::to_text(document);
}

bool write_golden(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--write") {
        check(write_golden(argv[2], build_document()), "the golden file is writable");
        return finish("lowering golden update");
    }

    const std::string path = std::string(TESSERA_LOWERING_GOLDEN_DIR) + "/expressions.json";
    const std::string golden = read_file(path);
    check(!golden.empty(), "the lowering golden is readable");
    check_eq(build_document(), golden, "lowered expressions match the golden");
    return finish("lowering golden tests");
}
