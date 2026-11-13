// SPDX-License-Identifier: MIT
// Developer-owned half of the compiler demo screen (ADR-040): edit freely. The generated base is
// Demo.g.hpp, written into the build directory from Demo.tsx by tessera_codegen --mode base.
#pragma once

#include "Demo.g.hpp"

namespace demo {

class Demo final : public gen::DemoBase<Demo> {
public:
    // How many times the `sync` effect body ran; the integration tests observe the effect with it.
    [[nodiscard]] int sync_runs() const {
        return sync_runs_;
    }

private:
    friend struct gen::DemoBase<Demo>;

    // bind::sync (Demo.tsx)
    void sync();

    // bind::sync_cleanup (Demo.tsx)
    void sync_cleanup();

    int sync_runs_ = 0;
};

} // namespace demo
