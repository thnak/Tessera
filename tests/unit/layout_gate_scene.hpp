// SPDX-License-Identifier: MIT
//
// The scene and edit machinery of the layout-unchanged gate's acceptance test (issue #735, slice
// A). Split out of `layout_gate_tests.cpp`, which is at its size budget: the scene builder, the
// edit kinds and the cold-layout comparison are one responsibility, and the test's `main` is
// another.
//
// The scene is deliberately built so that **every box is a function of the stylesheet alone**:
//   * each node carries an explicit width and height, so no box comes from measuring text (the ICU
//     lane uses a real text measurer, and a measured box would make the comparison test the
//     measurer rather than the gate);
//   * `flex-shrink: 0` and a card count that fits the smallest viewport, so nothing is ever shrunk
//     by an overflow whose amount would depend on content;
//   * inline-style text is owned by the scene, because `Node::set_inline_style` keeps a
//     `string_view` - a temporary there leaves the node reading freed memory.

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::layout_gate {

// A fixed-seed LCG: reproducible, no <random> engine differences between standard libraries.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed)
        : state_(seed) {}

    std::uint32_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33);
    }

    std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0 : next() % bound;
    }

private:
    std::uint64_t state_;
};

// Node ids at or above this belong to the children the `Shape` edit adds, so removing one can
// never take out a node the other edits hold a pointer to.
inline constexpr std::uint64_t kExtraIdBase = 900000;

// Every box this sheet produces is fixed by a length, never by measured text. See the file comment.
inline constexpr const char* kSheet =
    R"(.card { width: 120px; height: 40px; padding: 4px; margin: 2px; flex-shrink: 0; background-color: #1e293b; }
.card.hot { background-color: #334155; }
.card.wide { width: 180px; }
.card.tall { height: 60px; }
.card.hidden { display: none; }
.card.padded { padding: 12px; }
Text { width: 100px; height: 16px; font-size: 14px; color: #e2e8f0; flex-shrink: 0; }
Text.big { font-size: 20px; height: 24px; }
)";

// One node's box, keyed by node id. A `display: none` subtree emits zero boxes for every
// descendant, so two hosts can legitimately produce different *counts* of boxes; comparing by id
// rather than by position is what makes the comparison meaningful.
using Boxes = std::vector<std::pair<std::uint64_t, std::array<float, 4>>>;

void collect_boxes(const tessera::ui::Node& node, Boxes& out);

// A deep copy of the scene's *style inputs*, so the cold reference host can lay out an independent
// tree. Copying the tree rather than sharing it matters: `AppHost`'s destructor takes back the
// root's inline `width`/`height` declarations, so a cold host built on the live tree would strip
// the warm host's root size when it went out of scope.
std::unique_ptr<tessera::ui::Node> clone_tree(const tessera::ui::Node& node);

struct Scene {
    std::unique_ptr<tessera::ui::Node> root = std::make_unique<tessera::ui::Node>(1);
    std::vector<tessera::ui::Node*> cards;
    std::vector<tessera::ui::Node*> labels;
    // A monotonically increasing counter the layout edits use to guarantee they always change the
    // value they write. Alternating on frame parity is not enough: the same card can be picked on
    // two frames of the same parity, which would make the second edit a no-op that the gate
    // correctly accepts.
    std::uint32_t edit_serial = 0;
    // `Node::set_inline_style` keeps a `string_view`, so the text must outlive the node's
    // declaration. Passing a temporary `std::string` leaves the node reading freed memory, which
    // makes the computed style - and so the layout - depend on whatever the allocator did next.
    // The host documents the same rule for its root size buffers (see `write_pixels`).
    std::deque<std::string> inline_text;

    explicit Scene(int card_count);
};

// The edit kinds the gate must classify correctly. `moves_a_box` is the contract: a paint-only edit
// must be accepted (no layout pass), a layout edit must be declined.
enum class Edit {
    ClassPaint,   // .hot: background only
    ClassLayout,  // .wide / .tall / .padded: a box change
    ClassDisplay, // .hidden: display:none
    InlinePaint,  // background-color / border-radius
    InlineLayout, // width / margin-top
    Text,         // a text payload change
    FontSize,     // a Text class that changes the measured size
    Shape,        // add or remove a child
    Viewport,     // a resize
};

inline constexpr int kEditKindCount = 9;

const char* name_of(Edit edit);

// Applies one edit. Each kind owns a disjoint slice of the scene, so a later edit of a different
// kind cannot silently undo an earlier one. That matters for the gate's contract: a `ClassPaint`
// that wrote `"card hot"` over a `ClassLayout`'s `"card wide"` would move a box while claiming to be
// paint-only, which is a bug in the test rather than in the gate.
//
//   cards[0]      class-paint edits (paint-only)
//   cards[1]      class-layout edits (a box change)
//   cards[2]      class-display edits (display:none)
//   cards[3]      inline-paint edits (paint-only)
//   cards[4]      inline-layout edits (a box change)
//   cards[5]      shape edits (children added and removed)
//   labels[0..]   text and font-size edits
void apply_edit(Edit edit, Scene& scene, Lcg& rng, int frame, float& width, float& height);

// Whether an edit kind must move a box.
bool moves_a_box(Edit edit);

} // namespace tessera::test::layout_gate
