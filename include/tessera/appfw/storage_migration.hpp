// SPDX-License-Identifier: MIT
//
// Versioned stored data: an ordered, pure migration chain (issue #411, item 1).
//
// A stored entry carries the format version it was written with (see the envelope in
// docs/design/storage-module.md section 5). `MigrationChain` upgrades a payload from that version
// to the version the running build expects, one version at a time: step `N` turns a version-`N`
// payload into a version-`N+1` payload. The chain is pure - bytes in, bytes out, no file system, no
// clock - so every outcome is tested without a disk.
//
// Every way a run can end has a name, and none of them is "guess":
//
//   * `Current`        the payload is already at the target version; nothing ran.
//   * `Migrated`       every step from the stored version to the target ran in order.
//   * `FutureVersion`  the payload is newer than this build understands (the user downgraded). The
//                      payload is not touched and not parsed: reading a layout this build has never
//                      seen is how data gets destroyed. The caller keeps its default and the file.
//   * `MissingStep`    the chain has a gap at `failed_at`; a build that ships a format bump without
//                      its step is a bug, reported rather than papered over.
//   * `StepFailed`     a step rejected its input at `failed_at`; the original payload is untouched.
//   * `InvalidVersion` version 0 (the envelope never writes it).
//
// A failed or partial run never yields data: `MigrationResult::data` is only meaningful for
// `Current` and `Migrated`.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::appfw {

enum class MigrationOutcome : std::uint8_t {
    Current,
    Migrated,
    FutureVersion,
    MissingStep,
    StepFailed,
    InvalidVersion,
};

[[nodiscard]] constexpr std::string_view migration_outcome_name(MigrationOutcome outcome) noexcept {
    switch (outcome) {
    case MigrationOutcome::Current:
        return "current";
    case MigrationOutcome::Migrated:
        return "migrated";
    case MigrationOutcome::FutureVersion:
        return "future version";
    case MigrationOutcome::MissingStep:
        return "missing step";
    case MigrationOutcome::StepFailed:
        return "step failed";
    case MigrationOutcome::InvalidVersion:
        return "invalid version";
    }
    return "unknown";
}

struct MigrationResult {
    MigrationOutcome outcome = MigrationOutcome::InvalidVersion;
    // The payload at `to_version`. Empty unless `outcome` is `Current` or `Migrated`.
    std::string data;
    std::uint32_t from_version = 0;
    std::uint32_t to_version = 0;
    // For `MissingStep`/`StepFailed`: the version whose step was absent or failed. 0 otherwise.
    std::uint32_t failed_at = 0;
    // How many steps ran (0 for `Current`).
    std::uint32_t steps_run = 0;

    [[nodiscard]] bool usable() const noexcept {
        return outcome == MigrationOutcome::Current || outcome == MigrationOutcome::Migrated;
    }
};

class MigrationChain {
public:
    // Upgrades a version-`N` payload to version `N+1`; `nullopt` rejects the input.
    using Step = std::function<std::optional<std::string>(std::string_view)>;

    // Registers the step from `from_version` to `from_version + 1`. Returns false for version 0, an
    // empty step, or a version that already has one (two steps for one hop is ambiguous).
    bool add(std::uint32_t from_version, Step step);

    [[nodiscard]] std::size_t size() const noexcept {
        return steps_.size();
    }

    // Runs the chain from `stored_version` up to `target_version`. Const and re-entrant.
    [[nodiscard]] MigrationResult run(std::uint32_t stored_version, std::string_view payload,
                                      std::uint32_t target_version) const;

private:
    std::map<std::uint32_t, Step> steps_;
};

} // namespace tessera::appfw
