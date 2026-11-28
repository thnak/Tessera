// SPDX-License-Identifier: MIT

#pragma once

// LiveCapsule (docs/specs/live-capsule-v1.md, issue #224): the component that draws the front
// activity of a LiveCapsuleHost in the `toast` layer of an OverlayHost. It never takes focus,
// positions itself from the cutout (top-centre fallback), morphs between states on springs and
// idles at zero cost when nothing changed. UI thread only; call `sync` at a frame boundary.

#include "tessera/hal/geometry.hpp"
#include "tessera/hal/window_cutout_fold.hpp"
#include "tessera/ui/capsule/capsule_morph.hpp"
#include "tessera/ui/capsule/capsule_placement.hpp"
#include "tessera/ui/capsule/capsule_state.hpp"
#include "tessera/ui/capsule/live_capsule_host.hpp"
#include "tessera/ui/overlay/overlay.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace tessera::ui::capsule {

// What the capsule needs from the window each frame (a view of `hal::WindowMetrics`).
struct LiveCapsuleEnv {
    float window_width = 0.0f;
    float window_height = 0.0f;
    float top_inset = 0.0f;
    // Top system-gesture inset; a swipe starting inside it is ignored.
    float gesture_inset_top = 0.0f;
    hal::CutoutInfo cutouts;
    hal::WindowRect hinge;
    bool reduced_motion = false;
};

class LiveCapsule {
public:
    // Both are borrowed and must outlive the component.
    LiveCapsule(LiveCapsuleHost& host, OverlayHost& overlays);
    ~LiveCapsule();

    LiveCapsule(const LiveCapsule&) = delete;
    LiveCapsule& operator=(const LiveCapsule&) = delete;

    // Opens or closes the overlay with the host's front, rebuilds the parts when the host or the
    // state changed, advances the morph by `dt` seconds and writes the box. Writes nothing when
    // nothing changed. Returns true while the morph still moves (the pacer must wake).
    bool sync(const LiveCapsuleEnv& env, float dt);

    // Input. Each returns true when the state changed. `focused_node` is the node that held focus.
    bool activate(std::uint64_t focused_node);
    bool escape();
    // A press at (x, y); collapses an expanded capsule when it lands outside the box.
    bool pointer_down(float x, float y);
    bool swipe_up(float start_y);
    // The focus to restore after a collapse; 0 when none.
    [[nodiscard]] std::uint64_t take_focus_return() noexcept {
        return controller_.take_focus_return();
    }

    // Forgets the overlay without closing it, for a capsule whose OverlayHost is torn down before
    // it (the destructor would otherwise close a handle on a dead host). After this the component
    // no longer touches the host and must not be synced again.
    void abandon() noexcept {
        handle_ = {};
    }

    [[nodiscard]] CapsuleState state() const noexcept {
        return controller_.state();
    }
    [[nodiscard]] OverlayHandle handle() const noexcept {
        return handle_;
    }
    // The capsule box in window logical pixels, as last written.
    [[nodiscard]] const hal::WindowRect& rect() const noexcept {
        return rect_;
    }
    [[nodiscard]] bool active() const noexcept {
        return morph_.active();
    }

private:
    void close();
    void rebuild(Node& content, const Activity* front, const Activity* minimal);

    LiveCapsuleHost& host_;
    OverlayHost& overlays_;
    OverlayHandle handle_;
    CapsuleController controller_;
    CapsuleMorph morph_;
    hal::WindowRect rect_;
    CapsuleSize target_;
    float gesture_inset_top_ = 0.0f;
    bool morph_placed_ = false;
    // Owned copies of the action callbacks the part nodes call; stable addresses, cleared with the
    // nodes.
    std::deque<std::function<void()>> action_fns_;
    bool parts_dirty_ = true;
    bool box_dirty_ = true;
    std::uint64_t built_revision_ = ~std::uint64_t{0};
    std::uint64_t generation_ = ~std::uint64_t{0};
    CapsuleState built_state_ = CapsuleState::Compact;
    std::string last_name_;
};

} // namespace tessera::ui::capsule
