// SPDX-License-Identifier: MIT

#include "tessera/harness/line_framing.hpp"
#include "tessera/harness/utf8.hpp"

#include <utility>

namespace tessera::harness {
namespace {

constexpr char kLineFeed = '\n';
constexpr char kCarriageReturn = '\r';

} // namespace

LineFramer::LineFramer(std::size_t max_line_bytes) noexcept
    : max_line_bytes_(max_line_bytes) {}

void LineFramer::append(std::string_view bytes) {
    while (!bytes.empty()) {
        if (discarding_) {
            const std::size_t terminator = bytes.find(kLineFeed);
            if (terminator == std::string_view::npos) {
                return;
            }
            discarding_ = false;
            bytes.remove_prefix(terminator + 1);
            continue;
        }
        // Work one line at a time so bytes after a terminator never count towards the current
        // line's length: a buffer that already holds complete lines must not trip the cap.
        const std::size_t terminator = bytes.find(kLineFeed);
        const std::string_view chunk =
            terminator == std::string_view::npos ? bytes : bytes.substr(0, terminator + 1);
        bytes.remove_prefix(chunk.size());
        // Reserve room for a terminator so a line of exactly max_line_bytes_ still fits with its
        // LF (or CRLF). Anything beyond that can only be an over-long payload.
        const std::size_t line_bytes = buffer_.size() - line_start_;
        const std::size_t room = max_line_bytes_ + kTerminatorSlack - line_bytes;
        if (chunk.size() <= room) {
            buffer_.append(chunk);
            if (chunk.back() == kLineFeed) {
                line_start_ = buffer_.size();
            }
            continue;
        }
        // Reject the over-long line and drop its bytes, keeping any complete lines already
        // buffered so they are still delivered before the rejection.
        reject(FrameErrorCode::LineTooLong, max_line_bytes_);
        buffer_.resize(line_start_);
        scan_ = 0;
        // A terminator inside this chunk already moved `bytes` past it; otherwise the rest of the
        // over-long line is discarded on later appends.
        discarding_ = terminator == std::string_view::npos;
    }
}

void LineFramer::finish() noexcept {
    finished_ = true;
}

LineFramer::Status LineFramer::next(std::string& line, FrameError& error) {
    const std::size_t terminator = buffer_.find(kLineFeed, scan_);
    if (terminator != std::string::npos) {
        std::size_t payload_size = terminator;
        if (payload_size > 0 && buffer_[payload_size - 1] == kCarriageReturn) {
            --payload_size;
        }
        std::string payload = buffer_.substr(0, payload_size);
        const std::size_t consumed = terminator + 1;
        buffer_.erase(0, consumed);
        line_start_ = line_start_ >= consumed ? line_start_ - consumed : 0;
        scan_ = 0;
        return classify(std::move(payload), line, error);
    }
    scan_ = buffer_.size();
    // A line rejected while streaming costs no payload: its bytes are already gone.
    if (rejection_.has_value()) {
        error = *rejection_;
        rejection_.reset();
        return Status::Rejected;
    }
    if (!finished_) {
        return Status::NeedMore;
    }
    // EOF without a terminator: the tail is a frame of its own. A missing LF must not also strip a
    // CR that belongs to the payload.
    drained_ = true;
    if (buffer_.empty()) {
        return Status::EndOfStream;
    }
    std::string payload = std::move(buffer_);
    buffer_.clear();
    line_start_ = 0;
    scan_ = 0;
    return classify(std::move(payload), line, error);
}

LineFramer::Status LineFramer::classify(std::string&& payload, std::string& line,
                                        FrameError& error) {
    if (payload.empty()) {
        error = FrameError{FrameErrorCode::EmptyLine, 0};
        return Status::Rejected;
    }
    if (payload.size() > max_line_bytes_) {
        error = FrameError{FrameErrorCode::LineTooLong, max_line_bytes_};
        return Status::Rejected;
    }
    std::size_t bad_offset = 0;
    if (!is_valid_utf8(payload, &bad_offset)) {
        error = FrameError{FrameErrorCode::InvalidUtf8, bad_offset};
        return Status::Rejected;
    }
    line = std::move(payload);
    return Status::Line;
}

void LineFramer::reject(FrameErrorCode code, std::size_t offset) noexcept {
    rejection_ = FrameError{code, offset};
}

} // namespace tessera::harness
