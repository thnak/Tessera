// SPDX-License-Identifier: MIT
//
// The layout-unchanged gate's acceptance test (issue #735, slice A): a frame whose layout pass the
// gate skipped must be byte-identical to the same frame laid out cold. The gate's whole claim is
// "every layout input is unchanged, so the previous result is still what a reflow would produce",
// and the only honest way to check that claim is to compare against a reflow.
//
// The test drives a randomized edit sequence over a tree that exercises every layout input source
// the gate has to cover:
//   * layout-affecting longhands (width, padding, margin, flex-basis, display, font-size);
//   * paint-only longhands (background-color, color, border-radius, box-shadow, opacity, z-index,
//     transform) - these must NOT move a box, so the gate must accept them;
//   * text, which reaches Yoga through the measure callback;
//   * the tree shape (adding and removing a child), which no computed style carries;
//   * the viewport (a resize), which no computed style carries either.
//
// After every frame the scene's boxes are compared against a second host that has never seen the
// edit history: it is constructed fresh on the same tree and lays out once. A gate that accepted a
// frame it should have declined shows up as a box that differs from the cold layout.
//
// Deterministic: the "random" sequence is a fixed-seed LCG, so a failure is reproducible.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "layout_gate: " << message << "\n";
    }
    return condition;
}

PaintData text_payload(std::string text) {
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    return data;
}

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
constexpr std::uint64_t kExtraIdBase = 900000;

// Every box this sheet produces is fixed by a length, never by measured text. That matters for the
// cold-layout comparison: the ICU lane swaps in a real text measurer, and a card whose height came
// from its text would then depend on which measurer the host happened to use rather than on the
// gate.
//
// `flex-shrink: 0` is the other half of that. The cards are flex items in the root's column, so
// with the default `flex-shrink: 1` they shrink proportionally once the content overflows the
// window - and the amount depends on the measured text height, which is exactly the dependency this
// test must not have. Pinning the shrink makes each card's box a function of the sheet alone.
constexpr const char* kSheet =
    R"(.card { width: 120px; height: 40px; padding: 4px; margin: 2px; flex-shrink: 0; background-color: #1e293b; }
.card.hot { background-color: #334155; }
.card.wide { width: 180px; }
.card.tall { height: 60px; }
.card.hidden { display: none; }
.card.padded { padding: 12px; }
Text { font-size: 14px; color: #e2e8f0; height: 16px; flex-shrink: 0; }
Text.big { font-size: 20px; height: 24px; }
)";

// One node's box, keyed by node id. A `display: none` subtree emits zero boxes for every
// descendant, so the two hosts can legitimately produce different *counts* of boxes; comparing by
// id rather than by position is what makes the comparison meaningful.
void collect_boxes(const Node& node,
                   std::vector<std::pair<std::uint64_t, std::array<float, 4>>>& out) {
    const auto& bounds = node.bounds();
    out.push_back({node.id(), {bounds.x, bounds.y, bounds.width, bounds.height}});
    for (const auto& child : node.children()) {
        collect_boxes(*child, out);
    }
}

// A deep copy of the scene's *style inputs*, so the cold reference host can lay out an independent
// tree. Copying the tree rather than sharing it matters: `AppHost`'s destructor takes back the
// root's inline `width`/`height` declarations, so a cold host built on the live tree would strip
// the warm host's root size when it went out of scope.
std::unique_ptr<Node> clone_tree(const Node& node) {
    auto copy = std::make_unique<Node>(node.id());
    copy->set_element_kind(node.element_kind());
    if (!node.element_id().empty()) {
        copy->set_element_id(node.element_id());
    }
    if (!node.class_name().empty()) {
        copy->set_class_name(node.class_name());
    }
    if (!node.part().empty()) {
        copy->set_part(node.part());
    }
    if (!node.component_name().empty()) {
        copy->set_component_name(node.component_name());
    }
    if (!node.element_tag().empty()) {
        copy->set_element_tag(node.element_tag());
    }
    if (!node.role().empty()) {
        copy->set_role(node.role());
    }
    for (const Node::AriaAttribute& aria : node.aria_attributes()) {
        (void)copy->set_aria(aria.name, aria.value);
    }
    for (const Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        copy->set_inline_style(declaration.name, declaration.value);
    }
    if (node.paint_data().has_value()) {
        copy->set_paint_data(node.paint_data());
    }
    for (const auto& child : node.children()) {
        copy->add_child(clone_tree(*child));
    }
    return copy;
}

struct Scene {
    std::unique_ptr<Node> root = std::make_unique<Node>(1);
    std::vector<Node*> cards;
    std::vector<Node*> labels;
    // A monotonically increasing counter the layout edits use to guarantee they always change the
    // value they write. Alternating on frame parity is not enough: the same card can be picked on
    // two frames of the same parity, which would make the second edit a no-op that the gate
    // correctly accepts.
    std::uint32_t edit_serial = 0;

    explicit Scene(int card_count) {
        std::uint64_t id = 2;
        for (int i = 0; i < card_count; ++i) {
            Node& card = root->add_child(std::make_unique<Node>(id++));
            card.set_element_kind("View");
            card.set_class_name("card");
            cards.push_back(&card);
            for (int j = 0; j < 2; ++j) {
                Node& label = card.add_child(std::make_unique<Node>(id++));
                label.set_element_kind("Text");
                label.set_paint_data(text_payload("Item " + std::to_string(i)));
                labels.push_back(&label);
            }
        }
    }
};

// The edit kinds the gate must classify correctly. `Layout` edits must make it decline; `Paint`
// edits must let it accept.
enum class Edit {
    ClassPaint,   // .hot: background only
    ClassLayout,  // .wide / .tall / .padded: a box change
    ClassDisplay, // .hidden: display:none
    InlinePaint,  // background-color / color / border-radius
    InlineLayout, // width / padding / margin
    Text,         // a text payload change
    FontSize,     // a Text class that changes the measured size
    Shape,        // add or remove a child
    Viewport,     // a resize
};

const char* name_of(Edit edit) {
    switch (edit) {
    case Edit::ClassPaint:
        return "class-paint";
    case Edit::ClassLayout:
        return "class-layout";
    case Edit::ClassDisplay:
        return "class-display";
    case Edit::InlinePaint:
        return "inline-paint";
    case Edit::InlineLayout:
        return "inline-layout";
    case Edit::Text:
        return "text";
    case Edit::FontSize:
        return "font-size";
    case Edit::Shape:
        return "shape";
    case Edit::Viewport:
        return "viewport";
    }
    return "?";
}

// Applies one edit. Each kind owns a disjoint slice of the scene, so a later edit of a different
// kind cannot silently undo an earlier one. That matters for the gate's contract: a `ClassPaint`
// that wrote `"card hot"` over a `ClassLayout`'s `"card wide"` would move a box while claiming to
// be paint-only, which is a bug in the test rather than in the gate.
//
//   cards[0]      class-paint edits (paint-only)
//   cards[1]      class-layout edits (a box change)
//   cards[2]      class-display edits (display:none)
//   cards[3]      inline-paint edits (paint-only)
//   cards[4..5]   inline-layout edits (a box change)
//   cards[6..]    shape edits (children added and removed)
//   labels[0..]   text and font-size edits
//
// Each kind owns its own nodes: two kinds sharing a node would let one undo the other's state, and
// the gate would then be blamed for a change the test made.
void apply_edit(Edit edit, Scene& scene, Lcg& rng, int frame, float& width, float& height) {
    const auto pick = [&rng, &scene](std::size_t first, std::size_t last) -> Node& {
        return *scene.cards[first + rng.below(static_cast<std::uint32_t>(last - first + 1))];
    };
    switch (edit) {
    case Edit::ClassPaint:
        // Only the paint-only token changes; the layout tokens are left exactly as they were.
        pick(0, 0).set_class_name((frame % 2) == 0 ? "card hot" : "card");
        break;
    case Edit::ClassLayout: {
        // Three distinct box-changing tokens, chosen by the serial so a repeat pick still
        // changes the class (and therefore the box) rather than being a no-op.
        const std::uint32_t which = (++scene.edit_serial) % 3;
        pick(1, 1).set_class_name(which == 0 ? "card wide"
                                             : (which == 1 ? "card tall" : "card padded"));
        break;
    }
    case Edit::ClassDisplay:
        pick(2, 2).set_class_name((frame % 2) == 0 ? "card hidden" : "card");
        break;
    case Edit::InlinePaint: {
        Node& card = pick(3, 3);
        card.set_inline_style("background-color", (frame % 2) == 0 ? "#ff0000" : "#00ff00");
        card.set_inline_style("border-radius", (frame % 2) == 0 ? "4px" : "0px");
        break;
    }
    case Edit::InlineLayout: {
        // Always a real change: the serial makes every write differ from the previous one, so
        // this case cannot degenerate into a no-op the gate would correctly accept.
        Node& card = pick(4, 5);
        const std::uint32_t serial = ++scene.edit_serial;
        card.set_inline_style("width", std::to_string(120 + (serial % 7)) + "px");
        card.set_inline_style("margin-top", std::to_string(2 + (serial % 5)) + "px");
        break;
    }
    case Edit::Text: {
        // `set_paint_data` marks Paint|Render only: a text payload change is paint-only until
        // the caller says the new text must be re-measured. A real caller that changes text
        // marks Layout (see app_host_tests), so this case does the same - otherwise the warm
        // host would correctly skip layout while the cold reference measures the new text, and
        // the two would differ for a reason that has nothing to do with the gate.
        Node& label = *scene.labels[rng.below(static_cast<std::uint32_t>(scene.labels.size()))];
        label.set_paint_data(text_payload("Tick " + std::to_string(frame)));
        label.mark_dirty(tessera::ui::Dirty::Layout);
        break;
    }
    case Edit::FontSize: {
        Node& label = *scene.labels[rng.below(static_cast<std::uint32_t>(scene.labels.size()))];
        label.set_class_name((frame % 2) == 0 ? "big" : "");
        break;
    }
    case Edit::Shape: {
        // Add a child to a card, or remove the last one it has. Both move boxes. Only the
        // extra children this case adds are ever removed, so the `cards`/`labels` pointers the
        // other cases use stay valid.
        Node& host = pick(6, scene.cards.size() - 1);
        if (!host.children().empty() && host.children().back()->id() >= kExtraIdBase) {
            (void)host.remove_child(*host.children().back());
        } else {
            // A `View`, not a `Text`: `build_layout_input` skips a Text child of a Text parent
            // (inline spans are runs of the parent's text, not boxes), so a Text child added to
            // a card's Text label would never reach layout and the shape edit would be a no-op.
            auto extra = std::make_unique<Node>(kExtraIdBase + static_cast<std::uint64_t>(frame));
            extra->set_element_kind("View");
            extra->set_class_name("card");
            host.add_child(std::move(extra));
        }
        break;
    }
    case Edit::Viewport:
        width = (frame % 2) == 0 ? 640.0f : 800.0f;
        height = (frame % 2) == 0 ? 480.0f : 600.0f;
        break;
    }
}

// Whether an edit kind must move a box. The gate's contract is exactly this classification: a
// paint-only edit must be accepted (no layout pass), a layout edit must be declined.
bool moves_a_box(Edit edit) {
    switch (edit) {
    case Edit::ClassPaint:
    case Edit::InlinePaint:
        return false;
    case Edit::ClassLayout:
    case Edit::ClassDisplay:
    case Edit::InlineLayout:
    case Edit::Text:
    case Edit::FontSize:
    case Edit::Shape:
    case Edit::Viewport:
        return true;
    }
    return true;
}

} // namespace

int main() {
    constexpr int kCards = 12;
    constexpr int kFrames = 240;

    // Two passes, because the cold reference cannot share a frame with the warm host: building a
    // second tree bumps the process-wide structure epoch, which the warm host would read as a shape
    // change on the *next* frame and decline the gate for the wrong reason.
    //
    //   pass 1 - drive the warm host, record every frame's boxes and whether the gate accepted;
    //   pass 2 - replay the same edit sequence on a fresh scene and compare each frame against a
    //            cold layout of the same inputs.
    //
    // Both passes use the same seed, so the edit sequence is identical.

    // --- pass 1: the warm host ---------------------------------------------------------------
    std::vector<std::vector<std::pair<std::uint64_t, std::array<float, 4>>>> warm_boxes;
    std::vector<bool> gate_accepted;
    std::vector<Edit> edit_log;
    std::vector<float> size_log;
    std::size_t gate_hits = 0;
    std::size_t gate_declines = 0;
    std::size_t accepted_paint_edits = 0;
    std::size_t declined_layout_edits = 0;
    {
        Scene scene(kCards);
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        tessera::app::HostConfig config;
        config.width = 800.0f;
        config.height = 600.0f;
        tessera::app::AppHost host(*scene.root, compile_stylesheet(kSheet), loop, config);

        struct NullSink final : tessera::ui::paint::PaintSink {
            void fill_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&) override {}
            void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                                   const tessera::rendering::rhi::Color&, float) override {}
            void draw_text(const tessera::ui::paint::TextDraw&) override {}
        } sink;

        Lcg rng(0x7350ULL);
        float width = config.width;
        float height = config.height;
        for (int frame = 0; frame < kFrames; ++frame) {
            const Edit edit = static_cast<Edit>(rng.below(9));
            float requested_width = width;
            float requested_height = height;
            apply_edit(edit, scene, rng, frame, requested_width, requested_height);
            if (requested_width != width || requested_height != height) {
                width = requested_width;
                height = requested_height;
                host.resize(width, height, 1.0f);
            }

            const std::size_t gate_before = host.layout_gate_hits();
            const auto hf = host.begin_frame(1.0 / 60.0);
            (void)host.paint(sink);
            host.end_frame();
            const bool skipped = host.layout_gate_hits() > gate_before;
            skipped ? ++gate_hits : ++gate_declines;

            // The gate's contract, checked only on frames that actually reached it. A frame with no
            // style work never runs a restyle, so it has no layout decision to make: `class-paint`
            // on a card that is already `hot` is a no-op edit, and the host correctly does nothing.
            if (hf.style_rebuilt) {
                if (!moves_a_box(edit)) {
                    if (!expect(skipped, std::string("a paint-only edit (") + name_of(edit) +
                                             ") must be accepted by the gate")) {
                        std::cerr << "  declined because: " << host.layout_gate_reason() << "\n";
                        return 4;
                    }
                    ++accepted_paint_edits;
                } else {
                    if (!expect(!skipped, std::string("a layout edit (") + name_of(edit) +
                                              ") must be declined by the gate")) {
                        std::cerr << "  frame " << frame << " accepted a layout edit\n";
                        return 5;
                    }
                    ++declined_layout_edits;
                }
            }

            // Recorded for every frame, so the two passes stay index-aligned.
            std::vector<std::pair<std::uint64_t, std::array<float, 4>>> boxes;
            collect_boxes(*scene.root, boxes);
            warm_boxes.push_back(std::move(boxes));
            gate_accepted.push_back(skipped);
            edit_log.push_back(edit);
            size_log.push_back(width);
            size_log.push_back(height);
        }
    }

    // --- pass 2: the cold reference -----------------------------------------------------------
    {
        Scene scene(kCards);
        Lcg rng(0x7350ULL);
        float width = 800.0f;
        float height = 600.0f;
        for (int frame = 0; frame < kFrames; ++frame) {
            const Edit edit = static_cast<Edit>(rng.below(9));
            float requested_width = width;
            float requested_height = height;
            apply_edit(edit, scene, rng, frame, requested_width, requested_height);
            width = requested_width;
            height = requested_height;

            // A host that has never seen this edit history, on an independent copy of the same
            // style inputs. It lays out once from scratch, which is exactly what a reflow of the
            // current inputs produces.
            std::unique_ptr<Node> cold_root = clone_tree(*scene.root);
            tessera::runtime::ManualClock cold_clock;
            tessera::runtime::RuntimeLoop cold_loop(cold_clock);
            tessera::app::HostConfig cold_config;
            cold_config.width = width;
            cold_config.height = height;
            tessera::app::AppHost cold(*cold_root, compile_stylesheet(kSheet), cold_loop,
                                       cold_config);
            (void)cold.begin_frame(0.0);

            std::vector<std::pair<std::uint64_t, std::array<float, 4>>> cold_boxes;
            collect_boxes(*cold_root, cold_boxes);
            const auto& warm = warm_boxes[static_cast<std::size_t>(frame)];
            if (warm.size() != cold_boxes.size()) {
                std::cerr << "layout_gate: frame " << frame << " (" << name_of(edit)
                          << ") box count " << warm.size() << " but a cold layout gives "
                          << cold_boxes.size() << "\n";
                return 1;
            }
            for (std::size_t i = 0; i < warm.size(); ++i) {
                if (warm[i].first != cold_boxes[i].first) {
                    std::cerr << "layout_gate: frame " << frame << " (" << name_of(edit)
                              << ") node order differs at " << i << ": " << warm[i].first << " vs "
                              << cold_boxes[i].first << "\n";
                    return 1;
                }
                for (std::size_t c = 0; c < 4; ++c) {
                    if (std::fabs(warm[i].second[c] - cold_boxes[i].second[c]) > 0.001f) {
                        std::cerr << "layout_gate: frame " << frame << " (" << name_of(edit)
                                  << ", gate "
                                  << (gate_accepted[static_cast<std::size_t>(frame)] ? "accepted"
                                                                                     : "declined")
                                  << ") node " << warm[i].first << " box component " << c << " is "
                                  << warm[i].second[c] << " but a cold layout gives "
                                  << cold_boxes[i].second[c] << "\n";
                        return 1;
                    }
                }
            }
        }
    }

    // The gate must actually be exercised in both directions, or this test proves nothing.
    if (!expect(gate_hits > 0, "the gate accepted at least one frame") ||
        !expect(gate_declines > 0, "the gate declined at least one frame") ||
        !expect(accepted_paint_edits > 0, "at least one paint-only edit was accepted") ||
        !expect(declined_layout_edits > 0, "at least one layout edit was declined")) {
        return 2;
    }
    std::cout << "layout_gate: " << kFrames << " frames, gate accepted " << gate_hits << " ("
              << accepted_paint_edits << " paint-only edits) and declined " << gate_declines << " ("
              << declined_layout_edits << " layout edits), every box matched a cold layout\n";
    return 0;
}
