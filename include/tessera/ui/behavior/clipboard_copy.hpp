// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <array>
#include <cstddef>

namespace tessera::hal {
class Clipboard;
}

// `clipboard` (ADR-064 item 7, issue #632): copy text with feedback state.
//
// The engine owns the mechanism: `hal::Clipboard` is the platform seam (UTF-8 text only, bounded,
// failures as values, never a blocking wait), and `MemoryClipboard` is the deterministic test
// implementation. This behavior is the hook's half: it calls `write_text`, reports whether the copy
// succeeded, and clears the feedback after a bounded window so a "Copied" label does not stay on
// screen forever.
//
// **Why the behavior holds the clipboard, not the component.** A component that reached the HAL
// directly would need a platform handle in generated code, which ADR-020 forbids. The behavior is
// engine code, so it may hold the seam; the component only reads the outputs.
//
// **Why the reset is a deadline, not a timer.** The behavior reads the clock through the runtime
// clock interface (behavior-hooks-v1.md section 8 rule 4), so a `ManualClock` test is
// deterministic. `refresh` compares the current time against the deadline and clears the feedback
// when it passes; no thread, no timer, no callback.
namespace tessera::ui::behavior {

class ClipboardCopy final : public Behavior {
public:
    // The manifest's output order: `copied`, `failed`.
    static constexpr std::size_t kOutputCount = 2;
    // How long the feedback stays up, in seconds. Long enough to read, short enough not to linger.
    static constexpr double kFeedbackSeconds = 2.0;

    ClipboardCopy() = default;
    ~ClipboardCopy() override;

    ClipboardCopy(const ClipboardCopy&) = delete;
    ClipboardCopy& operator=(const ClipboardCopy&) = delete;

    void attach(const Targets& targets, const Services& services, const ArgsView& args) override;
    void update(const ArgsView& args) override;
    void tick(double now_s) override;
    void detach() override;
    [[nodiscard]] std::span<const OutputValue> outputs() const override;

    // Copies `text` through the seam and starts the feedback window. Returns whether the platform
    // accepted it. Public so a test can drive a copy without a scene.
    bool copy(std::string_view text, double now_s);

    // The seam this behavior writes through, or nullptr when the build has none.
    [[nodiscard]] const hal::Clipboard* clipboard() const {
        return clipboard_;
    }

private:
    hal::Clipboard* clipboard_ = nullptr;
    double deadline_s_ = 0.0;
    bool feedback_active_ = false;
    std::array<OutputValue, kOutputCount> outputs_{};
};

} // namespace tessera::ui::behavior
