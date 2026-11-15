// SPDX-License-Identifier: MIT
//
// Developer-owned class of the callback-forwarding fixture (issue #286, ADR-040): the caller that
// owns one `ForwardWrap` instance and binds its `onPress` to its own setter. Never regenerated.

#pragma once

#include "ForwardOuter.g.hpp"

namespace forward {

class ForwardOuter final : public gen::ForwardOuterBase<ForwardOuter> {
public:
private:
    friend struct gen::ForwardOuterBase<ForwardOuter>;
};

} // namespace forward
