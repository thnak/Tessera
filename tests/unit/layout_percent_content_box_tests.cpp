// SPDX-License-Identifier: MIT
//
// The Android shell's card must not overflow its parent (issue #791 part 2).
//
// The shell's stylesheet gives `View` a `width: 100%` and `padding: 24px`. Under the CSS default
// `box-sizing: content-box` those two combine into a box wider than its container - correct CSS,
// and exactly what a browser does - so every child ends up past the right edge and the text field
// runs to the screen edge. The fix is the stylesheet's: `box-sizing: border-box` makes the authored
// 100% the border box, so the padding comes out of it instead of being added to it.
//
// This test goes through the real path (a compiled stylesheet plus inline declarations, the same
// two sources the shell uses) because the pure `LayoutInput` path takes longhands that are already
// expanded and cannot catch a stylesheet mistake.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_percent_content_box: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

void set_text(Node& node, std::string text) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = std::move(text);
    node.set_paint_data(std::move(data));
}

// The shell's stylesheet, reduced to the rules this test needs. `box-sizing: border-box` on `View`
// is the fix under test: without it the root's `width: 100%` plus `padding: 24px` is 448 wide in a
// 400-wide viewport, and every child inherits that overflow.
constexpr const char* kSheet = R"(
View {
  box-sizing: border-box;
  width: 100%;
  display: flex;
  flex-direction: column;
  padding: 24px;
}
TextInput {
  box-sizing: border-box;
  width: 100%;
  height: 48px;
  padding: 12px;
}
)";

} // namespace

int main() {
    Node root(1);
    root.set_element_kind("View");
    root.set_inline_style("width", "100%");
    root.set_inline_style("height", "100%");

    // The card: `padding: 24px` as an INLINE declaration, which is how the shell writes it. The
    // registry must expand the shorthand into the four longhands the layout adapter reads.
    auto card_ptr = std::make_unique<Node>(2);
    Node& card = root.add_child(std::move(card_ptr));
    card.set_element_kind("View");
    card.set_inline_style("display", "flex");
    card.set_inline_style("flex-direction", "column");
    card.set_inline_style("padding", "24px");

    auto field_ptr = std::make_unique<Node>(3);
    Node& field = card.add_child(std::move(field_ptr));
    field.set_element_kind("TextInput");
    set_text(field, "");

    ManualClock clock;
    RuntimeLoop loop(clock);
    HostConfig config;
    config.width = 400.0f;
    config.height = 300.0f;
    AppHost host(root, compile_stylesheet(kSheet), loop, config);
    // The host's constructor may have consumed the initial dirty state, so ask for the first layout
    // explicitly: this test is about where a percentage resolves, not about what wakes the first
    // frame.
    root.mark_dirty(Dirty::Style | Dirty::Layout);
    (void)host.begin_frame(0.0);

    const auto& root_bounds = root.bounds();
    const auto& card_bounds = card.bounds();
    const auto& field_bounds = field.bounds();

    // With `box-sizing: border-box` the root's authored 100% IS its border box, so it is exactly
    // the viewport and its content box is 400 - 48 = 352.
    if (!expect(near(root_bounds.width, 400.0f),
                "the root's border box is the viewport, not the viewport plus padding")) {
        std::cerr << "  root x=" << root_bounds.x << " y=" << root_bounds.y
                  << " width=" << root_bounds.width << " height=" << root_bounds.height << "\n";
        return 1;
    }
    // The card is a 100% child, so it is the root's content box (352) starting at the root's left
    // padding (24).
    if (!expect(near(card_bounds.x, 24.0f), "the card starts at the root's left padding") ||
        !expect(near(card_bounds.width, 352.0f),
                "the card is the root's content box, not its border box")) {
        std::cerr << "  card x=" << card_bounds.x << " width=" << card_bounds.width << "\n";
        return 2;
    }
    // The field is the card's 100% child. With `box-sizing: border-box` on both, the field's
    // authored 100% is its border box, so it is the card's content box (352 - 48 = 304) and its own
    // 12px padding comes out of that rather than being added to it.
    if (!expect(near(field_bounds.width, 304.0f),
                "the field is the card's content box, not its border box")) {
        std::cerr << "  field x=" << field_bounds.x << " width=" << field_bounds.width << "\n";
        std::cerr << "  card x=" << card_bounds.x << " width=" << card_bounds.width
                  << " height=" << card_bounds.height << "\n";
        std::cerr << "  root x=" << root_bounds.x << " width=" << root_bounds.width << "\n";
        return 3;
    }
    // The decisive assertion, and the one the user saw: nothing reaches past the viewport. Before
    // `box-sizing: border-box` the root was 448 wide in a 400-wide viewport and the field ran off
    // the right edge.
    if (!expect(field_bounds.x + field_bounds.width <= 400.0f + 0.01f,
                "the field stays inside the viewport")) {
        std::cerr << "  field right=" << (field_bounds.x + field_bounds.width) << "\n";
        return 4;
    }
    // And it stays inside the card's content box, which is what the right padding is for.
    const float card_content_right = card_bounds.x + card_bounds.width - 24.0f;
    if (!expect(field_bounds.x + field_bounds.width <= card_content_right + 0.01f,
                "the field does not overflow the card's right padding")) {
        std::cerr << "  field right=" << (field_bounds.x + field_bounds.width)
                  << " card content right=" << card_content_right << "\n";
        return 5;
    }

    std::cout << "layout_percent_content_box: ok (card=" << card_bounds.width
              << " field=" << field_bounds.width << ")\n";
    return 0;
}
