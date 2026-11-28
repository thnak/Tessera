// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Host` component of the `codegen_state_init` fixture (#1055): the
// parent that passes `start` and `label` to a `Counter` instance.

#pragma once

#include "Host.g.hpp"

namespace state_init {

class Host final : public gen::HostBase<Host> {
private:
    friend struct gen::HostBase<Host>;
};

} // namespace state_init
