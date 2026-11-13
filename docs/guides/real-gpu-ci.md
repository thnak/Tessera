# Real-GPU test lane

Hosted CI runs no window or GPU test: the runners have no display and no Vulkan device, so every
`app_window_*`, `vulkan_*` and `caret_blink_host` test prints `skipped` and passes. That hides
defects that only a real swapchain shows (for example a frame skipped on a busy slot that lost its
repaint request, fixed in issue #203). The real-GPU lane runs those tests on a machine that has a GPU.

It is a **local script**, run by hand on the developer machine. There is no GitHub workflow and no
self-hosted runner: the machine is not always online, and a self-hosted runner on a public
repository would execute code from outside contributors.

## What runs

[`scripts/gpu_tests.ps1`](../../scripts/gpu_tests.ps1) configures a Release Ninja build in
`build\gpu-real`, builds everything, prints the Vulkan devices, then runs the ctest tests matching
`app_window|vulkan_|caret_blink|app_input|gpu`. Needs Windows, Visual Studio Build Tools, CMake,
Ninja and the Vulkan SDK; the script loads the Visual Studio environment itself.

```powershell
.\scripts\gpu_tests.ps1                      # build, then run every GPU/window test once
.\scripts\gpu_tests.ps1 -Repeat 5            # up to 5 runs of each test, stop at a failure
.\scripts\gpu_tests.ps1 -Filter caret_blink  # only the tests matching this ctest regex
.\scripts\gpu_tests.ps1 -BuildDir build\win-ninja   # reuse an existing Ninja build tree
```

| Option | Meaning |
|---|---|
| `-Filter <regex>` | Which ctest tests to run |
| `-Repeat <n>` | Run each up to `n` times and stop at the first failure (finds flakes) |
| `-BuildDir <dir>` | Build tree (default `build\gpu-real`); an existing Ninja tree is reused |
| `-Reconfigure` | Re-run CMake configure |
| `TESSERA_GPU_FAIL_ON_SKIP=1` | Exit non-zero when a test skipped, so a machine with no display or driver cannot look green |

The exit code is ctest's, so the script also works from another script or a git hook.

## When to run it

- Before a release and after a change to the window host, the swapchain, pacing, recovery or input:
  `.\scripts\gpu_tests.ps1 -Repeat 3`.
- After any change that hosted CI cannot exercise, with the machine unlocked and the display on:
  the window tests need an interactive desktop.
- Record the result in the pull request or commit message (device name and `-Repeat` count).

## Reading the result

- `100% tests passed` and no "tests that skipped" block: the GPU path ran and held.
- A skipped list means this machine had no display or driver for those tests: fix the setup, because
  those tests proved nothing.
- A failure that appears only with `-Repeat` is a flake: file an issue with the output, because a
  real-GPU flake has been a real defect before.

## Limits

- Windows and one GPU model only. A Linux machine with a GPU, and a matrix of vendors, are later
  steps (the Vulkan driver of the machine decides what the lane proves).
- A cold run builds the whole tree (about 1000 steps); `build\gpu-real` is kept between runs, so a
  warm run only rebuilds what changed.
- Pixel goldens stay lavapipe-only ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md));
  this lane checks behavior (presents, recovery, input, pacing), not images from a real GPU.
