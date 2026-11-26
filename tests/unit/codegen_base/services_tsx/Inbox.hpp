// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `codegen_services_tsx` fixture (ADR-050 section 3.1, issue #177). The
// TSX has only inline handlers, so there is no `bind::` member to define: the class exists to be
// the `Self` the generated base is instantiated with.

#pragma once

#include "Inbox.g.hpp"

namespace demo {

class Inbox final : public gen::InboxBase<Inbox> {
private:
    friend struct gen::InboxBase<Inbox>;
};

} // namespace demo
