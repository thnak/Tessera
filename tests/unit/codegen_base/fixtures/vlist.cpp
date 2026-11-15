// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `VirtualDemo` fixture compiles with its developer class
// (fixtures/VirtualDemo.hpp, the same class the runtime tests drive), and every member of the
// template instantiates (explicit instantiation), not only the ones a test calls.

#include "VirtualDemo.hpp"

namespace vlist {

template struct gen::VirtualDemoBase<VirtualDemo>;
static_assert(gen::VirtualDemoContract<VirtualDemo>);

} // namespace vlist
