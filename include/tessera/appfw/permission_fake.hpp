// SPDX-License-Identifier: MIT
//
// `FakePermissionBackend`: the deterministic in-tree back end (docs/specs/permissions-v1.md
// sections 5, 9 and 10; issue #451). It ships in the library rather than in `tests/unit` for the
// reason `FakeNet`, `FakeShell` and `FakeFileSystem` do: it fakes the module's *seam*, so a
// headless run, an app's own test and the studio's emulator panel (#448) share one definition of
// what a permission answer is.
//
// **Determinism, stated precisely.** A scripted prompt carries a delay in milliseconds, measured on
// the injected `Clock`, never a wall clock and never a sleep. The fake enqueues the prompt and a
// test advances a `ManualClock`, calls `deliver_due()`, then drains the completion queue. Time is
// an argument, so "the prompt is still up" is a fact the test asserts rather than waits for.
//
// **What it reproduces.** Every state of section 4 is reachable with data alone:
//
//   * a scripted `Granted`, `Denied`, `PermanentlyDenied` or `Restricted` answer (`push_outcome`);
//   * a prompt that never answers, so cancellation and staleness are testable (`push_no_answer`);
//   * a permission this platform does not have (`set_supported`), which is `Unsupported`;
//   * a permission granted by presence with no prompt (`set_promptless`), which is the desktop row
//   of
//     section 3 and the case that proves the module does not invent a dialogue;
//   * a platform failure (`push_failure`), which is `PermissionErrorKind::Platform`.
//
// Every call it receives is recorded in `received()`, in order, so a test asserts that the module
// *did not ask* - which is how the four "do not ask" rules of section 5 are proven rather than
// assumed.

#pragma once

#include "tessera/appfw/permission.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// One recorded call. The state a test asks about is "did the module call the platform, and for
// what", and a counter alone cannot answer the second half.
struct FakePermissionCall {
    Permission permission = Permission::Camera;
    // The rationale the module forwarded, so a test asserts the app's copy reached the platform.
    std::string rationale;
    bool cancelled_on_arrival = false;

    [[nodiscard]] friend bool operator==(const FakePermissionCall&,
                                         const FakePermissionCall&) = default;
};

// One scripted prompt answer.
struct FakePermissionScript {
    // The state the prompt resolves to. `Unknown` with `has_error` is a platform failure.
    PermissionState state = PermissionState::Granted;
    PermissionError error{};
    bool has_error = false;
    // How long after the call the prompt resolves, on the injected clock.
    std::uint32_t delay_ms = 0;
    // True to leave this prompt unanswered forever, which is how a test reaches cancellation and a
    // service destroyed with a request in flight.
    bool never_answers = false;
};

class FakePermissionBackend final : public PermissionBackend {
public:
    // The queue is where every completion is posted (seam contract 2: never inline) and the clock
    // is what a delay is measured on. Both are borrowed and must outlive the fake.
    FakePermissionBackend(runtime::CompletionQueue& completions, runtime::Clock& clock);

    FakePermissionBackend(const FakePermissionBackend&) = delete;
    FakePermissionBackend& operator=(const FakePermissionBackend&) = delete;

    // --- scripting ---------------------------------------------------------------------------

    // Appends one scripted answer. Answers are consumed in the order they were pushed; a call
    // beyond the script is refused (`request` returns false), which is how a test notices it asked
    // for more than it scripted.
    void push_outcome(PermissionState state, std::uint32_t delay_ms = 0);
    void push_denied(std::uint32_t delay_ms = 0);
    void push_permanently_denied(std::uint32_t delay_ms = 0);
    void push_restricted(std::uint32_t delay_ms = 0);
    void push_failure(std::string detail = "platform", std::uint32_t delay_ms = 0);
    // A prompt that never answers. The module's cancellation path is what resolves a request whose
    // back end went away.
    void push_no_answer();

    // The states `status()` reports. `set_state` is the "the user changed the answer in Settings
    // while the app was backgrounded" case that `refresh()` exists for.
    void set_state(Permission permission, PermissionState state) noexcept;

    // Section 3's platform rows. `supported = false` models a platform with no such permission
    // (`Unsupported`); `promptless = true` models the desktop rows, where a request resolves
    // `Granted` with no dialogue at all.
    void set_supported(Permission permission, bool supported) noexcept;
    void set_promptless(Permission permission, bool promptless) noexcept;
    // The whole capability set at once, for a test that models one platform in one line.
    void set_capabilities(PermissionBackendCapabilities capabilities) noexcept;

    // --- driving -----------------------------------------------------------------------------

    // Delivers every prompt whose due instant is at or before `clock().now_seconds()`, in call
    // order. Delivering means *posting* the completion, so the caller must then drain the
    // completion queue - the two stages stay separate, exactly as the real seam separates them.
    // Returns how many were delivered.
    std::size_t deliver_due();

    // Drops the script and every pending prompt.
    void clear();

    // --- observation -------------------------------------------------------------------------

    [[nodiscard]] const std::vector<FakePermissionCall>& received() const noexcept {
        return received_;
    }
    // Prompts still up (started and not yet due, and not `never_answers`).
    [[nodiscard]] std::size_t in_flight() const noexcept {
        return pending_.size();
    }
    // Times the module called `request` for this permission. The "do not ask" rules are proven by
    // this being 0.
    [[nodiscard]] std::size_t requests_for(Permission permission) const noexcept;
    // Prompts that arrived with an already-cancelled token, plus those cancelled after they
    // started.
    [[nodiscard]] std::size_t cancelled_prompts() const noexcept;
    [[nodiscard]] std::size_t scripted() const noexcept {
        return script_.size();
    }

    // `PermissionBackend`.
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] PermissionBackendCapabilities capabilities() const override;
    [[nodiscard]] PermissionState status(Permission permission) override;
    bool request(Permission permission, PermissionRequestOptions options,
                 runtime::CancellationToken token, PermissionCompletion on_done) override;

private:
    struct Pending {
        Permission permission = Permission::Camera;
        runtime::CancellationToken token;
        PermissionCompletion on_done;
        FakePermissionScript scripted;
        double due_seconds = 0.0;
    };

    runtime::CompletionQueue* completions_ = nullptr;
    runtime::Clock* clock_ = nullptr;
    PermissionBackendCapabilities capabilities_;
    std::array<PermissionState, kPermissionCount> states_{};
    std::vector<FakePermissionScript> script_;
    std::vector<Pending> pending_;
    std::vector<FakePermissionCall> received_;
    std::size_t script_cursor_ = 0;
    std::size_t cancelled_prompts_ = 0;
};

} // namespace tessera::appfw
