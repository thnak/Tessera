// SPDX-License-Identifier: MIT
//
// `FakeJobScheduler` (issue #1043). See include/tessera/appfw/background_job_fake.hpp.

#include "tessera/appfw/background_job_fake.hpp"

#include <algorithm>

namespace tessera::appfw {

bool FakeJobScheduler::schedule(std::string_view job_id, const JobConstraints& constraints) {
    ++schedule_calls_;
    if (refuse_ || job_id.empty()) {
        return false;
    }
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == job_id; });
    if (found != entries_.end()) {
        found->constraints = constraints;
        found->scheduled_at_seconds = now_;
        return true;
    }
    entries_.push_back({std::string(job_id), constraints, now_});
    return true;
}

bool FakeJobScheduler::cancel(std::string_view job_id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == job_id; });
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    return true;
}

FakeJobScheduler::WakeResult FakeJobScheduler::wake(std::string_view job_id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == job_id; });
    if (found == entries_.end()) {
        return WakeResult::UnknownId;
    }
    const JobConstraints& want = found->constraints;
    if (now_ < found->scheduled_at_seconds + want.earliest_after_seconds) {
        return WakeResult::TooEarly;
    }
    if (want.requires_network && !network_) {
        return WakeResult::NoNetwork;
    }
    if (want.requires_charging && !charging_) {
        return WakeResult::NotCharging;
    }
    entries_.erase(found);
    return WakeResult::Woken;
}

} // namespace tessera::appfw
