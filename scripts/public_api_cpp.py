#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""C++ header scanner for the public-API surface (issue #76).

The half of the surface record that reads one `.hpp` and reports what it declares at namespace
scope. Split out of `scripts/public_api_surface.py` so each file stays inside the 300/500 line
rules; that module is the façade and owns the taxonomy (kind names, sort order, baseline text), and
this one owns the C++ text.

It is a scanner, not a compiler: namespaces and brace/paren nesting are tracked, comments, literals
and preprocessor directives are dropped, and every `{` and `;` at nesting depth zero is classified.
That is enough for declaration-level facts. Everything it cannot do -- resolve a macro, a typedef or
an include -- is exactly what the baseline's under-promise is about; see the docstring of
`public_api_surface.py` for the list of what is deliberately not recorded.
"""

from __future__ import annotations

import re
from typing import Dict, List, Optional, Sequence, Tuple

TYPE_KINDS = ("class", "struct", "union", "enum")

# Keywords that are never a declaration's name: they can precede a `(` but name nothing.
NOT_A_NAME = frozenset(
    {
        "alignas",
        "alignof",
        "auto",
        "case",
        "catch",
        "decltype",
        "delete",
        "do",
        "else",
        "explicit",
        "for",
        "if",
        "inline",
        "namespace",
        "new",
        "noexcept",
        "operator",
        "requires",
        "return",
        "sizeof",
        "static_assert",
        "switch",
        "template",
        "throw",
        "typedef",
        "typename",
        "using",
        "while",
    }
)

# `operator<`, `operator<<`, `operator[]`, `operator co_await`, ... as one word, so a `<` in an
# operator name cannot be mistaken for the start of a template argument list.
_OPERATOR_RE = re.compile(
    r"\boperator\s*(co_await|new\s*\[\]|delete\s*\[\]|\[\]|\(\)|->\*|->|"
    r"<<=|>>=|\|=|&=|\+=|-=|\*=|/=|%=|\^=|[<>=!+\-*/%^&|~,\[\]]+)"
)
# Every operator gets a spelled-out name, so a canonical name never contains a space or a `=`.
_OPERATOR_WORDS = {
    "co_await": "co_await",
    "[]": "index",
    "()": "call",
    "->*": "arrow_star",
    "->": "arrow",
    "<<=": "shift_left_assign",
    ">>=": "shift_right_assign",
    "|=": "or_assign",
    "&=": "and_assign",
    "+=": "plus_assign",
    "-=": "minus_assign",
    "*=": "star_assign",
    "/=": "slash_assign",
    "%=": "percent_assign",
    "^=": "caret_assign",
    "<<": "shift_left",
    ">>": "shift_right",
    "<=": "le",
    ">=": "ge",
    "==": "eq",
    "!=": "ne",
    "&&": "and_and",
    "||": "or_or",
    "++": "plus_plus",
    "--": "minus_minus",
    "<": "lt",
    ">": "gt",
    "+": "plus",
    "-": "minus",
    "*": "star",
    "/": "slash",
    "%": "percent",
    "^": "caret",
    "&": "amp",
    "|": "pipe",
    "~": "tilde",
    "!": "not",
    "=": "assign",
    ",": "comma",
}

_NAMESPACE_RE = re.compile(r"^namespace\s+((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*)\s*$")
_NAMESPACE_ALIAS_RE = re.compile(r"^namespace\s+([A-Za-z_]\w*)\s*=")
# An alignment specifier may sit between the type keyword and the name
# (`struct alignas(64) Vertex2D`). It names nothing, so it is skipped rather than read as the
# declaration's name - the same rule `NOT_A_NAME` already applies to a bare `alignas`.
#
# `TESSERA_EMPTY_BASES` (`__declspec(empty_bases)` on MSVC, empty elsewhere) sits in the same place
# on `ui::Node`. The scanner resolves no macro, so it is named here as the one specifier macro.
#
# `TESSERA_CACHE_LINE_BEGIN`/`_END` (`detail/cache_line.hpp`) wrap an over-aligned type to suppress
# MSVC's C4324 around it (issue #898). They sit on their own lines *before* the type, so without
# naming them the scanner sees `TESSERA_CACHE_LINE_BEGIN` where it expects `struct` and the type
# disappears from the surface - which the api gate then reports as a breaking removal, even though
# the declaration is unchanged. Both are empty on a non-MSVC compiler, so a macro-aware scanner would
# see the same thing.
_ALIGNAS_SPECIFIER_RE = re.compile(
    r"(?:alignas\s*\([^)]*\)|TESSERA_EMPTY_BASES|TESSERA_CACHE_LINE_BEGIN|TESSERA_CACHE_LINE_END)"
    r"\s*"
)
_TYPE_RE = re.compile(r"^(class|struct|union|enum)\s+(?:(class|struct)\s+)?([A-Za-z_]\w*)")
_USING_ALIAS_RE = re.compile(r"^using\s+([A-Za-z_]\w*)\s*=")
_USING_DECL_RE = re.compile(r"^using\s+([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)+)\s*$")
_CONCEPT_RE = re.compile(r"\bconcept\s+([A-Za-z_]\w*)")
_TRAILING_IDENT_RE = re.compile(r"([A-Za-z_]\w*)\s*$")
_WORD_RE = re.compile(r"[A-Za-z_]\w*")

_PREPROCESSOR_RE = re.compile(r"(?m)^[ \t]*#.*$")


def strip_cpp(text: str) -> str:
    """Drop comments and literals, keeping newlines (so line structure survives).

    A `'` after an alphanumeric is a digit separator (`20'000`), not a character literal, and a raw
    string is skipped to its own closing delimiter.
    """
    out: List[str] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
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
        if char == "R" and text.startswith('R"', index) and (
            index == 0 or not text[index - 1].isalnum()
        ):
            open_paren = text.find("(", index + 2)
            if open_paren > 0:
                delimiter = text[index + 2 : open_paren]
                close = text.find(")" + delimiter + '"', open_paren + 1)
                stop = length if close < 0 else close + len(delimiter) + 2
                out.append('""')
                out.append("\n" * text.count("\n", index, stop))
                index = stop
                continue
        if char == '"':
            index += 1
            while index < length and text[index] != '"':
                index += 2 if text[index] == "\\" else 1
            out.append('""')
            index += 1
            continue
        if char == "'":
            previous = text[index - 1] if index else " "
            if not (previous.isalnum() or previous in "_'"):
                index += 1
                while index < length and text[index] != "'":
                    index += 2 if text[index] == "\\" else 1
                out.append("''")
                index += 1
                continue
        out.append(char)
        index += 1
    return "".join(out)


def normalize(text: str) -> str:
    """Collapse all whitespace runs to one space and strip."""
    return " ".join(text.split())


def matching(text: str, start: int, opening: str, closing: str) -> int:
    """Index of the delimiter closing the one at `start`, or -1."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    return -1


def top_level(text: str, target: str) -> int:
    """Index of `target` outside every `<>`, `()` and `[]`, or -1."""
    depth = 0
    for index, char in enumerate(text):
        if char == target and depth == 0:
            return index
        if char in "<([":
            depth += 1
        elif char in ">)]":
            depth = max(0, depth - 1)
    return -1


def mask_operators(text: str) -> Tuple[str, Dict[str, str]]:
    """Replace `operator|`-style names with one word, keeping a token -> display map.

    `operator|` becomes `operator_pipe` and `operator co_await` becomes `operator_co_await`, so a
    canonical name never contains a space or an `=` that could be read as a field separator.
    """
    display: Dict[str, str] = {}

    def replace(found: "re.Match[str]") -> str:
        word = _OPERATOR_WORDS.get(found.group(1))
        token = "operator_" + (word or "symbol")
        display[token] = token
        return token

    return _OPERATOR_RE.sub(replace, text), display


def strip_prefixes(text: str) -> str:
    """Remove leading `template<...>` and `[[attribute]]` noise from a declaration header."""
    current = text.strip()
    while True:
        if current.startswith("template"):
            rest = current[len("template") :].lstrip()
            if not rest.startswith("<"):
                break
            close = matching(rest, 0, "<", ">")
            if close < 0:
                break
            current = rest[close + 1 :].lstrip()
            continue
        if current.startswith("[["):
            close = current.find("]]")
            if close < 0:
                break
            current = current[close + 2 :].lstrip()
            continue
        break
    return current


def qualified(namespace: Sequence[str], name: str) -> str:
    """`tessera::ui::Node` from the enclosing namespace parts and a local name."""
    return "::".join([part for part in namespace if part] + [name])


def name_before(text: str, index: int) -> Optional[str]:
    """The identifier ending at `index`, or None when there is none or it is a keyword."""
    found = _TRAILING_IDENT_RE.search(text[:index].rstrip())
    if found is None or found.group(1) in NOT_A_NAME:
        return None
    return found.group(1)


def is_member_definition(text: str, name: str, index: int) -> bool:
    """True when the identifier ending at `index` is preceded by `::` (an out-of-class member)."""
    prefix = text[:index].rstrip()
    return prefix[: len(prefix) - len(name)].rstrip().endswith("::")


def value_declaration(masked: str) -> Optional[Tuple[str, str]]:
    """`(kind, name)` for a namespace-scope variable or constant, or None."""
    assign = top_level(masked, "=")
    if assign < 0:
        return None
    name = name_before(masked, assign)
    if name is None or is_member_definition(masked, name, assign):
        return None
    words = set(_WORD_RE.findall(masked))
    kind = "constant" if words & {"constexpr", "const", "consteval"} else "variable"
    return (kind, name)


def function_declaration(masked: str) -> Optional[str]:
    """The name of a free function declaration or definition, or None."""
    paren = top_level(masked, "(")
    if paren < 0:
        return None
    name = name_before(masked, paren)
    if name is None or is_member_definition(masked, name, paren):
        return None
    return name


def classify_brace(header: str) -> Tuple[str, str]:
    """Classify the header of a `{`: `(scope, payload)`.

    `scope` is `namespace`, `type`, `concept`, `function`, `value` or `body`. One `{` can open a
    namespace, a type definition, a concept, a function body or a brace-initialized variable; only
    those first four name something, and everything else is `body` (pushed, never recorded).
    """
    if header == "namespace" or header.startswith("namespace "):
        found = _NAMESPACE_RE.match(header)
        return ("namespace", found.group(1) if found else "")
    stripped = strip_prefixes(header)
    # `struct alignas(64) Vertex2D {` names `Vertex2D`, not `alignas`. The specifier is dropped
    # before matching so the recorded declaration is the type, not the alignment. A type may carry
    # more than one (`alignas(16) alignas(32)`), so every leading specifier is removed.
    while True:
        without = _ALIGNAS_SPECIFIER_RE.sub("", stripped, count=1)
        if without == stripped:
            break
        stripped = without
    found = _TYPE_RE.match(stripped)
    if found is not None:
        keyword, inner, name = found.group(1), found.group(2), found.group(3)
        scoped_enum = keyword == "enum" and inner is not None
        if (inner is None or scoped_enum) and keyword in TYPE_KINDS:
            if not stripped[found.end() :].lstrip().startswith("::"):
                return ("type", "{} {}".format(keyword, name))
    concept = _CONCEPT_RE.search(stripped)
    if concept is not None:
        return ("concept", concept.group(1))
    masked, display = mask_operators(stripped)
    value = value_declaration(masked)
    if value is not None:
        return ("value", value[1])
    function = function_declaration(masked)
    if function is not None:
        return ("function", display.get(function, function))
    return ("body", "")


def classify_semicolon(header: str) -> Optional[Tuple[str, str]]:
    """Classify a `;` header: `(kind, name)` for a namespace-scope declaration, or None."""
    if not header:
        return None
    found = _NAMESPACE_ALIAS_RE.match(header)
    if found is not None:
        return ("alias", found.group(1))
    if header == "namespace" or header.startswith("namespace "):
        # An opaque-enum declaration, or one the scanner already consumed; nothing to record.
        return None
    stripped = strip_prefixes(header)
    if stripped.startswith("using namespace "):
        return None
    found = _TYPE_RE.match(stripped)
    if found is not None:
        if found.group(2) is not None:
            return None
        if stripped[found.end() :].lstrip().startswith("::"):
            return None  # `class Ring::const_iterator;`: a member, not a namespace-scope type
        return (found.group(1), found.group(3))
    found = _USING_ALIAS_RE.match(stripped)
    if found is not None:
        return ("alias", found.group(1))
    found = _USING_DECL_RE.match(stripped)
    if found is not None:
        return ("alias", normalize(found.group(1)).split("::")[-1])
    concept = _CONCEPT_RE.search(stripped)
    if concept is not None:
        return ("concept", concept.group(1))
    masked, display = mask_operators(stripped)
    value = value_declaration(masked)
    if value is not None:
        return value
    function = function_declaration(masked)
    if function is not None:
        return ("function", display.get(function, function))
    # `extern int global_count;`: no initializer and no parameter list, but a type and a name, so
    # this is still a namespace-scope variable. Two words minimum, so a stray token is not one.
    words = _WORD_RE.findall(masked)
    name = name_before(masked, len(masked))
    if name is not None and len(words) > 1 and not is_member_definition(masked, name, len(masked)):
        return ("variable", name)
    return None


def header_declarations(text: str) -> List[Tuple[str, str]]:
    """Every namespace-scope declaration of one header body, as `(kind, name)`; may repeat.

    A declaration inside a type body or a function body is a member and is not recorded. `scopes`
    holds `(kind, namespace_depth_before)`, so closing a brace restores exactly the namespace stack
    its `{` saw (`namespace a::b {` pushes two parts and must pop two). `{` inside `(...)` or
    `[...]` is a brace-initialized default argument (`Limits limits = {}`), never a body, so those
    depths defer every decision.
    """
    body = _PREPROCESSOR_RE.sub("", strip_cpp(text))
    found: List[Tuple[str, str]] = []
    scopes: List[Tuple[str, int]] = []
    namespaces: List[str] = []
    pending: List[str] = []
    parens = 0
    brackets = 0

    def at_namespace_scope() -> bool:
        return all(scope[0] == "namespace" for scope in scopes)

    def record(declaration: Tuple[str, str]) -> None:
        kind, name = declaration
        if name and "::" not in name:
            found.append((kind, qualified(namespaces, name)))

    for char in body:
        if char == "(":
            parens += 1
        elif char == ")":
            parens = max(0, parens - 1)
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets = max(0, brackets - 1)
        flat = parens == 0 and brackets == 0
        if char == "{" and flat:
            header = normalize("".join(pending))
            pending = []
            scope, payload = classify_brace(header)
            namespace_scope = at_namespace_scope()
            scopes.append((scope, len(namespaces)))
            if scope == "namespace":
                namespaces.extend(part for part in payload.split("::") if part)
            elif namespace_scope and payload:
                if scope == "type":
                    keyword, _, name = payload.partition(" ")
                    found.append((keyword, qualified(namespaces, name)))
                elif scope == "concept":
                    record(("concept", payload))
                elif scope == "function":
                    record(("function", payload))
                else:
                    masked = mask_operators(strip_prefixes(header))[0]
                    record(value_declaration(masked) or ("variable", payload))
        elif char == "}" and flat:
            pending = []
            if scopes:
                _, depth = scopes.pop()
                del namespaces[depth:]
        elif char == ";" and flat:
            header = normalize("".join(pending))
            pending = []
            if not at_namespace_scope():
                continue
            declaration = classify_semicolon(header)
            if declaration is not None:
                record(declaration)
        else:
            pending.append(char)
    return found
