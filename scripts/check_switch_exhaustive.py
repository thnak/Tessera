#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exhaustive-switch gate (issue #805).

A `switch` over an enum that does not handle every enumerator and has no `default:` is a warning on
GCC and Clang (`-Wswitch`, part of `-Wall`) and *silence* on MSVC: `/W4` does not include C4062. The
tree builds with warnings as errors on every lane, so the same defect is green locally and red on
five CI lanes.

ADR-066's four steps hit it nine times:

  #651  ast_dump, binding_reads_expr, value_ops, emit_library_components_expr
  #652  ast_dump, expression_lowering
  #653  binding_reads_expr, value_ops, emit_library_components_expr
  #654  ast_dump, expression_lowering

Each cost a full CI round. The fix was always one line. This gate is the cheap half of issue #805: it
needs no build tree and no compiler, so it runs in the same second as the other gates and catches the
class before a push.

What "exhaustive" means here: for every `switch` whose `case` labels name enumerators of a declared
enum, the set of names it tests must equal that enum's enumerator set, unless the switch has a
`default:` label. A switch that names no enumerators at all is not judged (it may be over an int, a
string, or a boolean).

Usage:
  python -B scripts/check_switch_exhaustive.py
  python -B scripts/check_switch_exhaustive.py --verbose   # also list every exhaustive switch
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

SCRIPTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS_DIR.parent

# Every tree that is ours to build. `third_party/` is vendored and not restyled; `build*/` is output.
SCAN_ROOTS = ("include", "src", "tests", "tools")
SOURCE_SUFFIXES = (".hpp", ".h", ".cpp", ".cc", ".cxx")

ENUM_RE = re.compile(
    r"\benum\s+(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)\s*(?::\s*[^/{]+?)?\s*\{"
)
# An enumerator is the first identifier of a top-level comma-separated item in the body. A value
# (`A = 1`) is allowed after it; the name is what this gate compares.
ENUMERATOR_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*(?:=[^,]*)?$")
SWITCH_RE = re.compile(r"\bswitch\s*\(")
CASE_RE = re.compile(r"\bcase\s+([A-Za-z_]\w*)::([A-Za-z_]\w*)\s*:")
DEFAULT_RE = re.compile(r"\bdefault\s*:")


# A line comment, a block comment, or a string/char literal. One pass of `re.sub` is used instead of
# a per-character loop because the tree is ~2500 files and the loop cost 24 of the gate's 27 seconds.
BLANK_RE = re.compile(
    r"//[^\n]*"          # line comment
    r"|/\*.*?\*/"        # block comment (the tree does not nest them)
    r'|"(?:\\.|[^"\\\n])*"'   # string literal, may span a `\`-continuation
    r"|'(?:\\.|[^'\\\n])*'",  # char literal
    re.DOTALL,
)


def blank_comments_and_strings(text: str) -> str:
    """`text` with comments and string/char literals replaced by spaces.

    Line breaks are preserved, so line numbers and offsets survive. Without this a `switch` inside a
    string constant, or a `}` inside a comment, would break the brace matching below.
    """

    def repl(match: re.Match) -> str:
        return "".join("\n" if char == "\n" else " " for char in match.group(0))

    return BLANK_RE.sub(repl, text)


def match_bracket(text: str, start: int, open_char: str, close_char: str) -> Optional[int]:
    """The index of the bracket closing the one at `start`, or None when it is unbalanced."""
    if start >= len(text) or text[start] != open_char:
        return None
    depth = 0
    for i in range(start, len(text)):
        if text[i] == open_char:
            depth += 1
        elif text[i] == close_char:
            depth -= 1
            if depth == 0:
                return i
    return None


def enums_in(code: str) -> Dict[str, Set[str]]:
    """`Name -> enumerator names` for every `enum class` in one file's blanked text."""
    found: Dict[str, Set[str]] = {}
    for match in ENUM_RE.finditer(code):
        brace = code.index("{", match.start())
        end = match_bracket(code, brace, "{", "}")
        if end is None:
            continue
        body = code[brace + 1 : end]
        names: Set[str] = set()
        for piece in body.split(","):
            item = ENUMERATOR_RE.match(piece)
            if item is not None:
                names.add(item.group(1))
        if names:
            found[match.group("name")] = names
    return found

def switch_blocks(code: str) -> List[Tuple[int, str]]:
    """Every `switch` in one file as (line, body text), with the condition excluded."""
    blocks: List[Tuple[int, str]] = []
    for match in SWITCH_RE.finditer(code):
        paren = match.end() - 1
        paren_end = match_bracket(code, paren, "(", ")")
        if paren_end is None:
            continue
        brace = code.find("{", paren_end)
        if brace < 0:
            continue
        # A `;` between `)` and `{` means this `switch` has no body (a declaration, not a statement).
        if ";" in code[paren_end:brace]:
            continue
        end = match_bracket(code, brace, "{", "}")
        if end is None:
            continue
        line = code.count("\n", 0, match.start()) + 1
        blocks.append((line, code[brace + 1 : end]))
    return blocks


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--verbose", action="store_true", help="also list every exhaustive switch"
    )
    args = parser.parse_args(argv)

    # Two passes: the enum table is global, because a `switch` in one file names enumerators declared
    # in another (the IR kinds live in a header under include/).
    #
    # A name may be declared more than once in different namespaces: `jsx::ExprKind` and
    # `ir::ExprKind` are different enums with the same name, and a switch over one is not judged
    # against the other. So the table holds every declaration of a name, and the match below picks
    # the declaration whose enumerators actually cover what the switch tests.
    files: List[Tuple[str, str]] = []
    enums: Dict[str, List[Set[str]]] = {}
    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = path.relative_to(REPO_ROOT).as_posix()
            code = blank_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
            files.append((rel, code))
            for name, members in enums_in(code).items():
                known = enums.setdefault(name, [])
                if members not in known:
                    known.append(members)

    errors: List[str] = []
    checked = 0
    for rel, code in files:
        for line, body in switch_blocks(code):
            handled: Dict[str, Set[str]] = {}
            for case in CASE_RE.finditer(body):
                enum_name, member = case.group(1), case.group(2)
                handled.setdefault(enum_name, set()).add(member)
            if not handled:
                continue  # not an enum switch: an int, a string, a boolean
            has_default = DEFAULT_RE.search(body) is not None
            checked += 1
            if has_default:
                if args.verbose:
                    print("note: {}:{}: exhaustive by `default`".format(rel, line))
                continue
            missing_any = False
            for enum_name, members_named in sorted(handled.items()):
                # The declaration this switch is over: one that contains every member it tests. The
                # smallest such set is the tightest fit when a name is declared twice.
                candidates = [
                    declared
                    for declared in enums.get(enum_name, [])
                    if members_named <= declared
                ]
                if not candidates:
                    # An enum from a tree this gate does not scan, or a namespace-qualified name it
                    # did not match. Nothing to compare against.
                    continue
                declared = min(candidates, key=len)
                missing = sorted(declared - members_named)
                if missing:
                    missing_any = True
                    errors.append(
                        "{}:{}: switch over `{}` does not handle {} and has no `default:`".format(
                            rel, line, enum_name, ", ".join("`" + name + "`" for name in missing)
                        )
                    )
            if not missing_any and args.verbose:
                print("note: {}:{}: exhaustive".format(rel, line))

    for message in errors:
        print(
            "ERROR "
            + message
            + "; add the missing case(s), or a `default:` that reports it as unreachable"
        )
    if errors:
        print(
            "switch-exhaustive gate: {} error(s), {} enum switch(es) checked".format(
                len(errors), checked
            ),
            file=sys.stderr,
        )
        return 1
    print(
        "switch-exhaustive gate: 0 error(s), {} enum switch(es) checked, {} enum(s) declared".format(
            checked, len(enums)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
