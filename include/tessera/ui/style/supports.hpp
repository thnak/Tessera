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
    // `selector(<selector list>)`: asks whether the engine can match with that selector
    // (stylesheet-v1 section 6.4).
    Selector,
    Not,
    And,
    Or,
};

struct SupportsCondition {
    SupportsKind kind = SupportsKind::Declaration;
    std::string property;
    std::string value;
    // The selector text of a `Selector` condition, exactly as written.
    std::string selector;
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
