// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/live_capsule_host.hpp"

#include <algorithm>

namespace tessera::ui::capsule {

namespace {

void clamp_actions(Slots& slots) {
    if (slots.actions.size() > kMaxActions) {
        slots.actions.resize(kMaxActions);
    }
}

} // namespace

const LiveCapsuleHost::Entry* LiveCapsuleHost::front_of(ActivityForm form) const noexcept {
    const Entry* best = nullptr;
    for (const Entry& entry : entries_) {
        if (entry.activity.form != form) {
            continue;
        }
        if (best == nullptr || entry.activity.priority > best->activity.priority ||
            (entry.activity.priority == best->activity.priority && entry.arrival < best->arrival)) {
            best = &entry;
        }
    }
    return best;
}

const Activity* LiveCapsuleHost::front() const noexcept {
    const Entry* entry = front_of(ActivityForm::Compact);
    return entry != nullptr ? &entry->activity : nullptr;
}

const Activity* LiveCapsuleHost::minimal() const noexcept {
    const Entry* entry = front_of(ActivityForm::Minimal);
    return entry != nullptr ? &entry->activity : nullptr;
}

const Activity* LiveCapsuleHost::find(std::string_view id) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.activity.id == id) {
            return &entry.activity;
        }
    }
    return nullptr;
}

std::vector<std::string> LiveCapsuleHost::ordered_ids() const {
    std::vector<const Entry*> sorted;
    sorted.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        sorted.push_back(&entry);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) {
        if (a->activity.form != b->activity.form) {
            return a->activity.form < b->activity.form;
        }
        if (a->activity.priority != b->activity.priority) {
            return a->activity.priority > b->activity.priority;
        }
        return a->arrival < b->arrival;
    });
    std::vector<std::string> ids;
    ids.reserve(sorted.size());
    for (const Entry* entry : sorted) {
        ids.push_back(entry->activity.id);
    }
    return ids;
}

std::optional<Duration> LiveCapsuleHost::next_deadline() const noexcept {
    std::optional<Duration> earliest;
    for (const Entry& entry : entries_) {
        if (entry.deadline && (!earliest || *entry.deadline < *earliest)) {
            earliest = entry.deadline;
        }
    }
    return earliest;
}

void LiveCapsuleHost::announce(std::string_view text) {
    if (announcements_ != nullptr && !text.empty()) {
        announcements_->announce(accessibility::LiveChannel::Polite, text);
    }
}

bool LiveCapsuleHost::start(Activity activity) {
    if (activity.id.empty()) {
        return false;
    }
    clamp_actions(activity.slots);
    const Activity* old_front = front();
    const std::string old_front_id = old_front != nullptr ? old_front->id : std::string{};

    for (Entry& entry : entries_) {
        if (entry.activity.id == activity.id) {
            // In place: the id, priority, form and arrival keep their meaning; label, slots and
            // timeout are the new ones. No second announcement.
            entry.activity.label = std::move(activity.label);
            entry.activity.end_message = std::move(activity.end_message);
            entry.activity.slots = std::move(activity.slots);
            // A new timeout restarts the countdown; none keeps the one already running.
            if (activity.timeout) {
                entry.activity.timeout = activity.timeout;
                entry.deadline = now_ + *activity.timeout;
            }
            ++revision_;
            return true;
        }
    }

    Entry entry;
    entry.deadline =
        activity.timeout ? std::optional<Duration>(now_ + *activity.timeout) : std::nullopt;
    entry.arrival = next_arrival_++;
    entry.activity = std::move(activity);
    const std::string label = entry.activity.label;
    entries_.push_back(std::move(entry));
    ++revision_;
    const Activity* new_front = front();
    if (new_front != nullptr && new_front->id != old_front_id) {
        ++front_generation_;
    }
    announce(label);
    return true;
}

bool LiveCapsuleHost::update(std::string_view id, Slots slots) {
    clamp_actions(slots);
    for (Entry& entry : entries_) {
        if (entry.activity.id == id) {
            entry.activity.slots = std::move(slots);
            ++revision_;
            return true;
        }
    }
    return false;
}

bool LiveCapsuleHost::remove_at(std::size_t index) {
    const Activity* old_front = front();
    const std::string old_front_id = old_front != nullptr ? old_front->id : std::string{};
    Entry removed = std::move(entries_[index]);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    ++revision_;
    const Activity* new_front = front();
    const std::string new_front_id = new_front != nullptr ? new_front->id : std::string{};
    if (new_front_id != old_front_id) {
        ++front_generation_;
    }
    announce(removed.activity.end_message.empty() ? removed.activity.label + " ended"
                                                  : removed.activity.end_message);
    return true;
}

bool LiveCapsuleHost::end(std::string_view id) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].activity.id == id) {
            return remove_at(i);
        }
    }
    return false;
}

std::size_t LiveCapsuleHost::tick(Duration now) {
    if (now > now_) {
        now_ = now;
    }
    std::size_t ended = 0;
    for (std::size_t i = 0; i < entries_.size();) {
        if (entries_[i].deadline && *entries_[i].deadline <= now_) {
            remove_at(i);
            ++ended;
        } else {
            ++i;
        }
    }
    return ended;
}

} // namespace tessera::ui::capsule
