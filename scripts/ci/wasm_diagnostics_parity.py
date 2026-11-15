#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native-versus-Wasm diagnostics parity gate (issue #165, docs/design/compiler-wasm.md section 6).

P1's acceptance is "the page shows diagnostics that match the native compiler's for the same input".
That is a claim about two artifacts, so it is checked the way P0's IR parity is checked: run both
sides over the same inputs and require byte-identical documents.

The two sides are deliberately not two implementations:

  native  `tessera_codegen --diagnostics-json OUT FILE.tsx`
  wasm    the exported `tessera_diagnostics_json(source, file)` of `tessera_diagnostics.js`,
          the exact module the preview page loads

Both reach the same `write_diagnostics_json()`, so a difference means the *pipeline* diverged (a
different stage stopping first, a different module name, a span computed differently) rather than the
JSON being assembled twice. The Wasm side is driven through Node against the real C export, so the
gate also proves the ABI the page uses.

The corpus must not be all-valid: a gate that passes when both sides report nothing proves nothing.
Every case therefore declares how many diagnostics it must produce at minimum, and a case whose two
sides are both empty when a count was declared is a failure before any byte comparison.

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

# (name, project-relative file, source, minimum diagnostics). The minimum is the anti-vacuity guard:
# a case that is supposed to diagnose something but reports nothing on both sides fails the gate.
# The codes named in the comment are the class each case is here to cover; the gate asserts the count
# and byte equality, not the specific code, so a message reword does not fail it (the diagnostic
# registry golden owns wording) while a lost diagnostic does.
CORPUS: Tuple[Tuple[str, str, str, int], ...] = (
    # A clean file: the empty-document path, which must still be a real document on both sides.
    (
        "clean",
        "ui/Clear.tsx",
        "export function Clear() {\n  return <View />;\n}\n",
        0,
    ),
    # A JSX lexical error (TESSERA_JSX_LEX_*): invalid character in JSX text.
    (
        "jsx-lex",
        "ui/Lex.tsx",
        "export function Lex() {\n  return <View>}</View>;\n}\n",
        1,
    ),
    # A JSX structural error (TESSERA_JSX_PARSE_*): a closing tag that does not match.
    (
        "jsx-parse",
        "ui/Parse.tsx",
        "export function Parse() {\n  return <View></Text>;\n}\n",
        1,
    ),
    # A lowering error (TESSERA_IR_*): a tag that is neither an intrinsic nor a declared component.
    (
        "lowering",
        "ui/Tag.tsx",
        "export function Tag() {\n  return <NotAThing />;\n}\n",
        1,
    ),
    # A rules error (TESSERA_REACT_*): a hook called in an accepted-looking but rejected form.
    (
        "react-rules",
        "ui/Hook.tsx",
        "export function Hook() {\n  const v = useAnimatedValue(0, 12);\n  return <View />;\n}\n",
        1,
    ),
    # A multi-line file whose span is not on line 1: proves the line/column, not just the offset.
    (
        "span-not-first-line",
        "ui/Deep.tsx",
        "// a leading comment\n// and a second one\nexport function Deep() {\n  return <Nope />;\n}\n",
        1,
    ),
    # A UTF-8 source with a non-ASCII comment before the error: the span's byte offset must stay a
    # byte offset while the column counts code points (jsx-subset-v1.md section 13.2).
    (
        "utf8-offset",
        "ui/Utf8.tsx",
        "// caf\u00e9 na\u00efve \u00fcber\nexport function Utf8() {\n  return <Nope />;\n}\n",
        1,
    ),
)

# The Node driver for the Wasm side. It imports the module factory, calls the real C export with the
# request on argv, and prints the document. It is passed as an inline module string so the gate has no
# extra file to keep in step with the ABI.
NODE_DRIVER = r"""
const path = process.argv[2];
const file = process.argv[3];
const source = process.argv[4];
const imported = await import(path);
const factory = imported.default;
const module = await factory();
const textLength = module.lengthBytesUTF8(source) + 1;
const textPtr = module._malloc(textLength);
module.stringToUTF8(source, textPtr, textLength);
const nameLength = module.lengthBytesUTF8(file) + 1;
const namePtr = module._malloc(nameLength);
module.stringToUTF8(file, namePtr, nameLength);
let out;
try {
  const resultPtr = module._tessera_diagnostics_json(textPtr, namePtr);
  if (!resultPtr) { throw new Error("tessera_diagnostics_json returned null"); }
  try {
    out = module.UTF8ToString(resultPtr);
  } finally {
    module._tessera_free(resultPtr);
  }
} finally {
  module._free(textPtr);
  module._free(namePtr);
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
    cli: str, name: str, file: str, source: str, work_root: Path, repo_root: Path
) -> str:
    """Writes the case to a real file and compiles it with the host CLI.

    The CLI runs with its working directory set to the case directory and is given the *relative*
    path (`ui/Clear.tsx`), which is exactly the synthetic name the Wasm side passes. Both documents
    therefore carry the same `file` string in every span, so the comparison is about the diagnostic
    and never about where a scratch directory happened to be. The document path is absolute so it
    does not land inside the case directory the CLI is scanning.
    """
    case_dir = work_root / "native" / name
    target = case_dir / file
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source, encoding="utf-8")
    document = case_dir / "diagnostics.json"
    result = run(
        [cli, "--diagnostics-json", str(document), file], case_dir
    )
    # Exit 1 is expected for a case that diagnoses something; anything above it is a setup failure.
    if result.returncode > 1:
        raise SetupError(
            "{} --diagnostics-json failed (exit {}): {}".format(
                name, result.returncode, (result.stderr or result.stdout).strip()
            )
        )
    if not document.is_file():
        raise SetupError("{}: the native run wrote no document".format(name))
    return document.read_text(encoding="utf-8")


def wasm_document(
    node: str, module_js: Path, name: str, file: str, source: str, work_root: Path, repo_root: Path
) -> str:
    """Runs the real C export through Node and returns the document it produced."""
    driver = work_root / "wasm" / name
    driver.mkdir(parents=True, exist_ok=True)
    driver_file = driver / "drive.mjs"
    driver_file.write_text(NODE_DRIVER, encoding="utf-8")
    result = run(
        [node, str(driver_file), str(module_js), file, source], repo_root
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


def diagnostics_of(text: str, label: str) -> List[Dict]:
    """Decodes a document and returns its diagnostics, failing loudly on a malformed document."""
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SetupError("{}: the document is not JSON: {}".format(label, exc)) from exc
    if not isinstance(document, dict):
        raise SetupError("{}: the document is not a JSON object".format(label))
    if document.get("diagnosticsVersion") != 1:
        raise SetupError(
            "{}: diagnosticsVersion is {!r}, expected 1".format(
                label, document.get("diagnosticsVersion")
            )
        )
    diagnostics = document.get("diagnostics")
    if not isinstance(diagnostics, list):
        raise SetupError("{}: `diagnostics` is not an array".format(label))
    return diagnostics


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="host-built tessera_codegen CLI")
    parser.add_argument("--wasm", required=True, help="Emscripten build: tessera_diagnostics.js")
    parser.add_argument("--node", required=True, help="Node executable ($EMSDK_NODE)")
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument(
        "--work-dir",
        default=None,
        help="scratch directory (default: <repo-root>/build/wasm-diagnostics, which is gitignored)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    module_js = Path(args.wasm)

    for label, path in (("--native", Path(args.native)), ("--wasm", module_js), ("--node", Path(args.node))):
        if not path.is_file():
            print("SETUP ERROR: {} not found: {}".format(label, path), file=sys.stderr)
            return 2

    # Resolve to absolute paths before anything changes directory. The native CLI and the module are
    # executed with cwd set to a scratch case directory (see native_document), so a path relative to
    # the caller's cwd -- the natural way to write `--native build/.../tessera_codegen` on a command
    # line -- would not resolve there.
    native_cli = Path(args.native).resolve()
    module_js = module_js.resolve()
    node = Path(args.node).resolve()

    work_root = Path(args.work_dir) if args.work_dir else repo_root / "build" / "wasm-diagnostics"
    # Deliberately under the gitignored build/ tree rather than a temp directory: the run stays
    # inspectable beside other build output, and a confined sandbox needs no second write root.
    if work_root.exists():
        shutil.rmtree(work_root)
    work_root.mkdir(parents=True)

    print("native cli: {}".format(args.native))
    print("wasm module: {}".format(module_js))
    print("node:       {}".format(args.node))
    print("corpus:     {} case(s)".format(len(CORPUS)))
    print("work dir:   {}".format(work_root))

    failures: List[str] = []
    try:
        for name, file, source, minimum in CORPUS:
            native_text = native_document(str(native_cli), name, file, source, work_root, repo_root)
            wasm_text = wasm_document(
                str(node), module_js, name, file, source, work_root, repo_root
            )

            native_diagnostics = diagnostics_of(native_text, "native/" + name)
            wasm_diagnostics = diagnostics_of(wasm_text, "wasm/" + name)

            # Anti-vacuity: a case that declares a minimum must produce it on both sides before any
            # byte comparison, so "both sides silently found nothing" fails instead of passing.
            for side, diagnostics in (("native", native_diagnostics), ("wasm", wasm_diagnostics)):
                if len(diagnostics) < minimum:
                    failures.append(
                        "CORPUS {}: {} reported {} diagnostic(s), the case requires at least {}".format(
                            name, side, len(diagnostics), minimum
                        )
                    )

            if native_text == wasm_text:
                print(
                    "BYTE_IDENTICAL {:<22} {} diagnostic(s)".format(
                        name, len(native_diagnostics)
                    )
                )
                continue
            failures.extend(describe_difference(name, native_text, wasm_text))

        if failures:
            print("\n".join(failures))
            print("FAILED: native and Wasm diagnostics differ")
            return 1
        print("PARITY OK: native and Wasm diagnostics are byte-identical over the corpus")
        return 0
    except SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
