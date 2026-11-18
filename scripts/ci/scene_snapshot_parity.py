#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native-versus-Wasm scene-snapshot parity gate (issue #166, docs/design/compiler-wasm.md section 7).

P2's acceptance is "the Wasm scene snapshot equals the native harness snapshot for the parity
samples". That is a claim about two artifacts, so it is checked the way P0's IR parity and P1's
diagnostics parity are checked: run both sides over the same inputs and require byte-identical
documents.

The two sides are deliberately not two implementations:

  native  `tessera_scene_preview --scene-json OUT --file FILE --salt SALT FILE.tsx`
  wasm    the exported `tessera_scene_json(component, fold, width, height, dpr)` over a staged
          project of one file, from
          `tessera_scene_preview.js`, the exact module the preview page loads

Both call the same in-memory driver (`tools/codegen/scene_preview.cpp`), which builds the scene from
the lowered IR with the same evaluator, resolves style and runs the same Yoga layout engine, and then
walks the tree with the same `ui::automation::snapshot_scene()` the headless harness uses and
serializes it with `snapshot_to_json()`. A difference therefore means the *pipeline* diverged (a
different stage stopping first, a different id salt, a slightly different layout) rather than the JSON
being assembled twice. The Wasm side is driven through Node against the real C export, so the gate
also proves the ABI the page uses.

The corpus must not be all-empty: a gate that passes when both sides returned `{"nodes":[]}` proves
nothing. Every case therefore declares the minimum node count it must produce, and a case below that
count on either side is a failure before any byte comparison.

Why `samples/compiler-demo/Demo.tsx` is still not the corpus: its conditional child now *is* evaluated
(issue #389), so the file prepares, but its `bind::sync` effect, its `useRef` and its `TextInput`
`onChange` payload are outside this slice, and the corpus is meant to be the set of shapes whose whole
tree the preview builds. `samples/gallery/Gallery.tsx` imports a user component, which is still
`TESSERA_SCENE_UNSUPPORTED` (docs/design/compiler-wasm.md section 7.1). The static corpus below is
joined by the two dynamic shapes #389 added -- a ternary's branches and a `map`'s items -- so the gate
covers what the preview now evaluates instead of only what it always did.

Exit codes: 0 parity holds, 1 a mismatch or a corpus expectation failed, 2 setup failure.
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

# The salt both sides hash the ids with. It has to be passed to *both*: the salt and the source path
# are inputs to every `uiId` (docs/specs/automation-ids-v1.md section 2), so a side that invented its
# own would produce a document full of different ids and the gate would be comparing nothing.
SALT = "preview"

# (name, project-relative file, source, minimum node count). The minimum is the anti-vacuity guard: a
# case that should lay out a tree but reports fewer nodes on either side fails the gate. The comments
# name what each case is here to cover; the gate asserts the count and byte equality, not the specific
# geometry, so a layout tweak that changes boxes on both sides does not fail it while a lost node does.
# Each entry is `(name, project-relative file, source, minimum node count)` and, since issue #390,
# optionally a fifth element: the author stylesheet. A case with one is driven through the STYLED
# project on both sides (`--stylesheet` natively; a staged `ui/Preview.css` in Wasm, applied because the
# source imports it), so the gate compares the same author CSS through the same engine on two machines -- which is the acceptance the
# issue asks for ("the parity corpus gains a styled case; native CLI and prepare_scene produce the same
# scene JSON for it"). A case without one keeps the pre-#390 four-tuple and the unstyled exports, so
# the existing bytes are pinned exactly as before.
CORPUS: Tuple[Tuple[object, ...], ...] = (
    # The parity-demo sample's static shape: a column with a text leaf and two buttons, the smallest
    # tree that still exercises three element kinds and a computed-style layout.
    (
        "counter",
        "ui/Counter.tsx",
        """export function Counter(): Node {
  return (
    <View>
      <Text>Count: 0</Text>
      <Button>Add</Button>
      <Button>Reset</Button>
    </View>
  );
}
""",
        5,
    ),
    # State + a template expression: the evaluator must substitute the initializer, so the leaf text
    # is "Count: 3" and not the source. A preview that skipped evaluation would still emit 5 nodes,
    # which is why the gate also compares every byte.
    (
        "state-template",
        "ui/State.tsx",
        """export function State(): Node {
  const [count, setCount] = useState<int>(3);
  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
""",
        4,
    ),
    # A float cell and `formatNumber`: the evaluator's float text path (`ir-v1` section 4.1), whose
    # bytes differ from the int path. `useAnimatedValue` has one accepted form, a plain
    # `const v = useAnimatedValue(init)` binding, so it is written that way rather than destructured.
    (
        "float-template",
        "ui/Ratio.tsx",
        """export function Ratio(): Node {
  const ratio = useAnimatedValue(0.25);
  return (
    <View>
      <Text>{formatNumber(ratio, 2)}</Text>
    </View>
  );
}
""",
        3,
    ),
    # Inline style literals: the layout engine must read the node's `style={{...}}`, so the boxes have
    # authored sizes rather than their intrinsic ones. The lengths are longhands because the schema
    # table is longhand-only (`padding` is not a supported property; `padding-top` is).
    (
        "inline-style",
        "ui/Styled.tsx",
        """export function Styled(): Node {
  return (
    <View style={{ paddingTop: 8, paddingLeft: 8, width: 200 }}>
      <Text style={{ height: 24 }}>Styled</Text>
      <Button style={{ width: 120, height: 32 }}>Go</Button>
    </View>
  );
}
""",
        4,
    ),
    # Element metadata: id, className and role are selector inputs and aria-label is the snapshot's
    # accessible name, so this case proves the metadata channels reach the document.
    (
        "metadata",
        "ui/Meta.tsx",
        """export function Meta(): Node {
  return (
    <View id="page" className="card">
      <Text aria-label="caption">Hello</Text>
      <Button role="button">Press</Button>
    </View>
  );
}
""",
        4,
    ),
    # A nested tree: three levels, so parent indices and structural paths are compared and not just a
    # flat list of boxes.
    (
        "nested",
        "ui/Nested.tsx",
        """export function Nested(): Node {
  return (
    <View>
      <View className="row">
        <View className="cell">
          <Text>Deep</Text>
        </View>
      </View>
      <Text>After</Text>
    </View>
  );
}
""",
        6,
    ),
    # The controls the preview styles: a text input and a scroll container, both with a fixed size in
    # the preview's default sheet, so the case proves those element kinds are built and laid out.
    (
        "controls",
        "ui/Controls.tsx",
        """export function Controls(): Node {
  return (
    <View>
      <TextInput />
      <Scroll>
        <Text>Inside</Text>
      </Scroll>
      <Spacer />
    </View>
  );
}
""",
        5,
    ),
    # An explicit automationId: it replaces the computed uiId (section 2), so this is the one case that
    # would catch a preview that reported its own ids instead of the compiler's.
    (
        "explicit-id",
        "ui/Pinned.tsx",
        """export function Pinned(): Node {
  return (
    <View>
      <Text automationId="424242">Pinned</Text>
    </View>
  );
}
""",
        3,
    ),
    # A UTF-8 text payload, so the emitted JSON escapes nothing the writer should escape and passes the
    # non-ASCII bytes through (the serializer's documented rule).
    (
        "utf8-text",
        "ui/Unicode.tsx",
        """export function Unicode(): Node {
  return (
    <View>
      <Text>\u00c9cran \u2014 pr\u00eat</Text>
    </View>
  );
}
""",
        3,
    ),
    # Must be rejected: a user component instance is outside this slice. Both sides have to agree on an
    # *empty* document, which is the one place where zero nodes is the expected answer and the minimum
    # is therefore 0.
    (
        "unsupported-component",
        "ui/Nested-Component.tsx",
        """function Leaf(): Node {
  return <Text>Leaf</Text>;
}

export function Host(): Node {
  return (
    <View>
      <Leaf />
    </View>
  );
}
""",
        0,
    ),
    # Supported since issue #389: a conditional child. `flag` is true, so the `&&` branch is the shown
    # one and the tree is a View + the Text, not an empty document. The minimum is therefore a real
    # count: an empty result here would mean the branch stopped being built, which is the regression
    # this case now guards.
    (
        "conditional",
        "ui/Conditional.tsx",
        """export function Conditional(): Node {
  const [flag, setFlag] = useState<bool>(true);
  return (
    <View>
      <Text>Base</Text>
      {flag && <Text>Yes</Text>}
    </View>
  );
}
""",
        4,
    ),
    # Supported since issue #389: both branches of a ternary. The test above covers one `&&` branch; this
    # covers the complementary pair lowering emits for `? :`, where exactly one of two sibling nodes is
    # painted. A preview that painted neither would still report 4 nodes, which is why the byte
    # comparison -- not this count -- is what decides the case.
    (
        "conditional-both-branches",
        "ui/Ternary.tsx",
        """export function Ternary(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <Text>Count: {count}</Text>
      {count > 0 ? <Text>Positive</Text> : <Text>Zero</Text>}
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
""",
        5,
    ),
    # Supported since issue #389: a repeated child. Three items over a `list<string>` cell, each item a
    # `Text`, so the minimum counts the container, the three items and no fewer.
    (
        "repeated",
        "ui/Repeated.tsx",
        """export function Repeated(): Node {
  const [rows, setRows] = useState<string[]>(["alpha", "beta", "gamma"]);
  return (
    <View>
      {rows.map((row, i) => <Text key={i}>{row}</Text>)}
    </View>
  );
}
""",
        5,
    ),
    # Supported since issue #389: an empty list. The template contributes no item and the sibling after
    # it still builds, so an empty list is a 2-node tree rather than a failed build. This is the case a
    # "zero nodes means unsupported" gate would misread, which is why the two are separate entries.
    (
        "repeated-empty",
        "ui/RepeatedEmpty.tsx",
        """export function RepeatedEmpty(): Node {
  const [rows, setRows] = useState<string[]>([]);
  return (
    <View>
      {rows.map((row, i) => <Text key={i}>{row}</Text>)}
      <Text>After</Text>
    </View>
  );
}
""",
        3,
    ),
    # The author stylesheet (issue #390): `:root` tokens through `var()`, a class rule, and an inline
    # `style` that must win over it. Five elements of the tuple, so the runner drives it through the
    # styled exports; the minimum node count is 4 because the author's sheet does NOT size anything the
    # built-in sheet used to (that is the point: the built-in sheet is absent, not layered under).
    (
        "author-stylesheet",
        "ui/Themed.tsx",
        """import "./Preview.css";

export function Themed(): Node {
  return (
    <View className="page">
      <Text className="title">Themed</Text>
      <Button style={{ width: 90, height: 32 }}>Go</Button>
    </View>
  );
}
""",
        4,
        """:root { --brand: rgb(0, 128, 255); --gap: 6px; }
.page { display: flex; flex-direction: column; padding-top: 4px; background-color: rgb(11, 18, 32); }
.title { height: 24px; color: rgb(230, 237, 247); margin-bottom: var(--gap); }
Button { background-color: var(--brand); width: 40px; height: 20px; }
""",
    ),

)

# The Node driver for the Wasm side. It imports the module factory, calls the real C export with the
# request on argv, and prints the document. Inline so the gate has no extra file to keep in step with
# the ABI (the same shape as scripts/ci/wasm_diagnostics_parity.py).
NODE_DRIVER = r"""
import { pathToFileURL } from 'node:url';
const path = process.argv[2];
const file = process.argv[3];
const component = process.argv[4];
const source = process.argv[5];
// Issue #390: the author stylesheet, when the case carries one. Empty means "no author CSS", which
// keeps the pre-#390 export and the pre-#390 bytes.
const stylesheet = process.argv[6] ?? '';
// `import()` needs a URL, not a bare path: on Windows a `C:\...` argument parses as a `c:` scheme and
// Node rejects it with ERR_UNSUPPORTED_ESM_URL_SCHEME. `pathToFileURL` is correct on both platforms.
const imported = await import(pathToFileURL(path).href);
const factory = imported.default;
const module = await factory();
// The module previews a staged project (`tessera_project_*`): the one case file is a project of one
// whose entry is that file, plus the author stylesheet as `ui/Preview.css` when the case carries one.
// The stylesheet applies only because the case's source imports it, exactly as in a native build.
const withString = (text, use) => {
  const length = module.lengthBytesUTF8(text) + 1;
  const ptr = module._malloc(length);
  module.stringToUTF8(text, ptr, length);
  try { return use(ptr); } finally { module._free(ptr); }
};
const stage = (call, first, second) =>
  withString(first, (firstPtr) => second === undefined
    ? call(firstPtr)
    : withString(second, (secondPtr) => call(firstPtr, secondPtr)));
module._tessera_project_begin();
stage(module._tessera_project_add_source, file, source);
if (stylesheet !== '') {
  stage(module._tessera_project_add_stylesheet, 'ui/Preview.css', stylesheet);
}
stage(module._tessera_project_set_entry, file);
let out;
// Default viewport and DPR (a non-positive size falls back to 320x240 at 1) and no fold posture.
const resultPtr = withString(component, (componentPtr) =>
  module._tessera_scene_json(componentPtr, 0, 0, 0, 0));
if (!resultPtr) { throw new Error("the scene export returned null"); }
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


def native_document(
    cli: str, name: str, file: str, source: str, work_root: Path, stylesheet: str = ""
) -> str:
    """Evaluates the case with the host CLI and returns its document.

    The CLI runs with its working directory set to the case directory and is given the *relative*
    path (`ui/Counter.tsx`), exactly the synthetic name the Wasm side passes. The salt and the file
    name are the inputs to every `uiId`, so both sides pass the same two values; the document path is
    absolute so it does not land inside the case directory.
    """
    case_dir = work_root / "native" / name
    target = case_dir / file
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source, encoding="utf-8")
    document = case_dir / "scene.json"
    argv = [cli, "--scene-json", str(document), "--file", file, "--salt", SALT]
    if stylesheet:
        # The author stylesheet travels the same way the page sends it (issue #390): a file the CLI
        # reads and files as `ui/Preview.css`, so an `import "./Preview.css";` in the source would
        # resolve to it too.
        sheet_path = case_dir / "Preview.css"
        sheet_path.write_text(stylesheet, encoding="utf-8")
        argv += ["--stylesheet", str(sheet_path)]
    argv.append(file)
    result = run(argv, case_dir)
    # Exit 1 is expected for the unsupported cases (a valid document with no nodes); above it is a
    # setup failure.
    if result.returncode > 1:
        raise SetupError(
            "{} --scene-json failed (exit {}): {}".format(
                name, result.returncode, (result.stderr or result.stdout).strip()
            )
        )
    if not document.is_file():
        raise SetupError("{}: the native run wrote no document".format(name))
    return document.read_text(encoding="utf-8")


def wasm_document(
    node: str,
    module_js: Path,
    name: str,
    file: str,
    source: str,
    work_root: Path,
    repo_root: Path,
    stylesheet: str = "",
) -> str:
    """Runs the real C export through Node and returns the document it produced."""
    driver = work_root / "wasm" / name
    driver.mkdir(parents=True, exist_ok=True)
    driver_file = driver / "drive.mjs"
    driver_file.write_text(NODE_DRIVER, encoding="utf-8")
    result = run(
        [node, str(driver_file), str(module_js), file, "", source, stylesheet], repo_root
    )
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


def nodes_of(text: str, label: str) -> List[Dict]:
    """Decodes a document and returns its nodes, failing loudly on a malformed document."""
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SetupError("{}: the document is not JSON: {}".format(label, exc)) from exc
    if not isinstance(document, dict):
        raise SetupError("{}: the document is not a JSON object".format(label))
    if document.get("snapshotVersion") != 1:
        raise SetupError(
            "{}: snapshotVersion is {!r}, expected 1".format(label, document.get("snapshotVersion"))
        )
    nodes = document.get("nodes")
    if not isinstance(nodes, list):
        raise SetupError("{}: `nodes` is not an array".format(label))
    return nodes


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="host-built tessera_scene_preview CLI")
    parser.add_argument("--wasm", required=True, help="Emscripten build: tessera_scene_preview.js")
    parser.add_argument("--node", required=True, help="Node executable ($EMSDK_NODE)")
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument(
        "--work-dir",
        default=None,
        help="scratch directory (default: <repo-root>/build/scene-preview, which is gitignored)",
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

    # Resolve to absolute paths before anything changes directory. Both sides are executed with cwd
    # set to a scratch case directory (see native_document), so a path relative to the caller's cwd --
    # the natural way to write `--native build/.../tessera_scene_preview` on a command line -- would
    # not resolve there. P1 shipped exactly that bug and it failed on CI.
    native_cli = Path(args.native).resolve()
    module_js = module_js.resolve()
    node = Path(args.node).resolve()

    work_root = Path(args.work_dir) if args.work_dir else repo_root / "build" / "scene-preview"
    # Under the gitignored build/ tree rather than a temp directory: the run stays inspectable beside
    # other build output, and a confined sandbox needs no second write root.
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
        # Each entry is a four-tuple, or a five-tuple whose last element is the author stylesheet
        # (issue #390). Both sides then drive the styled export, so the same author CSS is compiled by
        # the same engine on two machines -- and a case with no stylesheet keeps the four-tuple and the
        # pre-#390 bytes.
        for case in CORPUS:
            name, file, source, minimum = case[0], case[1], case[2], case[3]
            stylesheet = case[4] if len(case) > 4 else ""
            native_text = native_document(
                str(native_cli), name, file, source, work_root, stylesheet
            )
            wasm_text = wasm_document(
                str(node), module_js, name, file, source, work_root, repo_root, stylesheet
            )

            native_nodes = nodes_of(native_text, "native/" + name)
            wasm_nodes = nodes_of(wasm_text, "wasm/" + name)

            # Anti-vacuity: both sides must produce the nodes the case is about before any byte
            # comparison, so "both empty" can never pass.
            if len(native_nodes) < minimum:
                failures.append(
                    "CORPUS native/{:<22} {} node(s), expected at least {}".format(
                        name, len(native_nodes), minimum
                    )
                )
                continue
            if len(wasm_nodes) < minimum:
                failures.append(
                    "CORPUS wasm/{:<24} {} node(s), expected at least {}".format(
                        name, len(wasm_nodes), minimum
                    )
                )
                continue

            if native_text == wasm_text:
                print(
                    "BYTE_IDENTICAL {:<22} {} node(s){}".format(
                        name, len(native_nodes), " [styled]" if stylesheet else ""
                    )
                )
                continue
            failures.extend(describe_difference(name, native_text, wasm_text))

        if failures:
            print("\n".join(failures))
            print("FAILED: native and Wasm scene snapshots differ")
            return 1
        print("PARITY OK: native and Wasm scene snapshots are byte-identical over the corpus")
        return 0
    except SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
