// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library Button fixture (issue #239, ADR-040): the library component
// has no `bind::` region, so the class only names itself. Never regenerated.

#pragma once

#include "Button.g.hpp"

namespace button_demo {

class Button final : public gen::ButtonBase<Button> {
public:
private:
    friend struct gen::ButtonBase<Button>;
};

} // namespace button_demo
