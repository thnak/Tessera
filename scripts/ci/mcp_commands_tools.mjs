// SPDX-License-Identifier: MIT

// The MCP projection of the Studio command registry (issues #566, #396): generates
// `tools/mcp/schema/commands-tools.json` from `designer/web/src/workbench/commands.json` and requires the
// committed file to match byte for byte, the same rule `tools.json` has. Also proves the refusals.
//
//   node scripts/ci/mcp_commands_tools.mjs          check (exit 1 on drift or a failed rule)
//   node scripts/ci/mcp_commands_tools.mjs --write  regenerate the committed file

import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { projectCommands, serializeProjection } from '../../designer/web/src/workbench/mcp-projection.js';
import { createRegistry } from '../../designer/web/src/workbench/registry.js';
import { createContext } from '../../designer/web/src/workbench/context.js';

const root = new URL('../../', import.meta.url);
const commandsPath = fileURLToPath(new URL('designer/web/src/workbench/commands.json', root));
const toolsPath = fileURLToPath(new URL('tools/mcp/schema/tools.json', root));
const outPath = fileURLToPath(new URL('tools/mcp/schema/commands-tools.json', root));

const records = JSON.parse(await readFile(commandsPath, 'utf8')).commands;
const existing = JSON.parse(await readFile(toolsPath, 'utf8')).tools.map((tool) => tool.name);
const failures = [];
const expect = (ok, what) => {
  if (!ok) failures.push(what);
};

const { tools, problems } = projectCommands(records, existing);
expect(problems.length === 0, `projection problems: ${problems.join('; ')}`);
const bytes = serializeProjection(tools);

if (process.argv.includes('--write')) {
  await writeFile(outPath, bytes);
  console.log(`wrote ${tools.length} tools to ${outPath}`);
} else {
  const committed = await readFile(outPath, 'utf8').catch(() => '');
  expect(committed === bytes, 'commands-tools.json differs from the registry (run: node scripts/ci/mcp_commands_tools.mjs --write)');
}

// Determinism: projecting twice gives the same bytes; UI-only extensions never leak.
expect(serializeProjection(projectCommands(records, existing).tools) === bytes, 'projection is not deterministic');
expect(!bytes.includes('x-provider'), 'x-provider leaked into the projection');
expect(tools.length > 0, 'no command is exposed to MCP');
for (const tool of tools) {
  expect(!existing.includes(tool.name) || tool.annotations.existing === true, `${tool.name} reuses a tools.json name without the existing flag`);
  expect(tool.inputSchema.$schema === 'https://json-schema.org/draft/2020-12/schema', `${tool.name}: inputSchema must declare 2020-12`);
}
// UI-only commands (panels, layout, menus) are never exposed.
expect(records.filter((record) => record.mcp && record.category === 'View').length === 0, 'a View command is exposed to MCP');

// Refusals, on synthetic records.
const base = { id: 'run.sample', title: 'Sample', category: 'Run', status: 'live', services: ['viewer'], args: { type: 'object', properties: {} }, mcp: { tool: 'sample_tool' } };
const refused = (patch, needle) => {
  const result = projectCommands([{ ...base, ...patch }], existing);
  expect(result.tools.length === 0 && result.problems.some((p) => p.includes(needle)), `not refused: ${needle}`);
};
refused({ services: ['project-io'] }, 'page-side-only');
refused({ services: ['viewer', 'layout'] }, 'page-side-only');
refused({ services: [] }, 'worker services');
refused({ args: null }, 'args JSON Schema');
refused({ status: 'reserved' }, 'live command');
refused({ mcp: { tool: 'BadName' } }, 'snake_case');
expect(projectCommands([base, { ...base, id: 'run.other' }], existing).problems.some((p) => p.includes('already claimed')), 'duplicate tool name accepted');
const reuse = projectCommands([{ ...base, mcp: { tool: existing[0] } }], existing);
expect(reuse.problems.length === 0 && reuse.tools[0].name === existing[0] && reuse.tools[0].annotations.existing === true, 'an existing tool name is not reused through mcp.tool');
// The registry itself refuses the same records at construction.
let threw = false;
try {
  createRegistry([{ ...base, services: ['project-io'] }], { context: createContext() });
} catch {
  threw = true;
}
expect(threw, 'createRegistry accepted a page-side-only MCP command');

if (failures.length > 0) {
  for (const failure of failures) console.error(`FAIL ${failure}`);
  process.exit(1);
}
console.log(`mcp_commands_tools: ok (${tools.length} tools)`);
