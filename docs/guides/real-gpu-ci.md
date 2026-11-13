# Real-GPU test lane

Hosted CI runs no window or GPU test: the runners have no display and no Vulkan device, so every
`app_window_*`, `vulkan_*` and `caret_blink_host` test prints `skipped` and passes. That hides
defects that only a real swapchain shows (for example a frame skipped on a busy slot that lost its
repaint request, fixed in issue #203). The real-GPU lane runs those tests on a machine that has a GPU.

It is **manual**: a person starts it. It never runs on a push or a pull request.

## What runs

[`scripts/gpu_tests.ps1`](../../scripts/gpu_tests.ps1) configures a Release Ninja build in
`build\gpu-real`, builds everything, prints the Vulkan devices, then runs the ctest tests matching
`app_window|vulkan_|caret_blink|app_input|gpu`. Options:

| Option | Meaning |
|---|---|
| `-Filter <regex>` | Which ctest tests to run |
| `-Repeat <n>` | Run each up to `n` times and stop at the first failure (finds flakes) |
| `-Reconfigure` | Re-run CMake configure |
| `TESSERA_GPU_FAIL_ON_SKIP=1` | Exit non-zero when a test skipped, so a machine with no display or driver cannot look green |

Run it by hand on any Windows machine with Visual Studio Build Tools, CMake, Ninja and the Vulkan SDK:

```powershell
.\scripts\gpu_tests.ps1 -Repeat 5
```

## The GitHub workflow

[`.github/workflows/gpu-real.yml`](../../.github/workflows/gpu-real.yml) (`Real GPU`, Actions tab,
**Run workflow**) calls the same script on a self-hosted runner. Inputs: `ref` (branch, tag or
commit; default `main`), `filter`, `repeat` (default 3) and `fail_on_skip` (default true). It
uploads `LastTest.log` as an artifact for 14 days. Runs queue one at a time.

## Registering the runner (once, by the owner)

1. Repository **Settings > Actions > Runners > New self-hosted runner**, Windows x64. Follow the
   download and `config.cmd` steps GitHub shows; it creates a registration token that expires in
   about an hour.
2. When `config.cmd` asks for labels, add **`tessera-gpu`** (the workflow also requires the default
   `self-hosted`, `windows` and `x64`).
3. Run it as a normal user session, **not** as a service: the window tests need an interactive
   desktop. Start `run.cmd` in a logged-in session (or use an auto-logon account) and keep the
   screen unlocked while a run is in progress.
4. Install and check: `vulkaninfo --summary` lists the GPU; `cmake`, `ninja` and the MSVC tools are
   found (the script loads the Visual Studio environment itself).

## Security rules

This repository is public, and a self-hosted runner executes whatever a workflow tells it to.

- The workflow has **only** `workflow_dispatch`. Never add `pull_request`, `pull_request_target`,
  `push` or `schedule` to it: a fork's pull request would run its code on the owner's machine.
- Keep **Settings > Actions > General > Fork pull request workflows** on "Require approval for all
  outside collaborators", and never assign the `tessera-gpu` label to a runner shared with other
  repositories.
- Only users with write access can start a dispatch. Treat the `ref` input as trusted only for
  refs of this repository; inputs reach the script through environment variables, not the script text.
- The runner holds no secrets, and the workflow has `contents: read` and no other permission.
- Use a dedicated Windows account for the runner, with no access to personal files or credentials.

## Reading the result

- `100% tests passed` and no "tests that skipped" block: the GPU path ran and held.
- A skipped list means the runner had no display or driver for those tests: fix the runner, because
  those tests proved nothing.
- A failure that appears only with `-Repeat` is a flake: file an issue with the log, because a
  real-GPU flake has been a real defect before.

## Limits

- Windows and one GPU model only. A Linux runner with a GPU, and a matrix of GPU vendors, are
  later steps (the Vulkan driver of the machine decides what the lane proves).
- It builds the whole tree each time the cache is cold (about 1000 steps); `build\gpu-real` is kept
  between runs, so a warm run only rebuilds what changed.
- Pixel goldens stay lavapipe-only ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md));
  this lane checks behavior (presents, recovery, input, pacing), not images from a real GPU.
