// SPDX-License-Identifier: MIT
//
// The permission seam and its value types (docs/specs/permissions-v1.md sections 3, 4 and 6;
// issue #451). This header is the *whole* platform boundary for permissions, exactly as
// `net_transport.hpp` is for the network module and `shell_backend.hpp` for the shell: a back end
// turns a permission and a rationale into a state, and names no platform type to do it.
//
// **What the seam owns, and what the module owns.** The split is what lets the contract suite pin
// behaviour once instead of per platform:
//
// | Rule (spec section) | Owner | Why |
// |---|---|---|
// | 4, the state model | **module** | one state machine, or five platforms disagree |
// | 5 rule 2, a granted permission does not prompt | **module** | it is a policy about the answer,
// not the OS | | 5 rule 3, `PermanentlyDenied` never prompts | **module** | the rule that stops an
// app nagging | | 5 rule 4, `Restricted` never prompts | **module** | a policy refusal cannot be
// retried | | 5 rule 5, a pending request coalesces | **module** | one prompt, one answer, N
// callers | | 5 rule 1, never block the UI thread | back end + caller | only the back end knows
// what blocks | | 8, the platform mapping | back end | the platform is the only thing that knows |
// | 7 rule 2, `NotDeclared` | **module** | the declaration is the app's, not the platform's |
//
// **The contract every back end keeps.**
//
//   1. `status()` is a pure query: it never prompts, never blocks and never allocates beyond the
//      state it returns.
//   2. `request()` returns at once and never runs `on_done` on its own stack - not even for an
//   answer
//      it already knows. The completion is posted through the injected `CompletionQueue`, so the
//      app's callback runs on the UI thread.
//   3. It observes the `CancellationToken`. A cancelled request may report `Cancelled` or nothing
//   at
//      all; reporting nothing is legal because the module has already resolved the operation.
//   4. It reports what the platform says, never what was asked. A prompt the user dismissed without
//      choosing is `Denied`, not `Granted` (spec section 4: the safe reading).
//   5. It does not implement the state rules. A back end that returned `Granted` on a
//      `PermanentlyDenied` request would be a bug in the back end, and the module's own tests would
//      not catch it - which is why the module never asks in that state.

#pragma once

#include "tessera/runtime/async.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {

// Spec section 3's closed set. A permission that is not here does not exist, which is what keeps
// the module from becoming a stringly-typed pass-through to each OS. The enumerator values are
// indices into the fixed per-permission state table, so adding one is an append and never a
// renumbering.
enum class Permission : std::uint8_t {
    Camera = 0,
    Microphone,
    Location,
    Notifications,
    PhotoLibrary,
    FileAccess,
};

inline constexpr std::size_t kPermissionCount = 6;

// Spec section 4's five states. The order is the order of the table there, and `NotDetermined` is
// zero because that is the state of a fresh install.
enum class PermissionState : std::uint8_t {
    // Never asked, or the OS has no answer yet.
    NotDetermined = 0,
    // Allowed now.
    Granted,
    // Refused this time; the OS will ask again.
    Denied,
    // Refused; the OS will not ask again. Only Settings can change it.
    PermanentlyDenied,
    // Blocked by policy the user cannot change (parental controls, MDM, an enterprise profile).
    Restricted,
    // The request was cancelled or the platform call failed, so no state was learned. The previous
    // state is unchanged; this value exists only in a result, never in the stored table.
    Unknown,
};

[[nodiscard]] constexpr std::string_view permission_name(Permission permission) noexcept {
    switch (permission) {
    case Permission::Camera:
        return "camera";
    case Permission::Microphone:
        return "microphone";
    case Permission::Location:
        return "location";
    case Permission::Notifications:
        return "notifications";
    case Permission::PhotoLibrary:
        return "photo-library";
    case Permission::FileAccess:
        return "file-access";
    }
    return "unknown";
}

// The inverse, for the project-config parser (spec section 7). False for an id outside the set,
// which is the config error the spec requires naming.
[[nodiscard]] constexpr bool permission_from_name(std::string_view name, Permission& out) noexcept {
    constexpr Permission kAll[] = {
        Permission::Camera,        Permission::Microphone,   Permission::Location,
        Permission::Notifications, Permission::PhotoLibrary, Permission::FileAccess,
    };
    for (const Permission permission : kAll) {
        if (permission_name(permission) == name) {
            out = permission;
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr std::string_view permission_state_name(PermissionState state) noexcept {
    switch (state) {
    case PermissionState::NotDetermined:
        return "not-determined";
    case PermissionState::Granted:
        return "granted";
    case PermissionState::Denied:
        return "denied";
    case PermissionState::PermanentlyDenied:
        return "permanently-denied";
    case PermissionState::Restricted:
        return "restricted";
    case PermissionState::Unknown:
        return "unknown";
    }
    return "unknown";
}

// The one predicate an app should branch on: the feature may run.
[[nodiscard]] constexpr bool permission_usable(PermissionState state) noexcept {
    return state == PermissionState::Granted;
}

// The bit a permission occupies in a `PermissionConfig::declared` set or a capability mask.
// Declared here rather than next to one consumer, because the config parser, the service and the
// fake all speak the same mask.
[[nodiscard]] constexpr std::uint64_t permission_mask(Permission permission) noexcept {
    return std::uint64_t{1} << static_cast<std::uint8_t>(permission);
}

// Every permission in the set, which is what a test that is not about the declaration rule wants.
[[nodiscard]] constexpr std::uint64_t all_permissions_mask() noexcept {
    return (std::uint64_t{1} << kPermissionCount) - 1;
}

// True when asking again is honest: the OS will show a prompt. `PermanentlyDenied` and `Restricted`
// are exactly the states where it will not, and offering "try again" to either is the bug spec
// section 4 rule 2 and rule 9 item 2 exist to prevent.
[[nodiscard]] constexpr bool permission_askable(PermissionState state) noexcept {
    return state == PermissionState::NotDetermined || state == PermissionState::Denied;
}

// Spec section 6's error vocabulary. A user's refusal is a `PermissionState`, never one of these:
// errors are for the module failing, which is a different thing to report and a different thing to
// retry.
enum class PermissionErrorKind : std::uint8_t {
    // This build or platform has no such permission. Note that spec section 3's `none` column is
    // NOT
    // this: a desktop with no permission model reports `Granted`, because it has nothing to refuse.
    Unsupported = 0,
    // The project config does not list it (spec section 7). A deployment bug, not a user one, which
    // is why it is its own kind: the manifest generator has to add the entry.
    NotDeclared,
    // The request cannot be asked in the current state where the platform is ambiguous.
    InvalidState,
    // The platform call itself failed. `detail` is a stable, non-localised token.
    Platform,
};

[[nodiscard]] constexpr std::string_view
permission_error_kind_name(PermissionErrorKind kind) noexcept {
    switch (kind) {
    case PermissionErrorKind::Unsupported:
        return "unsupported";
    case PermissionErrorKind::NotDeclared:
        return "not-declared";
    case PermissionErrorKind::InvalidState:
        return "invalid-state";
    case PermissionErrorKind::Platform:
        return "platform";
    }
    return "unknown";
}

struct PermissionError {
    PermissionErrorKind kind = PermissionErrorKind::Platform;
    // Short, stable, non-localised. It never carries a path, a rationale or user data (spec section
    // 10 rule 10 and the diagnostics privacy rule of security.md section 8).
    std::string detail;

    [[nodiscard]] friend bool operator==(const PermissionError&, const PermissionError&) = default;
};

// Spec section 5's `PermissionRequestOptions`.
struct PermissionRequestOptions {
    // The app's own sentence, shown *before* the OS prompt. Empty is legal and means "the
    // platform's own copy"; the module never writes prose, which is why it needs no message
    // catalogue.
    std::string rationale;
    // Whether a `Denied` permission may be asked again. An app that has shown its own "not now"
    // screen opts out; the module does not count retries itself (spec section 5 rule 7).
    bool retry_after_denial = true;
};

// What one request resolved to. `state` is `Unknown` exactly when `error` is set.
struct PermissionResult {
    Permission permission = Permission::Camera;
    PermissionState state = PermissionState::Unknown;
    // Set only when the module or the platform failed. A user's refusal is a state, not this.
    PermissionError error{};
    bool has_error = false;

    [[nodiscard]] bool ok() const noexcept {
        return !has_error;
    }
    [[nodiscard]] friend bool operator==(const PermissionResult&,
                                         const PermissionResult&) = default;
};

// What a back end reports about what it can do. `has_prompt` is the spec's per-platform
// distinction: false for the desktop rows whose "request" resolves without a dialogue (section 3),
// which the module turns into a `Granted` answer without calling `request`.
struct PermissionBackendCapabilities {
    // The permissions this build has a back end for at all.
    std::uint64_t supported = 0;
    // "A request needs no prompt on this platform": the permission is granted by presence.
    std::uint64_t promptless = 0;

    [[nodiscard]] bool supports(Permission permission) const noexcept {
        return (supported & (std::uint64_t{1} << static_cast<std::uint8_t>(permission))) != 0;
    }
    [[nodiscard]] bool is_promptless(Permission permission) const noexcept {
        return (promptless & (std::uint64_t{1} << static_cast<std::uint8_t>(permission))) != 0;
    }

    [[nodiscard]] friend constexpr bool operator==(const PermissionBackendCapabilities&,
                                                   const PermissionBackendCapabilities&) = default;
};

// What a request completion delivers. A back end reports a state (or an error), never a result: the
// module owns the permission the request was for and the "was there a prompt" bookkeeping.
using PermissionCompletion = std::function<void(std::expected<PermissionState, PermissionError>)>;

// The platform half. One implementation per platform; `FakePermissionBackend`
// (`permission_fake.hpp`) is the in-tree one the contract suite and the studio panel run against.
class PermissionBackend {
public:
    PermissionBackend() = default;
    PermissionBackend(const PermissionBackend&) = delete;
    PermissionBackend& operator=(const PermissionBackend&) = delete;
    virtual ~PermissionBackend() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // What this platform supports and what needs no prompt. Cached by the module at construction
    // and refreshed by `refresh_capabilities()`.
    [[nodiscard]] virtual PermissionBackendCapabilities capabilities() const = 0;

    // The current state, without prompting. `NotDetermined` for a permission never asked. Must not
    // prompt or block (contract 1).
    [[nodiscard]] virtual PermissionState status(Permission permission) = 0;

    // Asks the platform. Returns false when the back end cannot serve the request at all, in which
    // case `on_done` is never invoked and the module reports `Unsupported`. Otherwise it returns
    // true and posts exactly one completion.
    virtual bool request(Permission permission, PermissionRequestOptions options,
                         runtime::CancellationToken token, PermissionCompletion on_done) = 0;
};

} // namespace tessera::appfw
