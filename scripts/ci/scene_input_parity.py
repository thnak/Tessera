#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Input parity gate for the scene preview (issue #168, P4 of reference-equantic-ui.md section 7.4).

The P2 gate (`scene_snapshot_parity.py`) proves the native and Wasm builds draw the same tree. This
one proves they *react* the same: a scripted click sequence is dispatched into the in-memory evaluator
on both sides, and the state cells after every step must be byte-identical.

Both sides run the same C++ (`tools/codegen/scene_session.cpp`): the native side is
`tessera_scene_preview --input-json --script SCRIPT file.tsx`, the Wasm side is the module's
`tessera_input_json` export under `$EMSDK_NODE`. The comparison is over the whole document
byte-for-byte, because the document *is* the observable result: the state cells, the handler ids that
ran, whether the click found its node, and the per-step `found`/`dispatched` flags.

The corpus is anti-vacuous in the way that matters for input: every case declares a minimum number of
state cells and a minimum number of handlers that must actually run. A run where the click hits
nothing, or where the handler is reported as `unrun`, therefore fails even if both sides agree --
"both did nothing" is the failure mode a byte comparison alone cannot see.

Exit codes: 0 parity holds, 1 a case differs or is vacuous, 2 setup failure.
"""

from __future__ import annotations

import argparse
import difflib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]

# The project salt, shared with the P2 gate. It feeds every `uiId`, so the native and Wasm sides must
# pass the same value or the ids in the two documents would differ for reasons unrelated to input.
SALT = "preview"

# (name, file, source, script, minimum state cells, minimum handlers that must run).
#
# The scripts name automation ids explicitly, so each case is a statement about *which* node a click
# reaches, not a "click something and see" test. `automationId` on the element pins the id, which is
# what makes these cases readable and stable across a structural edit of the source above them.
CORPUS: Tuple[Tuple[str, str, str, str, int, int], ...] = (
    # A button's onClick increments its own state cell: the base case for "a click runs the preview
    # body and the cell moves".
    (
        "click-increments",
        "ui/Counter.tsx",
        """export function Counter(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Count: {count}</Text>
      <Button automationId="101" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
""",
        "click 101\nclick 101",
        1,
        1,
    ),
    # Two handlers, both of which must run: a click reaches one node, and the case proves the
    # document reports the handler that ran rather than a single anonymous "something happened".
    (
        "two-handlers",
        "ui/Pair.tsx",
        """export function Pair(): Node {
  const [count, setCount] = useState<int>(0);
  const [label, setLabel] = useState<string>("start");
  return (
    <View>
      <Text>{label}</Text>
      <Button automationId="201" onClick={() => { setCount(count + 2); setLabel("added"); }}>Add</Button>
    </View>
  );
}
""",
        "click 201\nclick 201\nclick 201",
        2,
        1,
    ),
    # A handler that depends on the previous step's value, so the cells are compared as a *sequence*
    # and not just at the end: a preview that dropped intermediate state still ends at 3 here, but a
    # preview that restarted from the initializer does not reach it.
    (
        "state-threading",
        "ui/Thread.tsx",
        """export function Thread(): Node {
  const [step, setStep] = useState<int>(0);
  const [total, setTotal] = useState<int>(0);
  return (
    <View>
      <Text>{formatNumber(total, 0)}</Text>
      <Button automationId="301" onClick={() => { setStep(step + 1); setTotal(total + step + 1); }}>Next</Button>
    </View>
  );
}
""",
        "click 301\nclick 301\nclick 301",
        2,
        1,
    ),
    # A click on a node that has no onClick: it must dispatch (a real target was hit) and run nothing.
    # This is the "a click is a gesture, not a handler lookup" case: `found` is true, `dispatched` is
    # true, `ran` is empty, and the cells do not move.
    (
        "click-without-handler",
        "ui/Miss.tsx",
        """export function Miss(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text automationId="401">Label</Text>
      <Button automationId="402" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
""",
        "click 401",
        1,
        0,
    ),
    # A handler on a node with no automation id anywhere in the tree still runs when an ancestor's
    # click bubbles to it: the scene's own bubbling decides, not the IR walk.
    (
        "bubbles-to-parent",
        "ui/Nest.tsx",
        """export function Nest(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View automationId="501" onClick={() => setCount(count + 1)}>
      <Text automationId="502">Inside</Text>
    </View>
  );
}
""",
        "click 502",
        1,
        1,
    ),
    # Empty script: the document must still carry the initial cells, so a caller has a baseline and a
    # corpus case with no steps cannot pass by producing nothing.
    (
        "initial-only",
        "ui/Idle.tsx",
        """export function Idle(): Node {
  const [count, setCount] = useState<int>(5);
  return (
    <View>
      <Text>{count}</Text>
      <Button automationId="601" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
""",
        "",
        1,
        0,
    ),
)

# The Node driver for the Wasm side: the same shape as the P2 gate's, with the script as a fifth
# argument. Inline so the gate has no extra file to keep in step with the ABI.
NODE_DRIVER = r"""
import { pathToFileURL } from 'node:url';
const path = process.argv[2];
const file = process.argv[3];
const component = process.argv[4];
const source = process.argv[5];
const script = process.argv[6];
// `import()` needs a URL, not a bare path: on Windows a `C:\...` argument parses as a `c:` scheme and
// Node rejects it with ERR_UNSUPPORTED_ESM_URL_SCHEME. `pathToFileURL` is correct on both platforms.
const imported = await import(pathToFileURL(path).href);
const factory = imported.default;
const module = await factory();
const write = (text) => {
  const length = module.lengthBytesUTF8(text) + 1;
  const pointer = module._malloc(length);
  module.stringToUTF8(text, pointer, length);
  return pointer;
};
// The module previews a staged project (`tessera_project_*`): the one case file is a project of one
// whose entry is that file, as the page stages a single-file project.
const withPointer = (text, use) => {
  const pointer = write(text);
  try { return use(pointer); } finally { module._free(pointer); }
};
module._tessera_project_begin();
withPointer(file, (filePtr) => withPointer(source, (sourcePtr) =>
  module._tessera_project_add_source(filePtr, sourcePtr)));
withPointer(file, (filePtr) => module._tessera_project_set_entry(filePtr));
const resultPtr = withPointer(component, (componentPtr) =>
  withPointer(script, (scriptPtr) => module._tessera_input_json(componentPtr, scriptPtr)));
if (!resultPtr) { throw new Error("tessera_input_json returned null"); }
let out;
try {
  out = module.UTF8ToString(resultPtr);
} finally {
  module._tessera_free(resultPtr);
}
process.stdout.write(out);
"""


class SetupError(Exception):
    """A CLI or the module could not be executed, or produced no document."""


def run(command: Sequence[str], cwd: Path) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            list(command),
            cwd=str(cwd),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        raise SetupError("cannot execute {}: {}".format(command[0], exc)) from exc


def write_case(name: str, file: str, source: str, script: str, work_root: Path) -> Tuple[Path, Path]:
    case_dir = work_root / "native" / name
    target = case_dir / file
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source, encoding="utf-8")
    script_path = case_dir / "steps.txt"
    script_path.write_text(script, encoding="utf-8")
    return case_dir, script_path


def native_document(
    cli: str, name: str, file: str, source: str, script: str, work_root: Path
) -> str:
    """Runs the script natively and returns the document.

    The CLI runs with cwd set to the case directory and is given the *relative* path, exactly the
    synthetic name the Wasm side passes: the source path is an input to every `uiId`.
    """
    case_dir, script_path = write_case(name, file, source, script, work_root)
    document = case_dir / "input.json"
    result = run(
        [
            cli,
            "--input-json",
            str(document),
            "--script",
            str(script_path),
            "--file",
            file,
            "--salt",
            SALT,
            file,
        ],
        case_dir,
    )
    # Exit 1 is expected for a source that produced no scene (a valid document with an error); above
    # it is a setup failure.
    if result.returncode > 1:
        raise SetupError(
            "{} --input-json failed (exit {}): {}".format(
                name, result.returncode, (result.stderr or result.stdout).strip()
            )
        )
    if not document.is_file():
        raise SetupError("{}: the native run wrote no document".format(name))
    return document.read_text(encoding="utf-8")


def wasm_document(
    node: str, module_js: Path, name: str, file: str, source: str, script: str, work_root: Path,
    repo_root: Path
) -> str:
    """Runs the real C export through Node and returns the document it produced."""
    driver = work_root / "wasm" / name
    driver.mkdir(parents=True, exist_ok=True)
    driver_file = driver / "drive.mjs"
    driver_file.write_text(NODE_DRIVER, encoding="utf-8")
    result = run([node, str(driver_file), str(module_js), file, "", source, script], repo_root)
    if result.returncode != 0:
        raise SetupError(
            "{}: the Wasm module failed under Node (exit {}): {}".format(
                name, result.returncode, (result.stderr or result.stdout).strip()
            )
        )
    return result.stdout


def describe_difference(label: str, native_text: str, wasm_text: str, limit: int = 40) -> List[str]:
    diff = list(
        difflib.unified_diff(
            native_text.splitlines(keepends=True),
            wasm_text.splitlines(keepends=True),
            fromfile="native/" + label,
            tofile="wasm/" + label,
            n=2,
        )
    )
    lines = ["MISMATCH {}".format(label)]
    if diff:
        lines.append("  unified diff (native -> wasm):")
        lines.extend("    " + line.rstrip("\n") for line in diff[:limit])
        if len(diff) > limit:
            lines.append("    ... {} more diff line(s)".format(len(diff) - limit))
    else:
        lines.append("  the texts differ but produced no diff (line endings?)")
    return lines


def inspect(text: str, label: str) -> Dict:
    """Decodes the document and checks the envelope before anything is compared."""
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SetupError("{}: the document is not JSON: {}".format(label, exc)) from exc
    if not isinstance(document, dict):
        raise SetupError("{}: the document is not a JSON object".format(label))
    if document.get("inputVersion") != 1:
        raise SetupError(
            "{}: inputVersion is {!r}, expected 1".format(label, document.get("inputVersion"))
        )
    steps = document.get("steps")
    if not isinstance(steps, list):
        raise SetupError("{}: `steps` is not an array".format(label))
    initial = document.get("initial")
    if not isinstance(initial, list):
        raise SetupError("{}: `initial` is not an array".format(label))
    return document


def anti_vacuous(document: Dict, label: str, minimum_cells: int, minimum_ran: int) -> List[str]:
    """The checks a byte comparison cannot make: something must actually have happened."""
    failures: List[str] = []
    cells = len(document.get("initial", []))
    if cells < minimum_cells:
        failures.append(
            "CORPUS {:<22} {} initial cell(s), expected at least {}".format(
                label, cells, minimum_cells
            )
        )
    ran = 0
    for step in document.get("steps", []):
        if not isinstance(step, dict):
            failures.append("CORPUS {:<22} a step is not an object".format(label))
            continue
        ran += len(step.get("ran") or [])
        unrun = step.get("unrun") or []
        if unrun:
            failures.append(
                "CORPUS {:<22} step {} reached a handler it could not run: {}".format(
                    label, step.get("step"), ", ".join(str(item) for item in unrun)
                )
            )
    if ran < minimum_ran:
        failures.append(
            "CORPUS {:<22} {} handler(s) ran, expected at least {}".format(
                label, ran, minimum_ran
            )
        )
    return failures


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="host-built tessera_scene_preview CLI")
    parser.add_argument("--wasm", required=True, help="Emscripten build: tessera_scene_preview.js")
    parser.add_argument("--node", required=True, help="Node executable ($EMSDK_NODE)")
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument(
        "--work-dir",
        default=None,
        help="scratch directory (default: <repo-root>/build/scene-input, which is gitignored)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    module_js = Path(args.wasm)

    for label, path in (
        ("--native", Path(args.native)),
        ("--wasm", module_js),
        ("--node", Path(args.node)),
    ):
        if not path.is_file():
            print("SETUP ERROR: {} not found: {}".format(label, path), file=sys.stderr)
            return 2

    # Absolute before anything changes directory: both sides run with cwd set to a scratch case
    # directory, so a path relative to the caller's cwd would not resolve there. P1 shipped exactly
    # that bug and it failed on CI.
    native_cli = Path(args.native).resolve()
    module_js = module_js.resolve()
    node = Path(args.node).resolve()

    work_root = Path(args.work_dir) if args.work_dir else repo_root / "build" / "scene-input"
    if work_root.exists():
        shutil.rmtree(work_root)
    work_root.mkdir(parents=True)

    print("native cli: {}".format(native_cli))
    print("wasm module: {}".format(module_js))
    print("node:       {}".format(node))
    print("salt:       {}".format(SALT))
    print("corpus:     {} case(s)".format(len(CORPUS)))
    print("work dir:   {}".format(work_root))

    failures: List[str] = []
    try:
        for name, file, source, script, minimum_cells, minimum_ran in CORPUS:
            native_text = native_document(
                str(native_cli), name, file, source, script, work_root
            )
            wasm_text = wasm_document(
                str(node), module_js, name, file, source, script, work_root, repo_root
            )

            native_doc = inspect(native_text, "native/" + name)
            wasm_doc = inspect(wasm_text, "wasm/" + name)

            # Anti-vacuity before any byte comparison, on both sides: "both did nothing" must not
            # pass. A case whose clicks all missed would be byte-identical and worthless.
            native_failures = anti_vacuous(
                native_doc, "native/" + name, minimum_cells, minimum_ran
            )
            wasm_failures = anti_vacuous(wasm_doc, "wasm/" + name, minimum_cells, minimum_ran)
            if native_failures or wasm_failures:
                failures.extend(native_failures)
                failures.extend(wasm_failures)
                continue

            if native_text == wasm_text:
                print(
                    "BYTE_IDENTICAL {:<22} {} step(s), {} cell(s)".format(
                        name, len(native_doc["steps"]), len(native_doc["initial"])
                    )
                )
                continue
            failures.extend(describe_difference(name, native_text, wasm_text))

        if failures:
            print("\n".join(failures))
            print("FAILED: native and Wasm input results differ")
            return 1
        print("PARITY OK: native and Wasm input results are byte-identical over the corpus")
        return 0
    except SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
