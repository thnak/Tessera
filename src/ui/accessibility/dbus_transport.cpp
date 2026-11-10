// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_transport.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

namespace tessera::ui::a11y {
namespace {
constexpr std::size_t kEndpointCount = 2;
} // namespace

// Shared state of a connected pair. `queues[i]` holds the bytes endpoint `i` can still read and
// `written[i]` the bytes endpoint `i` produced, so a test can assert exact golden output.
struct LoopbackTransport::Pipe {
    mutable std::mutex mutex;
    std::deque<std::uint8_t> queues[kEndpointCount];
    std::vector<std::uint8_t> written[kEndpointCount];
    bool open[kEndpointCount] = {true, true};
};

std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>>
LoopbackTransport::create() {
    auto pipe = std::make_shared<Pipe>();
    return {std::unique_ptr<LoopbackTransport>(new LoopbackTransport(pipe, 0)),
            std::unique_ptr<LoopbackTransport>(new LoopbackTransport(pipe, 1))};
}

LoopbackTransport::LoopbackTransport(std::shared_ptr<Pipe> pipe, std::size_t endpoint)
    : pipe_(std::move(pipe)),
      endpoint_(endpoint) {}

std::expected<void, TransportError> LoopbackTransport::connect(const BusAddress& /*address*/) {
    std::lock_guard lock(pipe_->mutex);
    if (!pipe_->open[endpoint_]) {
        return std::unexpected(TransportError::Closed);
    }
    return {};
}

std::expected<std::size_t, TransportError> LoopbackTransport::read_some(std::span<std::uint8_t> out,
                                                                        int /*timeout_ms*/) {
    if (out.empty()) {
        return 0;
    }
    std::lock_guard lock(pipe_->mutex);
    const std::size_t peer = kEndpointCount - 1 - endpoint_;
    if (!pipe_->open[endpoint_]) {
        return std::unexpected(TransportError::Closed);
    }
    auto& queue = pipe_->queues[endpoint_];
    if (queue.empty()) {
        // A closed peer means no further bytes can arrive; otherwise this is a deterministic
        // "nothing yet" and the caller decides whether to keep polling.
        return std::unexpected(pipe_->open[peer] ? TransportError::WouldBlock
                                                 : TransportError::Closed);
    }
    const std::size_t count = std::min(out.size(), queue.size());
    for (std::size_t index = 0; index < count; ++index) {
        out[index] = queue.front();
        queue.pop_front();
    }
    return count;
}

std::expected<void, TransportError>
LoopbackTransport::write_all(std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(pipe_->mutex);
    if (!pipe_->open[endpoint_]) {
        return std::unexpected(TransportError::Closed);
    }
    const std::size_t peer = kEndpointCount - 1 - endpoint_;
    if (!pipe_->open[peer]) {
        return std::unexpected(TransportError::Closed);
    }
    if (pipe_->queues[peer].size() + bytes.size() > kMaxLoopbackBytes) {
        return std::unexpected(TransportError::TooLarge);
    }
    auto& queue = pipe_->queues[peer];
    queue.insert(queue.end(), bytes.begin(), bytes.end());
    auto& written = pipe_->written[endpoint_];
    written.insert(written.end(), bytes.begin(), bytes.end());
    return {};
}

void LoopbackTransport::close() {
    std::lock_guard lock(pipe_->mutex);
    pipe_->open[endpoint_] = false;
}

bool LoopbackTransport::connected() const {
    std::lock_guard lock(pipe_->mutex);
    return pipe_->open[endpoint_];
}

std::vector<std::uint8_t> LoopbackTransport::take_written() {
    std::lock_guard lock(pipe_->mutex);
    std::vector<std::uint8_t> out;
    out.swap(pipe_->written[endpoint_]);
    return out;
}

void LoopbackTransport::feed(std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(pipe_->mutex);
    auto& queue = pipe_->queues[endpoint_];
    const std::size_t room = bytes.size() > kMaxLoopbackBytes ? kMaxLoopbackBytes : bytes.size();
    queue.insert(queue.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(room));
}

std::size_t LoopbackTransport::pending() const {
    std::lock_guard lock(pipe_->mutex);
    return pipe_->queues[endpoint_].size();
}

void LoopbackTransport::shutdown() {
    std::lock_guard lock(pipe_->mutex);
    pipe_->open[0] = false;
    pipe_->open[1] = false;
}

} // namespace tessera::ui::a11y
