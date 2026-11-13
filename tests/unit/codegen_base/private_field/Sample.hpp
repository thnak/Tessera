// SPDX-License-Identifier: MIT
//
// Compile-fail fixture (private_field): every binding is defined, but a member writes the generated
// field `state_count_` directly instead of calling `set_count()`. The build of this target must
// fail because generated fields are private (ADR-040 item 2). Never part of the normal build.

#pragma once

#include "Sample.g.hpp"

namespace demo {

class Sample final : public gen::SampleBase<Sample> {
public:
    void on_click(const tessera::ui::ClickEvent&) {
        state_count_ = 5; // bypasses set_count(): no dirty mark, no scheduled update
    }
    void sync() {}
    void sync_cleanup() {}

private:
    friend struct gen::SampleBase<Sample>;
};

} // namespace demo
