#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for the public-API surface gate (issue #76).

    python -B -m unittest discover -s scripts/tests -p "test_check_public_api.py" -v

Two halves, deliberately split the way `test_check_source_size.py` and
`test_preview_budget_ratchet.py` split theirs:

* the **classifier** is imported and driven directly (`public_api_surface`), because the interesting
  cases -- a renamed type, a nested type that must not be recorded, a default argument that looks
  like a body -- are three lines of header text, not three files;
* the **ratchet** is driven through the CLI in a throwaway repository, because what matters there is
  the exit code and the refusal to write, which only a real process proves.

The cases are the ones that decide whether the gate is worth having:

* unchanged surface passes, an added symbol passes once recorded, and an added symbol fails until it
  is recorded (a gate that never fires is a no-op);
* a removed header, a removed type and a removed prop/part all fail, each naming the line;
* a **rename** fails as one removal plus one addition, so it cannot pass as "additive";
* `--update` records an addition but refuses to drop a line and writes nothing when it does;
* a nested type, a class member, a default argument and a preprocessor branch are recorded or not
  exactly as the docstring promises (this is what keeps the gate off the "turned off" path);
* two extractions of one tree are byte-identical (determinism, no timestamps);
* the committed baseline is itself canonical, so CI cannot be red on `main` for a formatting reason.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
REPO_ROOT = SCRIPTS_DIR.parent
GATE = SCRIPTS_DIR / "check_public_api.py"

sys.path.insert(0, str(SCRIPTS_DIR))
import public_api_surface as surface  # noqa: E402  (sibling script, stdlib only)


def names(lines) -> set:
    """The `(kind, name)` pairs of a list of canonical lines, ignoring the path."""
    return {(line.split(" ", 2)[0], line.split(" ", 2)[2]) for line in lines if line.count(" ") > 1}


class ClassifyTests(unittest.TestCase):
    """The scanner directly: what one header contributes."""

    def declarations(self, text: str) -> set:
        return {(kind, name) for kind, name in surface.header_declarations(textwrap.dedent(text))}

    def test_namespace_scope_types_keep_their_kind_and_qualified_name(self) -> None:
        found = self.declarations(
            """
            namespace tessera::ui {

            class Node {
            public:
                void paint();
            };

            struct Rect { int x; };
            enum class Dirty : unsigned char { Clean, Style };
            using NodeId = unsigned long long;

            } // namespace tessera::ui
            """
        )
        self.assertEqual(
            found,
            {
                ("class", "tessera::ui::Node"),
                ("struct", "tessera::ui::Rect"),
                ("enum", "tessera::ui::Dirty"),
                ("alias", "tessera::ui::NodeId"),
            },
        )

    def test_an_alignment_specifier_is_not_the_type_name(self) -> None:
        # `struct alignas(64) Vertex2D {` names `Vertex2D`. Reading `alignas` as the name recorded
        # a phantom type and dropped the real one, which the baseline gate then reported as a
        # removed declaration (a breaking change) rather than as the parser bug it was.
        found = self.declarations(
            """
            namespace tessera::rendering::gpu {

            struct alignas(64) Vertex2D { float x; };
            struct alignas(16) alignas(32) OverAligned { float x; };

            } // namespace tessera::rendering::gpu
            """
        )
        self.assertEqual(
            found,
            {
                ("struct", "tessera::rendering::gpu::Vertex2D"),
                ("struct", "tessera::rendering::gpu::OverAligned"),
            },
        )

    def test_free_functions_and_constants_are_recorded(self) -> None:
        found = self.declarations(
            """
            namespace tessera::hal {
            [[nodiscard]] constexpr bool has_flag(Dirty value, Dirty flag) { return true; }
            inline constexpr std::size_t kMaxBytes = 20'000;
            extern int global_count;
            }
            """
        )
        self.assertIn(("function", "tessera::hal::has_flag"), found)
        self.assertIn(("constant", "tessera::hal::kMaxBytes"), found)
        self.assertIn(("variable", "tessera::hal::global_count"), found)

    def test_members_nested_types_and_enumerators_are_not_recorded(self) -> None:
        # The under-promise: only declaration-level facts. A member, a nested type and an enumerator
        # are exactly the churn the gate must ignore, or every edit would be "breaking".
        found = self.declarations(
            """
            namespace tessera::chart {
            class Ring final {
            public:
                struct Span { int a; };
                class const_iterator;
                void push(int value);
                int count() const;
            private:
                int count_ = 0;
            };
            class Ring::const_iterator { int i; };
            enum class Mode { Fast, Slow };
            }
            """
        )
        self.assertEqual(
            found,
            {("class", "tessera::chart::Ring"), ("enum", "tessera::chart::Mode")},
        )
        flat = {" ".join(entry) for entry in found}
        self.assertNotIn("Span", " ".join(flat))
        self.assertNotIn("push", " ".join(flat))
        self.assertNotIn("const_iterator", " ".join(flat))

    def test_a_default_argument_brace_is_not_a_type_body(self) -> None:
        # `Limits limits = {})` closes a function; a scanner that treated that `{` as a body would
        # lose the rest of the header.
        found = self.declarations(
            """
            namespace tessera::style {
            [[nodiscard]] std::string metrics_json(std::span<const Metric> samples,
                                                   ExportLimits limits = {});
            struct After { int x; };
            }
            """
        )
        self.assertIn(("function", "tessera::style::metrics_json"), found)
        self.assertIn(("struct", "tessera::style::After"), found)

    def test_operator_names_have_no_space_or_symbol(self) -> None:
        found = self.declarations(
            """
            namespace tessera::ui {
            constexpr Dirty operator|(Dirty lhs, Dirty rhs) { return lhs; }
            constexpr Dirty& operator|=(Dirty& lhs, Dirty rhs) { return lhs; }
            constexpr bool operator==(Dirty a, Dirty b) { return true; }
            }
            """
        )
        functions = {name for kind, name in found if kind == "function"}
        self.assertEqual(
            functions,
            {
                "tessera::ui::operator_pipe",
                "tessera::ui::operator_or_assign",
                "tessera::ui::operator_eq",
            },
        )
        for line in functions:
            self.assertNotIn(" ", line)
            self.assertNotIn("=", line)

    def test_disabled_preprocessor_branch_is_still_recorded(self) -> None:
        # Directives are dropped, not evaluated: a declaration compiled in only for one build is
        # still part of the surface, and its removal still has to be reviewed.
        found = self.declarations(
            """
            namespace tessera::diagnostics {
            #if defined(TESSERA_ENABLE_TRACING)
            [[nodiscard]] std::string chrome_trace_json(std::span<const TraceEvent> events);
            #else
            [[nodiscard]] std::string chrome_trace_json(std::span<const TraceEvent> events);
            #endif
            }
            """
        )
        self.assertEqual(found, {("function", "tessera::diagnostics::chrome_trace_json")})

    def test_components_record_the_name_props_and_parts(self) -> None:
        found, warnings = surface.component_declarations(
            textwrap.dedent(
                """
                import { Children } from "tessera";

                export type ButtonProps = {
                  label: string;
                  onPress: () => void;
                  variant?: string;
                };

                export function Button({ label, variant = "primary", children }: ButtonProps) {
                  return <View part="root icon"><Text part="label">{label}</Text></View>;
                }
                """
            )
        )
        self.assertEqual(warnings, [])
        self.assertEqual(
            {(kind, name) for kind, name in found},
            {
                ("component", "Button"),
                ("prop", "Button.label"),
                ("prop", "Button.onPress"),
                ("prop", "Button.variant"),
                ("part", "Button::root"),
                ("part", "Button::icon"),
                ("part", "Button::label"),
            },
        )

    def test_comments_do_not_contribute_surface(self) -> None:
        # Both sources are full of commented-out examples of declarations and of `part="..."`; none of
        # them may leak in, or the baseline would churn on prose edits.
        header = self.declarations(
            """
            namespace tessera::ui {
            // class Removed { int x; };
            /* struct AlsoRemoved { int y; }; */
            class Kept { int z; };
            }
            """
        )
        self.assertIn(("class", "tessera::ui::Kept"), header)
        self.assertNotIn(("class", "tessera::ui::Removed"), header)
        self.assertNotIn(("struct", "tessera::ui::AlsoRemoved"), header)
        tsx, _ = surface.component_declarations(
            'export function Kept() { return <View part="real" />; }\n'
            '// export function Removed() { return <View part="fake" />; }\n'
        )
        self.assertIn(("component", "Kept"), tsx)
        self.assertIn(("part", "Kept::real"), tsx)
        self.assertNotIn(("component", "Removed"), tsx)


class DeterminismTests(unittest.TestCase):
    """Two runs over one tree must be byte-identical: no clock, no path, no version number."""

    def test_two_collections_render_the_same_bytes(self) -> None:
        first, _ = surface.collect_surface(REPO_ROOT)
        second, _ = surface.collect_surface(REPO_ROOT)
        self.assertEqual(surface.render_baseline(first), surface.render_baseline(second))

    def test_rendering_carries_no_absolute_path_or_timestamp(self) -> None:
        text = surface.render_baseline(surface.collect_surface(REPO_ROOT)[0])
        self.assertNotIn(str(REPO_ROOT).replace("\\", "/"), text)
        self.assertNotIn("C:", text)
        for fragment in ("20", "T00:00", "generated on"):
            self.assertNotIn(fragment, text.splitlines()[0])

    def test_lines_are_sorted_by_path_then_kind_then_name(self) -> None:
        lines, _ = surface.collect_surface(REPO_ROOT)
        self.assertEqual(lines, sorted(lines, key=surface.sort_key))

    def test_committed_baseline_is_canonical(self) -> None:
        # If this fails, `main` is red for everyone; run `--update` (it drops no line) and commit.
        committed = (SCRIPTS_DIR / "public-api-baseline.txt").read_text(encoding="utf-8")
        current = surface.render_baseline(surface.collect_surface(REPO_ROOT)[0])
        self.assertEqual(committed, current)


class RatchetTests(unittest.TestCase):
    """The CLI in a throwaway repository: exit codes, messages, and what `--update` refuses."""

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name)
        (self.repo / "scripts").mkdir()
        # The gate plus every module it imports: the fixture is a whole small checkout, so the
        # sibling-import pattern is exercised rather than bypassed.
        for module in ("check_public_api.py", "public_api_surface.py", "public_api_cpp.py"):
            shutil.copy2(SCRIPTS_DIR / module, self.repo / "scripts" / module)
        self.write_header("include/tessera/ui/node.hpp", "class Node { public: int id; };")
        self.write_tsx(
            "packages/tessera-ui/Button.tsx",
            "export type ButtonProps = { label: string };\n"
            "export function Button({ label }: ButtonProps) { return <View part=\"label\" />; }\n",
        )
        self.assertEqual(self.run_gate("--update").returncode, 0)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_header(self, relative: str, body: str, namespace: str = "tessera::ui") -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "// SPDX-License-Identifier: MIT\n#pragma once\n\n"
            "namespace {} {{\n{}\n}} // namespace {}\n".format(namespace, body, namespace),
            encoding="utf-8",
        )

    def write_tsx(self, relative: str, body: str) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")

    def run_gate(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(self.repo / "scripts" / GATE.name), *args],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

    def baseline(self) -> str:
        return (self.repo / "scripts" / "public-api-baseline.txt").read_text(encoding="utf-8")

    def test_unchanged_surface_passes(self) -> None:
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 added, 0 removed", result.stdout)

    def test_added_declaration_fails_until_recorded(self) -> None:
        self.write_header(
            "include/tessera/ui/node.hpp",
            "class Node { public: int id; };\nstruct Extra { int x; };",
        )
        failing = self.run_gate("--check")
        self.assertEqual(failing.returncode, 1, failing.stdout + failing.stderr)
        self.assertIn("struct include/tessera/ui/node.hpp tessera::ui::Extra", failing.stdout)
        self.assertIn("run --update to record it", failing.stdout)
        self.assertEqual(self.run_gate("--update").returncode, 0)
        self.assertEqual(self.run_gate("--check").returncode, 0)

    def test_removed_header_fails_and_names_the_line(self) -> None:
        (self.repo / "include" / "tessera" / "ui" / "node.hpp").unlink()
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("header include/tessera/ui/node.hpp", result.stdout)
        self.assertIn("recorded in the baseline but gone from the tree", result.stdout)

    def test_removed_type_fails(self) -> None:
        self.write_header("include/tessera/ui/node.hpp", "struct Renamed { int id; };")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("class include/tessera/ui/node.hpp tessera::ui::Node", result.stdout)
        self.assertIn("struct include/tessera/ui/node.hpp tessera::ui::Renamed", result.stdout)

    def test_removed_prop_and_part_fail(self) -> None:
        self.write_tsx(
            "packages/tessera-ui/Button.tsx",
            "export type ButtonProps = { label: string };\n"
            "export function Button({ label }: ButtonProps) { return <View />; }\n",
        )
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("part packages/tessera-ui/Button.tsx Button::label", result.stdout)
        self.assertIn("BREAKING", result.stdout)

    def test_rename_is_one_removal_plus_one_addition(self) -> None:
        self.write_header("include/tessera/ui/node.hpp", "class Node2 { public: int id; };")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("1 added, 1 removed", result.stderr)
        # The removal is what makes a rename breaking rather than additive.
        self.assertIn("class include/tessera/ui/node.hpp tessera::ui::Node", result.stdout)
        self.assertIn("class include/tessera/ui/node.hpp tessera::ui::Node2", result.stdout)

    def test_update_refuses_to_drop_a_line_and_writes_nothing(self) -> None:
        (self.repo / "include" / "tessera" / "ui" / "node.hpp").unlink()
        before = self.baseline()
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("refusing to update", result.stderr)
        self.assertIn("nothing was written", result.stderr.replace("Nothing", "nothing"))
        self.assertEqual(self.baseline(), before, "a refused --update must not touch the baseline")

    def test_update_records_additions_but_still_refuses_a_removal(self) -> None:
        # Both at once: the addition must not be written, because the removal makes the whole update
        # a reviewed edit rather than a script run.
        self.write_header("include/tessera/ui/node.hpp", "struct Extra { int x; };")
        before = self.baseline()
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.baseline(), before)

    def test_json_report_lists_both_sides(self) -> None:
        import json

        self.write_header("include/tessera/ui/node.hpp", "class Node2 { public: int id; };")
        result = self.run_gate("--json", "--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertEqual(len(payload["added"]), 1)
        self.assertEqual(len(payload["removed"]), 1)

    def test_hand_edited_baseline_is_reported_not_ignored(self) -> None:
        # Sorting the file by hand (or by a formatter) must not silently pass: the canonical form is
        # part of the file's contract, and --update is the only writer.
        path = self.repo / "scripts" / "public-api-baseline.txt"
        path.write_text(path.read_text(encoding="utf-8") + "\n\n", encoding="utf-8")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not in canonical form", result.stdout)


if __name__ == "__main__":
    unittest.main()
