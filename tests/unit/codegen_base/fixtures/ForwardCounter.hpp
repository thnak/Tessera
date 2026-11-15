// SPDX-License-Identifier: MIT
//
// Developer-owned class of the callback-forwarding fixture (issue #286, ADR-040): a component whose
// handler body runs a setter and then calls a callback prop. Never regenerated.

#pragma once

#include "ForwardCounter.g.hpp"

namespace forward {

class ForwardCounter final : public gen::ForwardCounterBase<ForwardCounter> {
public:
private:
    friend struct gen::ForwardCounterBase<ForwardCounter>;
};

} // namespace forward
