// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Filtered` fixture (ADR-040, issue #830): derives from the generated
// base. The component has no `bind::` member, so the developer class only names itself.

#pragma once

#include "Filtered.g.hpp"

namespace dynamic {

class Filtered final : public gen::FilteredBase<Filtered> {
private:
    friend struct gen::FilteredBase<Filtered>;
};

} // namespace dynamic
