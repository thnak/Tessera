// SPDX-License-Identifier: MIT

#include "Inbox.hpp"

// The accessor is only instantiated when it is called, so the call is what makes this a compile
// failure (the same shape as build() being where TESSERA_CG_MISSING_BINDING fires).
void instantiate_service_read(demo::Inbox& inbox, tessera::ui::Node& parent) {
    tessera::ui::Event click;
    click.type = tessera::ui::EventType::Click;
    inbox.build(parent);
    inbox.open(click);
}
