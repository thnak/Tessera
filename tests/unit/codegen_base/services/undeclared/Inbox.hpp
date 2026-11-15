// SPDX-License-Identifier: MIT
//
// Compile-fail fixture (AF5, issue #177): `demo::Settings` is a perfectly valid service interface
// and the composition root could provide it, but `Inbox` never declared a `useService<Settings>()`.
// The build of this target must fail with TESSERA_CG_SERVICE_UNDECLARED naming the component, the
// requested type and the requirements the component does declare. Never part of the normal build.

#pragma once

#include "Inbox.g.hpp"

namespace demo {

class Inbox final : public gen::InboxBase<Inbox> {
public:
    void open(const tessera::ui::Event&) {
        // Not in Inbox's requirement set: the static_assert in ServiceRequirement<Settings> fires.
        (void)service<Settings>().theme();
    }

private:
    friend struct gen::InboxBase<Inbox>;
};

} // namespace demo
