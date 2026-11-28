// SPDX-License-Identifier: MIT

#pragma once

// Overlays and layers (docs/specs/overlays-and-layers-v1.md, issue #302): content above the base
// tree, in four ordered layers. The host owns the layer nodes and the entries inside them; nothing
// is reparented, and nothing opens or closes mid-frame.

#include "tessera/ui/overlay/placement.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui {

class FocusManager;

// Paint order, lowest first. The base layer is the app content itself and has no entry here.
enum class OverlayLayer : std::uint8_t {
    Overlay, // non-modal panels and popovers
    Modal,   // dialogs and sheets: inert background, trapped focus
    Tooltip, // transient hint, never focused
    Toast,   // transient status, never focused
};

inline constexpr std::size_t kOverlayLayerCount = 4;

// Names one opened overlay. Sequence numbers are never reused, so a handle to a closed overlay
// stays harmless: every call with it is a no-op.
struct OverlayHandle {
    std::uint64_t sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return sequence != 0;
    }
    bool operator==(const OverlayHandle&) const = default;
};

// Why the host closed an overlay on its own.
enum class OverlayDismissReason : std::uint8_t {
    Escape,  // the Escape key
    Back,    // the platform back gesture or button
    Outside, // a press outside the content
    Blur,    // the window lost focus (transient layers only)
};

// Initial focus and restore for an overlay (docs/specs/overlays-and-layers-v1.md section 10.3). A
// modal always traps focus and restores it through the focus scope; this governs the rest.
enum class OverlayFocus : std::uint8_t {
    Default, // modal: trapped by the scope; every other layer: focus is left alone
    Content, // overlay layer only: focus moves to the first focusable node in the content
             // after layout, and returns to the previous target when the overlay closes
    None,    // never touch focus
};

struct OverlaySpec {
    // Becomes the content root's element id and its `aria-label`; may be empty.
    std::string id;
    OverlayLayer layer = OverlayLayer::Overlay;
    // Entries of one layer are ordered by (priority, open order); higher paints above.
    int priority = 0;
    // Builds the content into the content root at the frame boundary the overlay opens on. A
    // throw drops the entry and counts as a failure; it never reaches the frame loop.
    std::function<void(Node& content)> mount;
    // Positions the content against an anchor at the frame boundary, tracking layout, scroll,
    // resize and the safe area. Unset: the content keeps the layer's default layout (a modal is
    // centred, everything else sits at the top-left).
    std::optional<OverlayPlacement> placement;
    // Unset takes the layer default: Escape and back dismiss everything but a toast; a press
    // outside dismisses an overlay-layer entry, never a dialog (destructive actions need an
    // explicit choice), tooltip or toast.
    std::optional<bool> dismiss_on_escape;
    std::optional<bool> dismiss_on_outside;
    // Honoured on the overlay layer; tooltips and toasts never take focus, a modal uses its trap.
    OverlayFocus focus = OverlayFocus::Default;
    // Failed mount (section 10.3): when set, runs on a content root emptied of whatever the failed
    // mount built, and the entry stays open with that content. When unset, or when it throws too,
    // the entry is dropped.
    std::function<void(Node& content)> fallback;
    // The error-boundary report: called once per failed mount with a deterministic message
    // ("mount threw: <what>" or "mount threw a non-standard exception"), before the fallback runs.
    std::function<void(OverlayHandle, std::string_view message)> on_failure;
    // Overlay and tooltip entries close when the window loses focus unless this is set.
    bool persistent = false;
    // Called when the host dismisses the entry, just before it closes, so app state can follow.
    // Not called for close() and close_if().
    std::function<void(OverlayHandle, OverlayDismissReason)> on_dismiss;
};

// What close_if() shows its predicate.
struct OverlayInfo {
    OverlayHandle handle;
    std::string_view id;
    OverlayLayer layer = OverlayLayer::Overlay;
    int priority = 0;
};

// One host per window root. open/close/close_if only record the request; flush() applies all of
// them, which the app host does at the start of a frame. Not thread-safe: UI thread only.
class OverlayHost {
public:
    // Nodes the host creates take ids from this range, clear of generated and virtual ids.
    static constexpr NodeId kFirstNodeId = NodeId{1} << 62;

    // `root` is borrowed and must outlive the host.
    explicit OverlayHost(Node& root);
    ~OverlayHost();

    OverlayHost(const OverlayHost&) = delete;
    OverlayHost& operator=(const OverlayHost&) = delete;

    // The handle is valid at once; the content mounts on the next flush().
    OverlayHandle open(OverlaySpec spec);
    // False for a handle that is unknown, already closed, or already closing.
    bool close(OverlayHandle handle);
    // Closes every live or pending entry the predicate accepts; returns how many.
    std::size_t close_if(const std::function<bool(const OverlayInfo&)>& predicate);
    std::size_t close_all();

    // The window's focus manager (borrowed, null to detach), which the host needs for
    // OverlayFocus::Content. AppHost wires it when a window supplies one.
    void set_focus_manager(FocusManager* focus) noexcept {
        focus_ = focus;
    }
    // Moves focus into entries that asked for it. Runs after layout, once per frame; free when no
    // entry is waiting. True when focus moved.
    bool apply_focus();

    // Applies pending opens and closes. True when the tree changed.
    bool flush();

    // Dismissal. Escape and back dismiss the topmost entry that opted in, top layer first; a
    // modal that has not opted in stops the search (it owns the keyboard, so nothing below it may
    // react). True when an entry was dismissed.
    bool handle_escape();
    bool handle_back();
    // A press at (x, y), window logical pixels. Entries that opted into outside dismissal and do
    // not contain the press close, top first, down to the first dialog. A press on a tooltip or
    // toast, inside an entry's content, or on the anchor node of an entry never dismisses it, so a
    // trigger can toggle its own popover. The press itself is not consumed. Returns the count.
    std::size_t handle_pointer_down(float x, float y);
    // Window blur: dismisses the overlay and tooltip entries that are not persistent.
    std::size_t close_transient();

    struct PlaceResult {
        bool moved = false;  // some content moved; the caller relayouts once
        bool closed = false; // an anchor node is gone; the entry closes on the next flush
    };
    // Positions every placed entry inside `viewport` (the window minus the safe-area insets).
    // Runs after layout, because it needs the content size and the anchor bounds; the app host
    // calls it once per frame. Free for a scene without placed entries.
    PlaceResult place(const rendering::rhi::Rect& viewport, bool rtl);
    [[nodiscard]] bool has_placed() const;

    // True from the flush that mounted the entry until close() is called for it.
    [[nodiscard]] bool is_open(OverlayHandle handle) const;
    // Opened or pending entries that are not closing.
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t size(OverlayLayer layer) const;
    // Entries whose mount callback threw since the host was created.
    [[nodiscard]] std::size_t failed_count() const noexcept {
        return failed_;
    }

    // The message of the most recent failed mount, empty until one failed.
    [[nodiscard]] std::string_view last_failure() const noexcept {
        return last_failure_;
    }

    // The content root of a mounted entry, or nullptr.
    [[nodiscard]] Node* content(OverlayHandle handle) const;
    // The layer node under the root, or nullptr until the first entry opened.
    [[nodiscard]] Node* layer_node(OverlayLayer layer) const;
    // The topmost mounted modal entry, or an empty handle.
    [[nodiscard]] OverlayHandle top_modal() const;

    // One entry's private state (src/ui/scene/overlay_entry.hpp); named here so the dismissal and
    // placement translation units can share it. Opaque to callers.
    struct Entry;

private:
    [[nodiscard]] Entry* find(OverlayHandle handle) const;
    // The next entry below `above` in stacking order (layer, priority, open order), or the topmost
    // when `above` is null. Mounted, not closing.
    [[nodiscard]] Entry* below(const Entry* above) const;
    bool dismiss_top(OverlayDismissReason reason);
    void dismiss(Entry& entry, OverlayDismissReason reason);
    Node& ensure_layer(OverlayLayer layer);
    void mount_entry(Entry& entry);
    // Runs the mount callback; on a throw reports it and tries the fallback. False: drop the entry.
    bool run_mount(Entry& entry, Node& content);
    void unmount_entry(Entry& entry);
    NodeId next_node_id() noexcept {
        return next_node_id_++;
    }

    Node& root_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::array<Node*, kOverlayLayerCount> layers_{};
    std::uint64_t next_sequence_ = 1;
    NodeId next_node_id_ = kFirstNodeId;
    std::size_t failed_ = 0;
    std::string last_failure_;
    FocusManager* focus_ = nullptr;
};

} // namespace tessera::ui
