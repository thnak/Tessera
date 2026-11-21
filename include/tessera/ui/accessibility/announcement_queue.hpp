// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::accessibility {

// The live-region channels of `aria-live` (WAI-ARIA 1.2 section 6.6). `Off` is a region that exists
// but never announces; `Polite` waits for the user to be idle; `Assertive` interrupts.
enum class LiveChannel : std::uint8_t {
    Off,
    Polite,
    Assertive,
};

// One queued announcement.
struct Announcement {
    LiveChannel channel = LiveChannel::Polite;
    std::string text;
    // The node that asked for it, so a modal layer can scope its announcements (issue #629). 0
    // means "the whole window".
    std::uint64_t scope = 0;
};

// The announcement queue behind `useAnnounce` (ADR-064 item 7, issue #629).
//
// **Why a queue and not a node.** An announcement is not content: it has no box, it is never
// painted, and it must survive the component that produced it (a toast that announces "Saved" and
// then unmounts still has to be announced). Putting it on a node would tie its lifetime to the
// scene and make a screen reader's read depend on whether the node was still attached. So the queue
// is its own bounded structure, and the accessibility bridge drains it.
//
// **Why de-duplication.** A form that re-renders on every keystroke would otherwise announce the
// same validation message dozens of times. Two identical texts in a row collapse into one, which is
// what a screen-reader user needs and what the issue asks for.
//
// **Why a cap.** An announcement is a bounded string and the queue is a bounded list, so a runaway
// component cannot grow memory without limit. The oldest polite announcement is dropped first; an
// assertive one is never dropped, because it is the one the user must hear.
class AnnouncementQueue {
public:
    // The most announcements the queue holds. A screen reader cannot usefully read more than a
    // handful before the user acts, and a bounded queue keeps the drain allocation-free.
    static constexpr std::size_t kMaxQueued = 16;
    // The longest text one announcement may carry. A live region is a short status message, not a
    // document; a longer one is truncated rather than rejected, because losing the tail of a
    // message is better than losing the message.
    static constexpr std::size_t kMaxTextBytes = 512;

    // Queues `text` on `channel`. Returns false when the channel is `Off`, the text is empty, or
    // the text is identical to the most recent announcement on the same channel (de-duplication).
    bool announce(LiveChannel channel, std::string_view text, std::uint64_t scope = 0);

    // Removes and returns the next announcement to deliver: an assertive one first, then a polite
    // one in queue order. Returns false when the queue is empty.
    bool take_next(Announcement& out);

    // The next announcement without removing it, or nullptr when the queue is empty.
    [[nodiscard]] const Announcement* peek() const;

    [[nodiscard]] std::size_t size() const {
        return queued_.size();
    }
    [[nodiscard]] bool empty() const {
        return queued_.empty();
    }
    // Drops everything. A modal layer that closes drops the announcements it scoped, so a message
    // about a dialog the user just dismissed is not read afterwards.
    void clear();
    void clear_scope(std::uint64_t scope);

    // The last text announced on `channel`, for the de-duplication rule. Empty when nothing has
    // been announced on it.
    [[nodiscard]] std::string_view last_text(LiveChannel channel) const;

private:
    std::vector<Announcement> queued_;
    // The last text per channel, indexed by `LiveChannel`. Kept after the queue drains, because a
    // re-render that re-announces the same message must still be suppressed.
    std::string last_[3];
};

} // namespace tessera::ui::accessibility
