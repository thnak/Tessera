// SPDX-License-Identifier: MIT
//
// Deep links and external activation for the navigation module (docs/specs/deep-linking-v1.md
// sections 2, 3, 5, 7 and 8; docs/specs/navigation-v1.md sections 5 and 10; ADR-050 section 8, row
// `navigation`; issue #196).
//
// What the two specs fix, and what this header therefore does:
//
//   * **Every source normalizes to one event before the UI sees it** (deep-linking-v1.md section 1:
//     "the UI never receives raw platform intents"). `DeepLink` is that value; a platform adapter
//     builds it, and nothing below this point sees an OS intent.
//   * **The link is untrusted input** (section 7). `parse_deep_link` checks the scheme and host
//   against
//     an allowlist, caps the length before parsing, percent-decodes each segment once, and rejects
//     control characters, malformed UTF-8 and encoded separators. A rejection is a value carrying
//     the byte offset that caused it, never a throw.
//   * **A cold arrival needs a stack** (navigation-v1.md section 5: "Cold start: wait for
//   app/route/auth
//     readiness; preserve the pending link"; section 10.1: a deep link "uses the same commit
//     path"). `install_deep_link` is that path: on an empty stack it seeds the route root and then
//     pushes the target, so back from a cold link reaches home rather than an edge. On a warm stack
//     it pushes (deep-linking-v1.md section 2's warm-start rule).
//   * **Unknown or malformed -> the not-found route, never a crash** (navigation-v1.md section 3,
//     deep-linking-v1.md section 8). `install_deep_link` reports the failure and the caller routes
//     it to `RouteTable::not_found_route()`; nothing is recorded, so a half state cannot exist.
//
// The one thing this slice deliberately does not do, and reports rather than invents: **auth-gated
// links** (navigation-v1.md section 4: auth-gated routes wait for auth and the pending link
// survives login). There is no `GuardPipeline` here, so `install_deep_link` pushes the target and
// exposes `DeepLinkResult::requires_auth` for the host; silently mounting a protected screen would
// violate section 10's "auth guards are enforced before mounting protected screens/data", and
// holding a pending queue would invent a policy deep-linking-v1.md section 10 leaves open.
//
// Determinism: a deep link is a string and a restore is a buffer. Nothing here reads a clock, an
// OS, the network or a file, which is why the module's suite is deterministic by construction.

#pragma once

#include "tessera/appfw/navigation_route.hpp"
#include "tessera/appfw/navigation_stack.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// deep-linking-v1.md section 2's `ActivationSource`, unchanged: the normalized event records where
// it came from, because a diagnostic and a policy ("cold start", "single instance") read it.
enum class ActivationSource : std::uint8_t {
    Url = 0,
    File,
    CommandLine,
    Notification,
    Share,
    Other,
};

[[nodiscard]] constexpr std::string_view activation_source_name(ActivationSource source) noexcept {
    switch (source) {
    case ActivationSource::Url:
        return "url";
    case ActivationSource::File:
        return "file";
    case ActivationSource::CommandLine:
        return "command-line";
    case ActivationSource::Notification:
        return "notification";
    case ActivationSource::Share:
        return "share";
    case ActivationSource::Other:
        return "other";
    }
    return "other";
}

// The normalized activation (deep-linking-v1.md section 2), reduced to what a route needs: the
// validated URL, its source, whether it was a cold start, and the resolved route/params. The
// `timestamp_ns` field of the spec's struct is deliberately absent: it is not an input to routing,
// and carrying a wall-clock value into the module would make a deep link non-deterministic to
// assert.
struct DeepLink {
    std::string url; // the input, normalized (scheme/host lowercased, no fragment)
    // The path+query half, always beginning with `/`. Kept separately because it is what a match
    // consumes; `url` is the normalized string a diagnostic and a dedupe key read.
    std::string path;
    ActivationSource source = ActivationSource::Url;
    bool cold_start = false;
    // Filled by `resolve_deep_link`; empty when the URL is valid but matches no route.
    std::string route_id;
    ParamMap params;
    // The `*` capture, when the pattern had one (section 3's `RoutePattern`).
    std::string rest;
};

// deep-linking-v1.md section 3's `deepLinks` declaration, minus the fields that belong to a
// platform manifest (`fileTypes`, `singleInstance`, `openIn`, `fallbackUrl`: no routing decision
// reads them).
struct DeepLinkConfig {
    // Allowlists (section 3: "schemes/hosts are allowlists; no wildcard hosts"). A URL whose scheme
    // or host is not listed is refused with `UnsupportedScheme`/`UnsupportedHost`, so an app cannot
    // be activated by a scheme it did not declare.
    std::vector<std::string> schemes;
    std::vector<std::string> hosts;
    // Section 3's `maxUrlLength` default, 4096. Checked before any parsing, so an oversized input
    // costs one comparison.
    std::size_t max_url_length = 4096;
    // When true, a URL with no scheme/host at all is read as a bare path (`/orders/42`). That is
    // what a command-line argument or a testing harness supplies; a production activation adapter
    // normally leaves it false, so a link must name a declared scheme.
    bool accept_bare_path = false;
};

// What `install` did, in the terms navigation-v1.md section 5 cares about.
enum class DeepLinkOutcome : std::uint8_t {
    // A warm stack: the target was pushed onto it.
    Pushed,
    // A cold start (or an empty stack): the route root was seeded and the target pushed, so back
    // has a
    // destination.
    Synthesized,
    // The target was already active with the same params and the route's duplicate policy ignored
    // it
    // (navigation-v1.md section 2). The link did not change the stack.
    DuplicateIgnored,
};

[[nodiscard]] constexpr std::string_view deep_link_outcome_name(DeepLinkOutcome outcome) noexcept {
    switch (outcome) {
    case DeepLinkOutcome::Pushed:
        return "pushed";
    case DeepLinkOutcome::Synthesized:
        return "synthesized";
    case DeepLinkOutcome::DuplicateIgnored:
        return "duplicate-ignored";
    }
    return "pushed";
}

struct DeepLinkResult {
    DeepLinkOutcome outcome = DeepLinkOutcome::Pushed;
    // deep-linking-v1.md section 3's `auth = true`. The host decides what to do (keep the link
    // pending and open login); this module only reports it, because it has no guard pipeline (see
    // the header comment).
    bool requires_auth = false;
    // True when the target route carried a `confirm = true`-style policy. This slice has no
    // confirmation UI, so it reports the flag for the host's own gate.
    std::string route_id;
};

// Parses and validates one activation URL against `config`. `text` is untrusted: it is
// length-checked first, then split into scheme/host/path/query, then each path segment is decoded
// once.
//
// Accepted forms:
//   `scheme://host/orders/42?tab=items`   a custom-scheme or App/Universal Link
//   `https://host/orders/42`              the same, with an allowlisted host
//   `/orders/42`                          only when `accept_bare_path` is set
[[nodiscard]] std::expected<DeepLink, NavigationError>
parse_deep_link(std::string_view text, const DeepLinkConfig& config,
                ActivationSource source = ActivationSource::Url, bool cold_start = false);

// Resolves a parsed link against the table: matches its path and records the route id, params and
// rest. A path that matches no route is `NotFound` (navigation-v1.md section 3), not a silent
// ignore (deep-linking-v1.md section 5: "never silently ignore").
[[nodiscard]] std::expected<DeepLink, NavigationError> resolve_deep_link(const RouteTable& table,
                                                                         DeepLink link);

// The whole cold/warm path: parse, resolve, then commit through the controller's normal push path
// (navigation-v1.md section 10.1: "it never bypasses guards" applies to the guards this slice has,
// which is the route-table validation every push performs).
[[nodiscard]] std::expected<DeepLinkResult, NavigationError>
install_deep_link(const RouteTable& table, NavController& controller, const DeepLinkConfig& config,
                  std::string_view text, ActivationSource source = ActivationSource::Url,
                  bool cold_start = false);

} // namespace tessera::appfw
