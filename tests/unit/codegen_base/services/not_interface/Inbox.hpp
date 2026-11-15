// SPDX-License-Identifier: MIT
//
// Compile-fail fixture (AF5, issue #177): `demo::NotAService` has no `kServiceId`, so it is not a
// service interface at all (ADR-050 section 3 item 2: `T` must be a registered interface, never a
// concrete type). The build of this target must fail with TESSERA_CG_SERVICE_NOT_INTERFACE. Never
// part of the normal build.

#pragma once

#include "Inbox.g.hpp"

namespace demo {

class Inbox final : public gen::InboxBase<Inbox> {
public:
    void open(const tessera::ui::Event&) {
        // A concrete, unregistered type: rejected by the ProvidedService check.
        (void)service<NotAService>();
    }

private:
    friend struct gen::InboxBase<Inbox>;
};

} // namespace demo
