// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

namespace tessera::ui::pending {

// A recoverable pending-model failure (ADR-067 spike). The model never throws and never allocates,
// so the message is a literal and the code is stable and grep-able.
struct PendingError {
    const char* code = "";
    const char* message = "";
};

// One suspending read of a resource, reduced to the two facts the boundary rule needs
// (docs/specs/pending-ui-v1.md section 2): a fetch is in flight, and data is published.
//
// `epoch` is the data store's per-key staleness counter (async-data-v1.md section 1): a completion
// whose epoch no longer matches the current one is dropped, which is what makes a late response for
// a superseded key harmless.
struct SuspenseRead {
    std::uint64_t epoch = 0;
    bool loading = false;
    bool has_data = false;

    // A suspending read: a fetch is in flight and there is nothing to show yet.
    [[nodiscard]] constexpr bool suspends() const noexcept {
        return loading && !has_data;
    }
    // Refreshing: a fetch is in flight and the previous data is still shown.
    [[nodiscard]] constexpr bool refreshing() const noexcept {
        return loading && has_data;
    }
};

// Which subtree a Suspense boundary shows.
enum class SuspensePhase : std::uint8_t { Content, Fallback };

// A Suspense boundary (ADR-067 item 2): a static node with one fallback subtree and one child
// subtree. It owns no scene node and no storage of its own: the generated component owns both
// subtrees and asks this model which one is live, so the decision costs no allocation.
//
// The rule is one line: the boundary shows its fallback while any read it covers suspends, and the
// content otherwise. `Refreshing` does not suspend (ADR-067 item 3), so a key change on a resource
// that already has data keeps the subtree.
//
// Header-only on purpose: the IR evaluator (which links only the IR) and generated C++ (which links
// the scene) must run the *same* state machine for the differential test to mean anything, and a
// header-only model is the only way both can without a new link edge.
class SuspenseBoundary {
public:
    static constexpr std::size_t kMaxReads = 8;

    // Registers `count` read slots, clamped to kMaxReads, and clears them. Total: the generated
    // code knows its read count at compile time, so this is a plain statement there.
    constexpr std::size_t reset_reads(std::size_t count) noexcept {
        count_ = count < kMaxReads ? count : kMaxReads;
        for (SuspenseRead& read : reads_) {
            read = SuspenseRead{};
        }
        phase_ = SuspensePhase::Content;
        hold_ = false;
        return count_;
    }

    // A transition that started a read this boundary covers is pending (pending-ui-v1.md section
    // 4.3): the boundary keeps the subtree it is showing instead of entering its fallback. The
    // hold applies only to the content subtree that was live when the transition began; a boundary
    // already in its fallback stays there, because that is the old UI. Setting it before the reads
    // are published (as `pending_sync` does) is what makes the first suspending frame keep the
    // content. Releasing the hold re-evaluates the rule, so a read still suspending then shows the
    // fallback.
    constexpr void set_hold(bool held) noexcept {
        hold_ = held;
        settle();
    }
    [[nodiscard]] constexpr bool held() const noexcept {
        return hold_;
    }

    // Records the state of one read. An unregistered slot is a recoverable error, never a silent
    // drop: a boundary that quietly ignored a read would show its content while loading.
    [[nodiscard]] constexpr std::expected<void, PendingError>
    set_read(std::size_t slot, bool loading, bool has_data, std::uint64_t epoch) {
        if (slot >= count_) {
            return std::unexpected(PendingError{"TESSERA_SUSPENSE_UNKNOWN_READ",
                                                "the read slot was never registered on this "
                                                "boundary"});
        }
        reads_[slot].loading = loading;
        reads_[slot].has_data = has_data;
        reads_[slot].epoch = epoch;
        settle();
        return {};
    }

    // The read in `slot`, or nullptr when the slot was never registered.
    [[nodiscard]] constexpr const SuspenseRead* read(std::size_t slot) const noexcept {
        return slot < count_ ? &reads_[slot] : nullptr;
    }

    [[nodiscard]] constexpr bool suspends() const noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            if (reads_[slot].suspends()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr SuspensePhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] constexpr bool fallback_live() const noexcept {
        return phase() == SuspensePhase::Fallback;
    }
    [[nodiscard]] constexpr std::size_t read_count() const noexcept {
        return count_;
    }

private:
    // The rule, applied after every change: no suspending read means the content; a suspending read
    // means the fallback unless a transition holds the content that is live.
    constexpr void settle() noexcept {
        if (!suspends()) {
            phase_ = SuspensePhase::Content;
        } else if (!(hold_ && phase_ == SuspensePhase::Content)) {
            phase_ = SuspensePhase::Fallback;
        }
    }

    SuspenseRead reads_[kMaxReads]{};
    std::size_t count_ = 0;
    SuspensePhase phase_ = SuspensePhase::Content;
    bool hold_ = false;
};

} // namespace tessera::ui::pending
