// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library SafeArea fixture (issue #223, ADR-040): the page that places
// one `<SafeArea>` with the default edges/sources. Never regenerated.

#pragma once

#include "SafeAreaDemo.g.hpp"

namespace safe_area_demo {

class SafeAreaDemo final : public gen::SafeAreaDemoBase<SafeAreaDemo> {
public:
private:
    friend struct gen::SafeAreaDemoBase<SafeAreaDemo>;
};

} // namespace safe_area_demo
