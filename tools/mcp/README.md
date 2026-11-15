# tessera-mcp — Model Context Protocol stdio adapter

`tessera-mcp` exposes the harness session to a coding agent over the Model Context Protocol: JSON-RPC
2.0 messages, one per line, on stdin/stdout. It is a **thin adapter**, not a third protocol — every
tool maps to a compiler entry point, a command of
[`headless-automation-v1.md`](../../docs/specs/headless-automation-v1.md), or a message of
[`designer-protocol-v1.md`](../../docs/specs/designer-protocol-v1.md)
([`designer-studio-v1.md`](../../docs/specs/designer-studio-v1.md) section 8,
[ADR-045](../../decisions/ADR-045-browser-hosted-studio.md) item 6, issue #172).

## Build and run

```powershell
cmake --build --preset vs2026-release --target tessera_mcp
.\build\vs2026\tools\mcp\Release\tessera-mcp.exe
```

The binary serves until stdin reaches EOF. `--help` and `--version` are the only non-serving modes.
stdout carries protocol frames and nothing else; anything for an operator goes to stderr, because a
single stray byte on stdout corrupts the client's stream.

Exit codes: `0` clean end of input, `1` the session could not continue (an answer could not be
encoded, or stdout closed), `2` a usage error.

## Tool surface

This is the compile-only slice (D-S6). A tool is **implemented** when this build can perform it, and
**declared** when it is part of the surface but a later slice owns the work. `tools/list` publishes
the difference in each entry's `annotations.tesseraStatus`, so an agent can tell the two apart before
calling; a declared tool answers with `isError: true` and `TESSERA_MCP_TOOL_UNIMPLEMENTED` rather than
returning an empty result that looks like success.

| Tool | Status | Maps to |
|---|---|---|
| `open_project` | implemented | validates and records a project path (`project.load` until the host reads files) |
| `diagnostics` | implemented | the compiler front end + `write_diagnostics_json` |
| `get_ir` | implemented | the same stages, then the canonical IR writer (`ir-v1.md`) |
| `list_files` | declared | host file tree |
| `apply_edits` | declared | `source.patch` |
| `set_viewport` | declared | `viewport.set` |
| `snapshot` | declared | harness `snapshot` |
| `query_style` | declared | `style.inspect` |
| `pick` | declared | `selection.set` |
| `screenshot` | declared | the native headless renderer |
| `run_script` | declared | the harness script runner |

`diagnostics` and `get_ir` compile **in memory**: the client sends the TSX text, no file is read, and
the spans are reported against the project-relative `file` the client names (default `buffer.tsx`).
That keeps the adapter free of a filesystem and security surface; reading a project is a later slice
(`designer-studio-v1.md` section 10, D-S7).

## Why `diagnostics` is byte-identical to the CLI

The compile tools call the same two units the native CLI does — `diagnose_source()` in
`tools/codegen/diagnose.cpp` and the IR writer in `src/compiler/ir` — so there is no second assembly
path for a diagnostics document. `mcp_diagnostics_parity` runs the file-driven CLI driver and the MCP
tool over one corpus and requires **byte-identical** documents, including the `file` field, on a
corpus that must actually diagnose (a clean-only corpus would pass by comparing two empty documents).

## Protocol behaviour

- `initialize` negotiates the revision: the client's version when this server supports it
  (`2025-06-18`, `2025-03-26`, `2024-11-05`), otherwise the newest one, so the client decides whether
  to continue.
- A request before `initialize` is refused with `-32002` (NotInitialized); `ping` is exempt so a client
  can probe a server it has not initialized.
- A notification is never answered (JSON-RPC forbids it), including an unknown one.
- An unknown method is `-32601`; a malformed line is `-32700` with a null id and the session stays
  usable.
- Errors carry a stable `TESSERA_*` code in `error.data.code` where one applies, so a client can
  branch without matching prose.
- Batching is refused: MCP removed it in 2025-06-18, so a batch array is one invalid request rather
  than a half-executed one.

## Threading and blocking

The serve loop is single-threaded, creates no thread, and never blocks: it reuses the harness
`StdioTransport` (bounded line length, UTF-8 validation, CRLF tolerance, EOF-terminated tail,
non-blocking readiness) and idles on its bounded `wait_readable` slice, which is also how a `stop`
flag is observed. A stalled client costs one short sleep per blocked write, never a spin (AGENTS.md
non-negotiable 12).

## Layout

| File | Responsibility |
|---|---|
| `src/mcp_protocol.hpp/.cpp` | wire constants: revisions, error codes, `TESSERA_*` codes |
| `src/jsonrpc.hpp/.cpp` | JSON-RPC 2.0 envelope decode/encode over `harness::JsonValue` |
| `src/session.hpp/.cpp` | the MCP lifecycle and `tools/*` dispatch |
| `src/tool_registry.hpp/.cpp` | the single declaration of the tool surface |
| `src/tool_session.hpp/.cpp` | per-session state (the open project) and path normalization |
| `src/compile_tools.hpp/.cpp` | the `diagnostics` and `get_ir` tool bodies |
| `src/in_memory_ir.hpp/.cpp` | the in-memory lowering that `get_ir` needs |
| `src/serve_stdio.hpp/.cpp` | the bounded read → dispatch → write loop |
| `src/main.cpp` | the executable, its two non-serving modes and its exit codes |

## Tests

| ctest name | What it proves |
|---|---|
| `mcp_jsonrpc` | every envelope rule, including the id echo and the canonical bytes |
| `mcp_tools` | the surface is exactly section 8's, and the published statuses are honest |
| `mcp_compile_tools` | the implemented tools, and which channel each failure uses |
| `mcp_diagnostics_parity` | the issue's acceptance: MCP diagnostics equal the CLI's, byte for byte |
| `mcp_stdio` | the lifecycle over a scripted stream: handshake, notification, unknown method, malformed line, CRLF, EOF tail |
| `mcp_stdio_call` | the loop's own contract: a `tools/call` across the wire, idling instead of spinning, the stop flag, a rejected frame |
| `mcp_process` | a real spawned process: the same exchange across a pipe, and that stdout is frames only |
