// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::pending {

// The fake resource of spike #661: one keyed record with the data store's status/epoch rules
// (docs/specs/async-data-v1.md sections 1 and 6), reduced to what the pending UI reads.
//
// It is deliberately not the data store: the store is a mutex-guarded, observer-driven cache with a
// completion queue, and a Suspense boundary needs none of that. What it *does* need is the two
// facts the store already keeps per key - "a fetch is in flight" and "the epoch a completion must
// still match" - which is why this type is the store's contract in miniature and not a second
// scheduler.
//
// Header-only and allocation-free: the evaluator (which links only the IR) and generated C++ (which
// links the scene) run this one implementation, so the differential test compares wiring rather
// than two copies of the same arithmetic.
class FakeResource {
public:
    // A key change or an explicit refresh. Bumps the epoch and marks a fetch in flight; the
    // published data, if any, is kept, which is what makes the status `Refreshing` rather than
    // `Loading`. Returns the new epoch, the value a completion must carry.
    constexpr std::uint64_t begin_fetch() noexcept {
        ++epoch_;
        loading_ = true;
        return epoch_;
    }

    // A completion for `epoch`. False when it is stale (a newer fetch started) or when no fetch is
    // in flight: the caller drops it, exactly as `FetchHandle::resolve` returns false for a
    // superseded fetch.
    constexpr bool resolve(std::uint64_t epoch, int value) noexcept {
        if (!loading_ || epoch != epoch_) {
            return false;
        }
        value_ = value;
        has_data_ = true;
        loading_ = false;
        return true;
    }

    // A failed completion for `epoch`. The previous data is kept (async-data-v1 section 6), so a
    // failed refresh does not blank the screen.
    constexpr bool fail(std::uint64_t epoch) noexcept {
        if (!loading_ || epoch != epoch_) {
            return false;
        }
        loading_ = false;
        failed_ = true;
        return true;
    }

    [[nodiscard]] constexpr std::uint64_t epoch() const noexcept {
        return epoch_;
    }
    [[nodiscard]] constexpr bool loading() const noexcept {
        return loading_;
    }
    [[nodiscard]] constexpr bool has_data() const noexcept {
        return has_data_;
    }
    [[nodiscard]] constexpr bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] constexpr int value() const noexcept {
        return value_;
    }

    // A suspending read: a fetch is in flight and there is nothing to show yet.
    [[nodiscard]] constexpr bool suspends() const noexcept {
        return loading_ && !has_data_;
    }
    // Refreshing: a fetch is in flight and the previous data is still shown.
    [[nodiscard]] constexpr bool refreshing() const noexcept {
        return loading_ && has_data_;
    }

private:
    std::uint64_t epoch_ = 0;
    int value_ = 0;
    bool loading_ = false;
    bool has_data_ = false;
    bool failed_ = false;
};

} // namespace tessera::ui::pending
