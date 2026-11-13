// SPDX-License-Identifier: MIT
// Developer-owned half of the preview-parity sample (ADR-040). The generated base Counter.g.hpp is
// written into the build directory from Counter.tsx by tessera_codegen --mode base.
#pragma once

#include "Counter.g.hpp"

namespace parity_sample {

class Counter final : public gen::CounterBase<Counter> {
private:
    friend struct gen::CounterBase<Counter>;

    // bind::add (Counter.tsx): the C++ twin of the preview body.
    void add(const tessera::ui::Event& event);
};

} // namespace parity_sample
