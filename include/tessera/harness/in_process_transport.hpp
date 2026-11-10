// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/line_transport.hpp"

#include <cstddef>
#include <memory>

namespace tessera::harness {

class InProcessTransport;

// Two connected endpoints of one in-process pipe. This is the harness's embedding/test transport:
// no OS handle, no background thread, and no blocking wait, so a request/response loop is exactly
// as deterministic as the bytes each side writes.
struct InProcessPair {
    std::unique_ptr<InProcessTransport> first;
    std::unique_ptr<InProcessTransport> second;
};

// Both endpoints share one state object and are single-thread affine: use one endpoint from one
// thread at a time. Writes are framed on the reader side, so the pair exercises the same
// CRLF/UTF-8/size rules a stdio transport would.
[[nodiscard]] InProcessPair make_in_process_pair(std::size_t max_line_bytes = kMaxLineBytes);

class InProcessTransport final : public LineTransport {
public:
    ~InProcessTransport() override;

    InProcessTransport(const InProcessTransport&) = delete;
    InProcessTransport& operator=(const InProcessTransport&) = delete;

    [[nodiscard]] std::expected<std::string, TransportError> read_line() override;
    [[nodiscard]] std::expected<void, TransportError> write_line(std::string_view line) override;
    [[nodiscard]] std::size_t max_line_bytes() const noexcept override;
    [[nodiscard]] bool closed() const noexcept override;
    void close() noexcept override;

    // True once queued bytes are exhausted and no line is readable without more writes. A closed
    // peer with buffered lines is not drained yet.
    [[nodiscard]] bool peer_closed() const noexcept;

private:
    struct Shared;

    InProcessTransport(std::shared_ptr<Shared> shared, bool is_first) noexcept;

    friend InProcessPair make_in_process_pair(std::size_t max_line_bytes);

    [[nodiscard]] LineFramer& inbound() noexcept;

    std::shared_ptr<Shared> shared_;
    bool is_first_ = true;
    bool closed_ = false;
};

} // namespace tessera::harness
