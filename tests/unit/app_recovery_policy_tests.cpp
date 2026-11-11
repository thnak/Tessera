// SPDX-License-Identifier: MIT
//
// Bounded device-loss recovery policy (app/recovery_policy.hpp): a transient loss charges one
// attempt, releases the lost generation exactly once and recovers; a persistent loss charges the
// explicit budget and then reports GiveUp - the value the window driver turns into
// WindowFrameStats::stalled. Deterministic: counters and enumerators only, no device and no clock.

#include "tessera/app/recovery_policy.hpp"

#include <cstdint>
#include <iostream>

namespace {

using tessera::app::kMaxDeviceRecoveryAttempts;
using tessera::app::RecoveryAction;
using tessera::app::RecoveryPolicy;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_recovery_policy: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    // --- 1. A transient loss: one attempt, one release, one recovery -------------------------
    {
        RecoveryPolicy policy;
        if (!expect(policy.on_blocked() == RecoveryAction::Release,
                    "the first blocked observation releases the lost generation")) {
            return 1;
        }
        if (!expect(policy.lost() && policy.attempts() == 1,
                    "the release charges one attempt and marks the policy lost")) {
            return 1;
        }
        if (!expect(policy.on_blocked() == RecoveryAction::None,
                    "a repeated blocked observation does not release twice")) {
            return 2;
        }
        if (!expect(policy.attempts() == 1, "waiting for the backend costs no extra attempt")) {
            return 2;
        }
        policy.on_recovered();
        if (!expect(!policy.lost() && policy.recoveries() == 1 && !policy.exhausted(),
                    "a successful recovery clears the loss")) {
            return 2;
        }
        // A later loss in the same run() is still allowed, and is charged separately.
        if (!expect(policy.on_blocked() == RecoveryAction::Release && policy.attempts() == 2,
                    "a new loss is charged against the same run budget")) {
            return 3;
        }
        policy.on_recovered();
        if (!expect(policy.recoveries() == 2, "each recovery is counted")) {
            return 3;
        }
    }

    // --- 2. A persistent loss stops after exactly the budget ---------------------------------
    {
        RecoveryPolicy policy;
        std::uint32_t attempts = 0;
        if (!expect(policy.on_blocked() == RecoveryAction::Release, "a persistent loss starts")) {
            return 4;
        }
        ++attempts;
        RecoveryAction action = RecoveryAction::None;
        for (std::uint32_t rebuild = 0; rebuild < 16; ++rebuild) {
            action = policy.on_failed();
            if (action == RecoveryAction::GiveUp) {
                break;
            }
            if (!expect(action == RecoveryAction::Rebuild,
                        "a failed attempt inside the budget rebuilds the device")) {
                return 5;
            }
            ++attempts;
        }
        if (!expect(action == RecoveryAction::GiveUp, "a persistent loss ends in GiveUp") ||
            !expect(attempts == kMaxDeviceRecoveryAttempts,
                    "the give-up lands exactly on the explicit attempt budget") ||
            !expect(policy.attempts() == kMaxDeviceRecoveryAttempts,
                    "the policy reports the attempts it charged") ||
            !expect(policy.exhausted() && !policy.lost(), "the policy is latched exhausted") ||
            !expect(policy.recoveries() == 0, "a persistent loss recovers nothing")) {
            return 6;
        }
        // Latched: the host cannot re-enter recovery in the same run().
        if (!expect(policy.on_blocked() == RecoveryAction::GiveUp,
                    "a blocked observation after the give-up stays given up") ||
            !expect(policy.on_failed() == RecoveryAction::GiveUp,
                    "a failed observation after the give-up stays given up")) {
            return 7;
        }
    }

    // --- 3. A zero budget gives up immediately (a run may opt out of retrying) ---------------
    {
        RecoveryPolicy policy(0);
        if (!expect(policy.on_blocked() == RecoveryAction::GiveUp,
                    "a zero budget gives up on the first loss") ||
            !expect(policy.attempts() == 0 && policy.exhausted(),
                    "a zero budget charges no attempt")) {
            return 8;
        }
    }

    // --- 4. Rebuild then recover: the sequence a spent in-place attempt produces --------------
    {
        RecoveryPolicy policy;
        if (!expect(policy.on_blocked() == RecoveryAction::Release, "the loss is observed")) {
            return 9;
        }
        if (!expect(policy.on_failed() == RecoveryAction::Rebuild && policy.attempts() == 2,
                    "a failed in-place attempt charges and requests a rebuild")) {
            return 9;
        }
        if (!expect(policy.on_blocked() == RecoveryAction::Release && policy.attempts() == 3,
                    "the rebuilt device's loss is a fresh charge")) {
            return 10;
        }
        policy.on_recovered();
        if (!expect(policy.recoveries() == 1 && !policy.exhausted(),
                    "the rebuilt device's recovery succeeds")) {
            return 10;
        }
        if (!expect(policy.on_blocked() == RecoveryAction::GiveUp,
                    "the budget is per run: a further loss gives up")) {
            return 11;
        }
    }

    std::cout << "app_recovery_policy: ok (budget=" << kMaxDeviceRecoveryAttempts << ")\n";
    return 0;
}
