// SPDX-License-Identifier: MIT
//
// `PermissionService`: the app-facing permission model (docs/specs/permissions-v1.md sections 4, 5
// and 6; issue #451).
//
// The service is the *policy* half of the module and `PermissionBackend` is the *platform* half.
// Everything that must be true on every platform lives here, once:
//
//   * **The state table** (section 4). One entry per permission, updated only by a result, never
//     inferred. `status()` is a cached read: it does not prompt, and a caller that only wants to
//     know what to render pays nothing.
//   * **The four "do not ask" rules** (section 5 rules 2, 3, 4 and 7). A granted permission
//   resolves
//     without a prompt; a `PermanentlyDenied` or `Restricted` one resolves without a prompt and
//     forever; a `Denied` one asked with `retry_after_denial = false` resolves without one. Those
//     four cases are the whole reason an app should never call the platform itself.
//   * **Coalescing** (section 5 rule 5). A second request for a permission that is already pending
//     joins the first: one platform call, one prompt, one answer for every caller. This is the
//     acceptance criterion the issue names.
//   * **The declaration check** (section 7). A request for a permission the project config does not
//     list is `NotDeclared` before the platform sees it.
//   * **Cancellation and staleness** (section 5 rule 9). A result for a cancelled request, or for a
//     request whose service is gone, is dropped rather than delivered to a freed callback.
//
// **Async shape.** Section 5 rule 1: a request is asynchronous and never blocks, because a prompt
// on the UI thread deadlocks Android. The seam's `request` already has that shape (it returns at
// once and posts its completion through the injected `CompletionQueue`), so this service exposes
// the coroutine-free `submit` form plus a `runtime::Task` wrapper for a caller that is itself a
// coroutine. The task's own token is deliberately empty, exactly as `NetClient::send` documents:
// cancelling it would make the runtime throw `TaskCancelled` at the `co_await`, and the spec wants
// a cancelled request resolved as a value.
//
// **Not in this slice**, named in the design note's open questions (spec section 11): permission
// groups, notification "provisional" authorization, and the per-platform back ends. The fake is the
// only back end that ships.

#pragma once

#include "tessera/appfw/permission.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
namespace tessera::appfw {

namespace detail {
class PermissionServiceImpl;
} // namespace detail

// A submitted request's identity. Opaque, comparable and stable for the record's lifetime: a slot
// plus a generation, so a ticket whose record was replaced identifies nothing rather than the new
// one.
struct PermissionTicket {
    std::uint64_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value != 0;
    }
    [[nodiscard]] friend constexpr bool operator==(PermissionTicket, PermissionTicket) = default;
};

// Everything the service is configured with, all of it explicit.
struct PermissionConfig {
    // The permissions the project config declares (section 7). A request for anything else is
    // `NotDeclared`. Empty by default: **no permission is on by default**, which is the
    // least-privilege rule made mechanical.
    std::uint64_t declared = 0;
    // Bound on distinct tickets kept so `status_of`/`result_of` keep answering after completion.
    // The oldest terminal record is evicted first; a pending request is never evicted.
    std::size_t max_records = 16;
};

// How many requests may be pending at once. It bounds the coalescing table, so a burst of requests
// cannot grow it without limit (the same reason `NetClientConfig::max_in_flight` exists).
inline constexpr std::size_t kMaxPendingPermissionRequests = 8;

// One per app scope. `backend` and `completions` are borrowed and must outlive the service;
// non-movable, because the impl is shared with in-flight requests so a destroyed service cannot
// dangle a callback into freed state.
class PermissionService {
public:
    using Callback = std::function<void(PermissionResult)>;

    PermissionService(PermissionConfig config, PermissionBackend& backend,
                      runtime::CompletionQueue& completions);
    ~PermissionService();

    PermissionService(const PermissionService&) = delete;
    PermissionService& operator=(const PermissionService&) = delete;
    PermissionService(PermissionService&&) = delete;
    PermissionService& operator=(PermissionService&&) = delete;

    // --- state ---------------------------------------------------------------------------------

    // The cached state. A pure read: no prompt, no platform call. `NotDetermined` before the first
    // answer for a permission this build cannot ask about is `Unsupported` handled by `request`;
    // the status here is the last thing learned.
    [[nodiscard]] PermissionState status(Permission permission) const noexcept;

    // Re-reads every permission from the back end without prompting (section 9 item 5: the OS can
    // change the answer while the app is backgrounded, so a `Resume` refreshes). Returns how many
    // states changed, which is what a caller uses to decide whether to repaint.
    std::size_t refresh();

    // The capabilities the back end reported, cached at construction and refreshed by `refresh()`.
    [[nodiscard]] const PermissionBackendCapabilities& capabilities() const noexcept;

    // Whether the config declared this permission (section 7).
    [[nodiscard]] bool declared(Permission permission) const noexcept;

    // --- requesting ----------------------------------------------------------------------------

    // Spec section 5's `request`. Returns a ticket at once; `on_done` runs on the completion queue
    // exactly once per accepted call, unless the service is destroyed first.
    //
    // Sections 5 rules 2, 3, 4 and 7 are applied *before* the back end is called: those cases
    // resolve on the next completion drain, not on this stack (contract 2 of the seam, applied to
    // the module's own answers so a caller cannot observe a difference between "answered by policy"
    // and "answered by the OS").
    //
    // A request for an undeclared permission still returns a ticket and still completes, with
    // `NotDeclared`. Returning a null ticket instead would make the caller's error handling
    // conditional on a bug.
    [[nodiscard]] PermissionTicket submit(Permission permission, PermissionRequestOptions options,
                                          runtime::CancellationToken token, Callback on_done);

    // The coroutine form, for a caller that is itself one.
    [[nodiscard]] runtime::Task<PermissionResult> request(Permission permission,
                                                          PermissionRequestOptions options,
                                                          runtime::CancellationToken token);

    // --- observation ---------------------------------------------------------------------------

    // The result of a finished ticket, or nullopt for an unknown, pending or evicted one.
    [[nodiscard]] std::optional<PermissionResult> result_of(PermissionTicket ticket) const;

    struct Stats {
        std::uint64_t requests = 0;
        std::uint64_t granted = 0;
        std::uint64_t denied = 0;
        std::uint64_t permanently_denied = 0;
        std::uint64_t restricted = 0;
        // Requests answered from the state table without a platform call: the four "do not ask"
        // rules. The counter that proves those rules ran instead of being hoped for.
        std::uint64_t answered_without_prompt = 0;
        // Requests that joined an already-pending one instead of prompting twice.
        std::uint64_t coalesced = 0;
        std::uint64_t undeclared = 0;
        std::uint64_t unsupported = 0;
        std::uint64_t cancelled = 0;
        // Completions that arrived for a request the service no longer tracks.
        std::uint64_t stale_completions = 0;
    };

    [[nodiscard]] const Stats& stats() const noexcept;

    [[nodiscard]] std::string_view backend_name() const noexcept;

    // The queue every answer is posted on. Public because the coroutine wrapper
    // (`permission_service_task.cpp`) builds its awaiter against it, and a caller that wants to
    // pump the service in a test may too; an app normally never names it.
    [[nodiscard]] runtime::CompletionQueue& completion_queue() const noexcept;

private:
    // The coroutine body, context-first because `runtime::Task`'s promise consumes the context
    // there (docs/design/services.md section 1.1).
    static runtime::Task<PermissionResult>
    request_body(runtime::TaskContext context, PermissionService& self, Permission permission,
                 PermissionRequestOptions options, runtime::CancellationToken token);

    // Shared, not unique: an in-flight prompt's completion holds a `weak_ptr` to the impl, so
    // destroying the service detaches it and a late answer is dropped rather than delivered into
    // freed state (spec section 5 rule 9). A `unique_ptr` would make `weak_from_this()` always
    // empty and silently drop *every* answer, which is why it is shared here and documented.
    std::shared_ptr<detail::PermissionServiceImpl> impl_;
};

} // namespace tessera::appfw
