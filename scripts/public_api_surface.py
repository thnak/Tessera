#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The public surface of the tree as canonical lines (issue #76).

`scripts/check_public_api.py` compares this module's output against a committed baseline. This half
is the façade: the taxonomy, the `.tsx` half, and the baseline text. No baseline file, no exit
codes, no printing, so the classify/compare logic is testable on text directly. The C++ scanner
lives in `scripts/public_api_cpp.py`; it is re-exported here so callers have one entry point.

## What a line is

One fact per line, `kind path name`, sorted (see `sort_key`) and rendered by `render_baseline`:

    header     <path>                        the header exists at all
    class/struct/union/enum <path> <name>    a type declared at namespace scope, with its kind
    alias      <path> <name>                 `using X = ...`, a namespace alias, a using-declaration
    concept    <path> <name>                 a `concept`
    function   <path> <name>                 a free function, `operator|` included
    constant   <path> <name>                 a namespace-scope constexpr/const variable
    variable   <path> <name>                 any other namespace-scope variable

    component  <path> <Name>                 `export function Name(` in a library `.tsx`
    prop       <path> <Name>.<prop>          one field of `export type NameProps = {...}`
    part       <path> <Name>::<part>         one name of a literal `part="a b"` attribute

`<name>` is fully qualified (`tessera::ui::Node`) and never contains whitespace: an operator name
with a space (`operator co_await`) is written with an underscore (`operator_co_await`).

## What is deliberately not recorded

Declaration-level only. The gate exists to catch a *removed or renamed* declaration; recording more
would make every legitimate edit a "breaking change" and the gate would be switched off:

* no signatures, return types, template parameters, `noexcept`, `[[nodiscard]]` or access specifiers,
  so editing a parameter list or an attribute is not a surface change;
* no class members: methods, data members, nested types and enumerators are out of scope, and so are
  their out-of-class definitions (`void ChartNode::paint(...)`, `class Ring::const_iterator {...}`);
* no default values, no prop optionality (`label?` and `label` are the same line) and no prop types:
  a prop's *name* is the contract here;
* no preprocessor conditionals: directives are dropped and both branches of an `#if` are kept, so a
  declaration that only exists under `TESSERA_ENABLE_TRACING` is still recorded;
* no `::part()` names written in stylesheets; only the `part="..."` literals a component source
  declares are recorded, which is the same source `tessera_codegen` turns into the library part
  registry (ADR-047 item 3);
* no `.tsx` default values, handler bodies or class names, and no `tessera.json` schema (issue #76
  names it, but only as a later item; #194 owns the config schema).

The extraction is a scanner, not a compiler: it tracks namespaces and brace nesting and never
resolves an include. That is enough for declaration-level facts and keeps it stdlib-only.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

# Sibling stdlib-only import, the same pattern check_public_api.py uses: `scripts/` is not on
# sys.path when a script in it is run as `python -B scripts/<name>.py`.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import public_api_cpp as cpp  # noqa: E402  (sibling script, stdlib only)

# Re-exported so `public_api_surface.header_declarations` stays the single entry point callers and
# tests already use; the C++ half is one implementation detail behind it.
from public_api_cpp import TYPE_KINDS, header_declarations, strip_cpp  # noqa: E402,F401

CPP_ROOT = "include/tessera"
TSX_ROOT = "packages/tessera-ui"

HEADER = "header"
COMPONENT = "component"
PROP = "prop"
PART = "part"

# Within one path's block the order is stable and readable: the file, then its types, then values.
KIND_ORDER: Dict[str, int] = {
    HEADER: 0,
    COMPONENT: 1,
    PROP: 2,
    PART: 3,
    "class": 4,
    "struct": 5,
    "union": 6,
    "enum": 7,
    "alias": 8,
    "concept": 9,
    "function": 10,
    "constant": 11,
    "variable": 12,
}

_COMPONENT_RE = re.compile(r"\bexport\s+function\s+([A-Za-z_]\w*)\s*\(")
_PROPS_RE = re.compile(r"\bexport\s+type\s+([A-Za-z_]\w*)Props\s*=\s*\{")
_PROP_FIELD_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*\??\s*:")
_PART_RE = re.compile(r"\bpart\s*=\s*\"([^\"]*)\"")


@dataclass(frozen=True)
class Record:
    """One canonical surface fact: `kind path name` (a header has no name)."""

    kind: str
    path: str
    name: str = ""

    def line(self) -> str:
        if self.name:
            return "{} {} {}".format(self.kind, self.path, self.name)
        return "{} {}".format(self.kind, self.path)


def sort_key(line: str) -> Tuple[str, int, str]:
    """Total order: path, then kind, then name. Two runs sort identically by construction."""
    parts = line.split(" ", 2)
    kind = parts[0]
    path = parts[1] if len(parts) > 1 else ""
    name = parts[2] if len(parts) > 2 else ""
    return (path, KIND_ORDER.get(kind, len(KIND_ORDER)), name)


def read_text(path: Path) -> str:
    """UTF-8 with universal newlines, so a CRLF checkout extracts identically to an LF one."""
    return path.read_text(encoding="utf-8", errors="replace")


def strip_tsx(text: str) -> str:
    """Drop `//` and `/* */` comments. String literals stay: `part="..."` is the point."""
    out: List[str] = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = length if end < 0 else end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            stop = length if end < 0 else end + 2
            out.append("\n" * text.count("\n", index, stop))
            index = stop
            continue
        out.append(text[index])
        index += 1
    return "".join(out)


def _field_segments(body: str) -> List[str]:
    """Split a type literal body into field segments: every line, and also each top-level `;` / `,`
    so a canonical one-line `{ a: string; b?: number }` yields both fields (tessera_fmt joins short
    literals). Separators nested in braces, parens, brackets do not split."""
    segments: List[str] = []
    depth = 0
    start = 0
    for index, char in enumerate(body):
        if char in "{([":
            depth += 1
        elif char in "})]":
            depth = max(depth - 1, 0)
        elif char == "\n" or (depth == 0 and char in ";,"):
            segments.append(body[start:index])
            start = index + 1
    segments.append(body[start:])
    return segments


def component_declarations(text: str) -> Tuple[List[Tuple[str, str]], List[str]]:
    """`(declarations, warnings)`: components, their props and their `::part()` names.

    Props come from the `export type <Name>Props = { ... }` type literal, one field name each. Parts
    come from literal `part="a b"` attributes, attached to the nearest preceding
    `export function <Name>(`. A dynamic `part={expr}` is not a literal and is not recorded; a
    stylesheet's `::part(...)` selectors are usage, not a declaration, and are not recorded either.
    """
    source = strip_tsx(text)
    declarations: List[Tuple[str, str]] = []
    warnings: List[str] = []
    components = [(found.start(), found.group(1)) for found in _COMPONENT_RE.finditer(source)]
    for _, name in components:
        declarations.append((COMPONENT, name))

    for found in _PROPS_RE.finditer(source):
        owner = found.group(1)
        opening = source.find("{", found.end() - 1)
        closing = cpp.matching(source, opening, "{", "}") if opening >= 0 else -1
        if opening < 0 or closing < 0:
            warnings.append("unterminated `{}Props` type literal".format(owner))
            continue
        for line in _field_segments(source[opening + 1 : closing]):
            field = _PROP_FIELD_RE.match(line)
            if field is not None:
                declarations.append((PROP, "{}.{}".format(owner, field.group(1))))

    for found in _PART_RE.finditer(source):
        owner = ""
        for start, name in components:
            if start < found.start():
                owner = name
        if not owner:
            warnings.append('`part="{}"` outside an exported component'.format(found.group(1)))
            continue
        for name in found.group(1).split():
            declarations.append((PART, "{}::{}".format(owner, name)))

    return declarations, warnings


def canonical_lines(declarations: Sequence[Tuple[str, str]], path: str) -> List[str]:
    """Deduplicated canonical lines for one file's declarations."""
    return [Record(kind, path, name).line() for kind, name in sorted(set(declarations))]


def collect_surface(repo_root: Path) -> Tuple[List[str], List[str]]:
    """`(lines, warnings)` for the whole tree; lines are deduplicated and sorted with `sort_key`."""
    lines: List[str] = []
    warnings: List[str] = []
    for path in sorted((repo_root / CPP_ROOT).rglob("*.hpp")):
        relative = path.relative_to(repo_root).as_posix()
        lines.append(Record(HEADER, relative).line())
        lines.extend(canonical_lines(header_declarations(read_text(path)), relative))
    for path in sorted((repo_root / TSX_ROOT).rglob("*.tsx")):
        relative = path.relative_to(repo_root).as_posix()
        declarations, file_warnings = component_declarations(read_text(path))
        warnings.extend("{}: {}".format(relative, warning) for warning in file_warnings)
        lines.extend(canonical_lines(declarations, relative))
    return sorted(set(lines), key=sort_key), warnings


def render_baseline(lines: Sequence[str]) -> str:
    """The committed baseline text: a fixed header, then one blank-separated block per path.

    No timestamp, no absolute path, no version number, so two runs over the same tree are
    byte-identical. `--update` writes exactly this and `--check` compares against it.
    """
    header = [
        "# SPDX-License-Identifier: MIT",
        "#",
        "# The recorded public surface (issue #76): every include/tessera/**/*.hpp with its",
        "# namespace-scope declarations, and the tessera-ui components, props and ::part() names.",
        "# Generated by scripts/check_public_api.py --update; do not reformat, and keep it sorted.",
        "#",
        "# A line is `kind path name`. A diff of this file IS the API diff: an added line is",
        "# additive, a removed or renamed line is breaking. --update records additions and REFUSES",
        "# to drop a line, so a removal must be a hand edit here in a reviewed change; the deleted",
        "# line is then the record of the break. Kinds: header, class, struct, union, enum, alias,",
        "# concept, function, constant, variable, component, prop, part.",
        "#",
        "# Declaration-level only: no signatures, no class members, no default values, no prop",
        "# types. See the module docstring of scripts/public_api_surface.py for the exact limits.",
    ]
    blocks: List[List[str]] = []
    current_path = None
    for line in sorted(lines, key=sort_key):
        path = line.split(" ", 2)[1]
        if path != current_path:
            blocks.append([])
            current_path = path
        blocks[-1].append(line)
    text = "\n".join(header) + "\n"
    for block in blocks:
        text += "\n" + "\n".join(block) + "\n"
    return text


def read_baseline(text: str) -> List[str]:
    """The canonical lines of a baseline document (comments and blank lines dropped)."""
    return [
        stripped
        for stripped in (raw.strip() for raw in text.splitlines())
        if stripped and not stripped.startswith("#")
    ]
