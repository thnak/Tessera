// SPDX-License-Identifier: MIT
//
// Developer-owned class of the callback-forwarding fixture (issue #286, ADR-040): a component that
// forwards its callback props to an intrinsic `onClick`/`onChange`; its handlers are inline, so the
// class has no `bind::` member. Never regenerated.

#pragma once

#include "ForwardWrap.g.hpp"

namespace forward {

class ForwardWrap final : public gen::ForwardWrapBase<ForwardWrap> {
public:
private:
    friend struct gen::ForwardWrapBase<ForwardWrap>;
};

} // namespace forward
