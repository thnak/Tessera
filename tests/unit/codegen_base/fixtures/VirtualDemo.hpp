// SPDX-License-Identifier: MIT
//
// Developer-owned half of the `VirtualDemo` fixture (ADR-040). The generated base owns the whole
// collection machinery; this class owns only the `bind::` members and the observations a real
// developer would keep: how often the collection asked it to scroll.
//
// The end-of-list behavior is observed through the component's own state instead: the fixture's
// inline `onEndReached` handler runs `setCount(1)`, so the public `count()` accessor plus the
// startled scene are the contract a developer can actually see.

#pragma once

#include "VirtualDemo.g.hpp"

namespace vlist {

class VirtualDemo final : public gen::VirtualDemoBase<VirtualDemo> {
public:
    void on_click(const tessera::ui::ClickEvent&) {
        ++clicks;
    }
    void on_scroll(const tessera::ui::ScrollEventInfo&) {
        ++scrolls;
    }

private:
    friend struct gen::VirtualDemoBase<VirtualDemo>;

public:
    int clicks = 0;
    int scrolls = 0;
};

} // namespace vlist
