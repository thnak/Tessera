// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* phase_name(tessera::ui::EventPhase phase) {
    switch (phase) {
    case tessera::ui::EventPhase::Capture:
        return "capture";
    case tessera::ui::EventPhase::Target:
        return "target";
    case tessera::ui::EventPhase::Bubble:
        return "bubble";
    }
    return "unknown";
}

struct Recorder : tessera::ui::Node {
    using Node::Node;

    std::vector<std::string> log;
    bool stop_at_this_node = false;

    void on_event(tessera::ui::Event& event) override {
        log.push_back(std::to_string(id()) + ":" + phase_name(event.phase));
        if (stop_at_this_node) {
            event.stop_propagation();
        }
    }
};

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    using tessera::ui::Event;
    using tessera::ui::EventType;
    using tessera::ui::Node;

    Recorder root(1);
    root.set_bounds({0, 0, 500, 500});

    auto panel_ptr = std::make_unique<Recorder>(2);
    panel_ptr->set_bounds({10, 10, 200, 200});
    panel_ptr->set_transform(tessera::rendering::rhi::Mat3::translation(10, 10));
    auto& panel = static_cast<Recorder&>(root.add_child(std::move(panel_ptr)));

    auto button_ptr = std::make_unique<Recorder>(3);
    button_ptr->set_bounds({20, 20, 100, 50});
    button_ptr->set_transform(tessera::rendering::rhi::Mat3::translation(5, 5));
    auto& button = static_cast<Recorder&>(panel.add_child(std::move(button_ptr)));

    const tessera::rendering::rhi::Rect world = button.world_bounds();
    if (!check(world.x == 45 && world.y == 45 && world.width == 100 && world.height == 50,
               "world bounds")) {
        return 1;
    }

    if (!check(root.hit_test_deep(50, 50) == &button, "deep hit test")) {
        return 2;
    }

    Event event{EventType::PointerDown, 50, 50};
    if (!check(tessera::ui::dispatch_event(root, event), "event dispatch")) {
        return 3;
    }

    const std::vector<std::string> expected_root = {"1:capture", "1:bubble"};
    const std::vector<std::string> expected_panel = {"2:capture", "2:bubble"};
    const std::vector<std::string> expected_button = {"3:target"};
    if (!check(root.log == expected_root && panel.log == expected_panel &&
                   button.log == expected_button,
               "capture/target/bubble order")) {
        return 4;
    }

    root.log.clear();
    panel.log.clear();
    button.log.clear();
    button.stop_at_this_node = true;
    Event stopped{EventType::PointerDown, 50, 50};
    tessera::ui::dispatch_event(root, stopped);
    const std::vector<std::string> stopped_root = {"1:capture"};
    const std::vector<std::string> stopped_panel = {"2:capture"};
    const std::vector<std::string> stopped_button = {"3:target"};
    if (!check(root.log == stopped_root && panel.log == stopped_panel &&
                   button.log == stopped_button,
               "stop propagation")) {
        return 5;
    }

    panel.set_visible(false);
    root.log.clear();
    panel.log.clear();
    button.log.clear();
    if (!check(root.hit_test_deep(50, 50) == &root, "invisible subtree is skipped")) {
        return 6;
    }

    root.set_bounds({0, 0, 10, 10});
    if (!check(root.hit_test_deep(400, 400) == nullptr, "outside bounds")) {
        return 7;
    }

    std::cout << "scene_graph: ok\n";
    return 0;
}
