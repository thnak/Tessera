// SPDX-License-Identifier: MIT

#pragma once

// LiveCapsuleHost (docs/specs/live-capsule-v1.md section 4, issue #224): the app-level queue of
// activities. The component draws the front of it. `start`, `update` and `end` are the only
// operations; an activity ends explicitly or when its timeout expires. UI thread only: from other
// threads the caller posts the call through the completion queue.

#include "tessera/ui/accessibility/announcement_queue.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::capsule {

enum class Priority : std::uint8_t { Low, Normal, High };

// Which visible slot an activity takes: one compact (or expanded) plus one minimal at a time.
enum class ActivityForm : std::uint8_t { Compact, Minimal };

using Duration = std::chrono::nanoseconds;

struct ActionSlot {
    std::string label;
    std::function<void()> on_press;
};

// Ordinary component content: each builder mounts into the node of its part.
struct Slots {
    std::function<void(Node&)> minimal_icon;
    std::function<void(Node&)> leading;
    std::function<void(Node&)> trailing;
    std::function<void(Node&)> content;
    std::string title;
    // At most two are kept; the rest are dropped.
    std::vector<ActionSlot> actions;
    // Fallback sizes in logical pixels, used only when the capsule has no slot measurer
    // (`LiveCapsule::set_slot_measurer`): the leading and trailing slot widths of the compact
    // state and the card of the expanded one.
    float leading_width = 0.0f;
    float trailing_width = 0.0f;
    float card_width = 280.0f;
    float card_height = 120.0f;
};

struct Activity {
    // Stable; starting with an id that is already live updates the activity in place.
    std::string id;
    // The accessible name, announced politely when the activity starts.
    std::string label;
    // Announced when the activity ends; empty uses "<label> ended".
    std::string end_message;
    Priority priority = Priority::Normal;
    ActivityForm form = ActivityForm::Compact;
    Slots slots;
    std::optional<Duration> timeout;
};

inline constexpr std::size_t kMaxActions = 2;

class LiveCapsuleHost {
public:
    // `announcements` is borrowed (the window's queue) and may be null.
    explicit LiveCapsuleHost(accessibility::AnnouncementQueue* announcements = nullptr) noexcept
        : announcements_(announcements) {}

    // Queues a new activity, or replaces the slots of the live one with the same id (priority and
    // arrival keep their place). Returns false for an empty id.
    bool start(Activity activity);
    // Replaces the slots of `id` in place and bumps its revision; never announces. False when
    // unknown.
    bool update(std::string_view id, Slots slots);
    // Removes `id` and announces the end once. False when unknown.
    bool end(std::string_view id);

    // Advances the host clock and ends every activity whose timeout expired; returns how many.
    // Timeouts count from the host time of `start`.
    std::size_t tick(Duration now);
    [[nodiscard]] Duration now() const noexcept {
        return now_;
    }
    // The earliest timeout deadline, for the pacer wake; empty when none.
    [[nodiscard]] std::optional<Duration> next_deadline() const noexcept;

    // The front compact activity (priority, then arrival) and the front minimal one.
    [[nodiscard]] const Activity* front() const noexcept;
    [[nodiscard]] const Activity* minimal() const noexcept;
    [[nodiscard]] const Activity* find(std::string_view id) const noexcept;
    // Every live id in display order: compact ones first, then minimal, each by priority then
    // arrival.
    [[nodiscard]] std::vector<std::string> ordered_ids() const;
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }
    // Bumps whenever the front or minimal activity changes or its slots change, so the component
    // rebuilds only then.
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    // Bumps only when a different activity takes the front (a new front resets the capsule state).
    [[nodiscard]] std::uint64_t front_generation() const noexcept {
        return front_generation_;
    }

private:
    struct Entry {
        Activity activity;
        std::uint64_t arrival = 0;
        std::optional<Duration> deadline;
    };

    [[nodiscard]] const Entry* front_of(ActivityForm form) const noexcept;
    void announce(std::string_view text);
    bool remove_at(std::size_t index);

    accessibility::AnnouncementQueue* announcements_ = nullptr;
    std::vector<Entry> entries_;
    std::uint64_t next_arrival_ = 1;
    std::uint64_t revision_ = 0;
    std::uint64_t front_generation_ = 0;
    Duration now_{};
};

} // namespace tessera::ui::capsule
