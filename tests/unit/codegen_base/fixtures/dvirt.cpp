// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `DynamicVirtual` fixture compiles with its developer class
// (fixtures/DynamicVirtual.hpp, the same class the runtime test drives), and every member of the
// template instantiates (explicit instantiation), not only the ones a test calls.

#include "DynamicVirtual.hpp"

namespace dvirt {

template struct gen::DynamicVirtualBase<DynamicVirtual>;
static_assert(gen::DynamicVirtualContract<DynamicVirtual>);

} // namespace dvirt
