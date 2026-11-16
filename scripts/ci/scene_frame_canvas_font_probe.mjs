// SPDX-License-Identifier: MIT
//
// Node probe for the canvas module's **font** ABI (issue #387).
//
// `scripts/ci/scene_frame_parity.py --mode canvas-font` runs this. It is the closest the browser lane
// can be checked without a browser: the module is loaded under Node, the same shipped font bytes the
// page fetches are written into its heap, and the module is asked for its font status.
//
// What this proves, and what it does not:
//
//  * the two exports the page calls (`tessera_canvas_set_font`, `tessera_canvas_font_status`) exist on
//    the module the page actually downloads, not only on a native build;
//  * the ABI is the intended one -- a pointer plus a byte count in, a 1/0/-1 code out -- so the
//    `_malloc` + `HEAPU8.set` + `_free` sequence `designer/web/src/views/canvas-font.ts` performs is
//    the sequence the module expects;
//  * the refusal paths are answers rather than crashes: no bytes and no device each return their own
//    code, and the status report carries the reason the pane shows.
//
// It does NOT render a glyph: Node has no WebGPU device, so `set_font` cannot build the atlas renderer
// and correctly refuses with "no device". That is the honest division of labour -- the *glyph* proof
// runs natively (`scripts/ci/scene_frame_parity.py --mode native-reference` and the `scene_frame`
// ctest, which paint text from these same bytes), and the pixel comparison needs a real browser.

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

function fail(message) {
  process.stderr.write(`scene_frame_canvas_font_probe: ${message}\n`);
  process.exitCode = 1;
  return false;
}

function ok(message) {
  process.stdout.write(`ok   ${message}\n`);
  return true;
}

async function main() {
  const modulePath = process.argv[2];
  const fontPath = process.argv[3];
  if (!modulePath || !fontPath) {
    fail("usage: scene_frame_canvas_font_probe.mjs <tessera_canvas.js> <font.ttf>");
    return;
  }

  const imported = await import(pathToFileURL(resolve(modulePath)).href);
  const factory = imported.default;
  if (typeof factory !== "function") {
    fail(`${modulePath} has no default factory`);
    return;
  }
  const module = await factory();

  // The ABI half: both exports, and the runtime helpers the page's handover uses.
  for (const name of [
    "_tessera_canvas_set_font",
    "_tessera_canvas_font_status",
    "_tessera_free",
    "_malloc",
    "_free",
  ]) {
    if (typeof module[name] !== "function") {
      fail(`the module does not export ${name}`);
      return;
    }
  }
  if (typeof module.HEAPU8 === "undefined") {
    fail("the module exposes no HEAPU8, so the page cannot write font bytes");
    return;
  }
  if (typeof module.UTF8ToString !== "function") {
    fail("the module does not export UTF8ToString, so the page cannot read the status");
    return;
  }
  ok("the module exports the font ABI the page calls");

  const status = () => {
    const pointer = module._tessera_canvas_font_status();
    try {
      return JSON.parse(module.UTF8ToString(pointer));
    } finally {
      module._tessera_free(pointer);
    }
  };

  // Before anything: no font, and the report says so rather than being absent or malformed.
  const initial = status();
  if (initial.fontVersion !== 1 || initial.loaded !== false) {
    fail(`the initial status should be fontVersion 1 / loaded false, got ${JSON.stringify(initial)}`);
    return;
  }
  ok("the initial status reports no font loaded");

  // Empty data is code 0, which the page distinguishes from a bad font.
  const empty = module._tessera_canvas_set_font(0, 0);
  if (empty !== 0) {
    fail(`set_font(nullptr, 0) should return 0, got ${empty}`);
    return;
  }
  ok("empty font data is refused with 0");

  // The real bytes, with no device: this module has no `navigator.gpu`, so the atlas renderer cannot
  // exist and the answer must be -1 with a reason that names the device, not a crash.
  const bytes = new Uint8Array(readFileSync(resolve(fontPath)));
  if (bytes.byteLength === 0) {
    fail(`${fontPath} is empty`);
    return;
  }
  const pointer = module._malloc(bytes.byteLength);
  if (!pointer) {
    fail("the module is out of memory for the font");
    return;
  }
  let code;
  try {
    module.HEAPU8.set(bytes, pointer);
    code = module._tessera_canvas_set_font(pointer, bytes.byteLength);
  } finally {
    module._free(pointer);
  }
  if (code !== -1) {
    fail(`set_font with no device should return -1, got ${code}`);
    return;
  }
  const refused = status();
  if (refused.loaded !== false || typeof refused.error !== "string" || refused.error.length === 0) {
    fail(`a refused font should report loaded:false with a reason, got ${JSON.stringify(refused)}`);
    return;
  }
  if (!refused.error.includes("device")) {
    fail(`the refusal should name the missing device, got "${refused.error}"`);
    return;
  }
  ok(`a font with no device is refused with a reason: ${refused.error}`);

  // The status object keeps its documented shape, so the pane's reader cannot be surprised by a
  // missing key after a refactor.
  for (const key of ["fontVersion", "loaded", "error"]) {
    if (!(key in refused)) {
      fail(`the status report is missing \`${key}\``);
      return;
    }
  }
  ok("the status report keeps its documented shape");

  process.stdout.write(`CANVAS FONT ${JSON.stringify(refused)}\n`);
}

await main();
