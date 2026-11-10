// SPDX-License-Identifier: MIT

#include "tessera/harness/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

namespace tessera::harness {
namespace {

ProtocolError timeout_error(std::string message) {
    return ProtocolError{ProtocolErrorCode::Timeout, std::move(message), "commandTimeout",
                         std::nullopt};
}

ProtocolError shutdown_error() {
    return ProtocolError{ProtocolErrorCode::Internal, "session is shutting down", "shutdown",
                         std::nullopt};
}

ProtocolError line_cap_error() {
    return ProtocolError{ProtocolErrorCode::Internal, "response exceeds the transport line cap",
                         "lineCap", std::nullopt};
}

Response failure_response(RequestId id, ProtocolError error) {
    Response response;
    response.id = id;
    response.ok = false;
    response.error = std::move(error);
    response.error.request_id = id;
    return response;
}

} // namespace

std::uint64_t SteadySessionClock::now_us() const {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

SessionExecutor::SessionExecutor(LineTransport& transport, CommandHandler& handler,
                                 SessionClock& clock, SessionLimits limits)
    : transport_(transport),
      handler_(handler),
      clock_(clock),
      limits_(limits) {
    // A zero caps would stall every pump forever; one is the smallest useful bound.
    limits_.max_queued_requests = std::max<std::size_t>(limits_.max_queued_requests, 1);
}

std::uint64_t SessionExecutor::deadline_from_now() const noexcept {
    const std::uint64_t now = clock_.now_us();
    const std::uint64_t timeout = limits_.command_timeout_us;
    if (now > std::numeric_limits<std::uint64_t>::max() - timeout) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return now + timeout;
}

void SessionExecutor::handle_line(std::string_view line) {
    auto request = decode_request(line);
    if (!request.has_value()) {
        ProtocolError error = std::move(request.error());
        Pending pending;
        pending.id = error.request_id.value_or(0);
        error.request_id = pending.id;
        pending.error = std::move(error);
        queue_.push_back(std::move(pending));
        return;
    }
    auto command = decode_command(*request);
    if (!command.has_value()) {
        Pending pending;
        pending.id = request->id;
        pending.error = std::move(command.error());
        pending.error.request_id = pending.id;
        queue_.push_back(std::move(pending));
        return;
    }
    Pending pending;
    pending.id = request->id;
    pending.command = std::move(*command);
    pending.deadline_us = deadline_from_now();
    queue_.push_back(std::move(pending));
}

void SessionExecutor::read_phase(SessionPumpStats& stats) {
    while (stats.lines_read < limits_.max_reads_per_pump) {
        if (queue_.size() >= limits_.max_queued_requests) {
            stats.read_backpressure = true;
            return;
        }
        auto line = transport_.read_line();
        if (!line.has_value()) {
            switch (line.error().code) {
            case TransportErrorCode::WouldBlock:
                return;
            case TransportErrorCode::Closed:
                input_closed_ = true;
                return;
            case TransportErrorCode::LineTooLong:
            case TransportErrorCode::InvalidUtf8:
            case TransportErrorCode::EmptyLine:
                // A rejected frame has no id to correlate an answer with, and the framer already
                // recovered at the next terminator; the session stays usable.
                ++stats.lines_read;
                ++stats.dropped_frames;
                continue;
            case TransportErrorCode::EmbeddedTerminator:
            case TransportErrorCode::QueueFull:
            case TransportErrorCode::IoError:
                input_closed_ = true;
                transport_closed_ = true;
                return;
            }
            input_closed_ = true;
            transport_closed_ = true;
            return;
        }
        ++stats.lines_read;
        handle_line(*line);
    }
}

void SessionExecutor::answer_error(RequestId id, ProtocolError error, SessionPumpStats& stats) {
    enqueue_response(failure_response(id, std::move(error)), stats);
}

void SessionExecutor::answer(const Pending& pending, SessionPumpStats& stats) {
    if (!pending.command.has_value()) {
        answer_error(pending.id, pending.error, stats);
        return;
    }
    const std::uint64_t start = clock_.now_us();
    if (start >= pending.deadline_us) {
        answer_error(pending.id, timeout_error("command deadline expired before dispatch"), stats);
        return;
    }
    auto result = handler_.handle(*pending.command);
    const std::uint64_t elapsed = clock_.now_us() - start;
    ++stats.commands_dispatched;

    Response response;
    response.id = pending.id;
    response.elapsed_us = static_cast<std::int64_t>(std::min<std::uint64_t>(
        elapsed, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
    if (elapsed > limits_.command_timeout_us) {
        response.ok = false;
        response.error = timeout_error("command exceeded its time budget");
    } else if (!result.has_value()) {
        response.ok = false;
        response.error = std::move(result.error());
    } else {
        auto value = encode_command_result(*result);
        if (!value.has_value()) {
            response.ok = false;
            response.error = std::move(value.error());
        } else {
            response.result = std::move(*value);
        }
    }
    enqueue_response(std::move(response), stats);
}

void SessionExecutor::enqueue_response(Response response, SessionPumpStats& stats) {
    const RequestId id = response.id;
    auto text = encode_response(response);
    if (!text.has_value() || text->size() > transport_.max_line_bytes()) {
        // A host snapshot can be larger than the line cap. Report that as a typed failure instead
        // of writing a partial frame the peer would have to resynchronize from.
        text = encode_response(failure_response(id, line_cap_error()));
        if (!text.has_value() || text->size() > transport_.max_line_bytes()) {
            ++stats.dropped_responses;
            return;
        }
    }
    pending_out_.push_back(std::move(*text));
    ++stats.requests_answered;
}

void SessionExecutor::dispatch_phase(SessionPumpStats& stats) {
    std::size_t processed = 0;
    while (!queue_.empty() && processed < limits_.max_commands_per_pump && !transport_closed_) {
        Pending pending = std::move(queue_.front());
        queue_.pop_front();
        ++processed;
        answer(pending, stats);
    }
}

void SessionExecutor::flush_output(SessionPumpStats& stats) {
    const bool had_pending = !pending_out_.empty();
    while (!pending_out_.empty() && !transport_closed_) {
        auto written = transport_.write_line(pending_out_.front());
        if (!written.has_value()) {
            switch (written.error().code) {
            case TransportErrorCode::WouldBlock:
            case TransportErrorCode::QueueFull:
                output_blocked_ = true;
                return;
            case TransportErrorCode::Closed:
            case TransportErrorCode::IoError:
            case TransportErrorCode::EmbeddedTerminator:
                transport_closed_ = true;
                pending_out_.clear();
                return;
            case TransportErrorCode::LineTooLong:
            case TransportErrorCode::InvalidUtf8:
            case TransportErrorCode::EmptyLine:
                // Should be unreachable: enqueue_response pre-checks the cap and the encoder
                // produces valid UTF-8. Dropping one response is better than wedging the queue.
                pending_out_.pop_front();
                ++stats.dropped_responses;
                continue;
            }
        }
        pending_out_.pop_front();
        ++stats.responses_written;
        output_blocked_ = false;
    }
    if (transport_closed_) {
        return;
    }
    // A transport with a stalled OS buffer but nothing we know about must not block input work:
    // only flush when this pump produced output or a previous flush is still owed.
    if (!had_pending && !output_blocked_) {
        return;
    }
    if (auto flushed = transport_.flush(); !flushed.has_value()) {
        switch (flushed.error().code) {
        case TransportErrorCode::WouldBlock:
        case TransportErrorCode::QueueFull:
            output_blocked_ = true;
            return;
        default:
            transport_closed_ = true;
            pending_out_.clear();
            return;
        }
    }
    output_blocked_ = false;
}

void SessionExecutor::finish_if_done() {
    if (finished_ || !input_closed_ || !queue_.empty() || !pending_out_.empty() ||
        output_blocked_) {
        return;
    }
    transport_.close();
    finished_ = true;
}

SessionPumpStats SessionExecutor::pump() {
    SessionPumpStats stats;
    if (finished_) {
        stats.closed = true;
        return stats;
    }
    flush_output(stats);
    if (!transport_closed_ && !output_blocked_) {
        read_phase(stats);
    }
    if (!transport_closed_ && !output_blocked_) {
        dispatch_phase(stats);
    }
    flush_output(stats);
    finish_if_done();
    stats.input_closed = input_closed_;
    stats.output_blocked = output_blocked_;
    stats.closed = finished_;
    stats.has_more_work = !finished_ && (!input_closed_ || !queue_.empty() ||
                                         !pending_out_.empty() || output_blocked_);
    return stats;
}

SessionPumpStats SessionExecutor::shutdown() {
    SessionPumpStats stats;
    if (finished_) {
        stats.closed = true;
        return stats;
    }
    input_closed_ = true;
    while (!queue_.empty()) {
        Pending pending = std::move(queue_.front());
        queue_.pop_front();
        if (pending.command.has_value()) {
            answer_error(pending.id, shutdown_error(), stats);
        } else {
            // A decode failure is already decided; report its own error rather than a shutdown.
            answer_error(pending.id, pending.error, stats);
        }
    }
    flush_output(stats);
    // Whatever the peer refused to read is dropped: shutdown must not wait for a reader.
    if (!pending_out_.empty()) {
        stats.dropped_responses += pending_out_.size();
        pending_out_.clear();
    }
    if (!transport_closed_) {
        transport_.close();
        transport_closed_ = true;
    }
    finished_ = true;
    output_blocked_ = false;
    stats.input_closed = true;
    stats.closed = true;
    stats.has_more_work = false;
    return stats;
}

} // namespace tessera::harness
