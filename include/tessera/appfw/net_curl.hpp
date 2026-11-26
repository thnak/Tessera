// SPDX-License-Identifier: MIT
//
// `CurlTransport`: the Linux `NetTransport` back end over libcurl's multi interface
// (docs/specs/networking-v1.md section 4, ADR-052 items 1 and 5; issue #836).
//
// What libcurl and the distro own, and what this class therefore does not: connections, keep-alive
// and pooling (one `CURLM` per transport, so requests share its connection cache), DNS, TLS and the
// trust store (the system libcurl uses the distro's TLS library and gets its security updates), and
// the proxy (libcurl reads the environment's proxy variables; there is no API field). Certificate
// and host verification are always on and no API turns them off (section 3 item 8).
//
// What it does for the module: it is **async**. `start()` queues the transfer and returns at once;
// a driver thread the transport owns runs `curl_multi_perform`, and every completion is posted
// through the injected `CompletionQueue`, never run inline. It follows **no** redirect (`NetClient`
// owns that, including the cross-origin `Authorization` rule), adds no header curl would add by
// itself, keeps no cookie, and enforces `max_response_bytes` by stopping the read. A cancelled
// token wakes the driver, which removes the transfer and so releases the connection.
//
// No curl type appears here: the header compiles on every platform, and the class is only built
// into `tessera_appfw_net` where libcurl was found.
#pragma once

#include "tessera/appfw/net_transport.hpp"
#include "tessera/runtime/async.hpp"

#include <memory>
#include <string_view>

namespace tessera::appfw {

class CurlTransport final : public NetTransport {
public:
    // `completions` is where every completion is posted; it must outlive the transport. The
    // destructor cancels what is still in flight and joins the driver thread.
    explicit CurlTransport(runtime::CompletionQueue& completions);
    ~CurlTransport() override;

    bool start(const NetRequest& request, runtime::CancellationToken token,
               Completion on_done) override;
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::appfw
