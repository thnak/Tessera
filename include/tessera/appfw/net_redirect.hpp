// SPDX-License-Identifier: MIT
//
// Redirect rules for the `net` module (docs/specs/networking-v1.md section 3 item 2; ADR-052 item
// 2; issue #197).
//
// Why the *module* follows redirects rather than each back end: section 3 item 2 is normative, and
// the platforms disagree on every part of it - WinHTTP follows silently, libcurl has its own
// `CURLOPT_MAXREDIRS` and POST-to-GET rule, `fetch` follows by default and forbids some header
// changes. A contract suite cannot pin behaviour it never sees. So every back end is configured to
// follow nothing (`WINHTTP_OPTION_REDIRECT_POLICY_NEVER`, `CURLOPT_FOLLOWLOCATION 0`,
// `redirect: "manual"`) and hands the 3xx back as an ordinary `NetResponse`; the module then
// applies this header's rules, once, in one place. The seam's contract therefore includes "do not
// follow redirects yourself", which `NetTransport` states as a requirement.
//
// The rules, each asserted by the suite:
//
//   * at most `kMaxRedirects` hops; the next 3xx is `too_many_redirects` (section 3 item 2);
//   * `301`, `302` and `303` change `POST` to `GET` and drop the body;
//   * `307` and `308` keep method and body;
//   * `Authorization` is dropped when the redirect changes the origin;
//   * an `https` -> `http` redirect is refused with `unsupported` (a downgrade is not a redirect a
//     caller can be assumed to have consented to).
//
// `309`-and-friends, `300`, `304` and a 3xx with no `Location` are *not* followed: they are
// delivered as a `NetResponse` and the caller decides, which is the same rule `RedirectPolicy::
// Manual` applies to all of them.

#pragma once

#include "tessera/appfw/net_types.hpp"
#include "tessera/appfw/net_url.hpp"

#include <cstdint>
#include <expected>

namespace tessera::appfw {

// The five statuses section 3 item 2 names, and only those.
[[nodiscard]] bool is_followable_redirect(int status) noexcept;

// The `Location` header of a 3xx response, or null. Header lookup is case-insensitive
// (section 3 item 7).
[[nodiscard]] const std::string* redirect_location(const NetResponse& response) noexcept;

// The next hop for `current`, which received `response`. `followed` is the number of hops already
// taken, so a call with `followed == kMaxRedirects` that meets another 3xx returns
// `too_many_redirects`.
//
// Errors: `too_many_redirects`, `unsupported` (a relative or non-http(s) `Location`, or an https to
// http downgrade) and `bad_url` (a `Location` that does not parse). `current.url` is replaced by
// the absolute target, `final_url` is not touched (the caller sets it from the response it finally
// keeps).
[[nodiscard]] std::expected<NetRequest, NetError>
follow_redirect(const NetRequest& current, const NetResponse& response, std::uint32_t followed);

} // namespace tessera::appfw
