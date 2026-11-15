#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native-only corpus check for the scene-input parity gate (issue #168).

`scene_input_parity.py` needs an Emscripten module, which cannot be built on every machine (the
development box has no `EMSDK`). This script exercises the half that *can* run anywhere: it runs the
same corpus through `tessera_scene_preview --input-json` and asserts the parts of the gate that do not
need the Wasm side --

  * every case produces a document of the right version that parses,
  * a case's `initial` array holds at least its minimum number of state cells (the anti-vacuity
    rule),
  * at least the case's minimum number of handlers actually ran, and no step reported a handler it
    could not run (`unrun` is empty),
  * the state cells after every step are deterministic (two runs are byte-identical),
  * the document's own claim about a click matches the corpus: `found`/`dispatched` are true exactly
    for the cases whose scripts name a node that exists.

It shares the corpus *only* by importing the gate, so the two can never drift apart.

Exit codes: 0 the corpus holds natively, 1 a case failed, 2 setup failure.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import List, Optional, Sequence

# Imported rather than copied: the corpus and the salt must be one definition, or this check would
# pass while the real gate compared something else.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene_input_parity as gate  # noqa: E402


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="host-built tessera_scene_preview CLI")
    parser.add_argument("--repo-root", default=str(gate.REPO_ROOT))
    parser.add_argument(
        "--work-dir",
        default=None,
        help="scratch directory (default: <repo-root>/build/scene-input-native)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    repo_root = Path(args.repo_root).resolve()
    native = Path(args.native)
    if not native.is_file():
        print("SETUP ERROR: --native not found: {}".format(native), file=sys.stderr)
        return 2
    native_cli = native.resolve()

    work_root = (
        Path(args.work_dir) if args.work_dir else repo_root / "build" / "scene-input-native"
    )
    if work_root.exists():
        shutil.rmtree(work_root)
    work_root.mkdir(parents=True)

    print("native cli: {}".format(native_cli))
    print("salt:       {}".format(gate.SALT))
    print("corpus:     {} case(s)".format(len(gate.CORPUS)))
    print("work dir:   {}".format(work_root))

    failures: List[str] = []
    try:
        for name, file, source, script, minimum_cells, minimum_ran in gate.CORPUS:
            try:
                first = gate.native_document(str(native_cli), name, file, source, script, work_root)
                # Determinism: the same case run twice is byte-identical. The preview owns no clock
                # and no counter, so a difference here is a real non-determinism bug (an
                # uninitialised read, a hash iteration order, a time value).
                second = gate.native_document(
                    str(native_cli), name + "-again", file, source, script, work_root
                )
            except gate.SetupError as exc:
                failures.append("SETUP {}: {}".format(name, exc))
                continue

            if first != second:
                failures.append(
                    "CORPUS {:<22} the same case produced two different documents".format(name)
                )
                continue

            try:
                document = gate.inspect(first, name)
            except gate.SetupError as exc:
                failures.append("CORPUS {}: {}".format(name, exc))
                continue

            case_failures = gate.anti_vacuous(document, name, minimum_cells, minimum_ran)
            if case_failures:
                failures.extend(case_failures)
                continue

            # The document's own claims, checked against the corpus rather than against the other
            # side: every step names a node that exists (the corpus scripts name real ids), and the
            # step count matches the script's `click` lines.
            expected_steps = len([line for line in script.splitlines() if line.strip()])
            if len(document["steps"]) != expected_steps:
                failures.append(
                    "CORPUS {:<22} {} step(s) for {} script line(s)".format(
                        name, len(document["steps"]), expected_steps
                    )
                )
                continue
            for step in document["steps"]:
                if not step.get("found"):
                    failures.append(
                        "CORPUS {:<22} step {} did not find automation id {}".format(
                            name, step.get("step"), step.get("automationId")
                        )
                    )
                    break

            print(
                "NATIVE_OK {:<22} {} step(s), {} cell(s), {} handler run(s)".format(
                    name,
                    len(document["steps"]),
                    len(document["initial"]),
                    sum(len(step.get("ran") or []) for step in document["steps"]),
                )
            )

        if failures:
            print("\n".join(failures))
            print("FAILED: the input corpus does not hold natively")
            return 1
        print(
            "NATIVE CORPUS OK: every case holds natively "
            "(Wasm parity is CI-only, see the gate)"
        )
        return 0
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
