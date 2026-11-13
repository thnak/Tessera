// SPDX-License-Identifier: MIT

#include "Sample.hpp"

// build() is where the generated base checks its bindings, so instantiating it is the test.
void instantiate_base_build(demo::Sample& sample, tessera::ui::Node& parent) {
    sample.build(parent);
}
