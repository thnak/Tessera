// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command_result.hpp"

#include <expected>

namespace tessera::harness {

// The single seam between the protocol loop and the UI runtime. The executor owns framing,
// decoding, ordering, timeouts, and response encoding; a handler only turns one decoded command
// into one result or a typed error. Keeping the seam this narrow is what lets the session loop be
// tested without a window, GPU, or input device, and what keeps every command mutating the UI on
// the one thread that calls handle().
class CommandHandler {
public:
    CommandHandler() = default;
    CommandHandler(const CommandHandler&) = delete;
    CommandHandler& operator=(const CommandHandler&) = delete;
    virtual ~CommandHandler() = default;

    // Synchronous by design: commands are serialized (spec section 6), so a handler may take as
    // long as one bounded step and must not wait on another thread. A host that needs more time
    // reports ProtocolErrorCode::Timeout and keeps the session usable.
    [[nodiscard]] virtual std::expected<CommandResult, ProtocolError>
    handle(const CommandPayload& command) = 0;
};

} // namespace tessera::harness
