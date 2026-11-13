// SPDX-License-Identifier: MIT
//
// Compile-fail fixture (wrong_signature): the build of this target must fail with
// TESSERA_CG_MISSING_BINDING naming the bind id. Never part of the normal build.

#pragma once

#include "Sample.g.hpp"

namespace demo {

class Sample final : public gen::SampleBase<Sample> {
public:
    void on_click(int) {} // wrong parameter: bind::on_click takes a ClickEvent
    void sync() {}
    void sync_cleanup() {}

private:
    friend struct gen::SampleBase<Sample>;
};

} // namespace demo
