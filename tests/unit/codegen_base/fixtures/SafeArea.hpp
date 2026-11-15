// SPDX-License-Identifier: MIT
//
// Developer-owned class of the library SafeArea fixture (issue #223, ADR-040): a role-less wrapper
// whose class tokens carry the props, so the class only names itself. Never regenerated.

#pragma once

#include "SafeArea.g.hpp"

namespace safe_area_demo {

class SafeArea final : public gen::SafeAreaBase<SafeArea> {
public:
private:
    friend struct gen::SafeAreaBase<SafeArea>;
};

} // namespace safe_area_demo
