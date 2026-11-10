// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::harness {

// One protocol frame is one JSON document on one line
// (docs/specs/headless-automation-v1.md section 6). kMaxLineBytes bounds both how much a peer can
// make us buffer and how large a document the decoder can be asked to walk; it is far above the
// largest envelope the harness sends today (a snapshot of ~10k nodes). The cap is on the payload:
// a line may additionally carry a CR before its LF.
inline constexpr std::size_t kMaxLineBytes = std::size_t{1} << 20U;

enum class FrameErrorCode {
    LineTooLong, // payload exceeded kMaxLineBytes; framing resumed after the terminator
    InvalidUtf8, // payload is not well-formed UTF-8
    EmptyLine,   // a terminator with no payload is not a frame
};

// Why one line was dropped. A rejected line is dropped whole, so a malformed frame can never be
// half-delivered to the JSON decoder.
struct FrameError {
    FrameErrorCode code = FrameErrorCode::EmptyLine;
    std::size_t offset = 0; // byte offset from the start of the rejected line's payload

    friend constexpr bool operator==(const FrameError&, const FrameError&) = default;
};

// Incremental byte-stream framer. LF terminates a frame; a CR immediately before the LF is
// tolerated and stripped (CRLF peers are common on Windows hosts); finish() delivers a final line
// that has no terminator, which is what an EOF-terminated stdin stream looks like.
//
// The framer is pull-style and allocation-light: bytes are appended to one bounded buffer and each
// next() call either hands out one line or reports that more bytes are needed. Nothing is decoded
// until a terminator arrives, so a partial line is never observable.
class LineFramer {
public:
    enum class Status {
        Line,        // `line` holds one validated payload
        NeedMore,    // no complete line yet: append() more bytes or call finish()
        Rejected,    // `error` says why the current line was dropped
        EndOfStream, // finish() was called and every byte was consumed
    };

    explicit LineFramer(std::size_t max_line_bytes = kMaxLineBytes) noexcept;

    // Feeds raw bytes. A line that crosses the cap is rejected immediately and the rest of that
    // line is discarded, so a peer cannot grow our buffer without bound; framing resumes at the
    // next LF. Only one rejection is held at a time: draining next() between feeds is how a
    // caller observes every rejected line, and the transport implementations do exactly that.
    void append(std::string_view bytes);

    // Declares that no more bytes will arrive. A pending unterminated line is still delivered.
    void finish() noexcept;

    // Advances to the next event. `line` is overwritten only for Status::Line.
    Status next(std::string& line, FrameError& error);

    [[nodiscard]] std::size_t max_line_bytes() const noexcept {
        return max_line_bytes_;
    }
    [[nodiscard]] std::size_t pending_bytes() const noexcept {
        return buffer_.size();
    }
    [[nodiscard]] bool drained() const noexcept {
        return drained_;
    }

private:
    // Number of extra bytes a terminator (LF, or CRLF) may add beyond the payload cap.
    static constexpr std::size_t kTerminatorSlack = 2;

    void reject(FrameErrorCode code, std::size_t offset) noexcept;
    // Validates one complete payload; returns Line on success and Rejected (with `error` set)
    // otherwise. Never mutates framing state.
    Status classify(std::string&& payload, std::string& line, FrameError& error);

    std::string buffer_;         // complete lines still to deliver, then the current partial line
    std::size_t line_start_ = 0; // offset in buffer_ where the current partial line begins
    std::size_t scan_ = 0;       // buffer_ prefix already proven free of a terminator
    std::size_t max_line_bytes_;
    std::optional<FrameError> rejection_;
    // Buffer offset where the rejected line began. Complete lines before this offset are still
    // owed to the caller and must be delivered before the rejection; lines at or after it follow
    // the rejected line and must not overtake it.
    std::size_t rejection_at_ = 0;
    bool discarding_ = false; // inside an over-long line: ignore bytes until the LF
    bool finished_ = false;
    bool drained_ = false;
};

} // namespace tessera::harness
