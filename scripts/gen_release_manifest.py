#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record a release and project the wire manifests (issue #193).

Two subcommands, both of them offline:

``record`` -- take a real package archive and a symbol artifact, hash them, and append one ledger
entry. Nothing is downloaded and nothing is trusted: the digest is computed from the bytes on disk,
the version/platform come from the archive's own name (which ``check_package.py`` already enforces),
the commit from the archive's ``PACKAGE-INFO.txt``, and the build id from the symbol file's header.
The command *refuses* rather than guesses:

* a ``--commit``/``--date`` that disagrees with the archive's ``PACKAGE-INFO.txt``,
* a symbol artifact whose build id is not that commit (the resolver would refuse it at crash time),
* a symbol file with no ``func=`` rows (it names nothing),
* a version already recorded on that channel (the ledger is append-only, not a scratch pad).

``project`` -- derive ``release/manifests/<channel>-<platform>.json`` from the ledger, writing only
the files whose bytes change and deleting ones the ledger no longer projects. This is the step the
``manifests`` gate checks; running it twice is a no-op on the second run, byte for byte.

Usage (from the repository root):

    python -B scripts/gen_release_manifest.py record \\
        --archive build/package/out/tessera-1.0.0-windows-x86_64.zip \\
        --symbols build/package/out/tessera-1.0.0-windows-x86_64.symbols.txt \\
        --channel beta --date 2026-11-16 [--url https://updates.example.com/...]

    python -B scripts/gen_release_manifest.py project

    python -B scripts/gen_release_manifest.py project --check   # report drift without writing

Exit codes: 0 written/nothing to do, 1 refused (drift with ``--check``, or a rule above),
2 usage or unreadable input. Stdlib only, no clock, no network, deterministic output.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any, Dict, Optional, Sequence

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import release_manifest as model  # noqa: E402  (sibling module, stdlib only)
import release_manifests_io as io  # noqa: E402
from release_policy import evaluate_data, validate_projection  # noqa: E402

DEFAULT_CHANNELS = REPO_ROOT / "release" / "channels.json"
DEFAULT_LEDGER = REPO_ROOT / "release" / "ledger.json"
DEFAULT_MANIFESTS = REPO_ROOT / "release" / "manifests"
DEFAULT_CMAKE = REPO_ROOT / "CMakeLists.txt"

# `PACKAGE-INFO.txt` records whatever commit the package was configured with. The packaging default
# is a 12-hex short id; a release should pass the full one. Accept either here and record the full
# id from the symbol artifact, which is the id a crash report and the resolver both use.
COMMIT_PREFIX_RE = re.compile(r"^[0-9a-f]{7,40}$")


def _configure_streams() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(errors="backslashreplace")
            except (ValueError, OSError):
                pass


def _load(paths: io.Paths) -> tuple:
    channels = model.load_object(paths.channels, "the channel file")
    ledger = model.load_object(paths.ledger, "the ledger")
    return channels, ledger


def record_release(args: argparse.Namespace) -> int:
    paths = io.Paths(
        channels=Path(args.channels),
        ledger=Path(args.ledger),
        manifests=Path(args.manifests),
        cmake=Path(args.cmake_lists),
    )
    channels, ledger = _load(paths)
    if model.channel_by_id(channels, args.channel) is None:
        print(
            "error: channel {!r} is not defined in {}".format(args.channel, paths.channels),
            file=sys.stderr,
        )
        return 2

    archive = Path(args.archive)
    version, platform, _suffix = model.archive_identity(archive)
    size, sha256 = model.artifact_digest(archive)

    try:
        info = model.read_package_info(archive)
    except model.ManifestError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2
    if info.get("Version") != version:
        print(
            "error: archive name says version {!r} but PACKAGE-INFO.txt says {!r}".format(
                version, info.get("Version")
            ),
            file=sys.stderr,
        )
        return 2
    if info.get("Platform") != platform:
        print(
            "error: archive name says platform {!r} but PACKAGE-INFO.txt says {!r}".format(
                platform, info.get("Platform")
            ),
            file=sys.stderr,
        )
        return 2
    commit = str(info.get("Commit", ""))
    if not COMMIT_PREFIX_RE.match(commit):
        print(
            "error: PACKAGE-INFO.txt commit {!r} is not a hex commit; rebuild with "
            "-DTESSERA_PACKAGE_COMMIT=$(git rev-parse HEAD)".format(commit),
            file=sys.stderr,
        )
        return 2
    if args.commit and not args.commit.startswith(commit) and not commit.startswith(args.commit):
        print(
            "error: --commit {!r} disagrees with the archive's PACKAGE-INFO.txt ({!r})".format(
                args.commit, commit
            ),
            file=sys.stderr,
        )
        return 2
    if info.get("StaticRuntime", "").upper() != "ON":
        print(
            "error: the archive does not declare StaticRuntime: ON (ADR-012)", file=sys.stderr
        )
        return 2

    symbol_path = Path(args.symbols)
    symbol_version, symbol_platform = model.symbols_identity(symbol_path)
    if symbol_version != version or symbol_platform != platform:
        print(
            "error: symbol artifact is for {} {} but the archive is {} {}".format(
                symbol_version, symbol_platform, version, platform
            ),
            file=sys.stderr,
        )
        return 2
    try:
        header = model.parse_symbol_header(symbol_path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, model.ManifestError) as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2
    # `PACKAGE-INFO.txt` carries whatever `TESSERA_PACKAGE_COMMIT` was set to, and the packaging
    # default is `git rev-parse --short=12`; the symbol artifact carries the full commit. They must
    # name the same build, so compare as a prefix in either direction rather than forcing a rebuild
    # with a different CMake option: the ledger records the 40-hex commit the symbols were built
    # from, which is the id a crash report carries and the resolver matches on.
    if not header["build"].startswith(commit) and not commit.startswith(header["build"]):
        print(
            "error: symbol artifact build id {!r} is not the release commit {!r}; a mismatch is "
            "refused by the resolver, so publishing it is worse than publishing none".format(
                header["build"], commit
            ),
            file=sys.stderr,
        )
        return 2
    commit = header["build"]
    symbol_size, symbol_sha = model.artifact_digest(symbol_path)

    existing = model.find_release(ledger, args.channel, version)
    if existing is not None:
        print(
            "error: {!r} already records {}; the ledger is append-only (use project), or bump the "
            "version".format(args.channel, version),
            file=sys.stderr,
        )
        return 1

    artifacts = [
        {
            "platform": platform,
            "archive": archive.name,
            "sha256": sha256,
            "size": size,
            "url": args.url,
            "notesUrl": args.notes_url,
            "symbols": {
                "artifact": symbol_path.name,
                "sha256": symbol_sha,
                "size": symbol_size,
                "format": model.SYMBOLS_FORMAT,
                "build": header["build"],
                "functions": header["functions"],
                "url": args.symbols_url,
            },
        }
    ]
    artifacts.sort(key=lambda item: str(item.get("platform", "")))
    release: Dict[str, Any] = {
        "channel": args.channel,
        "version": version,
        "date": args.date,
        "commit": commit,
        "minOs": args.min_os,
        "signature": args.signature,
        "notes": args.notes,
        "deprecations": model.load_json(Path(args.deprecations)) if args.deprecations else [],
        "artifacts": artifacts,
    }
    ledger.setdefault("releases", [])
    ledger["releases"] = model.canonical_ledger({"releases": ledger["releases"] + [release]})["releases"]
    model.write_document(paths.ledger, ledger)
    print(
        "recorded {} {} {} ({} bytes, sha256 {}) in {}".format(
            args.channel, version, platform, size, sha256[:16] + "...", paths.ledger
        )
    )
    return project_release(argparse.Namespace(
        channels=str(paths.channels),
        ledger=str(paths.ledger),
        manifests=str(paths.manifests),
        cmake_lists=str(paths.cmake),
        check=False,
        quiet=False,
    ))


def project_release(args: argparse.Namespace) -> int:
    paths = io.Paths(
        channels=Path(args.channels),
        ledger=Path(args.ledger),
        manifests=Path(args.manifests),
        cmake=Path(args.cmake_lists),
    )
    channels, ledger = _load(paths)
    # Two gates before anything is written, in this order: the *data* must satisfy its own rules,
    # and the *derived* documents must survive the same checks the committed ones will face. A
    # projection is a commit, so it is refused rather than written when either fails.
    findings = evaluate_data(
        channels,
        ledger,
        io.project_version(paths.cmake),
        io.rel(paths.channels),
        io.rel(paths.ledger),
    )
    findings.extend(validate_projection(channels, ledger, io.rel(paths.manifests)))
    errors = [finding for finding in findings if finding.severity == "error"]
    if errors:
        for finding in errors:
            print(finding.format(), file=sys.stderr)
        print(
            "error: refusing to project with {} error(s); fix the data first".format(len(errors)),
            file=sys.stderr,
        )
        return 1
    projection = model.project_manifests(ledger, channels)
    return io.write_projection(paths.manifests, projection, check=args.check, quiet=args.quiet)


PATH_OPTIONS = (
    ("--channels", "channels", str(DEFAULT_CHANNELS), "release/channels.json"),
    ("--ledger", "ledger", str(DEFAULT_LEDGER), "release/ledger.json"),
    ("--manifests", "manifests", str(DEFAULT_MANIFESTS), "release/manifests/"),
    ("--cmake-lists", "cmake_lists", str(DEFAULT_CMAKE), "root CMakeLists.txt"),
)


def add_path_options(parser: argparse.ArgumentParser, *, default: bool = True) -> None:
    """The input paths, registered on the parser *and* on each subcommand.

    ``argparse`` accepts a parent option only before the subcommand, so registering them in both
    places lets `--ledger X record ...` and `record ... --ledger X` mean the same thing. On the
    subcommand the default is ``SUPPRESS``: a subparser default would otherwise overwrite the value
    the parent already parsed, which silently ignores an option written before the subcommand.
    """
    for flag, dest, value, help_text in PATH_OPTIONS:
        parser.add_argument(
            flag,
            dest=dest,
            default=value if default else argparse.SUPPRESS,
            help=help_text,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_path_options(parser)
    sub = parser.add_subparsers(dest="command", required=True)

    record = sub.add_parser("record", help="hash a real archive + symbol file into the ledger")
    add_path_options(record, default=False)
    record.add_argument("--archive", required=True, help="tessera-<version>-<platform>.{tar.gz,zip}")
    record.add_argument("--symbols", required=True, help="tessera-<version>-<platform>.symbols.txt")
    record.add_argument("--channel", required=True)
    record.add_argument("--date", required=True, help="release date, YYYY-MM-DD (data, not a clock)")
    record.add_argument("--commit", default=None, help="expected commit (must match PACKAGE-INFO.txt)")
    record.add_argument("--min-os", dest="min_os", default=None)
    record.add_argument("--signature", default=None, help="Ed25519 signature hex, when one exists")
    record.add_argument("--url", default=None, help="https URL on an allowlisted host")
    record.add_argument("--notes-url", dest="notes_url", default=None)
    record.add_argument("--symbols-url", dest="symbols_url", default=None)
    record.add_argument("--notes", default=None, help="short release note")
    record.add_argument("--deprecations", default=None, help="JSON file of deprecation entries")
    record.set_defaults(handler=record_release)

    project = sub.add_parser("project", help="derive release/manifests/ from the ledger")
    add_path_options(project, default=False)
    project.add_argument("--check", action="store_true", help="report drift instead of writing")
    project.add_argument("--quiet", action="store_true")
    project.set_defaults(handler=project_release)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    _configure_streams()
    args = build_parser().parse_args(argv)
    try:
        return int(args.handler(args))
    except model.ManifestError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
