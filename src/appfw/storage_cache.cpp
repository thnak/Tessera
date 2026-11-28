// SPDX-License-Identifier: MIT
//
// `CacheStore` (issue #411). Same shape as `Storage`: validate on the calling thread, allocate one
// result slot, post one `IoTask`. The index is the only shared state and is touched under one
// mutex; file operations never run under it, so a slow remove cannot stall another body's lookup.

#include "storage_envelope.hpp"
#include "storage_paths.hpp"
#include "tessera/appfw/storage_cache.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace tessera::appfw {
namespace {

using detail::from_file_error;
using detail::is_temp_name;
using detail::join_path;

struct IndexEntry {
    std::size_t bytes = 0;
    std::uint64_t tick = 0;
};

struct Victim {
    std::string name;
    std::size_t bytes = 0;
};

} // namespace

struct CacheStore::Impl {
    Impl(CacheConfig in_config, runtime::IoTaskExecutor& in_executor, hal::FileSystem& in_files)
        : config(std::move(in_config)),
          executor(in_executor),
          files(in_files) {}

    // Drops the LRU entries (never `keep`) until both bounds hold. Caller holds `mutex`. Entries
    // leave the index here; the caller removes their files after releasing the lock.
    std::vector<Victim> pick_victims(const std::string& keep) {
        std::vector<Victim> victims;
        while (bytes > config.max_bytes || index.size() > config.max_entries) {
            auto oldest = index.end();
            for (auto it = index.begin(); it != index.end(); ++it) {
                if (it->first == keep) {
                    continue;
                }
                if (oldest == index.end() || it->second.tick < oldest->second.tick) {
                    oldest = it;
                }
            }
            if (oldest == index.end()) {
                break;
            }
            victims.push_back(Victim{oldest->first, oldest->second.bytes});
            bytes -= oldest->second.bytes;
            ++stats_evictions;
            stats_evicted_bytes += oldest->second.bytes;
            index.erase(oldest);
        }
        return victims;
    }

    void remove_victim_files(const std::vector<Victim>& victims) {
        for (const Victim& victim : victims) {
            const std::string path = join_path(config.root, victim.name);
            auto removed = files.remove(path);
            if (!removed && removed.error().code != hal::FileErrorCode::NotFound) {
                std::lock_guard<std::mutex> lock(mutex);
                ++evict_failures;
            }
        }
    }

    void drop(const std::string& name) {
        const auto found = index.find(name);
        if (found != index.end()) {
            bytes -= found->second.bytes;
            index.erase(found);
        }
    }

    CacheConfig config;
    runtime::IoTaskExecutor& executor;
    hal::FileSystem& files;
    std::atomic<std::uint64_t> next_generation{1};

    mutable std::mutex mutex;
    std::map<std::string, IndexEntry> index;
    std::size_t bytes = 0;
    std::uint64_t clock = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stats_evictions = 0;
    std::uint64_t stats_evicted_bytes = 0;
    std::uint64_t evict_failures = 0;
    std::uint64_t corrupt_dropped = 0;
};

CacheStore::CacheStore(CacheConfig config, runtime::IoTaskExecutor& executor,
                       hal::FileSystem& files)
    : impl_(std::make_shared<Impl>(std::move(config), executor, files)) {}

CacheStore::~CacheStore() = default;

const CacheConfig& CacheStore::config() const noexcept {
    return impl_->config;
}

CacheStats CacheStore::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return CacheStats{impl_->bytes,          impl_->index.size(),    impl_->hits,
                      impl_->misses,         impl_->stats_evictions, impl_->stats_evicted_bytes,
                      impl_->evict_failures, impl_->corrupt_dropped};
}

std::vector<std::string> CacheStore::names_by_recency() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::pair<std::uint64_t, std::string>> ordered;
    ordered.reserve(impl_->index.size());
    for (const auto& [name, entry] : impl_->index) {
        ordered.emplace_back(entry.tick, name);
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> names;
    names.reserve(ordered.size());
    for (auto& item : ordered) {
        names.push_back(std::move(item.second));
    }
    return names;
}

bool CacheStore::put(std::string name, std::string data, runtime::CancellationToken token,
                     StatusCallback completion) {
    if (!is_valid_entry_name(name) || token.cancelled()) {
        return false;
    }
    auto encoded = detail::encode_envelope(detail::EnvelopeKind::Document, 1, data,
                                           impl_->config.max_entry_bytes);
    if (!encoded || encoded->size() > impl_->config.max_entry_bytes ||
        encoded->size() > impl_->config.max_bytes) {
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
        const std::uint64_t generation =
            impl->next_generation.fetch_add(1, std::memory_order_relaxed);
        *outcome =
            atomic_replace(impl->files, path, encoded, impl->config.durability, token, generation);
        if (!*outcome) {
            return;
        }
        std::vector<Victim> victims;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->drop(name);
            impl->index[name] = IndexEntry{encoded.size(), ++impl->clock};
            impl->bytes += encoded.size();
            victims = impl->pick_victims(name);
        }
        impl->remove_victim_files(victims);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool CacheStore::get(std::string name, runtime::CancellationToken token, ValueCallback completion) {
    if (!is_valid_entry_name(name) || token.cancelled()) {
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
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                ++impl->misses;
                if (bytes.error().code == hal::FileErrorCode::NotFound) {
                    impl->drop(name);
                }
            }
            *outcome = std::unexpected(from_file_error("read", bytes.error()));
            return;
        }
        auto decoded = detail::decode_envelope(*bytes, detail::EnvelopeKind::Document, 1,
                                               impl->config.max_entry_bytes);
        if (!decoded) {
            // A cache entry is never the only copy: delete the bad one so it cannot be served or
            // keep counting against the quota.
            (void)impl->files.remove(path);
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                ++impl->misses;
                ++impl->corrupt_dropped;
                impl->drop(name);
            }
            *outcome = std::unexpected(decoded.error());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            ++impl->hits;
            const auto found = impl->index.find(name);
            if (found != impl->index.end()) {
                found->second.tick = ++impl->clock;
            } else {
                // Present on disk but not indexed (written before a scan): adopt it.
                impl->index[name] = IndexEntry{bytes->size(), ++impl->clock};
                impl->bytes += bytes->size();
            }
        }
        *outcome = StoredValue{std::move(decoded->payload), decoded->version, false};
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool CacheStore::remove(std::string name, runtime::CancellationToken token,
                        StatusCallback completion) {
    if (!is_valid_entry_name(name) || token.cancelled()) {
        return false;
    }
    const std::string path = join_path(impl_->config.root, name);
    auto outcome = std::make_shared<std::expected<void, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, path, name, token, outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        if (impl->files.exists(path)) {
            if (auto removed = impl->files.remove(path); !removed) {
                *outcome = std::unexpected(from_file_error("remove", removed.error()));
                return;
            }
        }
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->drop(name);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

bool CacheStore::scan(runtime::CancellationToken token, StatusCallback completion) {
    if (token.cancelled()) {
        return false;
    }
    auto outcome = std::make_shared<std::expected<void, StorageError>>();
    std::shared_ptr<Impl> impl = impl_;

    runtime::IoTask task;
    task.work = [impl, token, outcome] {
        if (token.cancelled()) {
            *outcome = std::unexpected(StorageError{StorageErrorCode::Cancelled, "cancelled"});
            return;
        }
        const std::string& root = impl->config.root;
        auto entries =
            impl->files.list(root.empty() ? std::string_view(".") : std::string_view(root));
        if (!entries) {
            *outcome = std::unexpected(from_file_error("list", entries.error()));
            return;
        }
        // Name order stands in for recency: ticks do not survive a restart, and any fixed order is
        // better than a directory-iteration order that differs between file systems.
        std::sort(entries->begin(), entries->end(),
                  [](const hal::DirEntry& a, const hal::DirEntry& b) { return a.name < b.name; });
        std::vector<Victim> victims;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->index.clear();
            impl->bytes = 0;
            for (const hal::DirEntry& entry : *entries) {
                if (!entry.is_file || is_temp_name(entry.name)) {
                    continue;
                }
                impl->index[entry.name] =
                    IndexEntry{static_cast<std::size_t>(entry.size), ++impl->clock};
                impl->bytes += static_cast<std::size_t>(entry.size);
            }
            victims = impl->pick_victims(std::string());
        }
        impl->remove_victim_files(victims);
    };
    task.completion = [outcome, completion = std::move(completion)]() mutable {
        if (completion) {
            completion(std::move(*outcome));
        }
    };
    return impl_->executor.post(std::move(task));
}

} // namespace tessera::appfw
