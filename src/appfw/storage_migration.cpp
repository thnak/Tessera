// SPDX-License-Identifier: MIT

#include "tessera/appfw/storage_migration.hpp"

#include <utility>

namespace tessera::appfw {

bool MigrationChain::add(std::uint32_t from_version, Step step) {
    if (from_version == 0 || !step) {
        return false;
    }
    return steps_.emplace(from_version, std::move(step)).second;
}

MigrationResult MigrationChain::run(std::uint32_t stored_version, std::string_view payload,
                                    std::uint32_t target_version) const {
    MigrationResult result;
    result.from_version = stored_version;
    result.to_version = target_version;
    if (stored_version == 0) {
        result.outcome = MigrationOutcome::InvalidVersion;
        return result;
    }
    if (stored_version > target_version) {
        result.outcome = MigrationOutcome::FutureVersion;
        return result;
    }
    if (stored_version == target_version) {
        result.outcome = MigrationOutcome::Current;
        result.data.assign(payload);
        return result;
    }

    // Resolve the whole path before running anything, so a gap is reported without having run a
    // prefix of the chain on the caller's data.
    for (std::uint32_t version = stored_version; version < target_version; ++version) {
        if (!steps_.contains(version)) {
            result.outcome = MigrationOutcome::MissingStep;
            result.failed_at = version;
            return result;
        }
    }

    std::string current(payload);
    for (std::uint32_t version = stored_version; version < target_version; ++version) {
        std::optional<std::string> next = steps_.find(version)->second(current);
        if (!next) {
            result.outcome = MigrationOutcome::StepFailed;
            result.failed_at = version;
            return result;
        }
        current = std::move(*next);
        ++result.steps_run;
    }
    result.outcome = MigrationOutcome::Migrated;
    result.data = std::move(current);
    return result;
}

} // namespace tessera::appfw
