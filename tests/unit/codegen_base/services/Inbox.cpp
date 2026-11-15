// SPDX-License-Identifier: MIT

#include "Inbox.hpp"

namespace demo {

void Inbox::open(const tessera::ui::Event& event) {
    (void)event;
    // The `nav()` accessor is the generated `service<demo::Navigator>()`; the read itself must not
    // reach the registry (ADR-050 section 3 item 5 makes a lookup here a contract violation).
    ++accessor_reads;
    nav().push("inbox");
    last_route = nav().last_route();
}

} // namespace demo
