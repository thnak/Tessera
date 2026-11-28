// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library Icon fixture (issue #530, ADR-040): `Button` has no `bind::`
// region, so the class only names itself. Never regenerated.

#pragma once

#include "Button.g.hpp"

namespace icon_demo {

class Button final : public gen::ButtonBase<Button> {
public:
private:
    friend struct gen::ButtonBase<Button>;
};

} // namespace icon_demo
