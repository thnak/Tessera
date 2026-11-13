// SPDX-License-Identifier: MIT
//
// Compile-fail fixture (missing_member): the build of this target must fail with
// TESSERA_CG_MISSING_BINDING naming the bind id. Never part of the normal build.

#pragma once

#include "Sample.g.hpp"

namespace demo {

class Sample final : public gen::SampleBase<Sample> {
public:
    void on_click(const tessera::ui::ClickEvent&) {}
    void sync_cleanup() {}
    // `sync` is deliberately not defined: bind::sync must be reported.

private:
    friend struct gen::SampleBase<Sample>;
};

} // namespace demo
