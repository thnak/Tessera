// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node_fwd.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <iterator>
#include <memory>
#include <unordered_map>

namespace tessera::ui::style {

// What a style-to-scene sync last applied for each node: the very style (by identity, see
// `ComputedStyle::identity`) and the box it was resolved against. A node whose restyle kept its
// entries - which the style tree's memo guarantees for a node outside every restyled subtree -
// and whose box did not move needs no second pass, so a sync costs one lookup per unchanged node
// instead of re-deriving what it already applied (issue #711). Bracket one walk with `begin` and
// `sweep`; an entry the walk did not touch (a removed or hidden node) is dropped.
class SyncMemo {
public:
    void begin() {
        ++generation_;
    }

    // True when `id` was last applied with exactly this style and box; marks it seen.
    [[nodiscard]] bool hit(NodeId id, const ComputedStyle& style, float width = 0.0f,
                           float height = 0.0f) {
        const auto found = entries_.find(id);
        if (found == entries_.end() || !style.has_identity(found->second.style) ||
            found->second.width != width || found->second.height != height) {
            return false;
        }
        found->second.generation = generation_;
        return true;
    }

    void put(NodeId id, const ComputedStyle& style, float width = 0.0f, float height = 0.0f) {
        entries_[id] = Entry{style.identity(), width, height, generation_};
    }

    void sweep() {
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = it->second.generation == generation_ ? std::next(it) : entries_.erase(it);
        }
    }

    void clear() {
        entries_.clear();
    }

private:
    struct Entry {
        std::shared_ptr<const void> style;
        float width = 0.0f;
        float height = 0.0f;
        std::uint64_t generation = 0;
    };
    std::unordered_map<NodeId, Entry> entries_;
    std::uint64_t generation_ = 0;
};

} // namespace tessera::ui::style
