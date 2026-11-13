// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `VectorIcon` fixture compiles with a developer class, and
// every member of the template instantiates (explicit instantiation), not only the ones a test
// calls.

#include "VectorIcon.hpp"

namespace vicon {

template struct gen::VectorIconBase<VectorIcon>;
static_assert(gen::VectorIconContract<VectorIcon>);

} // namespace vicon
