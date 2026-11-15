---
name: build-lanes
description: Use when building or testing Tessera costs too much, when a build is slow or duplicated, when deciding which machine should compile a change, or when a subagent needs a worktree plus a build directory. Covers the two build machines (a local Windows lane with a real GPU, and a remote Linux lane with many cores and lavapipe), per-lane worktree and build-directory conventions, and the create-and-delete lifecycle the Lead runs around subagents.
---

# Build lanes: cheap compilation across two machines

Tessera is C++23 with 280+ test executables and a heavy `third_party` tree. A naive
"build everything, twice, in several places" habit is the single largest avoidable cost in a
session: it burns wall-clock, disk, and — because the agent waits on it — context.

The normative policy stays in [AGENTS.md, Subagent lanes](../../../AGENTS.md#subagent-lanes).
This file adds the operating rules and the exact commands.

## The two lanes

| Lane | What it is | Use for |
|---|---|---|
| `win` | This machine. Windows, MSVC, `vs2026` preset, few cores, **a real GPU**. | GPU/window/present tests, anything Windows-only, the final pre-push verification. |
| `linux` | A remote Linux host reached over SSH. Clang/GCC, Ninja, many cores, **lavapipe only**. | Linux-only code, POSIX paths, cheap wide parallel CPU compiles, lavapipe goldens. |

**Machine-specific facts — hosts, addresses, core counts, GPU models, free disk — are deliberately
not in this file.** This repository is public. Read them from
`references/machines.local.md` beside this skill when that file exists; it is git-ignored and
holds the local values. If it is missing, ask the owner for the lane's SSH alias and capabilities
instead of guessing.

Neither lane is a CI substitute: hosted CI is the only thing that proves a landing.

### Only the local lane has a real GPU

GPU and window tests (`app_window_*`, `vulkan_*`, `caret_blink*`) skip silently on lavapipe and on
any headless host. A green run there proves nothing, so run those tests locally with
[scripts/gpu_tests.ps1](../../../scripts/gpu_tests.ps1) — see
[real-gpu-ci.md](../../../docs/guides/real-gpu-ci.md). Treat a skip on a lane without the
capability as "not tested here", never as a pass.

## Choose the cheapest lane that can actually fail

Ask what the change can break, then buy the cheapest check that detects it:

1. **Nothing compiled — run the gates.** Docs, links, formatting, and licenses need no compiler:
   `.\scripts\gates.ps1` and `.\scripts\format.ps1 -Check`. This is the correct first move for any
   doc or config change and costs seconds.
2. **One focused target.** Build only what changed; a single test executable rebuilds in seconds
   while the whole tree takes many minutes.
   ```powershell
   .\scripts\build.ps1 -Preset dev -Target scene_graph_tests
   ctest --preset dev -R scene_graph
   ```
3. **Linux-only work** goes to the `linux` lane, where wide parallelism and a warm `ccache` make a
   cold configure far cheaper than on a small Windows box.
4. **GPU/window/pacing/swapchain** runs on the local `win` lane only. Do not queue these remotely;
   they skip there and report a false green.

A run that cannot fail is worse than no run: it spends time and teaches nothing.

## Worktrees: one lane, one directory

Never edit the shared checkout while a build or a subagent is running in it. The shared index and
every push belong to the Lead ([AGENTS.md](../../../AGENTS.md#subagent-lanes)).

Local worktrees live beside the repo, and build trees are already git-ignored (`/build/`,
`/build-*/` in [.gitignore](../../../.gitignore)):

```powershell
# create a lane for a slice
git worktree add ..\Tessera-<slice> -b feat/<slice>

# from inside the worktree: its own build directory, never the shared build\
cmake --preset dev
cmake --build --preset dev --target <one_target>
ctest --preset dev -R <regex>
```

Name lanes after the slice, not the agent (`feat/text-overflow-ellipsis`, not `agent-3`), so a
stale lane stays identifiable later.

On the remote, reuse one lane directory and `git fetch` + reset it. **Do not create a new lane per
task**: each full checkout carries its own `third_party` and build tree, so lane sprawl fills the
disk quickly. Check free space before adding one.

## Lifecycle the Lead owns

The Lead creates the lane and the Lead deletes it. A subagent reports a diff and does not clean up
behind itself; nothing else will.

```powershell
# 0. before delegating: state behavior, allowed files, build dir, and acceptance test

# 1. create
git worktree add ..\Tessera-<slice> -b feat/<slice>

# 2. hand off: the subagent works in that worktree, builds in its own build\<lane>, never commits

# 3. Lead reviews the diff, re-runs the matching check at the exact HEAD, commits with explicit
#    pathspecs, then removes the lane
git worktree remove ..\Tessera-<slice>          # refuses if dirty — inspect first
git worktree prune
git branch -d feat/<slice>                      # only once merged
```

Refuse to remove a worktree that is dirty or still ahead of `main`. Check before removing:

```powershell
git worktree list
git -C ..\Tessera-<slice> status --porcelain
git -C ..\Tessera-<slice> rev-list --count main..feat/<slice>
```

### Audit lane sprawl

Worktrees are cheap to create and expensive to remember. List every lane with its state and prune
in one pass:

```powershell
git worktree list --porcelain | Select-String '^worktree ' |
  ForEach-Object { $_.Line -replace '^worktree ','' }
```

A lane that is `clean` and **0 commits ahead of `main`** is fully landed: nothing but a build tree
remains. Remove the worktree, then delete the branch once its remote counterpart is gone
(`git ls-remote --heads origin <branch>` returns empty).

## Remote lane invocation

Keep it non-interactive, and pass scripts on stdin. PowerShell interpolates `$` inside
double-quoted strings, which silently corrupts shell variables, so use a here-string and send the
text:

```powershell
$script = @'
cd ~/tessera-src
cmake --build --preset dev --target scene_graph_tests
'@
$tmp = Join-Path $env:TEMP 'lane.sh'
Set-Content -Path $tmp -Value $script -NoNewline -Encoding UTF8
Get-Content $tmp -Raw | ssh -o BatchMode=yes <lane-host> 'bash -s'
```

Use `-o BatchMode=yes` so a missing key fails fast instead of hanging on a prompt. Run Linux tests
under lavapipe explicitly:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --preset ci
```

Push commits to the remote lane instead of re-cloning, and never let a remote lane hold the only
copy of uncommitted work.

## Cost rules that actually matter

- **Never build the whole tree to check one file.** Use `-Target` and `ctest -R`.
- **Never run a suite twice in one turn.** Re-run only after a change that could invalidate it.
- **Never push to find out whether CI passes**
  ([AGENTS.md](../../../AGENTS.md#landing-size-and-ci-load)).
- **Batch verification to the end of a coherent slice**, not after every edit.
- **Use `dev` for iteration; `release`/`ci` only when the gate needs it.**
- **Keep the compiler cache warm.** Both lanes use `sccache`/`ccache`, so reuse an existing build
  directory rather than creating a fresh one per attempt; a cold cache means a full recompile.
- **Skipped is not passed.** A skip on a lane lacking the capability means untested.

## Two failed-looking suites that are the environment, not the code

Check these before debugging a "regression"; both have cost sessions real time.

**1. Five tests fail on `%TEMP%` writes, and they are not broken.** `codegen_cli`,
`codegen_stylesheet_embed`, `codegen_theme_embed`, `icons_import` and `harness_frame_check` shell
out or write to `fs::temp_directory_path()`. Launched from a reused build tree under a path carrying
the sandbox's restricting ACE (a raw `S-1-4-…` trustee on the checkout root) they fail with
`0xc0000409` or "writing … failed". The trap is that this reads like a code defect: the same tests
pass from a fresh worktree, and a **byte-identical** binary behaves differently purely by location —
copying it between the two trees moves the failure with the path, not the file. Check the location,
not the code:

```powershell
# The two binaries are identical, so no source difference can explain the split:
(Get-FileHash build/vs2026/src/compiler/codegen/Release/tessera_codegen.exe).Hash -eq
(Get-FileHash ..\Tessera-<slice>\build\vs2026/src/compiler/codegen/Release/tessera_codegen.exe).Hash

# The restricting trustee, present on the shared checkout and absent from a fresh worktree:
(Get-Acl .).Access | Where-Object { $_.IdentityReference -match '^S-1-4-' }
```

When the hashes match and the ACE is present, build and test the affected target in a fresh worktree
rather than reading the code or weakening the tests. `TMP`/`TEMP` overrides do not help, and a
`New-Item` from PowerShell proves nothing — PowerShell can write where a spawned process cannot.
Confirm the suite is otherwise green on CI (`gh run list --workflow=windows.yml --branch main`).

**2. A stale generated file in a reused build tree after a codegen change.** Codegen rewrites its
outputs in place, so a build tree configured before a generator change can hold an old shape and
fail to compile a sample or a fixture while the *source* is correct (`gallery` failed with
`'Gallery' is not a member of 'gallery'` after the ADR-040 migration, because the tree still held a
legacy `Gallery.hpp`). Delete the generated directory and rebuild the one target:

```powershell
Remove-Item -Recurse -Force build/vs2026/samples/gallery/generated
cmake --build --preset vs2026-release --target gallery
```

A **lane that built the same target before the generator changed** is the trap: it reports green on
the stale artifact. A fresh worktree or `git clean -xdf <generated dir>` is the honest check.

## Related

- [AGENTS.md, Subagent lanes](../../../AGENTS.md#subagent-lanes) — normative delegation policy.
- [docs/guides/testing.md](../../../docs/guides/testing.md) — presets and the test matrix.
- [docs/guides/real-gpu-ci.md](../../../docs/guides/real-gpu-ci.md) — the real-GPU lane.
- [docs/guides/code-style.md](../../../docs/guides/code-style.md) — warnings-as-errors builds.
