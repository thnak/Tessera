// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Leaf` fixture (ADR-056 slice 3). No `bind::` member, so the class
// only befriends its generated base. Never regenerated.

#pragma once

#include "Leaf.g.hpp"

namespace demo {

class Leaf final : public gen::LeafBase<Leaf> {
private:
    friend struct gen::LeafBase<Leaf>;
};

} // namespace demo
