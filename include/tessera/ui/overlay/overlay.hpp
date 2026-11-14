// SPDX-License-Identifier: MIT

#pragma once

// Overlays and layers (docs/specs/overlays-and-layers-v1.md, issue #302): content above the base
// tree, in four ordered layers. The host owns the layer nodes and the entries inside them; nothing
// is reparented, and nothing opens or closes mid-frame.

#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui {

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

struct OverlaySpec {
    // Becomes the content root's element id and its `aria-label`; may be empty.
    std::string id;
    OverlayLayer layer = OverlayLayer::Overlay;
    // Entries of one layer are ordered by (priority, open order); higher paints above.
    int priority = 0;
    // Builds the content into the content root at the frame boundary the overlay opens on. A
    // throw drops the entry and counts as a failure; it never reaches the frame loop.
    std::function<void(Node& content)> mount;
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

    // Applies pending opens and closes. True when the tree changed.
    bool flush();

    // True from the flush that mounted the entry until close() is called for it.
    [[nodiscard]] bool is_open(OverlayHandle handle) const;
    // Opened or pending entries that are not closing.
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t size(OverlayLayer layer) const;
    // Entries whose mount callback threw since the host was created.
    [[nodiscard]] std::size_t failed_count() const noexcept {
        return failed_;
    }

    // The content root of a mounted entry, or nullptr.
    [[nodiscard]] Node* content(OverlayHandle handle) const;
    // The layer node under the root, or nullptr until the first entry opened.
    [[nodiscard]] Node* layer_node(OverlayLayer layer) const;
    // The topmost mounted modal entry, or an empty handle.
    [[nodiscard]] OverlayHandle top_modal() const;

private:
    struct Entry;

    [[nodiscard]] Entry* find(OverlayHandle handle) const;
    Node& ensure_layer(OverlayLayer layer);
    void mount_entry(Entry& entry);
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
};

} // namespace tessera::ui
