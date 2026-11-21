// SPDX-License-Identifier: MIT
//
// `overflow` (ADR-064 item 7, issue #634): measure which children of a row overflow, and report the
// cut index.
//
// The engine owns the measurement (`layout::LayoutReadModel` holds every node's box from the last
// pass); this behavior owns the hook's half: the walk, the reserved trailing slot, and the count.
// These tests publish a layout by hand, so a rule about the cut index is proved by arithmetic
// rather than by running Yoga.

#include "tessera/ui/behavior/overflow.hpp"
#include "tessera/ui/layout/layout_read_model.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Overflow;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutReadModel;
using tessera::ui::layout::LayoutResult;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "overflow: " << message << "\n";
        ++failures;
    }
}

ArgValue number_arg(double value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Number;
    arg.number = value;
    return arg;
}

// The manifest's option order: `reserved`, `gap`.
std::array<ArgValue, 2> options(double reserved = 0.0, double gap = 0.0) {
    return {number_arg(reserved), number_arg(gap)};
}

// A row 300 px wide with `count` children of `width` each.
struct Scene {
    Node root{1};
    Node* row = nullptr;
    std::vector<Node*> items;

    Scene(std::size_t count, float width) {
        root.set_bounds({0, 0, 400, 400});
        auto box = std::make_unique<Node>(2);
        box->set_bounds({0, 0, 300, 40});
        row = &root.add_child(std::move(box));
        for (std::size_t i = 0; i < count; ++i) {
            auto item = std::make_unique<Node>(static_cast<tessera::ui::NodeId>(3 + i));
            item->set_bounds({static_cast<float>(i) * width, 0, width, 40});
            items.push_back(&row->add_child(std::move(item)));
        }
    }

    // Publishes the row and its children at their current bounds.
    void publish(LayoutReadModel& model) const {
        std::vector<LayoutBox> boxes;
        boxes.push_back(LayoutBox{root.id(), {0, 0, 400, 400}});
        boxes.push_back(LayoutBox{row->id(), {0, 0, 300, 40}});
        for (const Node* item : items) {
            const auto bounds = item->bounds();
            boxes.push_back(LayoutBox{item->id(), bounds});
        }
        model.publish(LayoutResult{std::move(boxes)}, 1.0f);
    }
};

void test_all_items_fit() {
    Scene scene(3, 80.0f); // 3 * 80 = 240 <= 300
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    const auto outputs = overflow.outputs();
    expect(outputs.size() == 2, "two outputs");
    expect(outputs[0].kind == OutputValue::Kind::Number, "`count` is a number");
    expect(outputs[0].number == 3.0, "all three items fit");
    expect(!outputs[1].boolean, "nothing overflows");
}

void test_cut_index() {
    Scene scene(5, 80.0f); // 3 * 80 = 240 fits, the fourth would be 320
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    expect(overflow.outputs()[0].number == 3.0, "three of five items fit");
    expect(overflow.outputs()[1].boolean, "the row overflows");
}

void test_reserved_slot() {
    Scene scene(5, 80.0f);
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    // A 100 px "more" button leaves 200 px, so only two items fit.
    auto args = options(100.0);
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    expect(overflow.outputs()[0].number == 2.0, "the reserved slot is subtracted before the fit");
}

void test_gap_is_counted() {
    Scene scene(5, 80.0f);
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    // 300 available, 80 per item, 20 gap: 80 + 100 + 100 = 280 fits three, a fourth is 380.
    auto args = options(0.0, 20.0);
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    expect(overflow.outputs()[0].number == 3.0, "the gap between items is counted");
}

void test_update_recomputes() {
    Scene scene(5, 80.0f);
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    expect(overflow.outputs()[0].number == 3.0, "three fit before the reserved slot");

    auto reserved = options(200.0);
    overflow.update(ArgsView{reserved});
    expect(overflow.outputs()[0].number == 1.0, "a larger reserved slot leaves room for one");
}

void test_no_model_reports_nothing_fits() {
    Scene scene(3, 80.0f);
    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{}, ArgsView{args});
    expect(overflow.outputs()[0].number == 0.0, "with no measurement nothing fits");
    expect(overflow.outputs()[1].boolean, "and the row reports overflow");
}

void test_unmeasured_child_stops_the_walk() {
    Scene scene(3, 80.0f);
    LayoutReadModel model;
    // Publish only the row and the first child: the other two were added since the pass.
    std::vector<LayoutBox> boxes;
    boxes.push_back(LayoutBox{scene.root.id(), {0, 0, 400, 400}});
    boxes.push_back(LayoutBox{scene.row->id(), {0, 0, 300, 40}});
    boxes.push_back(LayoutBox{scene.items[0]->id(), {0, 0, 80, 40}});
    model.publish(LayoutResult{std::move(boxes)}, 1.0f);

    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    expect(overflow.outputs()[0].number == 1.0,
           "the walk stops at the first unmeasured child rather than guessing its width");
}

void test_detach_resets() {
    Scene scene(3, 80.0f);
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{scene.row}}, Services{.layout = &model},
                    ArgsView{args});
    overflow.detach();
    expect(overflow.target() == nullptr, "a detached behavior has no target");
    expect(overflow.outputs()[0].number == 0.0, "a detached behavior reports the manifest default");
    expect(!overflow.outputs()[1].boolean, "both outputs are the default");
}

void test_missing_target_attaches_nothing() {
    Scene scene(3, 80.0f);
    LayoutReadModel model;
    scene.publish(model);
    Overflow overflow;
    auto args = options();
    overflow.attach(Targets{std::array<Node*, 1>{nullptr}}, Services{.layout = &model},
                    ArgsView{args});
    expect(overflow.target() == nullptr, "a behavior with an unresolved target attaches nothing");
}

} // namespace

int main() {
    test_all_items_fit();
    test_cut_index();
    test_reserved_slot();
    test_gap_is_counted();
    test_update_recomputes();
    test_no_model_reports_nothing_fits();
    test_unmeasured_child_stops_the_walk();
    test_detach_resets();
    test_missing_target_attaches_nothing();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
