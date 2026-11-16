#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rule tests for the release-manifest tooling (issue #193), no filesystem.

    python -B -m unittest discover -s scripts/tests -p "test_*.py" -v

These cases cover the pure policy half -- SemVer precedence, the deprecation window, the channel
direction rule and the update decision -- because those are the rules a wrong answer would turn into
a shipped mistake. They call the functions directly and touch no file, no clock and no socket, which
is also why they are fast enough to run on every push.

Fixtures (constants, builders, the throwaway repository) live in ``release_fixtures.py``.
"""

from __future__ import annotations

import json
import unittest
from typing import Any

from release_fixtures import CHANNELS, COMMIT, PLATFORM, ledger_with, release_record, symbols_text

import release_client as client
import release_manifest as model
import release_policy as policy
from release_semver import SemVer, deprecation_window


class SymbolArtifactTests(unittest.TestCase):
    """The ledger's contract with `crash_symbols.hpp`, checked from the Python side.

    The producer is C++ (`read_linker_map_symbols` / `read_elf_symbols`) and cannot run here, so this
    covers the other half: a release's manifest is only accepted when the symbol file is the format
    the resolver reads and names the same build. The fixture text is what `write_symbol_file`
    emits, including `func=<module index>|...` rather than the module name.
    """

    def test_valid_artifact_is_parsed(self) -> None:
        header = model.parse_symbol_header(symbols_text())
        self.assertEqual(header["build"], COMMIT)
        self.assertEqual(header["modules"], 1)
        self.assertEqual(header["functions"], 3)

    def test_schema_line_is_required(self) -> None:
        with self.assertRaises(model.ManifestError) as raised:
            model.parse_symbol_header("build=" + COMMIT + "\nfunc=0|1|2|x\n")
        self.assertIn("tessera.symbols.v1", str(raised.exception))

    def test_missing_build_id_is_refused(self) -> None:
        with self.assertRaises(model.ManifestError) as raised:
            model.parse_symbol_header("tessera.symbols.v1\nfunc=0|1|2|x\n")
        self.assertIn("build=", str(raised.exception))

    def test_artifact_that_names_nothing_is_refused(self) -> None:
        text = "tessera.symbols.v1\nbuild={}\nmodule=app.exe|0x0|0x1000\n".format(COMMIT)
        with self.assertRaises(model.ManifestError) as raised:
            model.parse_symbol_header(text)
        self.assertIn("no func=", str(raised.exception))

    def test_malformed_func_row_is_refused(self) -> None:
        text = "tessera.symbols.v1\nbuild={}\nfunc=0|1|name\n".format(COMMIT)
        with self.assertRaises(model.ManifestError):
            model.parse_symbol_header(text)

    def test_build_id_must_be_a_full_commit(self) -> None:
        text = "tessera.symbols.v1\nbuild=79fff8\nfunc=0|1|2|x\n"
        with self.assertRaises(model.ManifestError) as raised:
            model.parse_symbol_header(text)
        self.assertIn("40-hex", str(raised.exception))


class SemVerTests(unittest.TestCase):
    def test_precedence_follows_semver(self) -> None:
        ordered = ["1.0.0-beta.2", "1.0.0-beta.10", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0"]
        parsed = [SemVer.parse(text) for text in ordered]
        self.assertTrue(all(parsed), "every version parses")
        for left, right in zip(parsed, parsed[1:]):
            self.assertLess(left.compare(right), 0, "{} < {}".format(left, right))

    def test_release_outranks_its_own_prerelease(self) -> None:
        self.assertGreater(
            SemVer.parse("1.0.0").compare(SemVer.parse("1.0.0-rc.9")),
            0,
            "a release is newer than every pre-release of the same version",
        )

    def test_bump_class(self) -> None:
        self.assertEqual(SemVer.parse("1.2.3").bump_class(SemVer.parse("1.2.4")), "patch")
        self.assertEqual(SemVer.parse("1.2.3").bump_class(SemVer.parse("1.3.0")), "minor")
        self.assertEqual(SemVer.parse("1.2.3").bump_class(SemVer.parse("2.0.0")), "major")
        self.assertEqual(SemVer.parse("1.2.3").bump_class(SemVer.parse("1.2.3")), "none")

    def test_invalid_versions_rejected(self) -> None:
        for text in ("1.2", "1.2.3.4", "v1.2.3", "", "1.2.3-"):
            self.assertIsNone(SemVer.parse(text), text)


class DeprecationWindowTests(unittest.TestCase):
    def test_two_minors_and_ninety_days(self) -> None:
        version_floor, date_floor = deprecation_window(SemVer.parse("1.4.0"), "2026-01-01")
        self.assertEqual(version_floor, "1.6.0")
        self.assertEqual(date_floor, "2026-04-01")

    def test_major_bump_satisfies_the_minor_side(self) -> None:
        deprecated = release_record(
            "2.0.0",
            deprecations=[
                {
                    "entity": "rhi::DeviceDesc::oldFlag",
                    "deprecatedIn": "1.4.0",
                    "deprecatedDate": "2026-01-01",
                    "removedIn": "2.0.0",
                }
            ],
        )
        findings = policy.check_ledger(ledger_with(release_record("1.4.0"), deprecated), CHANNELS)
        self.assertEqual([f for f in findings if f.severity == "error"], [])

    def test_early_removal_is_an_error(self) -> None:
        deprecated = release_record(
            "1.5.0",
            deprecations=[
                {
                    "entity": "rhi::DeviceDesc::oldFlag",
                    "deprecatedIn": "1.4.0",
                    "deprecatedDate": "2026-01-01",
                    "removedIn": "1.5.0",
                }
            ],
        )
        findings = policy.check_ledger(ledger_with(deprecated), CHANNELS)
        errors = [f for f in findings if f.rule == "ledger.deprecation-window"]
        self.assertEqual(len(errors), 1, [f.format() for f in findings])
        self.assertIn("1.6.0", errors[0].message)

    def test_removal_before_the_ninety_day_floor_is_an_error(self) -> None:
        # Two minors pass but only forty days elapse: the date side must still refuse.
        deprecating = release_record(
            "1.4.0",
            day="2026-01-01",
            deprecations=[
                {
                    "entity": "rhi::DeviceDesc::oldFlag",
                    "deprecatedIn": "1.4.0",
                    "deprecatedDate": "2026-01-01",
                    "removedIn": "1.6.0",
                }
            ],
        )
        removing = release_record("1.6.0", day="2026-02-10")
        findings = policy.check_ledger(ledger_with(deprecating, removing), CHANNELS)
        errors = [f for f in findings if f.rule == "ledger.deprecation-window"]
        self.assertEqual(len(errors), 1, [f.format() for f in findings])
        self.assertIn("2026-04-01", errors[0].message)


class ChannelPolicyTests(unittest.TestCase):
    def test_valid_channel_file_passes(self) -> None:
        findings = policy.check_channels(CHANNELS)
        self.assertEqual(
            [f for f in findings if f.severity == "error"], [], [f.format() for f in findings]
        )
        # The unsigned state is a warning, deliberately: a real key needs a certificate this
        # repository does not have.
        self.assertEqual([f.rule for f in findings], ["channels.signing"])

    def test_missing_transition_row_is_an_error(self) -> None:
        broken = json.loads(json.dumps(CHANNELS))
        broken["transitions"] = [row for row in broken["transitions"] if row["to"] != "beta"]
        findings = policy.check_channels(broken)
        self.assertTrue(
            any(f.rule == "channels.transition" for f in findings), [f.format() for f in findings]
        )

    def test_direction_rule_is_enforced(self) -> None:
        broken = json.loads(json.dumps(CHANNELS))
        broken["transitions"][0]["policy"] = "free"  # stable -> beta must be optIn
        findings = policy.check_channels(broken)
        self.assertTrue(
            any(f.rule == "channels.transition-direction" for f in findings),
            [f.format() for f in findings],
        )

    def test_url_in_host_list_is_an_error(self) -> None:
        broken = json.loads(json.dumps(CHANNELS))
        broken["updateHosts"] = ["https://updates.example.com/app"]
        findings = policy.check_channels(broken)
        self.assertTrue(any(f.rule == "channels.hosts" for f in findings))

    def test_unsigned_state_warns_but_does_not_fail(self) -> None:
        findings = policy.check_channels(CHANNELS)
        self.assertEqual([f for f in findings if f.severity == "error"], [])
        self.assertTrue(any(f.rule == "channels.signing" for f in findings))

    def test_missing_stable_channel_is_an_error(self) -> None:
        broken = json.loads(json.dumps(CHANNELS))
        broken["channels"] = [channel for channel in broken["channels"] if channel["id"] != "stable"]
        findings = policy.check_channels(broken)
        self.assertTrue(any(f.rule == "channels.required" for f in findings))


class PlanUpdateTests(unittest.TestCase):
    def plan(self, **kwargs: Any) -> client.UpdateDecision:
        ledger = kwargs.pop("ledger", None) or ledger_with(release_record("1.0.0"))
        base = dict(
            installed_channel="beta",
            installed_version="0.0.0",
            platform=PLATFORM,
        )
        base.update(kwargs)
        return client.plan_update(CHANNELS, ledger, **base)

    def test_unsigned_release_is_never_offered(self) -> None:
        decision = self.plan()
        self.assertTrue(decision.blocked, decision)
        self.assertEqual(decision.reason, "manifests.shape")

    def test_signed_and_verified_release_is_offered(self) -> None:
        ledger = ledger_with(release_record("1.0.0", signature="c" * 128))
        decision = self.plan(ledger=ledger, signature_verified=True)
        self.assertTrue(decision.offered, decision)
        self.assertEqual(decision.version, "1.0.0")
        self.assertEqual(decision.apply_mode, "prompt")
        self.assertEqual(decision.symbols_artifact, "tessera-1.0.0-windows-x86_64.symbols.txt")

    def test_channel_move_to_less_stable_needs_consent(self) -> None:
        ledger = ledger_with(release_record("1.0.0", signature="c" * 128))
        refused = self.plan(
            ledger=ledger,
            installed_channel="stable",
            target_channel="beta",
            signature_verified=True,
        )
        self.assertTrue(refused.blocked)
        self.assertEqual(refused.reason, "channels.transition-direction")
        allowed = self.plan(
            ledger=ledger,
            installed_channel="stable",
            target_channel="beta",
            consent=True,
            signature_verified=True,
        )
        self.assertTrue(allowed.offered, allowed)

    def test_promotion_is_free(self) -> None:
        ledger = ledger_with(release_record("1.0.0", channel="stable", signature="c" * 128))
        decision = self.plan(ledger=ledger, target_channel="stable", signature_verified=True)
        self.assertTrue(decision.offered, decision)

    def test_downgrade_is_blocked(self) -> None:
        ledger = ledger_with(release_record("1.0.0", signature="c" * 128))
        decision = self.plan(ledger=ledger, installed_version="2.0.0", signature_verified=True)
        self.assertTrue(decision.blocked)
        self.assertEqual(decision.reason, "channels.downgrade")
        self.assertTrue(decision.downgrade)

    def test_same_version_is_up_to_date(self) -> None:
        ledger = ledger_with(release_record("1.0.0", signature="c" * 128))
        decision = self.plan(ledger=ledger, installed_version="1.0.0", signature_verified=True)
        self.assertEqual(decision.kind, "up-to-date")

    def test_unknown_channel_is_blocked(self) -> None:
        decision = self.plan(target_channel="canary")
        self.assertTrue(decision.blocked)
        self.assertEqual(decision.reason, "channels.id")

    def test_platform_without_an_artifact_is_up_to_date(self) -> None:
        decision = self.plan(ledger=ledger_with(release_record("1.0.0")), platform="linux-x86_64")
        self.assertEqual(decision.kind, "up-to-date")

    def test_apply_refuses_with_unsaved_work(self) -> None:
        ledger = ledger_with(release_record("1.0.0", signature="c" * 128))
        decision = self.plan(ledger=ledger, signature_verified=True)
        allowed, why = client.apply_allowed(decision, unsaved_work_active=True)
        self.assertFalse(allowed)
        self.assertIn("unsaved", why)
        allowed, why = client.apply_allowed(decision, unsaved_work_active=False)
        self.assertTrue(allowed, why)

    def test_rollback_after_two_failed_launches(self) -> None:
        self.assertFalse(client.rollback_after(1))
        self.assertTrue(client.rollback_after(2))

    def test_state_machine_is_walkable_offline(self) -> None:
        path = client.state_path()
        for source, target in zip(path, path[1:]):
            self.assertTrue(client.can_transition(source, target), "{} -> {}".format(source, target))
        self.assertTrue(client.can_transition("HealthCheck", "Rollback"))
        self.assertFalse(client.can_transition("Idle", "Applying"))


if __name__ == "__main__":
    unittest.main()
