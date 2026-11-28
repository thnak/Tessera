# MCP server contract (v1)

Status: sections 1 and 2 are implemented by `tools/mcp` (issue #406); section 3 records owner
decisions of 2026-11-17 for the tool surface of #396, of which the error-code row is in
[error-model-v1.md](error-model-v1.md); the schema and version rules and section 5 (`query_style`,
`pick`) are implemented by #396.

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
- **As implemented.** Every `inputSchema` declares `$schema` 2020-12; `tools/mcp/schema/tools.json` is
  the exact `tools/list` result (compact canonical JSON, one trailing newline) and `mcp_tools` fails
  until it is regenerated with `mcp_tools_tests --write`. `outputSchema` is not published yet: the
  decision allows it only for an implemented tool, and none has one so far.
- **`open_project` and files.** `open_project` validates and records a path and reads no file; the
  tools that read the folder are `list_files`, `generate` and `diagnostics` / `get_ir` without `source`
  (sections 1, 2 and 4).

### 3.1 Schema ownership (issue #566, studio-workbench-v1 open decision 5)

Two files describe tools, and a tool is in exactly one of them as its schema owner:

- `tools/mcp/schema/tools.json` owns the tools the native server implements. It is unchanged and is still the
  exact `tools/list` result.
- `tools/mcp/schema/commands-tools.json` owns the **studio-backed** tools. The Studio command registry
  (`designer/web/src/workbench/commands.json`) generates it: a command with `mcp: { "tool": "<snake_case>" }`
  contributes `description` (its `description`, else `title`) and `inputSchema` (its `args`, with the UI-only
  `x-*` keywords removed and `$schema` 2020-12 added). `annotations.tesseraCommand` names the command.
- A command may set `mcp` only when it is `live`, names worker services (`viewer`, `analysis`, `format`,
  `mock`; ADR-060) and no page-side-only one (`project-io`, `layout`, `prefs`), and has an `args` schema. The
  registry constructor and the generator both refuse anything else; UI-only commands are never exposed.
- A command takes an existing `tools.json` name through `mcp.tool`; the descriptor is then flagged
  `annotations.existing` and the name is not renamed (section 3).
- `node scripts/ci/mcp_commands_tools.mjs` fails when the committed file differs from the registry byte
  for byte (`--write` regenerates it). The native server does not serve these tools: the handler is the
  studio's own service call, reachable only from the page, so the file is metadata for clients and for the
  page-side MCP adapter.

## 4. `generate` (issue #407, ADR-040)

`generate` produces the C++ of the folder with the same driver as `tessera_codegen` (`cli::run`), over
the same inputs as the folder compile (section 2). Owner decisions of 2026-11-16:

- **Default mode.** A dry run. Without `write: true` it reports and changes nothing on disk; it does
  not even create the output directory.
- **Where it writes.** Only into the output directory: `codegen.outDir` of `tessera.json`, default
  `generated` ([project-config-v1.md](project-config-v1.md)). The directory is a project path (section 2),
  not inside a pruned directory, and neither it nor any directory on its way is a symbolic link; any of
  these is `TESSERA_SEC_PATH_ESCAPE` and nothing is written. The generated bases
  (`<Component>.g.hpp`, `automation.manifest.json`) are rewritten wholesale; a developer class
  (`<Component>.hpp` / `.cpp`) is scaffolded once when it does not exist and is **never modified**
  afterwards (ADR-040).
- **What it returns.** `{write, outDir, exitCode, drift, files, written, scaffold, warnings}`: each file
  has `path` (project-relative), `size`, `digest` (`sha256:<64 hex>`, unlike the `fnv1a64` of
  `list_files`) and `status` (`new`, `changed`, `unchanged` against the disk before the call);
  `exitCode` is the CLI `--check` meaning (1 when a dry run finds drift); `scaffold` lists the developer
  files missing or just created; `warnings` are the driver's messages with the host path removed. A
  second call after `write: true` reports `drift: false`. With a `path` argument it returns that one
  generated file's text (`generated/App.g.hpp` or `App.g.hpp`) and is always a dry run, so an agent
  can read a generated base without filesystem access.
- **Blocking diagnostics.** The driver computes every file before it writes the first, so a project
  that does not lower returns the diagnostics document with `isError` and writes nothing.
- **Wording.** The studio spec's "never edits C++" now reads "never edits developer C++".

Out of scope: building or running the generated C++, and editing developer classes.

## 5. `query_style` and `pick` (issue #396)

Both are **queries over the source the client sends**, like `snapshot`: the text is evaluated in
memory at the viewport `set_viewport` last set, no file is read, and nothing is dispatched, so asking
cannot change the picture. They share `snapshot`'s layout (one `prepare_scene()`), so a `uiId` or a box
means the same in all three.

- **`query_style`** `{source, file?, uiId}`: the style inspector's answer for one node, from the same
  function as the studio's `style.info` (`style_info_for`): `{styleVersion, ok, node, computed,
  matched, box}` with `computed` limited to properties with a non-initial value, `matched` highest
  precedence first with the cascade's own `overridden`, and `box` in logical pixels with `margin`,
  `border` and `padding` as top/right/bottom/left. `uiId` is a decimal string (or an integer that fits
  a JSON number). A `uiId` no node carries answers `{styleVersion, ok:false, reason:"no_such_node"}`
  and nothing else, as `isError: false`: the question was well formed. The designer's override layer
  is session state of the studio and is not part of this stateless tool.
- **`pick`** `{source, file?, x, y}`: `{pickVersion, found, automationId, elementKind, box}` for the
  node under the point, by `Node::hit_test_deep`, the input router's own hit test (so
  `pointer-events`, scroll clipping and z-order apply). `x` and `y` are **logical viewport pixels**,
  the space the snapshot's bounds are in, not device pixels. A point over no node is `found:false`.
- **Errors.** A missing or malformed argument (`source`, `uiId`, a non-finite `x`/`y`) is an
  `InvalidParams` protocol error; a source that does not evaluate is an `isError` result that points
  at `diagnostics`.

Related: [designer-studio-v1.md](designer-studio-v1.md) sections 7, 8 and 10,
[compiler-diagnostics-v1.md](compiler-diagnostics-v1.md), [project-config-v1.md](project-config-v1.md).
