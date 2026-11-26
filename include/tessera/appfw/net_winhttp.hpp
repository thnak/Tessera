// SPDX-License-Identifier: MIT
//
// `WinHttpTransport`: the Windows `NetTransport` back end (docs/specs/networking-v1.md section 4,
// ADR-052 items 1 and 5; issue #835).
//
// What the OS stack owns, and what this class therefore does not: connections, keep-alive and
// pooling (one WinHTTP session per transport, so requests share them), TLS and the Windows trust
// store, HTTP/2, and the system proxy (`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`). There is no API to
// disable certificate validation and no proxy field (section 3 items 8 and 10).
//
// What it does for the module: it is **async** (`WINHTTP_FLAG_ASYNC` and a status callback), so
// `start()` never blocks the calling thread and the completion is posted through the injected
// `CompletionQueue`, never run inline. It follows **no** redirect (`NetClient` owns that, including
// the cross-origin `Authorization` rule), adds no header, keeps no cookie, and enforces
// `max_response_bytes` by stopping the read. A cancelled token closes the request handle, which
// releases the connection.
//
// No Windows type appears here: the header compiles on every platform, and the class is only built
// into `tessera_appfw_net` on Windows. `available()` is false only when the session could not be
// opened.
#pragma once

#include "tessera/appfw/net_transport.hpp"
#include "tessera/runtime/async.hpp"

#include <memory>
#include <string_view>

namespace tessera::appfw {

class WinHttpTransport final : public NetTransport {
public:
    // `completions` is where every completion is posted; it must outlive the transport. The
    // destructor cancels what is still in flight and waits for the OS to release every handle.
    explicit WinHttpTransport(runtime::CompletionQueue& completions);
    ~WinHttpTransport() override;

    bool start(const NetRequest& request, runtime::CancellationToken token,
               Completion on_done) override;
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;

    // Opaque. Public only so the translation unit's status callback, which is not a member, can
    // name it; nothing outside `net_winhttp.cpp` can see its definition.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::appfw
