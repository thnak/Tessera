// SPDX-License-Identifier: MIT

// Access point for the 13 element rows (docs/specs/jsx-intrinsics-v1/README.md section 3); the data
// lives in elements_basic_data.hpp, elements_other_data.hpp and elements_virtual_data.hpp.

#include "elements_data.hpp"
#include "tables.hpp"

#include <span>

namespace tessera::schema::internal {

std::span<const ElementRow> element_table() {
    return kElements;
}

} // namespace tessera::schema::internal