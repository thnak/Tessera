// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/announcement_queue.hpp"

#include <algorithm>

namespace tessera::ui::accessibility {
namespace {

std::size_t channel_index(LiveChannel channel) {
    switch (channel) {
    case LiveChannel::Off:
        return 0;
    case LiveChannel::Polite:
        return 1;
    case LiveChannel::Assertive:
        return 2;
    }
    return 0;
}

} // namespace

bool AnnouncementQueue::announce(LiveChannel channel, std::string_view text, std::uint64_t scope) {
    if (channel == LiveChannel::Off || text.empty()) {
        return false;
    }
    // De-duplication: the same text on the same channel as the last announcement is the same
    // message a screen reader already read, so re-announcing it would repeat it. A component that
    // re-renders on every keystroke is the case this exists for.
    const std::size_t index = channel_index(channel);
    if (last_[index] == text) {
        return false;
    }
    Announcement announcement;
    announcement.channel = channel;
    announcement.scope = scope;
    announcement.text.assign(text.substr(0, kMaxTextBytes));
    last_[index] = announcement.text;
    if (queued_.size() >= kMaxQueued) {
        // Drop the oldest polite announcement; never drop an assertive one.
        const auto polite = std::find_if(queued_.begin(), queued_.end(), [](const Announcement& a) {
            return a.channel == LiveChannel::Polite;
        });
        if (polite != queued_.end()) {
            queued_.erase(polite);
        } else {
            return false; // the queue is full of assertive announcements
        }
    }
    queued_.push_back(std::move(announcement));
    return true;
}

bool AnnouncementQueue::take_next(Announcement& out) {
    if (queued_.empty()) {
        return false;
    }
    // An assertive announcement interrupts, so it goes first; within a channel the queue order
    // holds.
    const auto assertive =
        std::find_if(queued_.begin(), queued_.end(), [](const Announcement& announcement) {
            return announcement.channel == LiveChannel::Assertive;
        });
    const auto chosen = assertive != queued_.end() ? assertive : queued_.begin();
    out = std::move(*chosen);
    queued_.erase(chosen);
    return true;
}

const Announcement* AnnouncementQueue::peek() const {
    if (queued_.empty()) {
        return nullptr;
    }
    const auto assertive =
        std::find_if(queued_.begin(), queued_.end(), [](const Announcement& announcement) {
            return announcement.channel == LiveChannel::Assertive;
        });
    return assertive != queued_.end() ? &*assertive : &queued_.front();
}

void AnnouncementQueue::clear() {
    queued_.clear();
    // The last-text record survives a clear: a component that clears and re-announces the same
    // message is still repeating itself.
}

void AnnouncementQueue::clear_scope(std::uint64_t scope) {
    queued_.erase(std::remove_if(queued_.begin(), queued_.end(),
                                 [scope](const Announcement& announcement) {
                                     return announcement.scope == scope;
                                 }),
                  queued_.end());
}

std::string_view AnnouncementQueue::last_text(LiveChannel channel) const {
    return last_[channel_index(channel)];
}

} // namespace tessera::ui::accessibility
