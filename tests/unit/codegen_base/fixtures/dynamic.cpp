// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `Dynamic` fixture compiles with a developer class, and every
// member of the template instantiates (explicit instantiation), not only the ones a test calls.

#include "Dynamic.g.hpp"

namespace dynamic {

class Dynamic final : public gen::DynamicBase<Dynamic> {
public:
    void on_click(const tessera::ui::ClickEvent&) {}

private:
    friend struct gen::DynamicBase<Dynamic>;
};

template struct gen::DynamicBase<Dynamic>;
static_assert(gen::DynamicContract<Dynamic>);

} // namespace dynamic
