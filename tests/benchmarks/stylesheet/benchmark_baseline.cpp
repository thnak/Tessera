// SPDX-License-Identifier: MIT

#include "benchmark_baseline.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::benchmark {
namespace {

namespace harness = tessera::harness;

// Written into every regenerated baseline; the committed file keeps the same text.
constexpr std::string_view kBaselineNote =
    "Stage counters for the two stylesheet startup paths (issue #201, ADR-048). "
    "Structural counters are exact: they are a pure function of the stylesheet and the engine. "
    "Allocation counters carry tolerance.allocation_percent because the standard library's "
    "container layout differs between toolchains. Regenerate with --update and a recorded "
    "decision; there is no wall-clock number here on purpose (docs/guides/testing.md 7).";

bool read_size(const harness::JsonValue& object, std::string_view key, std::size_t& out) {
    const harness::JsonValue* const value = object.find(key);
    if (value == nullptr) {
        return false;
    }
    const std::optional<std::int64_t> parsed = value->as_integer();
    if (!parsed || *parsed < 0) {
        return false;
    }
    out = static_cast<std::size_t>(*parsed);
    return true;
}

bool read_number(const harness::JsonValue& object, std::string_view key, double& out) {
    const harness::JsonValue* const value = object.find(key);
    if (value == nullptr) {
        return false;
    }
    const std::optional<double> parsed = value->as_number();
    if (!parsed || *parsed < 0.0) {
        return false;
    }
    out = *parsed;
    return true;
}

bool read_object(const harness::JsonValue& root, std::string_view key,
                 const harness::JsonValue*& out) {
    const harness::JsonValue* const value = root.find(key);
    if (value == nullptr || !value->is_object()) {
        return false;
    }
    out = value;
    return true;
}

bool read_schema(const harness::JsonValue& root) {
    const harness::JsonValue* const value = root.find("schema");
    if (value == nullptr) {
        return false;
    }
    const std::optional<std::int64_t> schema = value->as_integer();
    return schema.has_value() && *schema == 1;
}

} // namespace

bool load_baseline(const char* path, Baseline& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "stylesheet_benchmark: cannot read the baseline %s\n", path);
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const auto parsed = harness::parse_json(text);
    if (!parsed) {
        std::fprintf(stderr, "stylesheet_benchmark: the baseline %s is not valid JSON\n", path);
        return false;
    }

    const harness::JsonValue& root = *parsed;
    const harness::JsonValue* stylesheet = nullptr;
    const harness::JsonValue* blob = nullptr;
    const harness::JsonValue* parse = nullptr;
    const harness::JsonValue* compiled = nullptr;
    const harness::JsonValue* tolerance = nullptr;
    const bool shape = read_schema(root) && read_object(root, "stylesheet", stylesheet) &&
                       read_object(root, "blob", blob) && read_object(root, "parse", parse) &&
                       read_object(root, "compiled", compiled) &&
                       read_object(root, "tolerance", tolerance);
    const bool keys =
        shape && read_size(*stylesheet, "bytes", out.css_bytes) &&
        read_size(*stylesheet, "tokens", out.css_tokens) &&
        read_size(*blob, "bytes", out.blob_bytes) &&
        read_size(*blob, "load_allocations", out.blob_load_allocations) &&
        read_size(*blob, "load_allocated_bytes", out.blob_load_allocated_bytes) &&
        read_size(*parse, "prepare_allocations", out.parse_prepare_allocations) &&
        read_size(*parse, "prepare_allocated_bytes", out.parse_prepare_allocated_bytes) &&
        read_size(*compiled, "rules", out.rules) &&
        read_size(*compiled, "selectors", out.selectors) &&
        read_size(*compiled, "declarations", out.declarations) &&
        read_size(*compiled, "diagnostics", out.diagnostics) &&
        read_size(*compiled, "keyframes", out.keyframes) &&
        read_size(*compiled, "layers", out.layers) &&
        read_size(*compiled, "container_queries", out.container_queries) &&
        read_number(*tolerance, "allocation_percent", out.allocation_tolerance_percent);
    if (!keys) {
        std::fprintf(stderr,
                     "stylesheet_benchmark: the baseline %s is missing a schema-1 key; regenerate "
                     "it with --update\n",
                     path);
        return false;
    }
    return true;
}

bool write_baseline(const char* path, const PathComparison& comparison) {
    // Fixed layout: the checked-in baseline is reviewed as a diff, so it is written with one nested
    // object per line and a stable key order instead of the canonical single-line encoding.
    std::string out;
    out += "{\n";
    out += "  \"schema\": 1,\n";
    out += "  \"note\": \"";
    out += kBaselineNote;
    out += "\",\n";
    out += "  \"stylesheet\": {\n";
    out += "    \"bytes\": " + std::to_string(comparison.css_bytes) + ",\n";
    out += "    \"tokens\": " + std::to_string(comparison.css_tokens) + "\n";
    out += "  },\n";
    out += "  \"blob\": {\n";
    out += "    \"bytes\": " + std::to_string(comparison.blob_bytes) + ",\n";
    out +=
        "    \"load_allocations\": " + std::to_string(comparison.blob.prepare.allocations) + ",\n";
    out += "    \"load_allocated_bytes\": " +
           std::to_string(comparison.blob.prepare.allocations_bytes) + "\n";
    out += "  },\n";
    out += "  \"parse\": {\n";
    out += "    \"prepare_allocations\": " + std::to_string(comparison.parse.prepare.allocations) +
           ",\n";
    out += "    \"prepare_allocated_bytes\": " +
           std::to_string(comparison.parse.prepare.allocations_bytes) + "\n";
    out += "  },\n";
    out += "  \"compiled\": {\n";
    out += "    \"rules\": " + std::to_string(comparison.parse.rules) + ",\n";
    out += "    \"selectors\": " + std::to_string(comparison.parse.selectors) + ",\n";
    out += "    \"declarations\": " + std::to_string(comparison.parse.declarations) + ",\n";
    out += "    \"diagnostics\": " + std::to_string(comparison.parse.diagnostics) + ",\n";
    out += "    \"keyframes\": " + std::to_string(comparison.parse.keyframes) + ",\n";
    out += "    \"layers\": " + std::to_string(comparison.parse.layers) + ",\n";
    out +=
        "    \"container_queries\": " + std::to_string(comparison.parse.container_queries) + "\n";
    out += "  },\n";
    out += "  \"tolerance\": {\n";
    out += "    \"allocation_percent\": 15.0\n";
    out += "  }\n";
    out += "}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) {
        file << out;
    }
    if (!file) {
        std::fprintf(stderr, "stylesheet_benchmark: cannot write the baseline %s\n", path);
        return false;
    }
    return true;
}

} // namespace tessera::benchmark
