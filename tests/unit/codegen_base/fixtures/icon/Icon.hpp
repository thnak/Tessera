// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library Icon fixture (issue #530, ADR-040): `Icon` has no `bind::`
// region, so the class only names itself. Never regenerated.

#pragma once

#include "Icon.g.hpp"

namespace icon_demo {

class Icon final : public gen::IconBase<Icon> {
public:
private:
    friend struct gen::IconBase<Icon>;
};

} // namespace icon_demo
