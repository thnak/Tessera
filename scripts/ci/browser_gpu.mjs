// SPDX-License-Identifier: MIT
//
// The one place the device-dependent browser gates (issue #958) agree on three things: how headless
// Chromium is launched for WebGPU, how a run that lost its device is retried, and how it is reported.
//
//   import { launchGpuBrowser, createGate, pageLostDevice } from './browser_gpu.mjs';
//
// **The policy.** A hosted runner with no GPU hands Chromium a SwiftShader adapter that can lose the
// device ("The WebGPU device was lost: unknown"), and a canvas check cannot be judged without a device.
// So a section whose page lost the device is retried, and if it loses it on every attempt it is
// reported as an explicit SKIP and never as a pass. A section that kept its device is judged normally,
// so a real regression on a runner that holds a device still fails. `passed with N skipped` is the
// verdict when nothing failed but something was not exercised.

import { chromium } from 'playwright';

/**
 * The flags that give headless Chromium a WebGPU device on a host with no GPU.
 *
 * `--no-sandbox` is what a containerised runner needs (unprivileged user namespaces are off on Ubuntu
 * 23.10+). `--use-webgpu-adapter=swiftshader` asks Dawn for the software adapter by name instead of
 * leaving the choice to the GPU blocklist, and `--enable-unsafe-swiftshader` is required by recent
 * Chrome before it will run SwiftShader at all. `--disable-gpu-sandbox` keeps the GPU process from being
 * killed by a seccomp policy the runner image cannot satisfy, which is the usual cause of a device that
 * dies right after it is created.
 */
export const GPU_ARGS = [
  '--enable-unsafe-webgpu',
  '--enable-features=Vulkan',
  '--use-angle=swiftshader',
  '--use-webgpu-adapter=swiftshader',
  '--enable-unsafe-swiftshader',
  '--ignore-gpu-blocklist',
  '--disable-gpu-sandbox',
  '--no-sandbox',
];

/**
 * Launches Chromium for a gate that needs the canvas.
 *
 * Playwright's default headless mode is the separate `chromium-headless-shell` binary, a reduced build
 * whose GPU stack is not the one a desktop Chrome ships. `channel: 'chromium'` selects the full build
 * in the new headless mode instead, which is the build WebGPU is exercised on upstream.
 * `TESSERA_BROWSER_CHANNEL=shell` goes back to the headless shell, and `TESSERA_BROWSER_CHANNEL=<name>`
 * names any other Playwright channel, so a runner can be A/B-tested without a code change. The browser
 * version is printed once: a runner-image or Chrome bump is the first suspect when a device starts
 * getting lost, and the log should say which build ran.
 */
export async function launchGpuBrowser() {
  const wanted = process.env.TESSERA_BROWSER_CHANNEL ?? 'chromium';
  const options = { args: GPU_ARGS };
  if (wanted !== 'shell') options.channel = wanted;
  const browser = await chromium.launch(options);
  process.stdout.write(`     browser: Chromium ${browser.version()} (${wanted === 'shell' ? 'headless shell' : `channel ${wanted}`})\n`);
  return browser;
}

/**
 * Whether the page's canvas pane reports a lost device: the unsupported pane is up and carries the
 * "device was lost" sentence (`views/canvas-session.ts`).
 */
export function pageLostDevice(page) {
  return page.evaluate(() => {
    const pane = document.getElementById('unsupported');
    const body = document.getElementById('unsupported-body')?.textContent ?? '';
    return pane !== null && !pane.hidden && /device was lost/i.test(body);
  });
}

/** A gate's counters and its retry-then-SKIP section runner. One per script. */
export function createGate() {
  let failures = 0;
  let skipped = 0;
  /** Lines of the attempt in progress; a section discards them when the runner lost the device under it. */
  let pending = [];

  return {
    check(name, ok, detail) {
      // The detail is printed on a failure: on a pass it is the observation, which is noise.
      pending.push({ ok, line: `${ok ? 'ok  ' : 'FAIL'} ${name}${!ok && detail ? `  -- ${detail}` : ''}\n` });
    },

    /**
     * Runs one browser section, up to three times. `run` returns true when the device was lost under
     * it (its checks are then discarded and the section is retried); any other return is a verdict.
     */
    async section(name, run) {
      const attempts = 3;
      for (let attempt = 1; attempt <= attempts; attempt += 1) {
        pending = [];
        const lost = await run();
        if (!lost) {
          for (const entry of pending) {
            process.stdout.write(entry.line);
            if (!entry.ok) failures += 1;
          }
          return;
        }
        process.stdout.write(
          `     ${name}, attempt ${attempt}: the runner lost the WebGPU device; ${attempt < attempts ? 'retrying' : 'giving up'}\n`,
        );
      }
      skipped += 1;
      process.stdout.write(`SKIP ${name}  -- this runner lost the WebGPU device on every attempt\n`);
    },

    /** Prints the verdict line and returns the process exit code. */
    finish(label) {
      const verdict =
        failures > 0
          ? `${failures} check(s) failed`
          : skipped > 0
            ? `passed with ${skipped} skipped section(s) -- a check this runner could not exercise is not a pass`
            : 'all checks passed';
      process.stdout.write(`\n${label}: ${verdict}\n`);
      return failures === 0 ? 0 : 1;
    },
  };
}
