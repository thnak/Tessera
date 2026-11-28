// SPDX-License-Identifier: MIT

#pragma once

#include <utility>

namespace tessera::ui::pending {

// The settled value of `useDeferredValue` (ADR-067 item 5, docs/specs/pending-ui-v1.md section 6):
// one stored scalar. `read(current, held)` returns `current` and remembers it unless `held` (a
// transition of the same component has a pending read), in which case it returns the value
// remembered by the last unheld read. The first read always settles, so a value that was never
// observed before a hold starts is its argument, not a default.
//
// Header-only for the same reason as `SuspenseBoundary`: the evaluator and generated C++ run this
// one rule. `T` is a bool, an int, a double or a std::string (an enum-like token); assigning an
// equal-or-shorter string reuses the capacity, so a steady-state update allocates nothing.
template<typename T> class DeferredScalar {
public:
    const T& read(const T& current, bool held) {
        if (!held || !settled_once_) {
            settled_ = current;
            settled_once_ = true;
        }
        return settled_;
    }

    [[nodiscard]] const T& settled() const noexcept {
        return settled_;
    }

private:
    T settled_{};
    bool settled_once_ = false;
};

} // namespace tessera::ui::pending
