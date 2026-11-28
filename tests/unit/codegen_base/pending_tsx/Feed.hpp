// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `codegen_pending_tsx` fixture (#656). The TSX has only inline
// handlers, so there is no `bind::` member to define: the class exists to be the `Self` the
// generated base is instantiated with.

#pragma once

#include "Feed.g.hpp"

namespace pending_tsx {

class Feed final : public gen::FeedBase<Feed> {
private:
    friend struct gen::FeedBase<Feed>;
};

} // namespace pending_tsx
