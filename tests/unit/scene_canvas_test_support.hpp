// SPDX-License-Identifier: MIT

#pragma once

// Shared fixtures for the canvas-session tests (issue #388): the sources, the tree helpers, the
// on-demand frame harness, and the failure counter. Two test translation units share it so each
// stays inside the source-size budget: `scene_canvas_session_tests.cpp` covers the session and its
// frame accounting, `scene_canvas_pointer_tests.cpp` covers the pointer path into the style engine.
//
// Nothing here re-implements the engine: the colour a test asserts on comes from
// `style::SceneStyleTree::compute()` over the session's own sheet, so a passing assertion is
// evidence that the cascade ran.

#include "canvas_report.hpp"
#include "scene_canvas_session.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace canvas_test {

namespace cli = tessera::compiler::codegen::cli;
namespace null = tessera::rendering::null;
namespace rhi = tessera::rendering::rhi;
namespace ui = tessera::ui;

// One counter for both translation units, so `main()` can report every failure once.
inline int& failures() {
    static int count = 0;
    return count;
}

inline void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_canvas_session: " << message << "\n";
        ++failures();
    }
}

// A button whose handler changes state: the cheapest source that exercises the whole path, and the
// shape the parity suite's `Counter` uses.
inline constexpr std::string_view kCounter = R"(export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Count</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)";

// A second button below the first: the "release somewhere else" case needs a target the press did
// not hit.
inline constexpr std::string_view kTwoButtons = R"(export function TwoButtons(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Button onClick={() => setCount(count + 1)}>Left</Button>
      <Button>Right</Button>
    </View>
  );
}
)";

inline cli::CanvasSessionSource source_for(std::string_view text, std::string_view file) {
    cli::CanvasSessionSource source;
    source.text = text;
    source.file = file;
    source.salt = "preview";
    source.width = 320.0f;
    source.height = 240.0f;
    source.dpi_scale = 1.0f;
    return source;
}

inline const ui::Node* find_kind(const ui::Node& node, std::string_view kind) {
    if (node.element_kind() == kind) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_kind(*child, kind)) {
            return found;
        }
    }
    return nullptr;
}

// The node carrying `class_name`, anywhere in the tree. A CSS module's scoped class is what the
// lowered tree carries, so a test that asserts on it must find the node by that name.
inline const ui::Node* find_class(const ui::Node& node, std::string_view class_name) {
    if (node.class_name() == class_name) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_class(*child, class_name)) {
            return found;
        }
    }
    return nullptr;
}

inline std::vector<const ui::Node*> find_all(const ui::Node& node, std::string_view kind) {
    std::vector<const ui::Node*> found;
    if (node.element_kind() == kind) {
        found.push_back(&node);
    }
    for (const auto& child : node.children()) {
        for (const ui::Node* nested : find_all(*child, kind)) {
            found.push_back(nested);
        }
    }
    return found;
}

// The computed `background-color` of `node` through the real cascade. If this returns a different
// colour after a pointer event, the stylesheet matched - no class was written by any test helper.
inline std::string background_of(const cli::CanvasSceneSession& session, const ui::Node& node) {
    ui::style::SceneStyleTree tree(*session.root());
    const ui::style::ComputedStyle style = tree.compute(session.sheet(), node);
    const std::string* value = style.find("background-color");
    return value != nullptr ? *value : std::string();
}

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

inline Point centre_of(const ui::Node& node) {
    const rhi::Rect bounds = node.world_bounds();
    return Point{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f};
}

// A point no node covers: outside the 320x240 layout viewport, which is what a `pointerleave` is
// translated into (see `tessera_canvas_pointer`'s contract). A hit test at negative coordinates
// finds nothing, so the router clears the hover exactly as a real pointer leaving the canvas would.
inline constexpr Point kOutside{-1.0f, -1.0f};

// The value of a `state::count`-shaped cell, or a sentinel when the component has none.
inline int counter_value(const cli::CanvasSceneSession& session,
                         std::string_view name = "state::count") {
    for (const ui::StateCell& cell : session.state()) {
        if (cell.name != name) {
            continue;
        }
        if (const int* value = std::get_if<int>(&cell.value)) {
            return *value;
        }
    }
    return -1;
}

// The page's contract, in one object: a null device, one render target, and a `draw()` that paints
// **only when the session says a frame is owed** - the on-demand rule, not a loop. `frames` counts
// what was actually painted, so "every step is one frame" and "a static scene presents nothing" are
// numbers a test asserts instead of prose.
class CanvasHarness {
public:
    CanvasHarness() {
        device_ = null::create_null_device(rhi::DeviceDesc{});
        if (device_ == nullptr) {
            return;
        }
        desc_.extent = {320, 240};
        desc_.format = rhi::TextureFormat::R8G8B8A8Unorm;
        desc_.clear_color = cli::scene_frame_clear_color();
        target_ = device_->create_render_target(desc_);
    }

    [[nodiscard]] bool ready() const {
        return device_ != nullptr && target_ != nullptr;
    }

    // One on-demand frame: paint if, and only if, the session owes one.
    bool draw(cli::CanvasSceneSession& session) {
        if (!ready() || !session.needs_frame()) {
            return false;
        }
        const cli::SceneFrameResult frame = session.paint(*device_, *target_, desc_);
        if (!frame.error.empty()) {
            expect(false, "the frame paints: " + frame.error);
            return false;
        }
        ++frames;
        last_draws = frame.draw_calls;
        last_nodes = frame.nodes;
        return true;
    }

    // The page's whole pointer path: dispatch, then draw at most once. The assertion that the frame
    // the session owed and the frame the page drew agree is the on-demand contract itself.
    cli::CanvasPointerResult event(cli::CanvasSceneSession& session, cli::PreviewInput::Kind kind,
                                   Point at, std::uint32_t button = 0) {
        cli::CanvasPointerResult result = session.pointer(kind, at.x, at.y, button);
        const bool drew = draw(session);
        expect(drew == result.changed, "the frame the session owes matches `changed`");
        return result;
    }

    // A paint the caller forces, for the "a session with no scene reports why" case: it must fail
    // with a reason instead of painting an empty tree.
    cli::SceneFrameResult paint_anyway(cli::CanvasSceneSession& session) {
        if (!ready()) {
            cli::SceneFrameResult none;
            none.error = "the harness has no device";
            return none;
        }
        return session.paint(*device_, *target_, desc_);
    }

    int frames = 0;
    std::uint32_t last_draws = 0;
    std::size_t last_nodes = 0;

private:
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<rhi::RenderTarget> target_;
    rhi::RenderTargetDesc desc_;
};

} // namespace canvas_test
