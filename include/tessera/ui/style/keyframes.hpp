// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/parser.hpp"

#include <string>
#include <vector>

namespace tessera::ui::style {

struct KeyframeStep {
    double offset = 0.0;
    std::vector<Declaration> declarations;
};

struct KeyframeBlock {
    std::string name;
    std::vector<KeyframeStep> steps;
};

struct KeyframesParseResult {
    KeyframeBlock keyframes;
    bool ok = true;
};

KeyframesParseResult parse_keyframes(const AtRule& at_rule);

} // namespace tessera::ui::style
