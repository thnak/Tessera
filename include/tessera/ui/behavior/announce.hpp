// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/announcement_queue.hpp"
#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

// `announce` (ADR-064 item 7, issue #629): announce a message to assistive technology.
//
// The engine owns the mechanism: `accessibility::AnnouncementQueue` is the bounded, de-duplicating
// queue the accessibility bridge drains, and the bridge maps a queued announcement to the
// platform's live-region API (UIA's `RaiseAutomationEvent`, AT-SPI's `Announcement`,
// NSAccessibility's `announcementRequested`). This behavior is the hook's half: it turns the
// `message` option into a queue entry on the right channel, and reports whether the message was
// accepted.
//
// **Why the behavior does not hold the queue.** The queue belongs to the window, not to a
// component: a toast that announces "Saved" and then unmounts must still be announced, so the queue
// outlives the behavior that filled it. The behavior writes into the window's queue through
// `Services`.
namespace tessera::ui::behavior {

class Announce final : public Behavior {
public:
    // The manifest's output order: `announced`.
    static constexpr std::size_t kOutputCount = 1;

    Announce() = default;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // The queue this behavior writes into, or nullptr when the build has none.
    [[nodiscard]] const accessibility::AnnouncementQueue* queue() const {
        return queue_;
    }

private:
    // Queues `message` on `channel` and recomputes the output.
    void apply(std::string_view message, accessibility::LiveChannel channel);

    accessibility::AnnouncementQueue* queue_ = nullptr;
    // The last message this behavior queued, so an update that repeats it is a no-op rather than a
    // second queue entry (the queue also de-duplicates, but a behavior that never asks is cheaper).
    std::string last_;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
