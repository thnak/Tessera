// SPDX-License-Identifier: MIT

// The MCP projection of the command registry (`studio-workbench-v1.md` section 2.4, issues #566 and #396):
// the commands that carry `mcp: { tool }` become tool descriptors in `tools/mcp/schema/commands-tools.json`.
// Pure data, no DOM: the registry validates records with `mcpProblems`, and `scripts/ci/mcp_commands_tools.mjs`
// generates and checks the file.

/** Services that run in a worker and report readiness (ADR-060); the rest are page-side only. */
export const WORKER_SERVICES = new Set(["viewer", "analysis", "format", "mock"]);

const TOOL_NAME = /^[a-z][a-z0-9]*(_[a-z0-9]+)*$/;
const SCHEMA_URI = "https://json-schema.org/draft/2020-12/schema";

/**
 * Why a record may not be an MCP tool; empty when it may (or sets no `mcp`).
 * @param {any} record
 * @returns {string[]}
 */
export function mcpProblems(record) {
  const mcp = record?.mcp;
  if (mcp === null || mcp === undefined) {
    return [];
  }
  const id = record.id;
  const problems = [];
  if (typeof mcp !== "object" || typeof mcp.tool !== "string" || !TOOL_NAME.test(mcp.tool)) {
    problems.push(`${id}: mcp.tool must be a snake_case name`);
  }
  if (record.status !== "live") {
    problems.push(`${id}: only a live command may set mcp (status is '${record.status}')`);
  }
  const services = record.services ?? [];
  if (services.length === 0) {
    problems.push(`${id}: an MCP command must name the worker services its handler calls`);
  }
  const pageSide = services.filter((name) => !WORKER_SERVICES.has(name));
  if (pageSide.length > 0) {
    problems.push(`${id}: page-side-only service '${pageSide[0]}' cannot back an MCP tool (the native server cannot see browser state)`);
  }
  if (record.args === null || record.args === undefined) {
    problems.push(`${id}: an MCP command needs an args JSON Schema, which becomes the inputSchema`);
  }
  return problems;
}

// UI-only schema extensions (`x-provider` feeds the palette quick-pick) have no meaning to an MCP client.
function publicSchema(value) {
  if (Array.isArray(value)) {
    return value.map(publicSchema);
  }
  if (value !== null && typeof value === "object") {
    return Object.fromEntries(Object.entries(value).filter(([key]) => !key.startsWith("x-")).map(([key, child]) => [key, publicSchema(child)]));
  }
  return value;
}

/**
 * The tool descriptors of the registry, in declaration order.
 * @param {Array<any>} records the `commands.json` `commands` array
 * @param {Iterable<string>} existingTools names already in `tools.json`; a command may reuse one through
 *   `mcp.tool` (the descriptor is then flagged `existing`, the name is never renamed)
 * @returns {{ tools: Array<any>, problems: string[] }}
 */
export function projectCommands(records, existingTools) {
  const existing = new Set(existingTools);
  const problems = [];
  const tools = [];
  const claimed = new Map();
  for (const record of records) {
    const found = mcpProblems(record);
    problems.push(...found);
    if (record.mcp === null || record.mcp === undefined || found.length > 0) {
      continue;
    }
    const name = record.mcp.tool;
    if (claimed.has(name)) {
      problems.push(`${record.id}: tool '${name}' is already claimed by '${claimed.get(name)}'`);
      continue;
    }
    claimed.set(name, record.id);
    const annotations = { tesseraCommand: record.id, tesseraSource: "registry" };
    if (existing.has(name)) {
      annotations.existing = true;
    }
    tools.push({
      name,
      description: record.description ?? record.title,
      inputSchema: { $schema: SCHEMA_URI, ...publicSchema(record.args) },
      annotations,
    });
  }
  return { tools, problems };
}

/** The exact bytes of `commands-tools.json`: compact JSON and one trailing newline, as `tools.json`. */
export function serializeProjection(tools) {
  return `${JSON.stringify({ tools })}\n`;
}
