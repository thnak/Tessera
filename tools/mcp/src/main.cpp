// SPDX-License-Identifier: MIT
//
// `tessera-mcp`: the Model Context Protocol stdio adapter over the harness session
// (docs/specs/designer-studio-v1.md section 8, slice D-S6; decisions/ADR-045 item 6).
//
// Usage:
//   tessera-mcp            serve JSON-RPC on stdin/stdout until EOF
//   tessera-mcp --help     print this text on stdout and exit 0
//   tessera-mcp --version  print the server version and exit 0
//
// stdout carries protocol frames only. A diagnostic for the operator goes to stderr, because a
// single stray byte on stdout corrupts the client's stream.
//
// Exit codes: 0 clean end of input, 1 the session could not continue (it could not encode an
// answer, or stdout was closed), 2 usage or the stdio transport could not be created.

#include "serve_stdio.hpp"
#include "session.hpp"
#include "tessera/harness/stdio_transport.hpp"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

namespace harness = tessera::harness;
namespace mcp = tessera::mcp;

constexpr std::string_view kUsage =
    "usage: tessera-mcp [--help] [--version]\n"
    "\n"
    "Serves the Model Context Protocol (JSON-RPC 2.0, one message per line) on stdin/stdout.\n"
    "stdout carries protocol frames only; anything for the operator goes to stderr.\n"
    "\n"
    "Tools: open_project, list_files, diagnostics, get_ir, apply_edits, set_viewport, snapshot,\n"
    "query_style, pick, screenshot, run_script. This build implements the compile-only subset\n"
    "(open_project, diagnostics, get_ir); list_files and the rest are declared and answer with\n"
    "TESSERA_MCP_TOOL_UNIMPLEMENTED.\n";

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return 0;
        }
        if (argument == "--version") {
            std::cout << mcp::kServerName << " " << mcp::kServerVersion << " (MCP "
                      << mcp::kLatestProtocolVersion << ")\n";
            return 0;
        }
        std::cerr << "tessera-mcp: unknown option `" << argument << "`\n\n" << kUsage;
        return 2;
    }

    // The transport is the harness's process stdio, so the framing and readiness rules are exactly
    // the ones the headless harness already proves (src/harness/stdio_transport.cpp).
    harness::StdioTransport transport(harness::make_process_stdio_stream());
    mcp::McpSession session;
    const mcp::ServeResult result = mcp::serve_stdio(transport, session);
    if (result.failed) {
        std::cerr << "tessera-mcp: the session could not continue\n";
        return 1;
    }
    return 0;
}
