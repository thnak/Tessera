// SPDX-License-Identifier: MIT

#include "Feed.hpp"

// The pending-UI fixture's developer class has no bindings to define: the generated base owns the
// boundary, the fake resource and the transition mark. This translation unit exists so the runtime
// and differential tests link the same shape a real component does.
namespace pending {

// Nothing to define (ADR-040: only `bind::` members are required, and the fixture has none).

} // namespace pending
