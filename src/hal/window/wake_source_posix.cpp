// SPDX-License-Identifier: MIT
//
// The POSIX wake primitive (issue #842): an `eventfd` the UI thread joins beside its display fd.
// A completion posted from a worker writes it, the parked `poll()` returns, and the next tick drains
// the queue on the UI thread. Nothing here runs a completion.
//
// `eventfd` is used rather than a self-pipe because it is one descriptor with no buffering to fill:
// a burst of wakes before the thread drains is a single readable descriptor, which is exactly the
// "one wake per burst" the completion queue already guarantees (its wake fires once per
// empty -> non-empty edge). Where `eventfd` is unavailable the implementation is absent and
// `create_wake_source` returns null, so the host keeps today's bounded sleep.

#include "tessera/hal/wake_source.hpp"
#include "tessera/hal/timer/steady_time.hpp"

#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>

#if defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace tessera::hal {
namespace {

#if defined(__linux__)

class EventfdWakeSource final : public WakeSource {
public:
    explicit EventfdWakeSource(int descriptor)
        : descriptor_(descriptor) {}
    ~EventfdWakeSource() override {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    EventfdWakeSource(const EventfdWakeSource&) = delete;
    EventfdWakeSource& operator=(const EventfdWakeSource&) = delete;

    void wake() noexcept override {
        // EFD_NONBLOCK: a counter already at its maximum leaves the descriptor readable anyway, so
        // a failed write is not a lost wake.
        const std::uint64_t one = 1;
        const ssize_t written = ::write(descriptor_, &one, sizeof one);
        (void)written;
    }

    void drain() noexcept override {
        std::uint64_t value = 0;
        while (::read(descriptor_, &value, sizeof value) > 0) {
        }
    }

    [[nodiscard]] bool wait_until(double deadline_seconds) override {
        const double remaining = seconds_until(steady_now_seconds(), deadline_seconds);
        if (!(remaining > 0.0)) {
            return false;
        }
        const double milliseconds = remaining * 1000.0;
        const int timeout = milliseconds >= static_cast<double>(std::numeric_limits<int>::max())
                                ? std::numeric_limits<int>::max()
                                : static_cast<int>(milliseconds) + 1;
        pollfd descriptor{};
        descriptor.fd = descriptor_;
        descriptor.events = POLLIN;
        for (;;) {
            const int result = ::poll(&descriptor, 1, timeout);
            if (result > 0) {
                return true;
            }
            if (result == 0) {
                return false;
            }
            if (errno != EINTR) {
                return false;
            }
        }
    }

    [[nodiscard]] int file_descriptor() const noexcept override {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

#endif

} // namespace

std::unique_ptr<WakeSource> create_wake_source() {
#if defined(__linux__)
    const int descriptor = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (descriptor < 0) {
        return nullptr;
    }
    return std::make_unique<EventfdWakeSource>(descriptor);
#else
    // No eventfd on this platform: the host falls back to its bounded sleep, which is the same
    // behaviour it had before the wake existed.
    return nullptr;
#endif
}

namespace {

// The platform-neutral manual wake: no descriptor, so it is only usable where the loop calls
// `wait_until` itself (the window-less park and the tests).
class ManualWakeSource final : public WakeSource {
public:
    void wake() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signalled_ = true;
        }
        condition_.notify_all();
    }

    void drain() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        signalled_ = false;
    }

    [[nodiscard]] bool wait_until(double deadline_seconds) override {
        std::unique_lock<std::mutex> lock(mutex_);
        const double remaining = seconds_until(steady_now_seconds(), deadline_seconds);
        if (!(remaining > 0.0)) {
            return signalled_;
        }
        const auto bound = std::chrono::duration<double>(remaining);
        condition_.wait_for(lock, bound, [this] { return signalled_; });
        return signalled_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signalled_ = false;
};

} // namespace

std::unique_ptr<WakeSource> create_manual_wake_source() {
    return std::make_unique<ManualWakeSource>();
}

} // namespace tessera::hal
