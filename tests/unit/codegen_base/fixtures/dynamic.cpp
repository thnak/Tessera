// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `Dynamic` fixture compiles with a developer class, and every
// member of the template instantiates (explicit instantiation), not only the ones a test calls.

#include "Dynamic.hpp"

namespace dynamic {

template struct gen::DynamicBase<Dynamic>;
static_assert(gen::DynamicContract<Dynamic>);

} // namespace dynamic
