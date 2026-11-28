// SPDX-License-Identifier: MIT

#pragma once

#include "other_formats.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The tessera.json document model shared by the printer (json_format.cpp) and the strict JSON
// parser (json_parse.cpp). Scalars keep their raw text so the printer never rewrites a string or a
// number.
namespace tessera::compiler::format {

struct Value {
    enum class Kind : std::uint8_t { Scalar, Object, Array } kind = Kind::Scalar;
    std::string raw; // scalar text, or an object key's raw string
    std::vector<std::pair<std::string, Value>> members;
    std::vector<Value> items;
};

// Parses strict JSON (no comments, no trailing commas). On failure `problem` says why and where.
bool parse_json_value(std::string_view source, Value& out, Problem& problem);

} // namespace tessera::compiler::format
