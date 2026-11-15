// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library SafeArea fixture (issue #223, ADR-040): the page that places
// one `<SafeArea>` with explicit edges, sources, a floor and margin mode. Never regenerated.

#pragma once

#include "SafeAreaOptions.g.hpp"

namespace safe_area_demo {

class SafeAreaOptions final : public gen::SafeAreaOptionsBase<SafeAreaOptions> {
public:
private:
    friend struct gen::SafeAreaOptionsBase<SafeAreaOptions>;
};

} // namespace safe_area_demo
