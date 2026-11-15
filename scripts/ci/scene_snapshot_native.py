#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native-only corpus check for the scene-snapshot parity gate (issue #166).

`scene_snapshot_parity.py` needs an Emscripten module, which cannot be built on every machine (the
development box has no `EMSDK`). This script exercises the half that *can* run anywhere: it evaluates
the same corpus through `tessera_scene_preview` and asserts the parts of the gate that do not need the
Wasm side --

  * every case produces a document of the right version that parses,
  * every case with a non-zero minimum produces at least that many nodes (the anti-vacuity rule),
  * every case with a zero minimum produces none (the documented "unsupported" answer),
  * the document is deterministic: two runs of the same case are byte-identical,
  * the ids are the compiler's: a node the case pins with `automationId` reports that number.

It exists so a lane without Emscripten can still prove its native half, and so a corpus change is
checked before CI spends a Wasm build on it. It shares the corpus *only* by importing the parity
gate, so the two can never drift apart.

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
import scene_snapshot_parity as gate  # noqa: E402


def parse_args(argv: Optional[Sequence[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="host-built tessera_scene_preview CLI")
    parser.add_argument("--repo-root", default=str(gate.REPO_ROOT))
    parser.add_argument(
        "--work-dir",
        default=None,
        help="scratch directory (default: <repo-root>/build/scene-preview-native)",
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
        Path(args.work_dir) if args.work_dir else repo_root / "build" / "scene-preview-native"
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
        for name, file, source, minimum in gate.CORPUS:
            first = gate.native_document(str(native_cli), name, file, source, work_root)
            nodes = gate.nodes_of(first, "native/" + name)

            if len(nodes) < minimum:
                failures.append(
                    "CORPUS {:<22} {} node(s), expected at least {}".format(
                        name, len(nodes), minimum
                    )
                )
                continue
            if minimum == 0 and nodes:
                failures.append(
                    "CORPUS {:<22} {} node(s), expected none (the case is unsupported)".format(
                        name, len(nodes)
                    )
                )
                continue

            # Determinism: the document is a pure function of the input and the options, so a second
            # run of the same case must be byte-identical. This is what makes the byte comparison
            # against the Wasm side meaningful rather than a coincidence of one lucky run.
            second = gate.native_document(str(native_cli), name, file, source, work_root)
            if second != first:
                failures.extend(gate.describe_difference(name + " (native rerun)", first, second))
                continue

            # An explicit `automationId` is the one id the case itself dictates, so it proves the
            # document reports the compiler's identity and not a counter of the walk's own.
            if name == "explicit-id":
                ids = {node.get("uiId") for node in nodes}
                if "424242" not in ids:
                    failures.append(
                        "IDS    {:<22} no node reports automationId 424242 (got {})".format(
                            name, sorted(ids)
                        )
                    )
                    continue

            print("NATIVE_OK {:<22} {} node(s)".format(name, len(nodes)))

        if failures:
            print("\n".join(failures))
            print("FAILED: the corpus does not hold on the native side")
            return 1
        print("NATIVE CORPUS OK: every case holds natively (Wasm parity is CI-only, see the gate)")
        return 0
    except gate.SetupError as exc:
        print("SETUP ERROR: {}".format(exc), file=sys.stderr)
        return 2
    finally:
        shutil.rmtree(work_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
