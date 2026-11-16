// SPDX-License-Identifier: MIT
//
// Node probe for the canvas module's unsupported-browser contract (issue #167, P3).
//
// `scripts/ci/scene_frame_parity.py --mode canvas-report` runs this. It loads `tessera_canvas.js` in
// Node, which has no `navigator.gpu` - exactly the state a browser without WebGPU is in - and asserts
// the module still loads and answers, with `hasDevice:false`, instead of throwing or aborting.
//
// This is NOT a render check and deliberately does not pretend to be one: Node has no WebGPU device,
// so no frame can be drawn here. What it proves is the contract the page depends on when it shows its
// "unsupported browser" state, which is one of P3's specific requirements.

import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

function fail(message) {
  process.stderr.write(`scene_frame_canvas_probe: ${message}\n`);
  process.exitCode = 1;
}

async function main() {
  const modulePath = process.argv[2];
  if (!modulePath) {
    fail("usage: scene_frame_canvas_probe.mjs <tessera_canvas.js>");
    return;
  }
  const imported = await import(pathToFileURL(resolve(modulePath)).href);
  const factory = imported.default;
  if (typeof factory !== "function") {
    fail(`${modulePath} has no default factory`);
    return;
  }
  const module = await factory();
  if (typeof module._tessera_canvas_frame !== "function") {
    fail("the module does not export _tessera_canvas_frame");
    return;
  }
  if (typeof module._tessera_webgpu_set_device !== "function") {
    fail("the module does not export _tessera_webgpu_set_device");
    return;
  }

  // No device was handed over, so this is the contract under test: a report, not a crash.
  const source = `export function Probe(): Node {
  return (
    <View>
      <Text>Probe</Text>
      <Button>Ok</Button>
    </View>
  );
}
`;
  const textLength = module.lengthBytesUTF8(source) + 1;
  const textPtr = module._malloc(textLength);
  const file = "ui/Probe.tsx";
  const fileLength = module.lengthBytesUTF8(file) + 1;
  const filePtr = module._malloc(fileLength);
  const componentPtr = module._malloc(1);
  let json;
  try {
    module.stringToUTF8(source, textPtr, textLength);
    module.stringToUTF8(file, filePtr, fileLength);
    module.stringToUTF8("", componentPtr, 1);
    // The last argument is the canvas view handle; 0 is "no canvas", and there is no device anyway.
    const result = module._tessera_canvas_frame(textPtr, filePtr, componentPtr, 320, 240, 1.0, 0);
    if (!result) {
      fail("tessera_canvas_frame returned null instead of a report");
      return;
    }
    try {
      json = module.UTF8ToString(result);
    } finally {
      module._tessera_free(result);
    }
  } finally {
    module._free(textPtr);
    module._free(filePtr);
    module._free(componentPtr);
  }

  let report;
  try {
    report = JSON.parse(json);
  } catch (error) {
    fail(`the report is not JSON: ${json}`);
    return;
  }
  if (report.canvasVersion !== 1) {
    fail(`canvasVersion is ${report.canvasVersion}, expected 1`);
    return;
  }
  if (report.hasDevice !== false) {
    fail("hasDevice must be false when no device was handed over");
    return;
  }
  if (typeof report.error !== "string" || report.error.length === 0) {
    fail("a no-device report must carry an error string for the page to show");
    return;
  }
  process.stdout.write(`CANVAS ${json.trim()}\n`);
}

main().then(
  () => {},
  (error) => {
    fail(error && error.message ? error.message : String(error));
  },
);
