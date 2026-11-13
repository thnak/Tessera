// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `DynamicVirtual` fixture compiles with a developer class, and
// every member of the template instantiates (explicit instantiation), not only the ones a test
// calls.

#include "DynamicVirtual.g.hpp"

namespace dvirt {

class DynamicVirtual final : public gen::DynamicVirtualBase<DynamicVirtual> {
public:
private:
    friend struct gen::DynamicVirtualBase<DynamicVirtual>;
};

template struct gen::DynamicVirtualBase<DynamicVirtual>;
static_assert(gen::DynamicVirtualContract<DynamicVirtual>);

} // namespace dvirt
