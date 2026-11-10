// SPDX-License-Identifier: MIT

#include "tessera/harness/in_process_transport.hpp"
#include "tessera/harness/line_transport.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::harness::InProcessPair;
using tessera::harness::kMaxLineBytes;
using tessera::harness::LineTransport;
using tessera::harness::make_in_process_pair;
using tessera::harness::TransportError;
using tessera::harness::TransportErrorCode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_transport: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string read_or(LineTransport& transport, const char* message) {
    const auto result = transport.read_line();
    if (!result.has_value()) {
        expect(false, message);
        return {};
    }
    return *result;
}

} // namespace

int main() {
    // A write on one endpoint is a readable line on the other, with the transport owning the
    // terminator.
    {
        InProcessPair pair = make_in_process_pair();
        LineTransport& first = *pair.first;
        LineTransport& second = *pair.second;
        expect(second.read_line().error().code == TransportErrorCode::WouldBlock,
               "empty pipe would block");
        expect(first.write_line("{\"id\":1}").has_value(), "first write");
        expect(read_or(second, "second reads first write") == "{\"id\":1}", "payload preserved");
        expect(second.read_line().error().code == TransportErrorCode::WouldBlock, "fifo drained");
        expect(second.write_line("reply").has_value(), "reply write");
        expect(read_or(first, "first reads reply") == "reply", "reply payload");
    }

    // Many lines keep their order in both directions.
    {
        InProcessPair pair = make_in_process_pair();
        for (int index = 0; index < 16; ++index) {
            expect(pair.first->write_line("f" + std::to_string(index)).has_value(),
                   "ordered write");
            expect(pair.second->write_line("s" + std::to_string(index)).has_value(),
                   "ordered reply write");
        }
        for (int index = 0; index < 16; ++index) {
            expect(read_or(*pair.second, "ordered read") == "f" + std::to_string(index),
                   "second endpoint order");
            expect(read_or(*pair.first, "ordered reply read") == "s" + std::to_string(index),
                   "first endpoint order");
        }
    }

    // Payloads are validated before they reach the peer: an embedded terminator would let one
    // write smuggle a second frame, so it is rejected instead.
    {
        InProcessPair pair = make_in_process_pair();
        expect(pair.first->write_line("").error().code == TransportErrorCode::EmptyLine,
               "empty payload rejected");
        expect(pair.first->write_line("a\nb").error().code ==
                   TransportErrorCode::EmbeddedTerminator,
               "embedded lf rejected");
        expect(pair.first->write_line("a\rb").error().code ==
                   TransportErrorCode::EmbeddedTerminator,
               "embedded cr rejected");
        expect(pair.first->write_line("\xC0\x80").error().code == TransportErrorCode::InvalidUtf8,
               "invalid utf-8 rejected");
        expect(pair.second->read_line().error().code == TransportErrorCode::WouldBlock,
               "rejected writes never arrive");
        expect(pair.first->write_line("ok").has_value(), "valid write after rejects");
        expect(read_or(*pair.second, "valid payload") == "ok", "peer sees only the valid frame");
    }

    // The cap is enforced on write and on the transport's advertised limit.
    {
        constexpr std::size_t kCap = 4;
        InProcessPair pair = make_in_process_pair(kCap);
        expect(pair.first->max_line_bytes() == kCap && pair.second->max_line_bytes() == kCap,
               "cap advertised");
        expect(pair.first->write_line("1234").has_value(), "exact cap accepted");
        expect(read_or(*pair.second, "exact cap read") == "1234", "exact cap payload");
        expect(pair.first->write_line("12345").error().code == TransportErrorCode::LineTooLong,
               "cap + 1 rejected");
        expect(kMaxLineBytes >= 65536, "documented cap is usable");
    }

    // Closing one endpoint drains queued lines first, then reports Closed forever; writes to a
    // closed peer fail.
    {
        InProcessPair pair = make_in_process_pair();
        expect(pair.first->write_line("last").has_value(), "write before close");
        pair.first->close();
        expect(pair.first->closed(), "closed flag");
        expect(pair.first->write_line("again").error().code == TransportErrorCode::Closed,
               "write after close");
        expect(pair.first->read_line().error().code == TransportErrorCode::Closed,
               "read after close");
        expect(pair.second->peer_closed(), "peer observes close");
        expect(pair.second->write_line("x").error().code == TransportErrorCode::Closed,
               "write to closed peer");
        expect(read_or(*pair.second, "buffered line survives close") == "last",
               "drain buffered line");
        expect(pair.second->read_line().error().code == TransportErrorCode::Closed,
               "closed after drain");
        expect(pair.second->read_line().error().code == TransportErrorCode::Closed,
               "closed is sticky");
        pair.first->close();
        expect(pair.first->closed(), "close is idempotent");
    }

    // Closing an endpoint with nothing queued reports Closed, never a spurious empty frame.
    {
        InProcessPair pair = make_in_process_pair();
        pair.first->close();
        expect(pair.second->read_line().error().code == TransportErrorCode::Closed,
               "empty closed peer");
    }

    // A transport lifetime ends its stream: dropping an endpoint closes it for the peer.
    {
        InProcessPair pair = make_in_process_pair();
        expect(pair.first->write_line("tail").has_value(), "write before drop");
        pair.first.reset();
        expect(read_or(*pair.second, "read before eof") == "tail", "queued line readable");
        expect(pair.second->read_line().error().code == TransportErrorCode::Closed,
               "drop closes the peer");
    }

    // Utf-8 payloads pass through unchanged.
    {
        InProcessPair pair = make_in_process_pair();
        const std::string text = "caf\xC3\xA9 \xF0\x9F\x98\x80";
        expect(pair.first->write_line(text).has_value(), "utf-8 write");
        expect(read_or(*pair.second, "utf-8 read") == text, "utf-8 payload");
    }

    if (failures != 0) {
        std::cerr << "harness_transport: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
