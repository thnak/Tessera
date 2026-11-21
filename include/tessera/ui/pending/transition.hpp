// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::pending {

// The transition mark (ADR-067 item 4): a setter-call handler marked as a transition. While a
// resource started by that change is `Loading` or `Refreshing`, `pending()` is true and a boundary
// above it keeps the old subtree instead of showing its fallback.
//
// The mark is one epoch and one flag, so it costs no allocation and no indirection. It is the
// generated component's member, not a scene node: `isPending` is a behavior output
// (behavior-hooks-v1.md), read by the expressions that reference it.
//
// Header-only for the same reason as `SuspenseBoundary`: the evaluator and generated C++ run this
// one implementation.
class TransitionMark {
public:
    // `startTransition(() => setQuery(q))`: the marked change starts a transition under `epoch`,
    // the epoch the caller observed for the resource the change writes.
    constexpr void begin(std::uint64_t epoch) noexcept {
        epoch_ = epoch;
        pending_ = true;
        started_ = true;
    }

    // The resource's state after the change. A completion whose epoch is not the current one is
    // dropped (async-data-v1.md section 6): the mark keeps its own epoch and ignores the stale one,
    // so a late response for a superseded key cannot clear a newer transition. Returns whether the
    // observation was accepted.
    constexpr bool observe(bool loading, std::uint64_t epoch) noexcept {
        if (!started_ || epoch != epoch_) {
            return false; // a stale epoch: the transition it belonged to was superseded
        }
        // `Loading` and `Refreshing` are both pending (ADR-067 item 4); a settled read is not.
        pending_ = loading;
        return true;
    }

    [[nodiscard]] constexpr bool pending() const noexcept {
        return pending_;
    }
    [[nodiscard]] constexpr std::uint64_t epoch() const noexcept {
        return epoch_;
    }
    // True when `epoch` is the transition's own epoch: the stale-epoch test, exposed so a caller
    // can drop a completion before it reaches `observe`.
    [[nodiscard]] constexpr bool accept(std::uint64_t epoch) const noexcept {
        return started_ && epoch == epoch_;
    }
    [[nodiscard]] constexpr bool started() const noexcept {
        return started_;
    }

    // The transition settled: `isPending` is false until the next `begin`.
    constexpr void clear() noexcept {
        pending_ = false;
        started_ = false;
    }

private:
    std::uint64_t epoch_ = 0;
    bool pending_ = false;
    bool started_ = false;
};

} // namespace tessera::ui::pending
