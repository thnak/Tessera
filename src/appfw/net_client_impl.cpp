// SPDX-License-Identifier: MIT
//
// `NetClientImpl`: records, submission, delivery and the state transitions (docs/specs/
// networking-v1.md sections 2 and 3; issue #197). This file owns everything except the per-hop
// plumbing (dispatch, the transport completion, the redirect loop and the deadline), which is in
// `net_client_hop.cpp`, and the public facade, which is in `net_client.cpp`.
//
// Threading: every method here runs on the thread that drains the completion queue. `dispatch` is
// the only place that touches the executor, and the body it posts touches only a copy of the
// request, so the UI thread never performs the blocking call.

#include "net_client_internal.hpp"

#include <string>
#include <utility>

namespace tessera::appfw::detail {

NetClientImpl::NetClientImpl(NetClientConfig config, NetTransport& transport,
                             runtime::IoTaskExecutor& executor,
                             runtime::CompletionQueue& completions, runtime::TimerQueue& timers,
                             runtime::Clock& clock)
    : config_(std::move(config)),
      transport_(&transport),
      executor_(&executor),
      completions_(&completions),
      timers_(&timers),
      clock_(&clock) {
    if (config_.max_in_flight == 0) {
        config_.max_in_flight = 1;
    }
    if (config_.max_records == 0) {
        config_.max_records = config_.max_in_flight;
    }
    // One slot per record plus one spare, so "evict the oldest terminal record" works even when the
    // record bound is reached with a request in flight: `acquire_slot` then refuses instead of
    // evicting live work.
    slots_.resize(config_.max_records + 1);
    diagnostics_.capacity = config_.diagnostic_capacity;
}

NetClientImpl::~NetClientImpl() {
    // A destroyed client resolves nothing and delivers nothing: the records own the callbacks, and
    // a callback after the client is gone would touch freed state. The transport's token is
    // cancelled so a real back end releases its connection rather than leaking it.
    stopped_ = true;
    for (Slot& slot : slots_) {
        if (slot.record == nullptr) {
            continue;
        }
        slot.record->attempt_token.cancel();
        if (slot.record->timer_armed) {
            timers_->cancel(slot.record->deadline_timer);
        }
        slot.record.reset();
        ++slot.generation;
    }
    records_ = 0;
}

bool NetClientImpl::release(NetTicket ticket) {
    Slot* const slot = slot_of(ticket);
    if (slot == nullptr || !slot->record->machine.finished()) {
        return false;
    }
    dispose(*slot);
    return true;
}

detail::NetRecord* NetClientImpl::find(NetTicket ticket) noexcept {
    Slot* const slot = slot_of(ticket);
    return slot == nullptr ? nullptr : slot->record.get();
}

const detail::NetRecord* NetClientImpl::find(NetTicket ticket) const noexcept {
    const Slot* const slot = slot_of(ticket);
    return slot == nullptr ? nullptr : slot->record.get();
}

detail::NetClientImpl::Slot* NetClientImpl::slot_of(NetTicket ticket) noexcept {
    return const_cast<Slot*>(static_cast<const NetClientImpl&>(*this).slot_of(ticket));
}

const detail::NetClientImpl::Slot* NetClientImpl::slot_of(NetTicket ticket) const noexcept {
    if (ticket.value == 0) {
        return nullptr;
    }
    const std::uint64_t index = (ticket.value >> 32) - 1;
    const auto generation = static_cast<std::uint32_t>(ticket.value & 0xffffffffu);
    if (index >= slots_.size()) {
        return nullptr;
    }
    const Slot& slot = slots_[static_cast<std::size_t>(index)];
    if (slot.record == nullptr || slot.generation != generation) {
        return nullptr;
    }
    return &slot;
}

detail::NetClientImpl::Slot* NetClientImpl::acquire_slot() {
    // Under the bound, a free slot is used without evicting anything. At the bound the oldest
    // terminal record is evicted, never one in flight - which is why `slots_` holds one spare: even
    // when `max_records` records are terminal, a new request still has somewhere to go.
    if (records_ < config_.max_records) {
        for (Slot& slot : slots_) {
            if (slot.record == nullptr) {
                return &slot;
            }
        }
    }
    for (Slot& slot : slots_) {
        if (slot.record != nullptr && slot.record->machine.finished()) {
            dispose(slot);
            return &slot;
        }
    }
    return nullptr;
}

void NetClientImpl::dispose(Slot& slot) noexcept {
    slot.record.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        // The generation wrapped: the next record in this slot starts at 1, never 0, so a ticket is
        // never `0` (which means "none").
        slot.generation = 1;
    }
    if (records_ > 0) {
        --records_;
    }
}

std::expected<NetTicket, NetError> NetClientImpl::submit(NetRequest request,
                                                         runtime::CancellationToken token,
                                                         NetClient::Callback callback) {
    if (stopped_) {
        return std::unexpected(NetError{NetErrorKind::Other, "the client is shut down", 0});
    }
    if (!transport_->available()) {
        // Section 4: "A back end may be unavailable in a build; `send` then yields `unsupported`."
        ++diagnostics_.refused;
        record_diagnostic(NetErrorKind::Unsupported, "no networking back end in this build");
        return std::unexpected(
            NetError{NetErrorKind::Unsupported, "no networking back end in this build", 0});
    }
    auto url = parse_url(request.url);
    if (!url.has_value()) {
        // Refused before a record exists. Section 3 item 9: the diagnostic names the redacted URL,
        // so a bad query string never reaches a log line.
        ++diagnostics_.refused;
        record_diagnostic(url.error().kind, redacted_url(request.url));
        return std::unexpected(url.error());
    }
    if (!config_.allow_cleartext && is_cleartext_scheme(*url)) {
        // Section 3 item 8: plain http only when the app enables it. Converted to `unsupported`
        // here, not in a back end, so every platform refuses it identically.
        ++diagnostics_.refused;
        record_diagnostic(NetErrorKind::Unsupported, redacted_url(request.url));
        return std::unexpected(
            NetError{NetErrorKind::Unsupported, "cleartext http is not enabled", 0});
    }
    if (in_flight() >= config_.max_in_flight) {
        ++diagnostics_.refused;
        record_diagnostic(NetErrorKind::Other, "at the in-flight bound");
        return std::unexpected(NetError{NetErrorKind::Other, "too many requests in flight", 0});
    }
    Slot* const slot = acquire_slot();
    if (slot == nullptr) {
        ++diagnostics_.refused;
        record_diagnostic(NetErrorKind::Other, "at the record bound");
        return std::unexpected(NetError{NetErrorKind::Other, "too many requests tracked", 0});
    }

    const auto index = static_cast<std::uint64_t>(slot - slots_.data()) + 1;
    const NetTicket ticket{(index << 32) | static_cast<std::uint64_t>(slot->generation)};
    auto record = std::make_unique<NetRecord>();
    record->ticket = ticket;
    record->request = std::move(request);
    record->request.url = url_string(*url);
    record->caller_token = std::move(token);
    record->callback = std::move(callback);
    // Section 3 item 7: the fixed `Tessera/<version>` and nothing else. A caller's own `User-Agent`
    // wins, because the rule is "add one if there is none", not "overwrite".
    if (config_.send_default_user_agent && !has_header(record->request.headers, "user-agent")) {
        set_header(record->request.headers, "User-Agent",
                   config_.user_agent.empty() ? default_user_agent() : config_.user_agent);
    }
    slot->record = std::move(record);
    ++records_;
    ++diagnostics_.submitted;

    if (slot->record->caller_token.cancelled()) {
        // Section 3 item 4 applies to a token that was already cancelled when the request arrived:
        // it resolves as `cancelled` and no attempt is started at all, so a caller that already
        // gave up does not open a connection. Resolved through the queue, so the callback stays
        // asynchronous.
        resolve_later(ticket, NetError{NetErrorKind::Cancelled, "the request was cancelled", 0});
        return ticket;
    }

    // The first hop is queued like every other one: the caller's thread does no I/O, so `submit`
    // returning is the proof of section 3 item 1 ("never waits on the network on the calling
    // thread").
    if (!dispatch(ticket)) {
        // The executor refused before anything ran, so no completion is coming for it. Resolving
        // through the queue keeps the callback asynchronous.
        resolve_later(ticket, NetError{NetErrorKind::Other, "the executor refused the request", 0});
    }
    return ticket;
}

bool NetClientImpl::cancel(NetTicket ticket) {
    NetRecord* const record = find(ticket);
    if (record == nullptr || record->machine.finished()) {
        return false;
    }
    return resolve(
        ticket, std::unexpected(NetError{NetErrorKind::Cancelled, "the request was cancelled", 0}));
}

void NetClientImpl::tick() {
    if (stopped_ || records_ == 0) {
        return;
    }
    scratch_.clear();
    for (Slot& slot : slots_) {
        const NetRecord* const record = slot.record.get();
        if (record == nullptr || record->machine.finished()) {
            continue;
        }
        // Two backstops, both resolved to the same value the primary path would produce:
        //   * the caller's token, so a back end that ignores its token cannot hold a cancelled
        //     request past one tick (section 3 item 4);
        //   * a deadline whose timer could not be armed because the timer queue was full
        //     (section 3 item 3).
        const bool expired = record->timer_armed == false && record->deadline_seconds > 0.0 &&
                             clock_->now_seconds() >= record->deadline_seconds;
        if (record->caller_token.cancelled() || expired) {
            scratch_.push_back(record->ticket);
        }
    }
    // Resolving invokes caller code, which may submit or cancel; the loop walks a snapshot (the
    // member vector, so a steady-state tick still allocates nothing).
    for (const NetTicket ticket : scratch_) {
        NetRecord* const record = find(ticket);
        if (record == nullptr || record->machine.finished()) {
            continue;
        }
        if (record->caller_token.cancelled()) {
            resolve(ticket, std::unexpected(
                                NetError{NetErrorKind::Cancelled, "the request was cancelled", 0}));
        } else {
            resolve(ticket, std::unexpected(NetError{NetErrorKind::Timeout,
                                                     "the attempt exceeded its timeout", 0}));
        }
    }
}

NetRequestStatus NetClientImpl::status_of(NetTicket ticket) const {
    const NetRecord* const record = find(ticket);
    NetRequestStatus status;
    status.ticket = ticket;
    if (record == nullptr) {
        return status;
    }
    status.known = true;
    status.state = record->machine.state();
    status.outcome = record->machine.outcome();
    status.attempts = record->machine.attempts();
    status.status = record->machine.status();
    status.error_kind = record->machine.error_kind();
    status.body_bytes = record->response.body.size();
    return status;
}

const NetResponse* NetClientImpl::response_of(NetTicket ticket) const {
    const NetRecord* const record = find(ticket);
    return record != nullptr && record->machine.state() == NetRequestState::Done ? &record->response
                                                                                 : nullptr;
}

const NetError* NetClientImpl::error_of(NetTicket ticket) const {
    const NetRecord* const record = find(ticket);
    return record != nullptr && record->machine.state() == NetRequestState::Error ? &record->error
                                                                                  : nullptr;
}

std::size_t NetClientImpl::in_flight() const noexcept {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.record != nullptr && !slot.record->machine.finished()) {
            ++count;
        }
    }
    return count;
}

NetDiagnostics NetClientImpl::diagnostics() const {
    NetDiagnostics copy = diagnostics_;
    copy.recorded = copy.entries.size();
    return copy;
}

std::string_view NetClientImpl::transport_name() const noexcept {
    return transport_->name();
}

void NetClientImpl::record_diagnostic(NetErrorKind kind, std::string_view detail) {
    if (config_.diagnostic_capacity == 0) {
        return;
    }
    if (diagnostics_.entries.size() >= config_.diagnostic_capacity) {
        diagnostics_.entries.erase(diagnostics_.entries.begin());
        ++diagnostics_.dropped;
    }
    diagnostics_.entries.push_back(NetDiagnostic{kind, std::string(detail)});
}

} // namespace tessera::appfw::detail
