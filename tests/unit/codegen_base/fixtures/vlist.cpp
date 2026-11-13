// SPDX-License-Identifier: MIT
//
// ADR-040: the generated base of the `VirtualDemo` fixture compiles with a developer class, and
// every member of the template instantiates (explicit instantiation), not only the ones a test
// calls.

#include "VirtualDemo.g.hpp"

namespace vlist {

class VirtualDemo final : public gen::VirtualDemoBase<VirtualDemo> {
public:
    void on_click(const tessera::ui::ClickEvent&) {}
    void on_scroll(const tessera::ui::ScrollEventInfo&) {}

private:
    friend struct gen::VirtualDemoBase<VirtualDemo>;
};

template struct gen::VirtualDemoBase<VirtualDemo>;
static_assert(gen::VirtualDemoContract<VirtualDemo>);

} // namespace vlist
