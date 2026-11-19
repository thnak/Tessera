// SPDX-License-Identifier: MIT
//
// The platform-event slices' acceptance check (issues #446 and #448; `designer-protocol-v1.md` section 5.1,
// `studio-device-profiles-v1.md` section 4).
//
//   node scripts/ci/preview_platform_events.mjs [--scene-module build/pages/tessera_scene_preview.js]
//                                               [--canvas-module build/pages/tessera_canvas.js]
//
// Two halves, kept apart because they need different things:
//
//  * **The page's vocabulary and script format** (always run): the catalog every control and event of a
//    device profile maps to, the log, the effect phrases and the record-and-replay script. These are the
//    pure modules the page ships (`dist-ssr/platform-events.js`), so `npm run build` is a prerequisite;
//    a missing build is exit code 2.
//
//  * **The engine, under Node** (run when `--scene-module` is given): the Wasm scene module's
//    `tessera_input_json` export runs `platform` steps through the same `PlatformEventDriver` the canvas
//    session uses, so each event type of #445 and the environment events are delivered and their replies
//    checked; the same script run through the native `tessera_scene_preview` CLI is compared by
//    `scene_input_parity.py`, which holds the byte-for-byte half of "the Wasm path and the native injector
//    run the same code". With `--canvas-module` the canvas export's no-session contract is checked: Node
//    has no WebGPU device, so a canvas session cannot start and the export must answer, not abort.
//
// Deterministic: no clock, no network. Exit codes: 0 all passed, 1 a check failed, 2 setup error.

import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const DIST_SSR = resolve(REPO_ROOT, "designer", "web", "dist-ssr");

let failures = 0;
const fail = (message) => {
  process.stdout.write(`FAIL ${message}\n`);
  failures += 1;
};
const ok = (message) => process.stdout.write(`ok   ${message}\n`);
const assert = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};
async function check(name, body) {
  try {
    await body();
    ok(name);
  } catch (error) {
    fail(`${name}\n     ${error && error.message ? error.message : error}`);
  }
}

function option(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : null;
}

for (const file of ["platform-events.js", "device-profile.js"]) {
  try {
    readFileSync(resolve(DIST_SSR, file));
  } catch {
    process.stderr.write(
      `SETUP ERROR: ${file} is missing from ${DIST_SSR}. Build the page first:\n` +
        "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
    );
    process.exit(2);
  }
}
const lib = await import(pathToFileURL(resolve(DIST_SSR, "platform-events.js")).href);
const profile = await import(pathToFileURL(resolve(DIST_SSR, "device-profile.js")).href);

const reply = (over) => ({
  ok: true, type: "back", handled: false, appClosed: false, resumed: false, state: "running",
  dispatch: "none", sequence: 1, timestampNs: 0, changed: false, savedState: false, relaunch: false, restore: false,
  ...over,
});

await check("every control and event a profile may list has a catalog entry that sends its own wire name", () => {
  for (const id of profile.CONTROL_IDS) {
    assert(lib.CONTROL_CATALOG[id], `control ${id} has no catalog entry`);
    assert(lib.isPlatformEventType(id), `control ${id} is not a wire name`);
  }
  for (const id of profile.EVENT_IDS) {
    const buttons = lib.EVENT_CATALOG[id];
    assert(buttons && buttons.length > 0, `event ${id} offers no button`);
    for (const button of buttons) {
      assert(button.type === id, `event ${id} has a button that sends ${button.type}`);
      assert(lib.isPlatformEventType(button.type), `${button.type} is not a wire name`);
    }
  }
  for (const state of ["running", "inactive", "background", "screenOff", "closed"]) {
    assert(lib.STATE_PRESENTATION[state], `state ${state} has no presentation`);
  }
  assert(lib.STATE_PRESENTATION.closed.action.type === "relaunch", "a closed app does not offer Relaunch");
  assert(lib.STATE_PRESENTATION.background.action.type === "resume", "a background app does not offer Resume");
  assert(lib.STATE_PRESENTATION.screenOff.action.label === "Wake", "a dark screen does not offer Wake");
});

await check("the effect phrases say what the engine did", () => {
  assert(lib.effectOf(reply({ type: "back", appClosed: true, handled: true, state: "closed", dispatch: "unhandled" })) === "unhandled: app closed", "back");
  assert(lib.effectOf(reply({ type: "home", handled: true, savedState: true, state: "background" })) === "in background (state saved)", "home");
  assert(lib.effectOf(reply({ type: "resume", handled: true, resumed: true })) === "resumed (state kept)", "resume");
  assert(lib.effectOf(reply({ type: "orientation", handled: true, changed: true })) === "layout updated in place", "orientation");
  assert(lib.effectOf(reply({ type: "theme" })) === "delivered, no effect", "theme");
  assert(lib.effectOf(reply({ type: "relaunch", relaunch: true, restore: true, handled: true })) === "relaunched, saved state restored", "relaunch");
  assert(lib.effectOf(reply({ ok: false, error: "TESSERA_PLATFORM_NOT_CLOSED: x" })).startsWith("TESSERA_PLATFORM_NOT_CLOSED"), "refused");
});

await check("the log is ordered, capped, and exports the native script format", () => {
  let log = [];
  for (let index = 0; index < lib.MAX_LOG_ENTRIES + 5; index += 1) {
    log = lib.appendEntry(log, lib.entryOf({ type: "theme", value: "dark", timestampNs: index }, reply({ type: "theme", sequence: index + 1, timestampNs: index })));
  }
  assert(log.length === lib.MAX_LOG_ENTRIES && log[0].sequence === 6, "the log is not bounded to its newest entries");
  const entries = [
    lib.entryOf({ type: "home", value: "", timestampNs: 1_000_000 }, reply({ type: "home", handled: true, state: "background", sequence: 1, timestampNs: 1_000_000 })),
    lib.entryOf({ type: "relaunch", value: "", timestampNs: 2_000_000 }, reply({ ok: false, error: "TESSERA_PLATFORM_NOT_CLOSED", type: "relaunch", sequence: 2 })),
    lib.entryOf({ type: "orientation", value: "landscape", timestampNs: 3_000_000 }, reply({ type: "orientation", handled: true, changed: true, sequence: 3, timestampNs: 3_000_000 })),
  ];
  const text = lib.exportScript(entries);
  const steps = text.split("\n").filter((line) => line.startsWith("platform"));
  assert(steps.join("|") === "platform home @1000000|platform orientation landscape @3000000", `exported ${steps.join("|")}`);
  assert(text.startsWith("# "), "the script has no header comment");
});

await check("a script round-trips and a malformed one is refused with its line number", () => {
  const text = "# a comment\nplatform home @1000\n\nplatform theme dark @2000\nplatform back\n";
  const steps = lib.parseScript(text);
  assert(steps.length === 3 && steps[1].value === "dark" && steps[1].timestampNs === 2000 && steps[2].timestampNs === -1, "parse");
  assert(lib.parseScript(lib.exportScript(lib.parseScript(text).map((step, index) => lib.entryOf(step, reply({ type: step.type, sequence: index + 1, timestampNs: Math.max(step.timestampNs, 0) })))) ).length === 3, "round trip");
  for (const [bad, line] of [["click 7", 1], ["platform kill", 1], ["platform theme dark light", 1], ["platform back @soon", 1], ["# ok\nplatform back @-1", 2]]) {
    try {
      lib.parseScript(bad);
      throw new Error(`"${bad}" was accepted`);
    } catch (error) {
      assert(error instanceof lib.ScriptError && error.line === line, `"${bad}": ${error.message}`);
    }
  }
  const long = Array.from({ length: lib.MAX_SCRIPT_STEPS + 1 }, () => "platform back").join("\n");
  try {
    lib.parseScript(long);
    throw new Error("an over-long script was accepted");
  } catch (error) {
    assert(error instanceof lib.ScriptError, error.message);
  }
});

await check("an engine reply that is not a reply becomes an unavailable one, never a throw", () => {
  assert(lib.parseReply("back", "not json").ok === false && lib.parseReply("back", "[]").ok === false, "bad documents");
  const good = lib.parseReply("home", JSON.stringify({ ok: true, type: "home", handled: true, state: "background" }));
  assert(good.ok && good.handled && good.relaunch === false, "defaults not filled");
  assert(lib.unavailableReply("back", "x").error.startsWith("TESSERA_PLATFORM_UNAVAILABLE"), "unavailable code");
});

// --- the engine under Node ---------------------------------------------------------------------------

const sceneModule = option("--scene-module");
if (sceneModule === null) {
  process.stdout.write("note the Wasm engine checks were skipped (no --scene-module)\n");
} else {
  const factory = (await import(pathToFileURL(resolve(sceneModule)).href)).default;
  const module = await factory();

  const SOURCE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Count: {count}</Text>
      <Button automationId="101" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

  function runScript(script) {
    const pointers = [];
    const alloc = (text) => {
      const length = module.lengthBytesUTF8(text) + 1;
      const pointer = module._malloc(length);
      module.stringToUTF8(text, pointer, length);
      pointers.push(pointer);
      return pointer;
    };
    try {
      module._tessera_project_begin();
      module._tessera_project_add_source(alloc("ui/Counter.tsx"), alloc(SOURCE));
      module._tessera_project_set_entry(alloc("ui/Counter.tsx"));
      const result = module._tessera_input_json(alloc("Counter"), alloc(script));
      const text = module.UTF8ToString(result);
      module._tessera_free(result);
      return JSON.parse(text);
    } finally {
      for (const pointer of pointers) {
        module._free(pointer);
      }
    }
  }

  const platformSteps = (document) => document.steps.filter((step) => step.kind === "platform");

  await check("every event type of the engine is delivered under Node and answers its effect", () => {
    const document = runScript([
      "click 101", "click 101",
      "platform volumeDown", "platform recents", "platform activate",
      "platform home @1000000", "platform back", "platform resume @2000000",
      "platform power", "platform volumeUp", "platform power",
      "platform orientation portrait", "platform fold book", "platform fold none",
      "platform theme dark", "platform locale vi-VN", "platform textScale 1.3", "platform ime show",
      "platform memoryPressure", "platform batterySaver on", "platform thermal serious",
      "platform suspend", "platform resume", "platform screenOff", "platform screenOn",
      "platform back",
    ].join("\n"));
    const steps = platformSteps(document);
    assert(steps.length === 24, `${steps.length} platform steps answered`);
    const expected = {
      volumeDown: [false, "running"], recents: [true, "inactive"], activate: [true, "running"],
      home: [true, "background"], back: [false, "background"], resume: [true, "running"],
      power: [true, "screenOff"], volumeUp: [false, "screenOff"],
    };
    const byIndex = (index) => steps[index].reply;
    assert(byIndex(0).handled === false && byIndex(0).state === "running", "an unfocused volume key is dropped");
    assert(byIndex(1).state === "inactive" && byIndex(2).state === "running", "recents then activate");
    assert(byIndex(3).state === "background" && byIndex(3).savedState === true, "home saves the state");
    assert(byIndex(4).handled === false && byIndex(4).state === "background", "a backgrounded app receives no back");
    assert(byIndex(5).resumed === true && byIndex(5).state === "running", "resume is a hot resume");
    assert(byIndex(6).state === "screenOff" && byIndex(8).resumed === true, "power cycles the screen");
    assert(byIndex(9).handled === true && byIndex(9).changed === true, "orientation re-lays out in place");
    assert(byIndex(10).changed === true && byIndex(11).changed === true, "fold in and out");
    for (let index = 12; index <= 18; index += 1) {
      assert(byIndex(index).ok === true && byIndex(index).handled === false, `step ${index} (${steps[index].type}) is delivered without an effect`);
    }
    const last = byIndex(steps.length - 1);
    assert(last.appClosed === true && last.state === "closed" && last.dispatch === "unhandled", "the final back closes the app");
    const cells = document.steps[document.steps.length - 1].state;
    assert(cells.length === 1 && cells[0].value.value === "2", `the count after a hot resume and a close is ${JSON.stringify(cells)}`);
    assert(Object.keys(expected).length === 8, "unreachable");
  });

  await check("a malformed platform script is an error document, not a crash", () => {
    const document = runScript("platform kill");
    assert(typeof document.error === "string" && document.error.includes("not a platform event type"), JSON.stringify(document));
  });
}

const canvasModule = option("--canvas-module");
if (canvasModule !== null) {
  const factory = (await import(pathToFileURL(resolve(canvasModule)).href)).default;
  const module = await factory();
  await check("the canvas module exports platform.event and answers when no session can start", () => {
    assert(typeof module._tessera_canvas_platform_event === "function", "the module does not export _tessera_canvas_platform_event");
    const call = (type, value, ns) => {
      const a = module._malloc(module.lengthBytesUTF8(type) + 1);
      module.stringToUTF8(type, a, module.lengthBytesUTF8(type) + 1);
      const b = module._malloc(module.lengthBytesUTF8(value) + 1);
      module.stringToUTF8(value, b, module.lengthBytesUTF8(value) + 1);
      const result = module._tessera_canvas_platform_event(a, b, ns);
      const text = module.UTF8ToString(result);
      module._tessera_free(result);
      module._free(a);
      module._free(b);
      return JSON.parse(text);
    };
    const noSession = call("back", "", -1);
    assert(noSession.ok === false && noSession.error.startsWith("TESSERA_PLATFORM_NO_SESSION"), JSON.stringify(noSession));
    const unknown = call("kill", "", -1);
    assert(unknown.ok === false && unknown.error.startsWith("TESSERA_PLATFORM_UNKNOWN_EVENT"), JSON.stringify(unknown));
  });
}

process.stdout.write(failures === 0 ? "\nplatform event checks: all passed\n" : `\nplatform event checks: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
