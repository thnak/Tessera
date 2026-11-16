// SPDX-License-Identifier: MIT
//
// The `net` per-request state machine (docs/specs/networking-v1.md sections 2 and 3; issue #197).

#include "tessera/appfw/net_state.hpp"

namespace tessera::appfw {

bool NetStateMachine::finish(NetRequestState next, NetOutcome outcome) noexcept {
    if (finished()) {
        return false;
    }
    state_ = next;
    outcome_ = outcome;
    return true;
}

bool NetStateMachine::begin_attempt() noexcept {
    if (finished()) {
        return false;
    }
    state_ = NetRequestState::Loading;
    ++attempts_;
    return true;
}

bool NetStateMachine::accept(int status) noexcept {
    status_ = status;
    if (!finish(NetRequestState::Done, outcome_of_status(status))) {
        return false;
    }
    error_kind_.reset();
    return true;
}

bool NetStateMachine::fail(const NetError& error) noexcept {
    error_kind_ = error.kind;
    return finish(NetRequestState::Error, outcome_of(error.kind));
}

bool NetStateMachine::timed_out() noexcept {
    error_kind_ = NetErrorKind::Timeout;
    return finish(NetRequestState::Error, NetOutcome::Timeout);
}

bool NetStateMachine::cancelled() noexcept {
    error_kind_ = NetErrorKind::Cancelled;
    return finish(NetRequestState::Error, NetOutcome::Cancelled);
}

} // namespace tessera::appfw
