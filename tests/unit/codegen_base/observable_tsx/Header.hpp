// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `codegen_observable_tsx` fixture (ADR-050 open question 6). The TSX
// has no `bind::` member, so the class only exists to be the `Self` the generated base is
// instantiated with.

#pragma once

#include "Header.g.hpp"

namespace demo {

class Header final : public gen::HeaderBase<Header> {
private:
    friend struct gen::HeaderBase<Header>;
};

} // namespace demo
