// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_session.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

namespace tessera::ui::a11y {
namespace {

constexpr int kMinIdleWaitMs = 1;

} // namespace

std::uint64_t AtspiSession::steady_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

AtspiSession::AtspiSession(AtspiBridge& bridge, TransportProvider& provider, SessionConfig config)
    : bridge_(bridge),
      provider_(&provider),
      config_(std::move(config)) {}

AtspiSession::AtspiSession(AtspiBridge& bridge, SessionConfig config)
    : bridge_(bridge),
      owned_provider_(std::make_unique<UnixTransportProvider>()),
      provider_(owned_provider_.get()),
      config_(std::move(config)) {}

AtspiSession::~AtspiSession() {
    stop();
}

bool AtspiSession::running() const {
    return running_.load(std::memory_order_acquire);
}

std::expected<void, SessionError> AtspiSession::start() {
    if (running()) {
        return {};
    }
    reset_state();
    {
        std::lock_guard lock(worker_mutex_);
        stop_requested_ = false;
        wake_pending_ = true; // the first pump opens the session bus
        worker_busy_ = false;
        worker_done_ = false;
        wake_serial_ = 0;
        completed_serial_ = 0;
        next_timeout_ms_ = -1;
    }
    try {
        thread_ = std::thread([this] { run_loop(); });
    } catch (const std::system_error&) {
        // Thread creation is the one OS call that can fail synchronously; report it as a value.
        std::lock_guard lock(worker_mutex_);
        stop_requested_ = true;
        worker_done_ = true;
        return std::unexpected(SessionError::ThreadFailed);
    }
    running_.store(true, std::memory_order_release);
    return {};
}

void AtspiSession::stop() {
    {
        std::lock_guard lock(worker_mutex_);
        stop_requested_ = true;
    }
    worker_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
    if (phase() == SessionPhase::Idle) {
        return; // never started: nothing was opened
    }
    close_transport();
    if (phase() != SessionPhase::Failed) {
        phase_.store(SessionPhase::Closed);
    }
}

std::expected<PumpResult, SessionError> AtspiSession::pump_sync(int timeout_ms, int wait_ms) {
    if (!running()) {
        return std::unexpected(SessionError::NotRunning);
    }
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(worker_mutex_);
        ticket = ++wake_serial_;
        wake_pending_ = true;
        next_timeout_ms_ = timeout_ms;
    }
    worker_cv_.notify_all();
    if (!wait_for_pump(ticket, wait_ms)) {
        const SessionError recorded = error();
        return std::unexpected(recorded == SessionError::None ? SessionError::SyncTimeout
                                                              : recorded);
    }
    const SessionError recorded = error();
    if (recorded != SessionError::None) {
        return std::unexpected(recorded);
    }
    std::lock_guard lock(worker_mutex_);
    return last_pump_;
}

bool AtspiSession::wait_for_pump(std::uint64_t ticket, int wait_ms) {
    std::unique_lock lock(worker_mutex_);
    const auto bound = std::chrono::milliseconds(std::max(wait_ms, 0));
    return worker_cv_.wait_for(lock, bound, [this, ticket] {
        return completed_serial_ >= ticket || (!worker_busy_ && worker_done_);
    });
}

void AtspiSession::run_loop() {
    for (;;) {
        std::uint64_t ticket = 0;
        int read_timeout = config_.read_timeout_ms;
        {
            std::unique_lock lock(worker_mutex_);
            const auto idle =
                std::chrono::milliseconds(std::max(config_.idle_wait_ms, kMinIdleWaitMs));
            worker_cv_.wait_for(lock, idle, [this] { return stop_requested_ || wake_pending_; });
            if (stop_requested_) {
                break;
            }
            wake_pending_ = false;
            worker_busy_ = true;
            ticket = wake_serial_;
            if (next_timeout_ms_ >= 0) {
                read_timeout = next_timeout_ms_;
                next_timeout_ms_ = -1;
            }
        }
        const auto result = pump(read_timeout);
        {
            std::lock_guard lock(worker_mutex_);
            worker_busy_ = false;
            completed_serial_ = std::max(completed_serial_, ticket);
            if (result) {
                last_pump_ = *result;
            }
        }
        pumps_.fetch_add(1, std::memory_order_relaxed);
        worker_cv_.notify_all();
        if (!result || result->closed) {
            break;
        }
    }
    {
        std::lock_guard lock(worker_mutex_);
        worker_busy_ = false;
        worker_done_ = true;
    }
    worker_cv_.notify_all();
}

SessionError AtspiSession::fail(SessionError error) {
    if (error_.load(std::memory_order_relaxed) == SessionError::None) {
        error_.store(error, std::memory_order_release);
    }
    phase_.store(SessionPhase::Failed, std::memory_order_release);
    close_transport();
    return error;
}

void AtspiSession::reset_state() {
    close_transport();
    out_.clear();
    in_.clear();
    in_offset_ = 0;
    writer_.reset();
    clear_calls();
    on_a11y_bus_ = false;
    handshake_started_ = false;
    handshake_started_ms_ = 0;
    {
        std::lock_guard lock(state_mutex_);
        unique_name_.clear();
        a11y_address_.clear();
        bus_error_name_.clear();
    }
    {
        std::lock_guard lock(worker_mutex_);
        last_pump_ = PumpResult{};
    }
    pumps_.store(0, std::memory_order_relaxed);
    error_.store(SessionError::None, std::memory_order_release);
    phase_.store(SessionPhase::Idle, std::memory_order_release);
}

SessionPhase AtspiSession::phase() const {
    return phase_.load(std::memory_order_acquire);
}

SessionError AtspiSession::error() const {
    return error_.load(std::memory_order_acquire);
}

std::string AtspiSession::unique_name() const {
    std::lock_guard lock(state_mutex_);
    return unique_name_;
}

std::string AtspiSession::a11y_bus_address() const {
    std::lock_guard lock(state_mutex_);
    return a11y_address_;
}

std::string AtspiSession::bus_error_name() const {
    std::lock_guard lock(state_mutex_);
    return bus_error_name_;
}

std::size_t AtspiSession::pumps() const {
    return pumps_.load(std::memory_order_relaxed);
}

bool AtspiSession::handshaking() const {
    switch (phase()) {
    case SessionPhase::Idle:
    case SessionPhase::SessionConnect:
    case SessionPhase::SessionHello:
    case SessionPhase::A11yAddress:
    case SessionPhase::A11yConnect:
    case SessionPhase::A11yHello:
    case SessionPhase::Registering:
        return true;
    case SessionPhase::Ready:
    case SessionPhase::Closed:
    case SessionPhase::Failed:
        return false;
    }
    return false;
}

} // namespace tessera::ui::a11y
