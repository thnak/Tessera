// SPDX-License-Identifier: MIT
//
// `HttpService`: the interface an app resolves to make an HTTP request (ADR-075 section 1; ADR-050
// section 3). It is the seam developer code, the `fetch` shim and the query layer all call, so a
// test or the Studio preview substitutes a fake and a developer can wrap or replace it (an auth
// header, a retry policy, a cache) without touching the platform back end.
//
// It is deliberately the same three operations `NetClient` already has and nothing more: `send`
// (the spec's coroutine form), `submit` (the callback form for a caller that is not a coroutine)
// and `cancel`. `NetClient` is the default implementation and keeps the module's rules (redirects,
// the cross-origin `Authorization` drop, the size cap, no retry), so replacing the service is a
// deliberate whole replacement, not a way to skip them by accident.
#pragma once

#include "tessera/appfw/net_state.hpp"
#include "tessera/appfw/net_types.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"

#include <cstdint>
#include <expected>
#include <functional>

namespace tessera::appfw {

// A submitted request's identity. Opaque and comparable; `NetClient` documents its lifetime.
struct NetTicket {
    std::uint64_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value != 0;
    }
    [[nodiscard]] friend constexpr bool operator==(NetTicket, NetTicket) = default;
};

class HttpService {
public:
    static constexpr ServiceId kServiceId = service_id("tessera.http");

    // Delivered on the thread that drains the completion queue, exactly once per accepted submit.
    using Callback = std::function<void(NetResult)>;

    virtual ~HttpService() = default;

    // Section 2's `send`. `token` is the cancellation input; a cancelled request resolves as a
    // `cancelled` value, never as an exception.
    [[nodiscard]] virtual runtime::Task<NetResult> send(NetRequest request,
                                                        runtime::CancellationToken token) = 0;

    // The coroutine-free form. The ticket when accepted, or the error when refused before anything
    // started (in which case `on_done` is never invoked).
    [[nodiscard]] virtual std::expected<NetTicket, NetError>
    submit(NetRequest request, runtime::CancellationToken token, Callback on_done) = 0;

    // False for an unknown or already-terminal ticket, so a second cancel is a no-op.
    virtual bool cancel(NetTicket ticket) = 0;
};

} // namespace tessera::appfw
