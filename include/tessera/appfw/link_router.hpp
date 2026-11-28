// SPDX-License-Identifier: MIT
//
// `LinkRouter`: what activating an `a href` does (docs/specs/jsx-intrinsics-v1/html-aliases.md
// section 3.5, ADR-059 group B, issue #542). One string comes in and one of two services runs:
//
//   * an internal path (`/settings/profile`) is pushed on the window's `NavController`, so the
//     route table validates it and back restores the previous entry;
//   * an external URL (`https:`, `mailto:`, ...) goes to the `ShellService`, only when its scheme
//     is on the policy's allow list. `javascript:`, `file:`, `data:` and every other scheme are
//     refused here, before the shell sees them.
//
// Nothing throws and nothing blocks: a refusal is a `LinkResult` with a stable outcome, and the
// counters say how often each happened (ADR-022). The router owns no navigation or shell state; it
// borrows both services, and either may be absent (a headless run without a shell), which is itself
// an outcome rather than a crash.

#pragma once

#include "tessera/appfw/navigation_stack.hpp"
#include "tessera/appfw/shell.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

enum class LinkOutcome : std::uint8_t {
    Navigated,         // the path was pushed (or was already the active route)
    OpenedExternal,    // the shell accepted the URL
    UnresolvedPath,    // an internal path the route table does not resolve
    SchemeNotOpened,   // a URL whose scheme is not on the allow list
    Malformed,         // empty, protocol-relative, relative or too long
    ServiceUnavailable // the needed service is absent, lacks the capability or refused the call
};

[[nodiscard]] constexpr std::string_view link_outcome_name(LinkOutcome outcome) noexcept {
    switch (outcome) {
    case LinkOutcome::Navigated:
        return "navigated";
    case LinkOutcome::OpenedExternal:
        return "opened-external";
    case LinkOutcome::UnresolvedPath:
        return "unresolved-path";
    case LinkOutcome::SchemeNotOpened:
        return "scheme-not-opened";
    case LinkOutcome::Malformed:
        return "malformed";
    case LinkOutcome::ServiceUnavailable:
        return "service-unavailable";
    }
    return "malformed";
}

struct LinkResult {
    LinkOutcome outcome = LinkOutcome::Malformed;
    // A short stable reason for a diagnostic. It names the scheme or the route problem, never the
    // whole URL (a link can carry a token).
    std::string detail;

    [[nodiscard]] bool ok() const noexcept {
        return outcome == LinkOutcome::Navigated || outcome == LinkOutcome::OpenedExternal;
    }
};

struct LinkPolicy {
    // Lower-case schemes the shell may open. `http` is on the list because a port meets it; an app
    // that wants https only removes it.
    std::vector<std::string> external_schemes{"https", "http", "mailto", "tel"};
};

struct LinkDiagnostics {
    std::uint64_t navigated = 0;
    std::uint64_t opened_external = 0;
    std::uint64_t unresolved_path = 0;
    std::uint64_t scheme_not_opened = 0;
    std::uint64_t malformed = 0;
    std::uint64_t service_unavailable = 0;
    // The shell accepted the hand-off and then reported that the platform failed it.
    std::uint64_t external_failed = 0;
};

class LinkRouter {
public:
    // Both services are borrowed, may be null, and must outlive the router.
    LinkRouter(NavController* navigation, ShellService* shell, LinkPolicy policy = {});

    LinkRouter(const LinkRouter&) = delete;
    LinkRouter& operator=(const LinkRouter&) = delete;

    // Routes `href`. An internal path only records the push; the host commits it at the frame
    // boundary like every other navigation request.
    [[nodiscard]] LinkResult open(std::string_view href);

    [[nodiscard]] LinkDiagnostics diagnostics() const;

private:
    struct State; // shared with the in-flight shell completion, which can outlive the router

    NavController* navigation_ = nullptr;
    ShellService* shell_ = nullptr;
    LinkPolicy policy_;
    std::shared_ptr<State> state_;
};

} // namespace tessera::appfw
