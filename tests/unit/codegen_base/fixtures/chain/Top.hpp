// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Top` fixture (ADR-056 slice 3). No `bind::` member, so the class
// only befriends its generated base. Never regenerated.

#pragma once

#include "Mid.hpp"
#include "Top.g.hpp"

namespace demo {

class Top final : public gen::TopBase<Top> {
private:
    friend struct gen::TopBase<Top>;
};

} // namespace demo
