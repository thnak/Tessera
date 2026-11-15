// SPDX-License-Identifier: MIT
//
// Developer class of the `#line` map compile test (issue #78). It deliberately defines neither
// `bind::first` nor `bind::second`, so a build of `instantiate.cpp` fails twice, and the two
// diagnostics must name TSX lines 5 and 10 of tests/fixtures/codegen-line-map/Two.tsx.
//
// The header is compiled twice by the CMake script that drives this test: once against the base
// generated with `--line-directives` (the test) and once without (the control, whose diagnostics
// name lines of the generated `.g.hpp`). Nothing here may depend on which base it is compiled
// against.

#pragma once

#include "Two.g.hpp"

namespace two {

class Two final : public gen::TwoBase<Two> {
private:
    friend struct gen::TwoBase<Two>;
};

} // namespace two
