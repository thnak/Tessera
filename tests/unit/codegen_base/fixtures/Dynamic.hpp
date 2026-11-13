// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `Dynamic` fixture (ADR-040): derives from the generated base and
// defines the one `bind::` member. Never regenerated.

#pragma once

#include "Dynamic.g.hpp"

namespace dynamic {

class Dynamic final : public gen::DynamicBase<Dynamic> {
public:
    void on_click(const tessera::ui::ClickEvent&) {}

private:
    friend struct gen::DynamicBase<Dynamic>;
};

} // namespace dynamic
