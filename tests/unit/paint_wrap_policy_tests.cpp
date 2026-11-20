// SPDX-License-Identifier: MIT

// A `Text` element wraps by default, any other carrier only when PaintData opts in (a long
// paragraph was measured on two lines by layout but painted on one).

#include "tessera/ui/scene/text_wrap.hpp"

#include <iostream>

int main() {
    using tessera::ui::paint_wraps;
    using tessera::ui::PaintData;
    using tessera::ui::PaintWrap;

    bool ok = true;
    PaintData data;
    ok &= paint_wraps("Text", data);
    ok &= !paint_wraps("TextInput", data);
    ok &= !paint_wraps("Button", data);
    data.wrap = PaintWrap::Wrap;
    ok &= paint_wraps("TextInput", data);
    if (!ok) {
        std::cerr << "paint_wrap_policy: unexpected wrap decision\n";
    }
    return ok ? 0 : 1;
}
