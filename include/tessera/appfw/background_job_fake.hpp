// SPDX-License-Identifier: MIT
//
// `FakeJobScheduler`: the deterministic in-tree `JobScheduler` (issue #1043). It records what the
// app asked for and lets a test play the OS: set the device state (network, charging), advance
// time, and `wake` an entry, which succeeds only when the constraints hold. No wall clock.

#pragma once

#include "tessera/appfw/background_job.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

class FakeJobScheduler final : public JobScheduler {
public:
    struct Entry {
        std::string id;
        JobConstraints constraints;
        double scheduled_at_seconds = 0.0;
    };

    void set_refuse(bool refuse) noexcept {
        refuse_ = refuse;
    }
    void set_device(bool network, bool charging) noexcept {
        network_ = network;
        charging_ = charging;
    }
    void set_now(double seconds) noexcept {
        now_ = seconds;
    }

    // Why a wake did or did not happen: the test asserts on this instead of on a log.
    enum class WakeResult : std::uint8_t { Woken, UnknownId, TooEarly, NoNetwork, NotCharging };

    // Plays the OS waking the app for `job_id`. A woken entry is removed (one-shot, like a
    // WorkManager one-time request); a refused wake leaves it pending.
    WakeResult wake(std::string_view job_id);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::size_t schedule_calls() const noexcept {
        return schedule_calls_;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "fake";
    }
    bool schedule(std::string_view job_id, const JobConstraints& constraints) override;
    bool cancel(std::string_view job_id) override;

private:
    std::vector<Entry> entries_;
    double now_ = 0.0;
    bool network_ = true;
    bool charging_ = false;
    bool refuse_ = false;
    std::size_t schedule_calls_ = 0;
};

} // namespace tessera::appfw
