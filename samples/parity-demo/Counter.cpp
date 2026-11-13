// SPDX-License-Identifier: MIT

#include "Counter.hpp"

namespace parity_sample {

void Counter::add(const tessera::ui::Event& event) {
    (void)event;
    set_count(count() + 2);
    set_label("added");
}

} // namespace parity_sample
