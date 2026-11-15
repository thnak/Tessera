// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `DynamicVirtual` fixture (ADR-040): the prop-driven
// virtual-collection component derives from the generated base and defines the one `bind::` member
// its TSX declares. Never regenerated; the runtime section of codegen_dynamic_virtual_tests drives
// this class.

#pragma once

#include "DynamicVirtual.g.hpp"

namespace dvirt {

class DynamicVirtual final : public gen::DynamicVirtualBase<DynamicVirtual> {
private:
    friend struct gen::DynamicVirtualBase<DynamicVirtual>;
};

} // namespace dvirt
