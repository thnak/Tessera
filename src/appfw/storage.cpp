// SPDX-License-Identifier: MIT
//
// The storage module's implementation (ADR-050 AF7, issue #179).
//
// Every public method does three things and nothing else: validate on the calling (UI) thread,
// which is a string check and no I/O; allocate the one result slot the completion will deliver;
// post a single `IoTask`. The work body is the only code that touches the file system, and the
// completion runs on the thread that drains the completion queue. Nothing in this file waits, polls
// or reads a file outside a work body.
//
// The result slot is allocated at submit time and captured by both halves, so they agree on where
// the result goes without either owning the other. It is released by whichever half runs last,
// which also covers the case where the completion queue drops the completion (a full queue): the
// body still ran, the slot is still released, and no store state stays reserved.

#include "storage_envelope.hpp"
#include "storage_paths.hpp"
#include "storage_settings.hpp"
#include "tessera/appfw/storage.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace tessera::appfw {
namespace {

// Up-front cancellation, matching `runtime::IoExecutor::submit`: an operation whose token is
// already cancelled never starts, so it returns false and the completion is not invoked. The submit
// contract in the header says exactly this, and the alternative (posting a body that immediately
// returns `Cancelled`) would burn a slot to report something the caller already knew.
[[nodiscard]] bool already_cancelled(const runtime::CancellationToken& token) noexcept {
    return token.cancelled();
}

using detail::from_file_error;
using detail::is_temp_name;
using detail::join_path;

} // namespace

struct Storage::Impl {
    Impl(StorageConfig in_config, runtime::IoTaskExecutor& in_executor, hal::FileSystem& in_files)
        : config(std::move(in_config)),
          executor(in_executor),
          files(in_files),
          diagnostic_capacity(config.diagnostic_capacity) {}

    // Bounded diagnostics ring (ADR-022: structured, bounded, cheap when disabled). A store may run
    // several I/O bodies at once, so recording takes the lock; that is one lock per *failure*, not
    // a per-write cost.
    void record(StorageErrorCode code, std::string entry, std::string_view step,
                std::int64_t native) {
        std::lock_guard<std::mutex> lock(diagnostics_mutex);
        if (code == StorageErrorCode::Corrupted) {
            ++diagnostics.corruptions;
        }
        ++diagnostics.recorded;
        if (diagnostic_capacity == 0 || diagnostics.entries.size() >= diagnostic_capacity) {
            ++diagnostics.dropped;
            return;
        }
        diagnostics.entries.push_back(StorageDiagnostic{code, std::move(entry), step, native});
    }

    void note_result(std::string_view step, bool succeeded) {
        std::lock_guard<std::mutex> lock(diagnostics_mutex);
        if (succeeded) {
            ++diagnostics.writes;
            return;
        }
        if (step == "cancelled" || step == "write-temp" || step == "flush-temp" ||
            step == "replace" || step == "flush-dir") {
            ++diagnostics.aborted_writes;
        }
    }

    void note_migration() {
        std::lock_guard<std::mutex> lock(diagnostics_mutex);
        ++diagnostics.migrations;
    }

    void note_temp_removed() {
        std::lock_guard<std::mutex> lock(diagnostics_mutex);
        ++diagnostics.temp_files_removed;
    }

    [[nodiscard]] StorageDiagnostics snapshot() const {
        std::lock_guard<std::mutex> lock(diagnostics_mutex);
        return diagnostics;
    }

    // One atomic write of an already-encoded entry. Runs on the executor.
    [[nodiscard]] std::expected<void, StorageError>
    write_entry(const std::string& name, const std::string& path, std::string_view bytes,
                const runtime::CancellationToken& token) {
        const std::uint64_t generation = next_generation.fetch_add(1, std::memory_order_relaxed);
        AtomicWriteTrace trace;
        auto result =
            atomic_replace(files, path, bytes, config.durability, token, generation, &trace);
        if (trace.temp_removed) {
            note_temp_removed();
        }
        note_result(result ? std::string_view("ok") : result.error().step, result.has_value());
        if (!result && result.error().code == StorageErrorCode::Io) {
            record(StorageErrorCode::Io, name, result.error().step, result.error().native);
        }
        return result;
    }

    StorageConfig config;
    runtime::IoTaskExecutor& executor;
    hal::FileSystem& files;
    std::atomic<std::uint64_t> next_generation{1};

    mutable std::mutex diagnostics_mutex;
    StorageDiagnostics diagnostics;
    std::size_t diagnostic_capacity = 0;
};

Storage::Storage(StorageConfig config, runtime::IoTaskExecutor& executor, hal::FileSystem& files)
    : impl_(std::make_shared<Impl>(std::move(config), executor, files)) {}

Storage::~Storage() = default;

const StorageConfig& Storage::config() const noexcept {
    return impl_->config;
}

StorageDiagnostics Storage::diagnostics() const {
    return impl_->snapshot();
}

bool Storage::load(std::string name, runtime::CancellationToken token, ValueCallback completion) {
    if (!is_valid_entry_name(name) || already_cancelled(token)) {
        return false;
    }
    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<StoredValue, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, name, token, outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        auto bytes =
            impl->files.read(path, impl->config.max_entry_bytes + detail::kEnvelopeHeaderBytes);
        if (!bytes) {
            *outcome = std::unexpected(from_file_error("read", bytes.error()));
            return;
        }
        auto decoded =
            detail::decode_envelope(*bytes, detail::EnvelopeKind::Document,
                                    impl->config.format_version, impl->config.max_entry_bytes);
        if (!decoded) {
            impl->record(decoded.error().code, name, decoded.error().step, 0);
            *outcome = std::unexpected(decoded.error());
            return;
        }
        *outcome = StoredValue{std::move(decoded->payload), decoded->version};
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool Storage::load_migrated(std::string name, std::shared_ptr<const MigrationChain> chain,
                            runtime::CancellationToken token, ValueCallback completion) {
    if (!chain || !is_valid_entry_name(name) || already_cancelled(token)) {
        return false;
    }
    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<StoredValue, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, name, token, chain = std::move(chain), outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        auto bytes =
            impl->files.read(path, impl->config.max_entry_bytes + detail::kEnvelopeHeaderBytes);
        if (!bytes) {
            *outcome = std::unexpected(from_file_error("read", bytes.error()));
            return;
        }
        // Decode with the widest version the header can hold: the version policy is the chain's
        // here, not the envelope's, so a future version gets its own name instead of `Corrupted`.
        auto decoded = detail::decode_envelope(*bytes, detail::EnvelopeKind::Document, 0xFFFFu,
                                               impl->config.max_entry_bytes);
        if (!decoded) {
            impl->record(decoded.error().code, name, decoded.error().step, 0);
            *outcome = std::unexpected(decoded.error());
            return;
        }
        MigrationResult result =
            chain->run(decoded->version, decoded->payload, impl->config.format_version);
        if (!result.usable()) {
            const StorageErrorCode code = result.outcome == MigrationOutcome::FutureVersion
                                              ? StorageErrorCode::FutureVersion
                                              : StorageErrorCode::MigrationFailed;
            const std::string_view step = migration_outcome_name(result.outcome);
            impl->record(code, name, step, static_cast<std::int64_t>(result.failed_at));
            *outcome = std::unexpected(StorageError{code, step, result.failed_at});
            return;
        }
        const bool migrated = result.outcome == MigrationOutcome::Migrated;
        if (migrated) {
            auto encoded =
                detail::encode_envelope(detail::EnvelopeKind::Document, impl->config.format_version,
                                        result.data, impl->config.max_entry_bytes);
            // Best effort: the caller has usable data either way, and the next load migrates again.
            if (encoded && !token.cancelled()) {
                (void)impl->write_entry(name, path, *encoded, token);
            }
            impl->note_migration();
        }
        *outcome = StoredValue{std::move(result.data), impl->config.format_version, migrated};
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool Storage::save(std::string name, std::string data, runtime::CancellationToken token,
                   StatusCallback completion) {
    if (!is_valid_entry_name(name) || already_cancelled(token)) {
        return false;
    }
    if (data.size() > impl_->config.max_entry_bytes) {
        return false;
    }
    // The envelope is built on the calling thread, not on the worker: it is a bounded copy of a
    // payload the caller already holds, and doing it here means a size failure is reported before a
    // slot is taken.
    auto encoded =
        detail::encode_envelope(detail::EnvelopeKind::Document, impl_->config.format_version, data,
                                impl_->config.max_entry_bytes);
    if (!encoded) {
        return false;
    }

    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<void, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, name, token, encoded = std::move(*encoded), outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        *outcome = impl->write_entry(name, path, encoded, token);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool Storage::remove(std::string name, runtime::CancellationToken token,
                     StatusCallback completion) {
    if (!is_valid_entry_name(name) || already_cancelled(token)) {
        return false;
    }
    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<void, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, token, outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        if (!impl->files.exists(path)) {
            // Idempotent delete: an absent entry is success, not `NotFound`.
            return;
        }
        if (auto removed = impl->files.remove(path); !removed) {
            *outcome = std::unexpected(from_file_error("remove", removed.error()));
        }
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool Storage::list(runtime::CancellationToken token, ListCallback completion) {
    if (already_cancelled(token)) {
        return false;
    }
    auto outcome = std::make_shared<std::expected<std::vector<std::string>, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;
    const std::string root = impl_->config.root;

    runtime::IoTask task;
    task.work = [impl, root, token, outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        auto entries =
            impl->files.list(root.empty() ? std::string_view(".") : std::string_view(root));
        if (!entries) {
            *outcome = std::unexpected(from_file_error("list", entries.error()));
            return;
        }
        std::vector<std::string> names;
        names.reserve(entries->size());
        for (const hal::DirEntry& entry : *entries) {
            if (!entry.is_file || is_temp_name(entry.name)) {
                continue;
            }
            names.push_back(entry.name);
        }
        *outcome = std::move(names);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool Storage::save_settings(std::string name, Settings values, runtime::CancellationToken token,
                            StatusCallback completion) {
    if (!is_valid_entry_name(name) || already_cancelled(token)) {
        return false;
    }
    auto payload = detail::encode_settings(values, impl_->config.max_entry_bytes);
    if (!payload) {
        return false;
    }
    auto encoded =
        detail::encode_envelope(detail::EnvelopeKind::Settings, impl_->config.format_version,
                                *payload, impl_->config.max_entry_bytes);
    if (!encoded) {
        return false;
    }

    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<void, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, name, token, encoded = std::move(*encoded), outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        *outcome = impl->write_entry(name, path, encoded, token);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool Storage::load_settings(std::string name, runtime::CancellationToken token,
                            SettingsCallback completion) {
    if (!is_valid_entry_name(name) || already_cancelled(token)) {
        return false;
    }
    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<Settings, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, name, token, outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        auto bytes =
            impl->files.read(path, impl->config.max_entry_bytes + detail::kEnvelopeHeaderBytes);
        if (!bytes) {
            *outcome = std::unexpected(from_file_error("read", bytes.error()));
            return;
        }
        auto decoded =
            detail::decode_envelope(*bytes, detail::EnvelopeKind::Settings,
                                    impl->config.format_version, impl->config.max_entry_bytes);
        if (!decoded) {
            impl->record(decoded.error().code, name, decoded.error().step, 0);
            *outcome = std::unexpected(decoded.error());
            return;
        }
        auto values = detail::decode_settings(decoded->payload, impl->config.max_entry_bytes);
        if (!values) {
            impl->record(StorageErrorCode::Corrupted, name, values.error().step, 0);
            *outcome = std::unexpected(values.error());
            return;
        }
        *outcome = std::move(*values);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

} // namespace tessera::appfw
