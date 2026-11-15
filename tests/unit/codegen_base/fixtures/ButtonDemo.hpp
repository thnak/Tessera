// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library Button fixture (issue #239, ADR-040): the demo page that
// instantiates `<Button>` three times (enabled, disabled, small danger) and counts presses. Never
// regenerated.

#pragma once

#include "ButtonDemo.g.hpp"

namespace button_demo {

class ButtonDemo final : public gen::ButtonDemoBase<ButtonDemo> {
public:
private:
    friend struct gen::ButtonDemoBase<ButtonDemo>;
};

} // namespace button_demo
