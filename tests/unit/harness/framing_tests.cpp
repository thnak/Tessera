// SPDX-License-Identifier: MIT

#include "tessera/harness/line_framing.hpp"
#include "tessera/harness/utf8.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::harness::FrameError;
using tessera::harness::FrameErrorCode;
using tessera::harness::is_valid_utf8;
using tessera::harness::LineFramer;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_framing: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Drains every event the framer can produce without more input.
struct Event {
    LineFramer::Status status = LineFramer::Status::NeedMore;
    std::string line;
    FrameError error;
};

std::vector<Event> drain(LineFramer& framer, std::size_t limit = 32) {
    std::vector<Event> events;
    while (events.size() < limit) {
        Event event;
        event.status = framer.next(event.line, event.error);
        if (event.status == LineFramer::Status::NeedMore ||
            event.status == LineFramer::Status::EndOfStream) {
            events.push_back(event);
            break;
        }
        events.push_back(event);
    }
    return events;
}

} // namespace

int main() {
    // LF terminates a frame; several frames arrive from one append.
    {
        LineFramer framer;
        framer.append("{\"id\":1}\n{\"id\":2}\n");
        const std::vector<Event> events = drain(framer);
        expect(events.size() == 3, "two frames plus NeedMore");
        expect(events[0].status == LineFramer::Status::Line, "first frame");
        expect(events[0].line == "{\"id\":1}", "first payload");
        expect(events[1].status == LineFramer::Status::Line, "second frame");
        expect(events[1].line == "{\"id\":2}", "second payload");
        expect(events[2].status == LineFramer::Status::NeedMore, "needs more after two frames");
        expect(framer.pending_bytes() == 0, "no pending bytes");
    }

    // CRLF is tolerated and stripped; a CR that is not before the LF stays in the payload.
    {
        LineFramer framer;
        framer.append("a\r\nb\r\nc\r");
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "a", "crlf strip");
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "b", "crlf strip 2");
        expect(framer.next(line, error) == LineFramer::Status::NeedMore, "partial line");
        framer.finish();
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "c\r",
               "unterminated tail keeps its CR");
        expect(framer.next(line, error) == LineFramer::Status::EndOfStream, "eof after tail");
    }

    // A line split across appends is never observable until its terminator arrives, and a
    // multi-byte sequence may be split too.
    {
        LineFramer framer;
        std::string line;
        FrameError error;
        framer.append("\"caf");
        expect(framer.next(line, error) == LineFramer::Status::NeedMore, "partial append");
        framer.append("\xC3");
        expect(framer.next(line, error) == LineFramer::Status::NeedMore, "split utf-8 lead");
        framer.append("\xA9\"\n");
        expect(framer.next(line, error) == LineFramer::Status::Line, "split utf-8 line");
        expect(line == "\"caf\xC3\xA9\"", "utf-8 payload reassembled");
    }

    // A final line without a terminator is delivered at EOF, and an empty buffer ends the stream.
    {
        LineFramer framer;
        framer.append("tail");
        framer.finish();
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "tail", "eof tail");
        expect(framer.next(line, error) == LineFramer::Status::EndOfStream, "eof drained");
        expect(framer.drained(), "drained flag");
        expect(framer.next(line, error) == LineFramer::Status::EndOfStream, "eof is sticky");
    }
    {
        LineFramer framer;
        framer.finish();
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::EndOfStream, "empty eof");
    }

    // The cap is on the payload: exactly max bytes pass, with or without a CRLF, and one more
    // byte is rejected without losing the following frame.
    {
        constexpr std::size_t kCap = 8;
        LineFramer framer(kCap);
        framer.append("12345678\n12345678\r\n123456789\nok\n");
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "12345678",
               "exact cap accepted");
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "12345678",
               "exact cap with crlf accepted");
        expect(framer.next(line, error) == LineFramer::Status::Rejected, "over cap rejected");
        expect(error.code == FrameErrorCode::LineTooLong && error.offset == kCap,
               "line-too-long offset is the cap");
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "ok",
               "framing resumes after the discarded line");
    }

    // An over-long line is rejected as soon as the cap is crossed, so a peer that never sends a
    // terminator cannot grow the buffer past the cap + terminator slack.
    {
        constexpr std::size_t kCap = 16;
        LineFramer framer(kCap);
        std::string line;
        FrameError error;
        framer.append(std::string(kCap, 'x'));
        expect(framer.next(line, error) == LineFramer::Status::NeedMore, "at cap is not over");
        framer.append(std::string(kCap, 'x'));
        expect(framer.next(line, error) == LineFramer::Status::Rejected,
               "rejected while streaming");
        expect(error.code == FrameErrorCode::LineTooLong, "streaming too-long code");
        for (std::size_t chunk = 0; chunk < 64; ++chunk) {
            framer.append(std::string(kCap, 'x'));
            expect(framer.pending_bytes() <= kCap + 2, "bounded buffer while discarding");
        }
        framer.append("\n");
        framer.append("after\n");
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "after",
               "recovered after discarded oversized line");
    }

    // Invalid UTF-8 rejects the whole line but keeps framing: the bad line is dropped, not
    // truncated. Overlong, surrogate, out-of-range, bare continuation and truncated sequences are
    // all ill-formed.
    {
        LineFramer framer;
        framer.append("good\n\xC0\x80\n\xED\xA0\x80\n\xF4\x90\x80\x80\n\x80\n\xE2\x82\nafter\n");
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "good", "good line");
        const char* labels[] = {"overlong", "surrogate", "out-of-range", "bare continuation",
                                "truncated"};
        for (const char* label : labels) {
            expect(framer.next(line, error) == LineFramer::Status::Rejected, label);
            expect(error.code == FrameErrorCode::InvalidUtf8, "invalid utf-8 code");
            expect(error.offset == 0, "invalid utf-8 offset");
        }
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "after",
               "framing survives bad utf-8");
    }

    // A blank line is rejected as a frame and framing continues.
    {
        LineFramer framer;
        framer.append("\n\r\nok\n");
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::Rejected, "blank line rejected");
        expect(error.code == FrameErrorCode::EmptyLine, "blank line code");
        expect(framer.next(line, error) == LineFramer::Status::Rejected, "crlf blank rejected");
        expect(framer.next(line, error) == LineFramer::Status::Line && line == "ok",
               "blank line skipped");
    }

    // Empty appends are no-ops, and a rejected line is reported exactly once.
    {
        LineFramer framer;
        framer.append("");
        framer.append("\xFF\n");
        std::string line;
        FrameError error;
        expect(framer.next(line, error) == LineFramer::Status::Rejected, "rejection reported");
        expect(framer.next(line, error) == LineFramer::Status::NeedMore, "once only");
    }

    // The standalone UTF-8 validator is the same rule the framer applies.
    {
        std::size_t offset = 999;
        expect(is_valid_utf8("plain ascii"), "ascii valid");
        expect(is_valid_utf8("\xE2\x9C\x93", &offset), "three-byte valid");
        expect(offset == 999, "offset untouched on success");
        expect(is_valid_utf8("\xF0\x9F\x98\x80"), "four-byte valid");
        expect(!is_valid_utf8("ab\xE2\x28\xA1"), "bad continuation");
        expect(!is_valid_utf8("ab\xF0\x9F\x98", &offset) && offset == 2, "truncated offset");
        expect(!is_valid_utf8("\xC1\xBF"), "overlong lead");
        expect(!is_valid_utf8("\xED\xBF\xBF"), "surrogate");
        expect(!is_valid_utf8("\xF5\x80\x80\x80"), "past U+10FFFF");
    }

    if (failures != 0) {
        std::cerr << "harness_framing: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
