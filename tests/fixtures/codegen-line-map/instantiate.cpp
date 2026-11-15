// SPDX-License-Identifier: MIT
//
// The translation unit the `#line` map compile test builds (issue #78): instantiating build() is
// what triggers the generated base's binding contract, so this is the only statement the test
// needs. Not part of the normal build.

#include "Two.hpp"

void instantiate_base_build(two::Two& two, tessera::ui::Node& parent) {
    two.build(parent);
}
