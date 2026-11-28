// SPDX-License-Identifier: MIT

// #1055 end to end: a callee's `useState` initializer that reads a prop must start at the value the
// caller passed, not at the prop's declared default. The TSX is compiled through the real front end
// into generated C++ (`Counter`, `Host`); the parent passes `start={7}` to one instance and
// nothing to the other, and the built scene shows what each instance's state started at.

#include "Counter.hpp"
#include "Host.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool has_text(const ui::Node& node, std::string_view text) {
    const std::optional<ui::PaintData>& paint = node.paint_data();
    if (paint && paint->text == text) {
        return true;
    }
    for (const auto& child : node.children()) {
        if (has_text(*child, text)) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    // The parent writes its instances' props, then builds them.
    {
        ui::Node root(1);
        state_init::Host host;
        host.build(root);
        check(has_text(root, "7"), "an int state reading a prop starts at the caller's 7");
        check(has_text(root, "seven"),
              "a string state reading a prop starts at the caller's label");
        check(has_text(root, "3"), "an instance given no prop starts at the declared default");
        check(has_text(root, "n"), "a string default survives for an instance given no prop");
    }
    // A standalone component has no owner writing props: the declared defaults apply.
    {
        ui::Node root(2);
        state_init::Counter counter;
        counter.build(root);
        check(has_text(root, "3"), "a standalone component starts at the prop default");
    }
    if (failures == 0) {
        std::cout << "codegen_state_init: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
