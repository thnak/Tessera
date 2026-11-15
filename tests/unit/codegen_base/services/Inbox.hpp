// SPDX-License-Identifier: MIT
//
// Developer-owned half of the AF5 fixture (ADR-050 section 3.1, issue #177): derives from the
// generated `InboxBase<Self>` and defines the `bind::open` member. Never regenerated (the generator
// only writes Inbox.g.hpp).
//
// It reads its services through the generated accessors -- `nav()` and `store()` -- which is the
// shape a lowered `useService<T>()` use compiles to. `service<T>()` is available too; the negative
// fixtures under services/undeclared and services/not_interface use it with a `T` that must be
// rejected.

#pragma once

#include "Inbox.g.hpp"

namespace demo {

class Inbox final : public gen::InboxBase<Inbox> {
public:
    // Observed by the runtime test: every accessor read is counted, so "a read is a cached pointer,
    // not a lookup" is asserted against the appfw audit counters rather than believed.
    int accessor_reads = 0;
    std::string last_route;

    void open(const tessera::ui::Event& event);

private:
    friend struct gen::InboxBase<Inbox>;
};

} // namespace demo
