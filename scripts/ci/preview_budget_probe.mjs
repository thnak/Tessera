// SPDX-License-Identifier: MIT
//
// Preview budget probe (issue #170, P6 of docs/design/reference-equantic-ui.md section 7.4).
//
// `scripts/check_preview_budget.py` owns the budget file and the ratchet; this file only *measures*
// and prints one JSON object, so the numbers and the policy stay in separate places. Node is already a
// CI dependency of this repository (the P1 parity gate drives the Emscripten module through
// `$EMSDK_NODE`), so this adds no new dependency and no new toolchain.
//
// Two modes:
//
//   --cold          instantiate the module and run one compile: the engine half of "cold time to
//                   first frame". The page's first paint also needs HTML/CSS parse and a GPU frame
//                   (P3, not landed), so this is honestly only the engine's share; the budget file
//                   says so.
//   --edits N       run N compiles after `--warmup` unmeasured ones: the engine half of
//                   "edit-to-view". The frame is not in this number for the same reason.
//
// Every figure is wall clock, which is the point of the budget, and therefore noisy. Since issue
// #395 each process also times a fixed integer workload and reports `calibration_ms`, which the
// Python side divides out (see scripts/preview_budget_measure.py). That turns "how fast was this
// box while it ran" into a divisor instead of into the budget: the enforced number is
// `measured_ms * reference_ms / calibration_ms`, and `*_raw_ms` is still reported for triage.
//
// The calibration is not a formality, so it is guarded: `calibration_checksum` is the workload's
// result, and the Python side refuses a run whose checksum differs from the expected value. Without
// that guard a future edit that let the JIT optimise the loop away would collapse `calibration_ms`
// to near zero, which would *inflate* every normalized latency and look like a huge regression --
// or, worse, a cheap loop would make the normalization a no-op and quietly restore the raw number.
//
// Nothing here writes a file, touches the network or reads the clock more than `performance.now()`.

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

// The calibration workload. Iterations and the exact arithmetic are part of the measurement's
// definition -- they are echoed into the budget file, so changing either rebases every latency pin.
// `Math.imul` and `| 0` keep every intermediate an exact 32-bit integer, so the checksum is the same
// on every engine and platform (a float sum would not be).
const CALIBRATION_ITERATIONS = 4000000;
const CALIBRATION_WARMUP = 200000;
const CALIBRATION_SEED = 0x2545f491;

function calibrate(iterations) {
  let accumulator = CALIBRATION_SEED;
  for (let index = 0; index < iterations; index += 1) {
    accumulator = (accumulator + Math.imul(index, 2654435761)) | 0;
    accumulator = (accumulator ^ (accumulator >>> 13)) | 0;
    accumulator = Math.imul(accumulator, 1274126177) | 0;
  }
  return accumulator;
}

function measureCalibration() {
  calibrate(CALIBRATION_WARMUP); // warm the JIT so the timed run is not interpreted bytecode
  const started = performance.now();
  const checksum = calibrate(CALIBRATION_ITERATIONS);
  return { calibrationMs: performance.now() - started, checksum: checksum >>> 0 };
}

function usage(program) {
  process.stderr.write(
    `usage: ${program} <module.js> <source.tsx> --cold\n` +
      `       ${program} <module.js> <source.tsx> --edits N [--warmup M]\n`,
  );
}

function parseArgs(argv) {
  const options = { modulePath: null, sourcePath: null, mode: null, edits: 0, warmup: 3 };
  for (let index = 2; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--cold") {
      options.mode = "cold";
    } else if (argument === "--edits") {
      index += 1;
      options.edits = Number(argv[index]);
      options.mode = options.mode || "edits";
    } else if (argument === "--warmup") {
      index += 1;
      options.warmup = Number(argv[index]);
    } else if (argument.startsWith("-")) {
      return null;
    } else if (options.modulePath === null) {
      options.modulePath = argument;
    } else if (options.sourcePath === null) {
      options.sourcePath = argument;
    } else {
      return null;
    }
  }
  if (options.modulePath === null || options.sourcePath === null || options.mode === null) {
    return null;
  }
  if (!Number.isInteger(options.edits) || options.edits < 0) {
    return null;
  }
  if (options.mode === "edits" && options.edits < 1) {
    return null;
  }
  if (!Number.isInteger(options.warmup) || options.warmup < 0) {
    return null;
  }
  return options;
}

async function main() {
  const options = parseArgs(process.argv);
  if (options === null) {
    usage(process.argv[1]);
    return 2;
  }
  const source = readFileSync(options.sourcePath, "utf8");
  // The file name is synthetic on purpose: the page compiles the textarea contents and labels them
  // `ui/Preview.tsx`, so the measurement uses the same span file the page would report.
  const file = "ui/Preview.tsx";

  // A bare relative path is not a valid ESM specifier on any platform and an absolute one is only a
  // valid specifier as a file URL on Windows, so normalize here and let the caller pass what a shell
  // or `subprocess` naturally produces.
  const imported = await import(pathToFileURL(resolve(options.modulePath)).href);
  const factory = imported.default;
  if (typeof factory !== "function") {
    process.stderr.write(`preview_budget_probe: ${options.modulePath} has no default factory\n`);
    return 2;
  }

  const instantiateStarted = performance.now();
  const module = await factory();
  const instantiateMs = performance.now() - instantiateStarted;

  const compileOnce = () => {
    const textLength = module.lengthBytesUTF8(source) + 1;
    const textPtr = module._malloc(textLength);
    module.stringToUTF8(source, textPtr, textLength);
    const nameLength = module.lengthBytesUTF8(file) + 1;
    const namePtr = module._malloc(nameLength);
    module.stringToUTF8(file, namePtr, nameLength);
    try {
      const result = module._tessera_diagnostics_json(textPtr, namePtr);
      if (!result) {
        throw new Error("tessera_diagnostics_json returned null");
      }
      try {
        return module.UTF8ToString(result);
      } finally {
        module._tessera_free(result);
      }
    } finally {
      module._free(textPtr);
      module._free(namePtr);
    }
  };

  const firstStarted = performance.now();
  const firstDocument = compileOnce();
  const firstCompileMs = performance.now() - firstStarted;

  const calibration = measureCalibration();

  const report = {
    probe: "preview_budget",
    instantiate_ms: Number(instantiateMs.toFixed(3)),
    first_compile_ms: Number(firstCompileMs.toFixed(3)),
    // The raw engine-ready figure: instantiation plus the first compile is what "cold" means before
    // a scene/frame exists. Reported for triage; the budget row is the normalized one below.
    cold_engine_ready_raw_ms: Number((instantiateMs + firstCompileMs).toFixed(3)),
    // The number the Python side divides by. Its checksum proves the workload actually ran in full
    // (a JIT that hoisted the loop would leave `calibration_ms` near zero and invert the result).
    calibration_ms: Number(calibration.calibrationMs.toFixed(3)),
    calibration_checksum: calibration.checksum,
    calibration_iterations: CALIBRATION_ITERATIONS,
    document_bytes: firstDocument.length,
    samples: [],
  };

  if (options.mode === "edits") {
    for (let index = 0; index < options.warmup; index += 1) {
      compileOnce();
    }
    for (let index = 0; index < options.edits; index += 1) {
      const started = performance.now();
      compileOnce();
      report.samples.push(Number((performance.now() - started).toFixed(3)));
    }
  }

  process.stdout.write(`${JSON.stringify(report)}\n`);
  return 0;
}

main().then(
  (code) => {
    process.exitCode = code;
  },
  (error) => {
    process.stderr.write(`preview_budget_probe: ${error && error.message ? error.message : error}\n`);
    process.exitCode = 1;
  },
);
