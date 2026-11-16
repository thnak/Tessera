// SPDX-License-Identifier: MIT
//
// The per-hop plumbing of `NetClient`: dispatch to the executor, the transport completion, the
// section 3 item 2 redirect loop, the section 3 item 3 deadline and the exactly-once delivery
// (issue #197).
//
// The structural rule this file implements: **the UI thread never runs the attempt.** `dispatch`
// posts an `IoTask` whose `work` body is the only code that calls `transport_->start`, and that
// body touches no record - it captures a copy of the request, the token and a `weak_ptr`, so a
// destroyed client is a no-op rather than a dangling call. Everything else here runs on the thread
// that drains the completion queue.

#include "net_client_internal.hpp"

#include <memory>
#include <string>
#include <utility>

namespace tessera::appfw::detail {

namespace {

// Shared between a hop's work body and its completion. The executor separates the two, and "did the
// transport accept the attempt" is decided on the worker and consumed on the UI thread, so it
// travels here rather than being lost.
struct HopStart {
    bool accepted = false;
};

} // namespace

bool NetClientImpl::dispatch(NetTicket ticket) {
    NetRecord* const record = find(ticket);
    if (record == nullptr) {
        return false;
    }
    auto state = std::make_shared<HopStart>();
    // Copied: the work body must not touch the record, which the UI thread may resolve while the
    // attempt is being handed over.
    NetRequest request = record->request;
    runtime::CancellationToken token = record->attempt_token.token();
    std::weak_ptr<NetClientImpl> weak = weak_from_this();
    NetTransport* const transport = transport_;
    runtime::CompletionQueue* const completions = completions_;

    runtime::IoTask task;
    task.work = [transport, request = std::move(request), token, weak, ticket, state,
                 completions]() mutable {
        // The transport's completion must reach the UI thread even when the back end calls it from
        // its own thread (net_transport.hpp contract 1). Posting here is what marshals it; a full
        // queue drops it, which is the bounded-queue contract and shows up as a request the
        // client's own deadline or `tick()` resolves.
        state->accepted = transport->start(
            request, std::move(token), [weak, ticket, completions](NetResult result) {
                if (auto impl = weak.lock()) {
                    completions->post(runtime::CompletionPriority::Visible,
                                      [impl, ticket, result = std::move(result)]() mutable {
                                          impl->on_transport_result(ticket, std::move(result));
                                      });
                }
            });
    };
    task.completion = [weak, ticket, state]() {
        if (auto impl = weak.lock()) {
            impl->on_started(ticket, state->accepted);
        }
    };
    return executor_->post(std::move(task));
}

void NetClientImpl::on_started(NetTicket ticket, bool accepted) {
    NetRecord* const record = find(ticket);
    if (record == nullptr || record->machine.finished()) {
        return;
    }
    if (!accepted) {
        // The transport never started the attempt, so its completion will never arrive; resolving
        // here is the only way this hop can finish.
        resolve(ticket, std::unexpected(
                            NetError{NetErrorKind::Other, "the transport refused the request", 0}));
        return;
    }
    if (record->machine.state() == NetRequestState::Idle) {
        record->machine.begin_attempt();
        arm_deadline(*record);
    }
}

void NetClientImpl::on_transport_result(NetTicket ticket, NetResult result) {
    NetRecord* const record = find(ticket);
    if (record == nullptr || record->machine.finished()) {
        // A late result for a request a cancel, a timeout or the client's bound already resolved.
        // Section 3 item 4: a cancelled request never delivers a body, so it is dropped - and
        // counted, because a silently dropped completion is a bug a diagnostic should show.
        ++diagnostics_.stale_completions;
        return;
    }
    if (record->machine.state() == NetRequestState::Idle) {
        // The transport reported before the executor's start notification was drained. That is
        // legal ordering, and the attempt still counts as one hop.
        record->machine.begin_attempt();
        arm_deadline(*record);
    }
    if (!result.has_value()) {
        resolve(ticket, std::move(result));
        return;
    }
    handle_response(*record, std::move(*result));
}

void NetClientImpl::handle_response(NetRecord& record, NetResponse response) {
    const NetTicket ticket = record.ticket;
    if (response.final_url.empty()) {
        response.final_url = record.request.url;
    }
    if (record.request.redirect == RedirectPolicy::Follow &&
        is_followable_redirect(response.status) && redirect_location(response) != nullptr) {
        // `attempts` counts requests, so after N requests N-1 redirects were followed.
        const std::uint32_t followed =
            record.machine.attempts() == 0 ? 0 : record.machine.attempts() - 1;
        auto next = follow_redirect(record.request, response, followed);
        if (next.has_value()) {
            ++diagnostics_.redirects;
            record.request = std::move(*next);
            if (!record.machine.begin_attempt()) {
                return;
            }
            // A redirect is a new attempt, so section 3 item 3's deadline restarts. The timer is
            // re-armed before the dispatch, because a hop may resolve synchronously inside `start`.
            arm_deadline(record);
            if (!dispatch(ticket)) {
                resolve_later(ticket,
                              NetError{NetErrorKind::Other, "the executor refused the request", 0});
            }
            return;
        }
        // A relative target, a downgrade or a `Location` that does not parse ends the chain as an
        // error; `too_many_redirects` is the one the spec names, the others are the seam's
        // documented gaps (docs/design/networking-module.md section 7).
        if (next.error().kind != NetErrorKind::Other) {
            resolve(ticket, std::unexpected(next.error()));
            return;
        }
        // `Other`: a 3xx whose `Location` is empty. There is nothing to follow, so the response is
        // the result - the caller sees the status and decides, exactly as with
        // `RedirectPolicy::Manual`.
    }
    resolve(ticket, NetResult{std::move(response)});
}

bool NetClientImpl::resolve(NetTicket ticket, NetResult result) {
    NetRecord* const record = find(ticket);
    if (record == nullptr || record->machine.finished()) {
        return false;
    }
    disarm_deadline(*record);
    // Section 3 item 4: cancelling the attempt's token is how the back end learns to release the
    // connection, including on a timeout this module measured itself.
    record->attempt_token.cancel();

    if (result.has_value()) {
        record->response = std::move(*result);
        record->machine.accept(record->response.status);
        ++diagnostics_.completed;
    } else {
        record->error = std::move(result.error());
        record->machine.fail(record->error);
        ++diagnostics_.failed;
        switch (record->machine.outcome()) {
        case NetOutcome::Cancelled:
            ++diagnostics_.cancelled;
            break;
        case NetOutcome::Timeout:
            ++diagnostics_.timed_out;
            break;
        default:
            break;
        }
    }
    deliver(*record);
    return true;
}

void NetClientImpl::resolve_later(NetTicket ticket, NetError error) {
    std::weak_ptr<NetClientImpl> weak = weak_from_this();
    const bool posted = completions_->post(
        runtime::CompletionPriority::Visible, [weak, ticket, error = std::move(error)]() mutable {
            if (auto impl = weak.lock()) {
                impl->resolve(ticket, std::unexpected(std::move(error)));
            }
        });
    if (!posted) {
        // A full queue would otherwise strand the record forever. The caller has not been handed
        // the ticket yet in the one path that reaches here (`submit`'s refused dispatch), so
        // resolving inline cannot call back into a caller that is still inside `submit`.
        resolve(ticket, std::unexpected(std::move(error)));
    }
}

void NetClientImpl::deliver(NetRecord& record) {
    // The caller's payment is collected *before* the callback runs: a callback that releases,
    // cancels or submits re-enters this class, and the record it was delivered for may be gone by
    // the time it returns.
    NetClient::Callback callback = std::move(record.callback);
    record.callback = nullptr;
    if (!callback) {
        return;
    }
    NetResult outcome = record.machine.state() == NetRequestState::Done
                            ? NetResult{record.response}
                            : NetResult{std::unexpected(record.error)};
    callback(std::move(outcome));
}

void NetClientImpl::arm_deadline(NetRecord& record) {
    disarm_deadline(record);
    if (record.request.timeout_ms == 0) {
        return;
    }
    const double timeout_seconds = static_cast<double>(record.request.timeout_ms) / 1000.0;
    record.deadline_seconds = clock_->now_seconds() + timeout_seconds;
    const NetTicket ticket = record.ticket;
    std::weak_ptr<NetClientImpl> weak = weak_from_this();
    auto id = timers_->schedule_one_shot(record.deadline_seconds, [weak, ticket]() {
        if (auto impl = weak.lock()) {
            impl->resolve(ticket, std::unexpected(NetError{NetErrorKind::Timeout,
                                                           "the attempt exceeded its timeout", 0}));
        }
    });
    if (id.has_value()) {
        record.deadline_timer = *id;
        record.timer_armed = true;
    }
    // A full timer queue is not fatal: `tick()` re-checks `deadline_seconds` against the clock, so
    // the deadline is still enforced, just not woken by the queue.
}

void NetClientImpl::disarm_deadline(NetRecord& record) {
    if (record.timer_armed) {
        timers_->cancel(record.deadline_timer);
        record.timer_armed = false;
    }
    record.deadline_seconds = 0.0;
}

} // namespace tessera::appfw::detail
