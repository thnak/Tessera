// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library Icon fixture (issue #530, ADR-040): `IconDemo` has no
// `bind::` region, so the class only names itself. Never regenerated.

#pragma once

#include "IconDemo.g.hpp"

namespace icon_demo {

class IconDemo final : public gen::IconDemoBase<IconDemo> {
public:
private:
    friend struct gen::IconDemoBase<IconDemo>;
};

} // namespace icon_demo
