#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-tests for scripts/check_version_policy.py (the versioning-policy gate, issue #194).

    python -B -m unittest discover -s scripts/tests -p "test_check_version_policy.py" -v

Each test builds a throwaway repository holding one small versioned surface per gate input -- the
RHI contract header and ``version.hpp``, the IR spec and ``kIrVersion``, the ``tessera.json`` spec
and the accepted ``schemaVersion`` in the loader -- records it with ``--update``, then mutates
exactly one thing. That is the whole contract of the gate, so the cases below are the behaviours the
acceptance in issue #194 actually needs:

* a breaking change to each of the three surfaces with the version unchanged fails (the acceptance),
* a version bump alone does not pass: the reviewed change has to be recorded too,
* ``--update`` refuses an unversioned breaking change, so it cannot launder the failure,
* an additive optional field needs no bump and is recorded,
* a doc-comment edit on the RHI headers is not a contract change (or the gate would be too noisy to
  trust), while any declaration edit is,
* the output is byte-identical across runs (the cheap-and-deterministic constraint), and
* a surface input that cannot be read is a setup error, not a traceback.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = TESTS_DIR.parent
GATE = SCRIPTS_DIR / "check_version_policy.py"
# The gate is split by responsibility (surface reading, baseline I/O, the rules, the command), and
# the fixture repository must hold all four or it is not testing the shipped gate.
GATE_MODULES = (
    SCRIPTS_DIR / "check_version_policy.py",
    SCRIPTS_DIR / "version_surface.py",
    SCRIPTS_DIR / "version_baseline.py",
    SCRIPTS_DIR / "version_policy_rules.py",
)

RHI_TYPES = """\
#pragma once

namespace tessera::rendering::rhi {

// A comment that is not part of the contract.
enum class BlendMode { None, Alpha };

struct DeviceDesc {
    int device_index = -1;
};

} // namespace tessera::rendering::rhi
"""

RHI_VERSION = """\
#pragma once

namespace tessera::rendering::rhi {

inline constexpr std::uint32_t kRhiVersionMajor = 1;
inline constexpr std::uint32_t kRhiVersionMinor = 4;

} // namespace tessera::rendering::rhi
"""

IR_MODULE = """\
#pragma once

namespace tessera::compiler::ir {

inline constexpr int kIrVersion = 1;

} // namespace tessera::compiler::ir
"""

IR_SPEC = """\
# IR v1

## 1. Top level

| Field | Type | Required | Notes |
|---|---|---|---|
| `irVersion` | integer | yes | Must be `1` |
| `components` | Component[] | yes | Deterministic order |
| `types` | RecordType[] | no | Declared record types |
"""

TESSERA_JSON_LOADER = """\
// SPDX-License-Identifier: MIT

const JsonValue* schema = config->find("schemaVersion");
if (schema == nullptr || schema->as_integer() != std::optional<std::int64_t>(1)) {
    return fail(AppConfigErrorCode::ConfigInvalid, "schemaVersion must be 1");
}
"""

TESSERA_JSON_SPEC = """\
# tessera.json

## Top-level

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `schemaVersion` | integer | yes | \u2014 | Must be `1` |
| `app` | object | yes | \u2014 | Application identity |

## `window`

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `scale` | string | no | `auto` | `auto` or a number |
| `resizable` | boolean | no | `true` | |
"""

IR_TYPES_ROW = "| `types` | RecordType[] | no | Declared record types |"


class VersionPolicyGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = Path(self._temp.name) / "repo"
        (self.repo / "scripts").mkdir(parents=True)
        for module in GATE_MODULES:
            shutil.copy2(module, self.repo / "scripts" / module.name)
        self.write("include/tessera/rendering/rhi/types.hpp", RHI_TYPES)
        self.write("include/tessera/rendering/rhi/version.hpp", RHI_VERSION)
        self.write("src/compiler/ir/include/tessera/compiler/ir/module.hpp", IR_MODULE)
        self.write("src/harness/manifest_loader.cpp", TESSERA_JSON_LOADER)
        self.write("docs/specs/ir-v1.md", IR_SPEC)
        self.write("docs/specs/ir-v1-expr.md", "# IR v1 part 2\n")
        self.write("docs/specs/project-config-v1.md", TESSERA_JSON_SPEC)
        self.write("docs/specs/runtime-config-v1.md", "# runtime\n")
        self.write("docs/specs/diagnostics-config-v1.md", "# diagnostics\n")
        recorded = self.run_gate("--update")
        if recorded.returncode != 0:
            raise AssertionError("fixture recording failed:\n" + recorded.stdout + recorded.stderr)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, relative: str, text: str) -> None:
        """Write a fixture file with LF newlines, so a comparison never depends on the host OS."""
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text.replace("\r\n", "\n"), encoding="utf-8", newline="\n")

    def read(self, relative: str) -> str:
        return (self.repo / relative).read_text(encoding="utf-8")

    def replace(self, relative: str, old: str, new: str) -> None:
        text = self.read(relative)
        self.assertIn(old, text, "fixture text not found in {}".format(relative))
        self.write(relative, text.replace(old, new, 1))

    def run_gate(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(self.repo / "scripts" / GATE.name), "--repo-root",
             str(self.repo), *args],
            capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=str(self.repo),
        )

    def assert_fails(self, result: subprocess.CompletedProcess) -> None:
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    # --- the recorded tree ----------------------------------------------------------------

    def test_recorded_fixture_passes(self) -> None:
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 error(s), 0 warning(s), 3 surface(s) checked", result.stdout)
        self.assertIn("version\tir\t1", self.read("scripts/versioning-baseline.txt"))
        self.assertIn("version\trhi\t1.4", self.read("scripts/versioning-baseline.txt"))
        self.assertIn("version\ttessera.json\t1", self.read("scripts/versioning-baseline.txt"))

    # --- the acceptance: one unversioned break per surface --------------------------------

    def test_unversioned_rhi_change_fails_and_is_deterministic(self) -> None:
        # Removing a public enumerator narrows the contract: breaking, so the version must move.
        self.replace("include/tessera/rendering/rhi/types.hpp",
                     "enum class BlendMode { None, Alpha };", "enum class BlendMode { Alpha };")
        first = self.run_gate("--check")
        second = self.run_gate("--check")
        self.assert_fails(first)
        self.assertIn("ERROR include/tessera/rendering/rhi/version.hpp:", first.stdout)
        self.assertIn("the RHI source contract changed but", first.stdout)
        self.assertIn("kRhiVersionMinor for an additive change", first.stdout)
        self.assertIn("is unchanged (1.4)", first.stdout)
        # Two runs of the same tree are byte-identical: the gate is safe to run on every PR.
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(first.stderr, second.stderr)

    def test_comment_only_rhi_change_needs_no_bump(self) -> None:
        # The contract is the declarations, not the prose around them; otherwise the gate would
        # force a version move for every clarifying comment and authors would record blindly.
        self.replace("include/tessera/rendering/rhi/types.hpp",
                     "// A comment that is not part of the contract.",
                     "// A reworded comment.")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unversioned_ir_field_removal_fails(self) -> None:
        self.replace("docs/specs/ir-v1.md", IR_TYPES_ROW + "\n", "")
        result = self.run_gate("--check")
        self.assert_fails(result)
        self.assertIn("ERROR docs/specs/ir-v1.md:", result.stdout)
        self.assertIn("breaking IR schema change without a version bump", result.stdout)
        self.assertIn("stays 1", result.stdout)
        self.assertIn("removed `ir-v1::1. Top level::types`", result.stdout)

    def test_unversioned_tessera_json_break_fails(self) -> None:
        self.replace("docs/specs/project-config-v1.md",
                     "| `scale` | string | no | `auto` |",
                     "| `scale` | number | no | `auto` |")
        self.replace("docs/specs/project-config-v1.md",
                     "| `resizable` | boolean | no | `true` |",
                     "| `resizable` | boolean | no | `false` |")
        result = self.run_gate("--check")
        self.assert_fails(result)
        self.assertIn("ERROR docs/specs/project-config-v1.md:", result.stdout)
        self.assertIn("breaking `tessera.json` schema change without a version bump",
                      result.stdout)
        self.assertIn("changed type from `string` to `number`", result.stdout)
        self.assertIn("changed default from `true` to `false`", result.stdout)
        self.assertIn("schemaVersion", result.stdout)

    def test_ir_type_and_required_ness_changes_without_a_bump_fail(self) -> None:
        self.replace("docs/specs/ir-v1.md", "| `components` | Component[] | yes |",
                     "| `components` | Component | yes |")
        self.replace("docs/specs/ir-v1.md", IR_TYPES_ROW,
                     "| `types` | RecordType[] | yes | Declared record types |")
        result = self.run_gate("--check")
        self.assert_fails(result)
        self.assertIn("changed type from `Component[]` to `Component`", result.stdout)
        self.assertIn("changed required-ness from `no` to `yes`", result.stdout)

    # --- a version move is necessary, and never backwards --------------------------------

    def test_version_moves_need_recording_and_never_go_backwards(self) -> None:
        # A bump without a recording is still a failure: the baseline and the bump ship together.
        self.replace("src/compiler/ir/include/tessera/compiler/ir/module.hpp",
                     "kIrVersion = 1", "kIrVersion = 2")
        self.replace("docs/specs/ir-v1.md", IR_TYPES_ROW + "\n", "")
        result = self.run_gate("--check")
        self.assert_fails(result)
        self.assertIn("breaking IR schema change (version 1 -> 2)", result.stdout)
        self.assertIn("--update", result.stdout)
        recorded = self.run_gate("--update")
        self.assertEqual(recorded.returncode, 0, recorded.stdout + recorded.stderr)
        self.assertEqual(self.run_gate("--check").returncode, 0)
        # A version that goes backwards is an error that recording cannot fix.
        self.replace("include/tessera/rendering/rhi/version.hpp",
                     "kRhiVersionMinor = 4", "kRhiVersionMinor = 3")
        backwards = self.run_gate("--check")
        self.assert_fails(backwards)
        self.assertIn("moved from 1.4 to 1.3 without increasing", backwards.stdout)

    # --- recording cannot launder a break, and additive changes still record ------------

    def test_update_refuses_a_break_and_records_an_additive_change(self) -> None:
        self.replace("docs/specs/project-config-v1.md",
                     "| `scale` | string | no | `auto` |",
                     "| `scale` | number | no | `auto` |")
        before = self.read("scripts/versioning-baseline.txt")
        refused = self.run_gate("--update")
        self.assert_fails(refused)
        self.assertIn("refusing --update", refused.stderr)
        self.assertEqual(self.read("scripts/versioning-baseline.txt"), before,
                         "--update must not rewrite the baseline when it refuses")
        # An additive optional field keeps the version and only needs recording.
        self.write("docs/specs/project-config-v1.md", TESSERA_JSON_SPEC.replace(
            "| `resizable` | boolean | no | `true` | |",
            "| `resizable` | boolean | no | `true` | |\n| `gamma` | string | no | `srgb` | |"))
        additive = self.run_gate("--check")
        self.assert_fails(additive)
        self.assertIn("additive `tessera.json` schema change, no version bump needed",
                      additive.stdout)
        self.assertIn("added optional `project-config-v1::`window`::gamma`", additive.stdout)
        recorded = self.run_gate("--update")
        self.assertEqual(recorded.returncode, 0, recorded.stdout + recorded.stderr)
        self.assertEqual(self.run_gate("--check").returncode, 0)

    def test_unreadable_surface_input_is_a_setup_error(self) -> None:
        (self.repo / "docs/specs/ir-v1.md").write_bytes(b"# IR\n\n\xff not utf-8\n")
        result = self.run_gate("--check")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not valid UTF-8", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
