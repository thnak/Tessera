// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/edit_buffer.hpp"

#include <string>
#include <string_view>

namespace tessera::ui::text {

// An IME preedit in progress (docs/architecture/god-type-audit-v1.md section 3.7). The controller
// used to keep the five fields that make up this state inline and open-code the
// start/update/commit/ cancel transitions in `handle_event`; owning them as one type keeps the
// invariant together:
//
//   - the preedit text is NOT part of the buffer until a commit;
//   - `start`/`end` are the half-open buffer byte range the composition replaces (equal offsets
//     mean "insert at `start`");
//   - `CompositionEnd` without a commit cancels and restores the pre-edit state.
//
// `PreeditRange` is the borrowed view the paint pass reads to decorate the uncommitted run.
struct PreeditRange {
    EditOffset begin = 0;
    EditOffset end = 0;
    std::string_view text;
    bool active = false;
};

class CompositionState {
public:
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] std::string_view preedit() const noexcept {
        return preedit_;
    }
    [[nodiscard]] EditOffset start() const noexcept {
        return start_;
    }
    [[nodiscard]] EditOffset end() const noexcept {
        return end_;
    }

    // Begins a composition over the current selection: the replaced range is the selection the
    // caller passes, and the preedit starts empty.
    void begin(EditOffset start, EditOffset end) {
        start_ = start;
        end_ = end;
        preedit_.clear();
        active_ = true;
    }

    // Replaces the uncommitted preedit bytes. An update without an active composition is a no-op,
    // so a platform that sends an update first cannot fabricate a range.
    void update(std::string_view preedit) {
        if (!active_) {
            return;
        }
        preedit_.assign(preedit);
    }

    // Ends the composition. Returns the byte range it replaced so the caller can apply the final
    // text; the returned range stays valid after the state resets.
    [[nodiscard]] PreeditRange finish() {
        const PreeditRange range{start_, end_, {}, false};
        active_ = false;
        preedit_.clear();
        start_ = 0;
        end_ = 0;
        return range;
    }

    // Cancels without committing (CompositionEnd with no commit, or an external set_text).
    void cancel() noexcept {
        active_ = false;
        preedit_.clear();
        start_ = 0;
        end_ = 0;
    }

    // The view the paint pass consumes. A collapsed composition reports the caret position as an
    // empty range at the caret and is only marked active while composing.
    [[nodiscard]] PreeditRange range(EditOffset caret) const {
        if (!active_) {
            return PreeditRange{caret, caret, {}, false};
        }
        return PreeditRange{start_, end_, preedit_, true};
    }

private:
    std::string preedit_;
    EditOffset start_ = 0;
    EditOffset end_ = 0;
    bool active_ = false;
};

} // namespace tessera::ui::text
