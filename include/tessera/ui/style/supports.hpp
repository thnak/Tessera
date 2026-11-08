// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/property_registry.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

enum class SupportsKind : std::uint8_t {
    Declaration,
    Not,
    And,
    Or,
};

struct SupportsCondition {
    SupportsKind kind = SupportsKind::Declaration;
    std::string property;
    std::string value;
    std::vector<SupportsCondition> children;
};

struct SupportsParseResult {
    SupportsCondition condition;
    bool ok = true;
    std::size_t error_offset = 0;
};

SupportsParseResult parse_supports_condition(std::string_view text);
bool evaluate_supports(const SupportsCondition& condition, const PropertyRegistry& registry);

} // namespace tessera::ui::style
