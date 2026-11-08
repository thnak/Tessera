// SPDX-License-Identifier: MIT

#include "tessera/ui/style/keyframes.hpp"

#include <iostream>

namespace {

using tessera::ui::style::AtRule;
using tessera::ui::style::parse_keyframes;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "keyframes: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    AtRule at_rule;
    at_rule.name = "keyframes";
    at_rule.prelude = "pulse";
    at_rule.block = "from { opacity: 1; } 50% { opacity: 0.5; } to { opacity: 0; }";
    at_rule.has_block = true;

    const auto parsed = parse_keyframes(at_rule);
    if (!expect(parsed.ok, "parse keyframes") ||
        !expect(parsed.keyframes.name == "pulse", "keyframes name") ||
        !expect(parsed.keyframes.steps.size() == 3, "step count") ||
        !expect(parsed.keyframes.steps[0].offset == 0.0, "from offset") ||
        !expect(parsed.keyframes.steps[1].offset == 50.0, "50% offset") ||
        !expect(parsed.keyframes.steps[2].offset == 100.0, "to offset") ||
        !expect(parsed.keyframes.steps[0].declarations.size() == 1, "from declaration") ||
        !expect(parsed.keyframes.steps[0].declarations[0].value == "1", "from value") ||
        !expect(parsed.keyframes.steps[2].declarations[0].value == "0", "to value")) {
        return 1;
    }

    at_rule.block = "0%, 100% { opacity: 1; } 50% { opacity: 0.25; }";
    const auto comma = parse_keyframes(at_rule);
    if (!expect(comma.ok, "comma keyframes") ||
        !expect(comma.keyframes.steps.size() == 3, "comma step count") ||
        !expect(comma.keyframes.steps[0].offset == 0.0, "comma zero") ||
        !expect(comma.keyframes.steps[1].offset == 50.0, "comma fifty") ||
        !expect(comma.keyframes.steps[2].offset == 100.0, "comma hundred")) {
        return 2;
    }

    at_rule.block = "middle { opacity: 1; }";
    const auto invalid = parse_keyframes(at_rule);
    if (!expect(!invalid.ok, "invalid step") ||
        !expect(invalid.keyframes.steps.empty(), "invalid step skipped")) {
        return 3;
    }

    at_rule.has_block = false;
    at_rule.block.clear();
    const auto no_block = parse_keyframes(at_rule);
    if (!expect(!no_block.ok, "no block")) {
        return 4;
    }

    std::cout << "keyframes: ok\n";
    return 0;
}
