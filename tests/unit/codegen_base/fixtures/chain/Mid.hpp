// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Mid` fixture (ADR-056 slice 3). No `bind::` member, so the class
// only befriends its generated base. Never regenerated.

#pragma once

#include "Leaf.hpp"
#include "Mid.g.hpp"

namespace demo {

class Mid final : public gen::MidBase<Mid> {
private:
    friend struct gen::MidBase<Mid>;
};

} // namespace demo
