// SPDX-License-Identifier: MIT

#include "tessera/appfw/permission_fake.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace tessera::appfw {
namespace {

std::size_t permission_index(Permission permission) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint8_t>(permission));
}

} // namespace

FakePermissionBackend::FakePermissionBackend(runtime::CompletionQueue& completions,
                                             runtime::Clock& clock)
    : completions_(&completions),
      clock_(&clock) {
    // The desktop default of section 3: every permission in the set is supported and every one
    // needs a prompt. A test that models another platform calls `set_supported`/`set_promptless`,
    // so the per-platform rows are explicit rather than a default nobody reads.
    capabilities_.supported = all_permissions_mask();
    capabilities_.promptless = 0;
    states_.fill(PermissionState::NotDetermined);
}

void FakePermissionBackend::push_outcome(PermissionState state, std::uint32_t delay_ms) {
    FakePermissionScript script;
    script.state = state;
    script.delay_ms = delay_ms;
    script_.push_back(std::move(script));
}

void FakePermissionBackend::push_denied(std::uint32_t delay_ms) {
    push_outcome(PermissionState::Denied, delay_ms);
}

void FakePermissionBackend::push_permanently_denied(std::uint32_t delay_ms) {
    push_outcome(PermissionState::PermanentlyDenied, delay_ms);
}

void FakePermissionBackend::push_restricted(std::uint32_t delay_ms) {
    push_outcome(PermissionState::Restricted, delay_ms);
}

void FakePermissionBackend::push_failure(std::string detail, std::uint32_t delay_ms) {
    FakePermissionScript script;
    script.state = PermissionState::Unknown;
    script.error = PermissionError{PermissionErrorKind::Platform, std::move(detail)};
    script.has_error = true;
    script.delay_ms = delay_ms;
    script_.push_back(std::move(script));
}

void FakePermissionBackend::push_no_answer() {
    FakePermissionScript script;
    script.state = PermissionState::Unknown;
    script.never_answers = true;
    script_.push_back(std::move(script));
}

void FakePermissionBackend::set_state(Permission permission, PermissionState state) noexcept {
    states_[permission_index(permission)] = state;
}

void FakePermissionBackend::set_supported(Permission permission, bool supported) noexcept {
    const std::uint64_t bit = permission_mask(permission);
    if (supported) {
        capabilities_.supported |= bit;
    } else {
        capabilities_.supported &= ~bit;
    }
}

void FakePermissionBackend::set_promptless(Permission permission, bool promptless) noexcept {
    const std::uint64_t bit = permission_mask(permission);
    if (promptless) {
        capabilities_.promptless |= bit;
    } else {
        capabilities_.promptless &= ~bit;
    }
}

void FakePermissionBackend::set_capabilities(PermissionBackendCapabilities capabilities) noexcept {
    capabilities_ = capabilities;
}

std::size_t FakePermissionBackend::deliver_due() {
    std::vector<Pending> due;
    std::vector<Pending> still_pending;
    still_pending.reserve(pending_.size());
    for (Pending& entry : pending_) {
        // A prompt that never answers is never due; it stays in flight until the service is
        // destroyed or its token is cancelled.
        if (!entry.scripted.never_answers && entry.due_seconds <= clock_->now_seconds()) {
            due.push_back(std::move(entry));
        } else {
            still_pending.push_back(std::move(entry));
        }
    }
    pending_ = std::move(still_pending);

    for (Pending& entry : due) {
        if (entry.scripted.never_answers) {
            continue;
        }
        PermissionCompletion on_done = std::move(entry.on_done);
        const FakePermissionScript scripted = std::move(entry.scripted);
        // Posting, never calling inline: seam contract 2, and the separation a test relies on to
        // see "the platform answered" and "the app heard it" as two steps.
        (void)completions_->post(runtime::CompletionPriority::Visible,
                                 [on_done = std::move(on_done), scripted]() mutable {
                                     if (!on_done) {
                                         return;
                                     }
                                     if (scripted.has_error) {
                                         on_done(std::unexpected(scripted.error));
                                         return;
                                     }
                                     on_done(scripted.state);
                                 });
    }
    return due.size();
}

void FakePermissionBackend::clear() {
    script_.clear();
    pending_.clear();
    script_cursor_ = 0;
}

std::size_t FakePermissionBackend::requests_for(Permission permission) const noexcept {
    std::size_t count = 0;
    for (const FakePermissionCall& call : received_) {
        if (call.permission == permission) {
            ++count;
        }
    }
    return count;
}

std::size_t FakePermissionBackend::cancelled_prompts() const noexcept {
    return cancelled_prompts_;
}

std::string_view FakePermissionBackend::name() const noexcept {
    return "fake";
}

PermissionBackendCapabilities FakePermissionBackend::capabilities() const {
    return capabilities_;
}

PermissionState FakePermissionBackend::status(Permission permission) {
    return states_[permission_index(permission)];
}

bool FakePermissionBackend::request(Permission permission, PermissionRequestOptions options,
                                    runtime::CancellationToken token,
                                    PermissionCompletion on_done) {
    const bool cancelled = token.cancelled();
    received_.push_back(FakePermissionCall{permission, std::move(options.rationale), cancelled});
    if (cancelled) {
        ++cancelled_prompts_;
    }

    // A call beyond the script is refused, which is the "the test asked for more than it scripted"
    // signal. Returning false makes the module report `Unsupported`, so the mistake surfaces as a
    // typed failure instead of a hang.
    if (script_cursor_ >= script_.size()) {
        return false;
    }
    FakePermissionScript scripted = script_[script_cursor_++];
    const double due_seconds =
        clock_->now_seconds() + static_cast<double>(scripted.delay_ms) / 1000.0;
    pending_.push_back(
        Pending{permission, token, std::move(on_done), std::move(scripted), due_seconds});
    return true;
}

} // namespace tessera::appfw
