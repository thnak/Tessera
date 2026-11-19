// SPDX-License-Identifier: MIT

// `tessera_canvas_platform_event` (issue #446, designer-protocol-v1 section 5.1): the canvas
// module's spelling of the protocol's `platform.event`. It takes the running session through
// `canvas_query_session()` like the query exports do, but it is not a query: a Back or a process
// death changes the session, and a relaunch ends the evaluation. It never creates the session.
//
// All of the behaviour is `PlatformEventDriver` (`scene_platform.cpp`), the code the native
// scripted run executes; this file is only argument parsing and the C ABI.

#include "scene_canvas_session.hpp"
#include "scene_platform.hpp"

#include <cstdint>
#include <cstdlib>
#include <emscripten/emscripten.h>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::cli {

// Defined in `wasm_canvas_entry.cpp`, which owns the module's state.
[[nodiscard]] CanvasSceneSession* canvas_query_session();
[[nodiscard]] char* canvas_duplicate_out(std::string_view text);

} // namespace tessera::compiler::codegen::cli

extern "C" {

// Delivers one normalized platform event to the running session and answers its effect as the
// one-line JSON object of designer-protocol-v1 section 5.1 (`handled`, `appClosed`, `resumed`,
// `state`, `dispatch`, `sequence`, `timestampNs`, `changed`, ...).
//
//   type          the wire name of the event (`back`, `home`, `volumeUp`, `orientation`, ...); an
//                 unknown name is an `ok:false` reply with a `TESSERA_PLATFORM_*` code, never a
//                 silent no-op
//   value         the event's single argument (`landscape`, `book`, `dark`); empty when it has none
//   timestamp_ns  the simulated clock in nanoseconds, or a negative number for "now". It is a
//                 double because a 64-bit integer crosses the ABI as a BigInt, and 2^53 ns is 104
//                 days of simulated time
//
// A `relaunch` reply ends the evaluation (`started()` is false afterwards): the page calls
// `tessera_canvas_apply` next, which is the cold start.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_platform_event(const char* type, const char* value,
                                                               double timestamp_ns) {
    namespace cli = tessera::compiler::codegen::cli;
    cli::PlatformEvent event;
    const std::string_view name = type != nullptr ? std::string_view(type) : std::string_view();
    cli::PlatformReply reply;
    if (!cli::platform_kind_from(name, event.kind)) {
        reply.ok = false;
        reply.error = "TESSERA_PLATFORM_UNKNOWN_EVENT: `" + std::string(name) +
                      "` is not a platform event type";
        return cli::canvas_duplicate_out(cli::platform_reply_json(reply));
    }
    event.value = value != nullptr ? std::string(value) : std::string();
    event.timestamp_ns = timestamp_ns >= 0.0 ? static_cast<std::int64_t>(timestamp_ns) : -1;
    cli::CanvasSceneSession* session = cli::canvas_query_session();
    if (session == nullptr) {
        reply.kind = event.kind;
        reply.ok = false;
        reply.error =
            "TESSERA_PLATFORM_NO_SESSION: no canvas session: call tessera_canvas_apply first";
        return cli::canvas_duplicate_out(cli::platform_reply_json(reply));
    }
    reply = session->platform_event(event);
    return cli::canvas_duplicate_out(cli::platform_reply_json(reply));
}

} // extern "C"
