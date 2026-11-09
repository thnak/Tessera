# Contributing to Tessera

Tessera is pre-alpha. The repository is documentation-heavy and code is landing in phases; read
the design before assuming a documented subsystem exists. The canonical remote is
<https://github.com/thnak/Tessera.git>.

By contributing, you agree that your contribution is licensed under the project's MIT license
([LICENSE](LICENSE)).

## 1. Before you start

1. Read [AGENTS.md](AGENTS.md) — the operating manual and non-negotiables.
2. Read the [documentation map](docs/README.md), then the docs for the area you will touch.
3. Read [code style](docs/guides/code-style.md) and [testing](docs/guides/testing.md).
4. Search existing issues/PRs before opening a new one. For a large or architectural change,
   open an issue first so the design and ADR scope can be agreed before code is written.
5. For security-sensitive reports, do not post exploit details in a public issue. Contact the
   maintainer through GitHub so a private fix can be coordinated.

## 2. Setup

Requirements:

- CMake >= 3.24 and a C++23 compiler.
- Ninja for the `dev`/`release`/`ci` presets, or Visual Studio 2022/2026 for the VS presets.
- Vulkan SDK for the native Vulkan backend (`VULKAN_SDK` set).
- Linux CI uses Mesa lavapipe (software Vulkan ICD) for headless/golden tests.

```bash
git clone https://github.com/thnak/Tessera.git
cd Tessera
```

Windows:

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
.\scripts\format.ps1 -Check
.\scripts\lint.ps1 -BuildDir build\dev
```

Linux:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
bash scripts/format.sh --check
BUILD_DIR=build/dev bash scripts/lint.sh
```

The manual `cmake -S . -B build ...` path still works, but presets are the supported route. See
[AGENTS.md](AGENTS.md#build--test) for the PoC/demo commands.

### Repository gates

Run the fast release gates with one command from the repository root:

```bash
python -B scripts/gates.py            # --only docs,pins runs a subset
```

`--archive <path>` also validates a distributable archive, `--list` prints the gate order, and
`--json` emits a machine-readable summary. The same command runs in CI on Linux, Windows and macOS;
details are in [`docs/guides/packaging.md`](docs/guides/packaging.md#13-run-the-gates-locally).

## 3. Branch, commit, and PR workflow

- Branch from `main`; use a short descriptive name (`fix/rhi-resource-lifetime`,
  `docs/gpu-frames`, `feat/navigation-guards`).
- Keep a PR focused on one concern. Split unrelated refactors, docs, and behavior changes.
- Commit style: `<area>: <imperative summary>`, for example `docs(gpu): add frame contract` or
  `fix(rhi): retire buffers after the frame fence`. Explain **why** in the body when it is not
  obvious.
- Do not commit generated build output, `.spv` files, local editor state, secrets, or machine-
  specific paths.
- Reference the issue/ADR in the PR description when applicable; include screenshots or golden
  diffs for visual changes.
- Do not force-push shared branches or rewrite published history without coordination.

## 4. Code style rules

The authoritative guide is [docs/guides/code-style.md](docs/guides/code-style.md); the summary
below is review-blocking.

- **Language:** C++23. Recoverable hot paths return `std::expected<T, Error>`; exceptions are for
  API boundaries only.
- **Files:** soft 300 lines, hard 500 lines; one primary type per header. Split by
  responsibility, not by arbitrary line count.
- **Naming:** `tessera::lower_case` namespaces, `PascalCase` types, `snake_case` functions and
  variables, `snake_case_` members, `kPascalCase` constants, `TESSERA_UPPER_SNAKE` macros.
- **Headers:** `#pragma once`; include what you use; no `using namespace` in headers; no
  Vulkan/WebGPU/third-party headers in public UI headers.
- **Hot paths:** no RTTI/reflection, hidden allocations, blocking calls, or per-frame resource
  creation. Comments explain why, not what.
- **License header:** new repository source files carry `SPDX-License-Identifier: MIT`.
- **Formatting/linting:** `.clang-format` (LLVM base, 4 spaces, 100 cols, pointer left) and
  `.clang-tidy`; run the scripts above before opening a PR.
- **Warnings:** `TESSERA_WARNINGS_AS_ERRORS=ON` is the default (`-Werror`/`/WX`). Fix warnings;
  suppress only the narrow third-party source that cannot be fixed.
- **Dependencies:** static linking is required; no LGPL/GPL in statically linked code. Every new
  dependency needs entries in [docs/specs/dependencies.md](docs/specs/dependencies.md) and
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## 5. Documentation rules

The authoritative rules are in [AGENTS.md](AGENTS.md#documentation-rules) and
[docs/README.md](docs/README.md#0-documentation-rules).

- Put docs in the right place: `docs/architecture/` (cross-cutting), `docs/specs/` (normative),
  `docs/design/` (rationale), `docs/guides/` (how-to), `docs/reference/` (glossary),
  `decisions/` (ADR).
- **Size:** target 400 lines, hard limit **600 lines**. A larger topic becomes a folder with a
  `README.md` index and linked parts (for example `docs/design/gpu/`).
- Update [docs/README.md](docs/README.md) for every add/move/rename, and
  [decisions/README.md](decisions/README.md) for every ADR.
- Use relative links; keep headings stable; no orphan docs; code fences must balance.
- Keep one normative source per rule. Design docs explain and link to the spec instead of
  copying it.
- A new subsystem needs a design note; a contentious or hard-to-reverse decision needs an ADR;
  a new artifact/format needs an entry in the versioning spec.

## 6. Tests

- Every behavior change gets a deterministic regression test.
- No wall clock, network, system fonts, random ids, or real-GPU baselines in tests/goldens.
- Put UI/compiler unit tests in `tests/unit/`; GPU tests in `tests/gpu/<area>/`; integration
  tests in `tests/integration/`; goldens in `tests/golden/` (GPU: `tests/golden/gpu/`);
  fixtures in `tests/fixtures/` (GPU: `tests/fixtures/gpu/`).
- Register GPU tests in `tests/gpu/CMakeLists.txt` and UI/compiler tests in
  `tests/unit/CMakeLists.txt`.
- Golden updates are a separate, reviewed change; never update a baseline from a real GPU.
- If a change touches protected regions, the merger idempotency test must stay green.

## 7. PR checklist

- [ ] Builds with the relevant preset on Windows and/or Linux.
- [ ] `ctest` passes; new behavior has a deterministic test.
- [ ] `scripts/format.* --check` and `scripts/lint.*` pass for changed code.
- [ ] Code stays within the 300/500-line soft/hard limits.
- [ ] Docs are updated and indexed; links/fences are valid; no doc exceeds 600 lines.
- [ ] New dependency is recorded in `dependencies.md` and `THIRD_PARTY_NOTICES.md`.
- [ ] New source files have the MIT SPDX header.
- [ ] No generated artifacts, build output, secrets, or machine-specific paths.
- [ ] Visual changes include a screenshot/golden diff and a reproduction command.

## 8. Review

Expect review to focus on correctness, determinism, the RHI/UI/compiler boundaries, and whether
the docs/ADR match the code. Small, focused PRs with tests and docs are easier to review and land.
If a review reveals a new architectural decision, add or update an ADR rather than leaving the
decision implicit.
