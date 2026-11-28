## mcp: project the Studio command registry into commands-tools.json (#566)

### What changed
- `designer/web/src/workbench/mcp-projection.js`: `mcpProblems` (live, worker services only, args schema, snake_case name) and `projectCommands` / `serializeProjection`. `registry.js` now refuses a bad `mcp` record at construction.
- `tools/mcp/schema/commands-tools.json` generated from `commands.json`; three commands now carry `mcp.tool`: `device.setFold` (`set_fold_posture`), `device.profile` (`apply_device_profile`), `run.component` (`select_component`). UI-only `x-*` schema keywords are stripped.
- `scripts/ci/mcp_commands_tools.mjs`: byte-for-byte check (`--write` regenerates), determinism, refusals (page-side-only service, no services, no schema, not live, bad name, duplicate), reuse of an existing `tools.json` name (flagged `existing`). Wired into the Pages workflow beside the workbench core gate.
- Docs: `mcp-server-v1.md` section 3.1 (schema ownership), pointer in `studio-workbench-v1.md` 2.4.
- The native server and `tools.json` are unchanged (the studio-backed tools are served by the page, not by `tessera-mcp`).

### Verification
`node scripts/ci/mcp_commands_tools.mjs`, `node scripts/ci/preview_workbench.mjs`, `python -B scripts/gates.py --quiet`: pass. No C++ changed.

### What is left
#396 is not closed: `apply_edits` (needs the edit shape and the in-memory session overlay decided), `screenshot` (native headless renderer, open question 4) and `run_script` (harness) remain.

Closes #566
Refs #396
