# MCP server contract (v1)

Status: sections 1 and 2 are implemented by `tools/mcp` (issue #406); section 3 records owner
decisions of 2026-11-17 for the tool surface of #396, of which the error-code row is in
[error-model-v1.md](error-model-v1.md) and the schema and version rules are implemented with #396.

`tessera-mcp` ([designer-studio-v1.md](designer-studio-v1.md) section 8) is the agent-facing twin of the
studio: it reads a project folder, compiles it, and previews it. This document holds the rules that
both the native server and the Wasm module must agree on, so that they live in one place.

## 1. The project folder

The tools that read files (`list_files`, and `diagnostics` / `get_ir` called without `source`) work on
one folder, resolved in this order:

1. an explicit `open_project({path})`;
2. the first `file://` root of the client's `roots/list`, when the client declared the `roots`
   capability at `initialize`. The server sends `roots/list` (id `tessera-roots-<n>`) after
   `notifications/initialized`, and again after `notifications/roots/list_changed`. A remote host or a
   non-file URI is ignored;
3. the working directory the server process was started in.

Each result names `rootSource` (`argument`, `roots`, `cwd`) and the root's own folder name, never an
absolute path. The root is resolved lazily: a tool that runs before the client answers `roots/list`
uses the working directory and says so, and the answer then re-resolves the folder. An explicit
`open_project` is never overridden by `roots`. `open_project` itself reads and writes nothing.

This slice is **read-only**: the server writes nothing, runs no shell, opens no connection and never
reads outside the root.

## 2. Which files count

The policy is a pure function over paths and sizes, implemented once in
`tessera/schema/project_files.hpp` (`tessera_schema`, which the Wasm module links) so the agent and the
browser cannot disagree. Its corpus is `tests/unit/schema_project_files_tests.cpp`. The host walks the
directory (or a zip, or a File System Access handle) and hands the policy what it found.

- **Paths.** Relative, `/` separated, no `.` or `..` segment, no empty segment, no backslash or NUL,
  at most 1024 bytes. A violation is `TESSERA_SEC_PATH_ESCAPE`. A symbolic link whose target leaves the
  root is the same error, detected by the host; a link that stays inside is listed and never followed.
- **Kinds.** `source` (`.tsx`, `.jsx`), `style` (`.css`), `config` (`tessera.json` at the root),
  `theme` (the file named by `assets.theme`), `asset` (a font or image listed in `tessera.json`; an
  entry ending in `/` is a directory), and `other`. Only `other` is unreadable: it is listed with
  `readable: false` and no digest, and its bytes are never read.
- **Pruned directories.** `node_modules` and any directory whose name starts with `.` are one `other`
  entry and are never entered, so a dependency tree can neither exhaust the file budget nor be read.
- **Limits.** At most 500 listed files (all kinds), 1 MiB per readable file, 20 MiB of readable bytes.
  Crossing one is `TESSERA_SEC_FILE_LIMIT` and names the limit and the first path that crossed it.
  An unaccepted file costs nothing against the byte limits.
- **Digest.** `fnv1a64:<16 hex>` of the file bytes, the form of `ir::StylesheetRef::hash`
  ([ir-v1.md](ir-v1.md)).

`list_files` returns `{folder, rootSource, files: [{path, kind, size, readable, digest?}]}` with the
files sorted by path in byte order. The folder compile runs the codegen driver (`cli::run` with
`Options::query`) over every readable source file, the entry of `tessera.json` first, with inputs
resolved against the root (`Options::base_dir`); its document is byte-identical to
`tessera_codegen --diagnostics-json` for the same files, and spans are project-relative.

## 3. Decisions (owner, 2026-11-17)

- **Error codes.** `TESSERA_MCP_` is a family of [error-model-v1.md](error-model-v1.md) section 3
  (unknown tool, not initialized, invalid params, input too large, tool unimplemented). The five codes
  the server already ships keep their names. File-policy errors use `TESSERA_SEC_`; the server wraps
  them and never re-codes them, so the browser reports the same codes.
- **Tool naming and schema.** The names in `tools/list` are the contract (snake_case, no prefix) and
  are not renamed. Every tool publishes `inputSchema` as JSON Schema 2020-12. The checked-in
  `tools/mcp/schema/tools.json` is the normative copy and a test requires `tools/list` to equal it
  byte for byte. `outputSchema` is published only for an implemented tool.
- **Surface version.** One integer, `toolSurfaceVersion`, starting at 1, reported in the `initialize`
  result. Adding a tool, an optional parameter or a result field does not change it; a removal, a
  rename, a new required parameter or a changed meaning of a field does, and the old form stays for
  one release with a deprecation note in the result. A declared tool keeps answering
  `TESSERA_MCP_TOOL_UNIMPLEMENTED`, never an empty result.

Related: [designer-studio-v1.md](designer-studio-v1.md) sections 7, 8 and 10,
[compiler-diagnostics-v1.md](compiler-diagnostics-v1.md), [project-config-v1.md](project-config-v1.md).
