#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure what the JSX/TSX front end accepts over the React port corpus (spike #660, ADR-066).

This is a measurement, not a gate: it exits 0 whenever it could measure, whatever the accepted share is,
and it is not registered in `gates.py` or CTest (it needs a built `tessera_corpus_measure`, and the
share moves with every front-end change, so pinning it would turn each landing into a golden edit).
The pinned counterpart for the in-repo fixtures is the `corpus_measure` test.

It runs the existing `tessera_corpus_measure` tool (tools/corpus) with `--verbose` over a corpus
directory and reports, per file, accepted or rejected with the diagnostic codes, then per rejected form
the code, its occurrences, the files it appears in and the files it blocks first. Two stages are shown
because a port needs both: the front end (parse plus Tier-1 rules) and the pipeline (the first blocking
diagnostic of the whole single-file path).

Anomalies are listed explicitly, never folded into a rejection count: a corpus file the tool did not
report (a crash or a skipped file), a non-zero exit, and a file rejected with no diagnostic code.

Usage:
    python -B scripts/corpus_react_port.py [--binary PATH] [--corpus DIR] [--json] [--number-as-int]

`--number-as-int` measures a copy of the corpus with the type name `number` rewritten to `int`.
`TESSERA_JSX_TYPE_NUMBER` is a deliberate rejection (Tessera has `int` and `float`, not `number`) that
blocks a file before later forms are reached; this second run shows what the files need once it is
cleared, so the order of work is not read from a table that one early diagnostic masks.

Build the tool first, e.g.
    cmake --preset vs2026
    cmake --build build/vs2026 --config Release --target tessera_corpus_measure
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CORPUS = ROOT / "tests" / "fixtures" / "corpus" / "react-port"
BINARY_CANDIDATES = (
    "build/vs2026/tools/corpus/Release/tessera_corpus_measure.exe",
    "build/vs2022/tools/corpus/Release/tessera_corpus_measure.exe",
    "build/dev/tools/corpus/tessera_corpus_measure",
    "build/release/tools/corpus/tessera_corpus_measure",
    "build/ci/tools/corpus/tessera_corpus_measure",
)
FILE_LINE = re.compile(
    r"^  (accepted|rejected) (\S+) components=(\d+)(?::((?: \S+)*?))? pipeline=(\S*)$"
)


def find_binary(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    for candidate in BINARY_CANDIDATES:
        path = ROOT / candidate
        if path.exists():
            return path
    sys.exit("tessera_corpus_measure not found; build it (see the module docstring) or pass --binary")


def parse_files(report: str) -> list[dict]:
    files = []
    in_files = False
    for line in report.splitlines():
        if line == "files:":
            in_files = True
            continue
        if not in_files:
            continue
        match = FILE_LINE.match(line)
        if not match:
            if line.strip():
                sys.exit(f"unparsed report line: {line!r}")
            continue
        verdict, path, components, errors, pipeline = match.groups()
        files.append(
            {
                "path": path,
                "accepted": verdict == "accepted",
                "components": int(components),
                "errors": (errors or "").split(),
                "pipeline": pipeline,
            }
        )
    return files


def measure(binary: str, corpus: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [binary, "--verbose", "--label", "react-port", str(corpus)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )


def share(part: int, whole: int) -> str:
    return f"{part}/{whole} ({100.0 * part / whole:.1f}%)" if whole else "0/0"


def build_summary(files: list[dict], on_disk: list[str], exit_code: int) -> dict:
    occurrences: Counter[str] = Counter()
    in_files: Counter[str] = Counter()
    first: Counter[str] = Counter()
    pipeline: Counter[str] = Counter()
    for f in files:
        occurrences.update(f["errors"])
        in_files.update(set(f["errors"]))
        if f["errors"]:
            first[f["errors"][0]] += 1
        if f["pipeline"] != "ok":
            pipeline[f["pipeline"] or "(none)"] += 1
    reported = {Path(f["path"]).name for f in files}
    return {
        "total": len(files),
        "front_end_accepted": sum(f["accepted"] for f in files),
        "pipeline_accepted": sum(f["pipeline"] == "ok" for f in files),
        "occurrences": occurrences.most_common(),
        "files_per_code": dict(in_files),
        "first_rejections": first.most_common(),
        "pipeline_rejections": pipeline.most_common(),
        "anomalies": {
            "exit_code": exit_code,
            "missing_from_report": sorted(set(on_disk) - reported),
            "rejected_without_code": [f["path"] for f in files if not f["accepted"] and not f["errors"]],
            "pipeline_rejected_without_code": [
                f["path"] for f in files if f["pipeline"] in ("", "(none)")
            ],
        },
    }


def print_text(files: list[dict], s: dict) -> None:
    total = s["total"]
    print(f"corpus files: {total}")
    print(f"front end accepted: {share(s['front_end_accepted'], total)}")
    print(f"pipeline accepted:  {share(s['pipeline_accepted'], total)}")
    print("\nper file (front end verdict, codes; pipeline):")
    for f in files:
        codes = " ".join(f["errors"]) if f["errors"] else "-"
        print(f"  {'accepted' if f['accepted'] else 'rejected'}  {Path(f['path']).name:34} {codes}  | {f['pipeline']}")
    print("\nfront-end rejection by code (occurrences, files, first-rejection files):")
    first = dict(s["first_rejections"])
    for code, count in s["occurrences"]:
        print(f"  {code:46} {count:3} {s['files_per_code'][code]:3} {first.get(code, 0):3}")
    print("\npipeline first blocking code (files):")
    for code, count in s["pipeline_rejections"]:
        print(f"  {code:46} {count:3}")
    a = s["anomalies"]
    print("\nanomalies:")
    print(f"  tool exit code: {a['exit_code']}")
    print(f"  files missing from the report: {a['missing_from_report'] or 'none'}")
    print(f"  front-end rejected with no code: {a['rejected_without_code'] or 'none'}")
    print(f"  pipeline rejected with no code: {a['pipeline_rejected_without_code'] or 'none'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--binary", help="path to tessera_corpus_measure")
    parser.add_argument("--corpus", default=str(DEFAULT_CORPUS), help="corpus directory of .tsx files")
    parser.add_argument("--json", action="store_true", help="print the summary as JSON")
    parser.add_argument(
        "--number-as-int",
        action="store_true",
        help="measure a copy with the type name number rewritten to int (see the module docstring)",
    )
    args = parser.parse_args()

    corpus = Path(args.corpus)
    on_disk = sorted(p.name for p in corpus.rglob("*.tsx"))
    if not on_disk:
        sys.exit(f"no .tsx files under {corpus}")
    binary = str(find_binary(args.binary))
    if args.number_as_int:
        with tempfile.TemporaryDirectory() as tmp:
            for src in corpus.rglob("*.tsx"):
                text = src.read_text(encoding="utf-8")
                (Path(tmp) / src.name).write_text(re.sub(r"\bnumber\b", "int", text), encoding="utf-8")
            run = measure(binary, Path(tmp))
    else:
        run = measure(binary, corpus)
    files = parse_files(run.stdout)
    summary = build_summary(files, on_disk, run.returncode)
    if run.returncode != 0:
        sys.stderr.write(run.stderr)
    if args.json:
        print(json.dumps({"files": files, **summary}, indent=2))
    else:
        print_text(files, summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
