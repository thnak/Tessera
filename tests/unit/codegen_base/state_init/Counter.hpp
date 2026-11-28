// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Counter` component of the `codegen_state_init` fixture (#1055). The
// TSX has only inline handlers, so there is nothing to define.

#pragma once

#include "Counter.g.hpp"

namespace state_init {

class Counter final : public gen::CounterBase<Counter> {
private:
    friend struct gen::CounterBase<Counter>;
};

} // namespace state_init
