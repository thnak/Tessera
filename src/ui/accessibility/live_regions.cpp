// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/live_regions.hpp"

#include <string_view>
#include <unordered_map>

namespace tessera::ui::a11y {
namespace {

std::string_view live_text(const A11yNode& node) {
    const std::string_view name = name_of(node);
    return name.empty() ? value_of(node) : name;
}

accessibility::LiveChannel channel_of(Live live) {
    switch (live) {
    case Live::Polite:
        return accessibility::LiveChannel::Polite;
    case Live::Assertive:
        return accessibility::LiveChannel::Assertive;
    case Live::Off:
        break;
    }
    return accessibility::LiveChannel::Off;
}

} // namespace

std::size_t collect_live_announcements(const A11ySnapshot& previous, const A11ySnapshot& next,
                                       accessibility::AnnouncementQueue& queue) {
    if (previous.nodes.empty()) {
        return 0;
    }
    std::unordered_map<NodeId, std::string_view> before;
    before.reserve(previous.nodes.size());
    for (const A11yNode& node : previous.nodes) {
        before.emplace(node.id, live_text(node));
    }
    std::size_t accepted = 0;
    for (const A11yNode& node : next.nodes) {
        if (node.live == Live::Off) {
            continue;
        }
        const std::string_view text = live_text(node);
        if (text.empty()) {
            continue;
        }
        const auto found = before.find(node.id);
        if (found != before.end() && found->second == text) {
            continue;
        }
        if (queue.announce(channel_of(node.live), text)) {
            ++accepted;
        }
    }
    return accepted;
}

} // namespace tessera::ui::a11y
