// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/container_query_tracker.hpp"

#include <algorithm>

namespace tessera::ui::layout {
namespace {

bool names_contain(const std::string& names, std::string_view wanted) {
    std::size_t pos = 0;
    while (pos < names.size()) {
        const std::size_t end = names.find(' ', pos);
        const std::size_t stop = end == std::string::npos ? names.size() : end;
        if (std::string_view(names).substr(pos, stop - pos) == wanted) {
            return true;
        }
        pos = stop + 1;
    }
    return false;
}

void mark_descendants_style_dirty(Node& node) {
    for (const auto& child : node.children()) {
        child->mark_dirty(Dirty::Style);
        mark_descendants_style_dirty(*child);
    }
}

} // namespace

void ContainerQueryTracker::add(Node& container, bool block_axis, std::string_view names) {
    for (Entry& entry : entries_) {
        if (entry.node == &container) {
            entry.block_axis = block_axis;
            entry.names = std::string(names);
            return;
        }
    }
    entries_.push_back(Entry{&container, block_axis, std::string(names), false, {}});
}

void ContainerQueryTracker::remove(const Node& container) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.node == &container; }),
                   entries_.end());
}

void ContainerQueryTracker::clear() {
    entries_.clear();
}

std::size_t ContainerQueryTracker::update(const LayoutReadModel& model,
                                          const std::vector<style::ContainerQuery>& queries) {
    std::size_t changed = 0;
    for (Entry& entry : entries_) {
        const std::optional<LayoutInfo> info = model.find(*entry.node);
        if (!info) {
            continue;
        }
        const style::ContainerSize now{info->content_box.width, info->content_box.height,
                                       entry.block_axis};
        bool crossed = !entry.styled;
        if (entry.styled) {
            for (const style::ContainerQuery& query : queries) {
                const bool relevant = query.name.empty() || names_contain(entry.names, query.name);
                if (relevant && style::evaluate(query, entry.last) != style::evaluate(query, now)) {
                    crossed = true;
                    break;
                }
            }
        }
        // The remembered size is the one the descendants were styled at, so it only moves when
        // they are re-styled; a move inside one breakpoint keeps the old value, which is
        // equivalent for every query.
        if (crossed) {
            entry.last = now;
            entry.styled = true;
            mark_descendants_style_dirty(*entry.node);
            ++changed;
        }
    }
    return changed;
}

bool ContainerQueryTracker::content_size(void* context, const Node& container,
                                         style::ContainerSize& out) {
    const auto* model = static_cast<const LayoutReadModel*>(context);
    const std::optional<LayoutInfo> info = model->find(container);
    if (!info) {
        return false;
    }
    out.width = info->content_box.width;
    out.height = info->content_box.height;
    return true;
}

} // namespace tessera::ui::layout
