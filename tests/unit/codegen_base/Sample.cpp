// SPDX-License-Identifier: MIT

#include "Sample.hpp"

namespace demo {

void Sample::on_click(const tessera::ui::ClickEvent& event) {
    (void)event;
    ++clicks;
    set_count(count() + 1);
}

void Sample::sync() {
    ++syncs;
    last_count_seen = count();
}

void Sample::sync_cleanup() {
    ++cleanups;
}

} // namespace demo
