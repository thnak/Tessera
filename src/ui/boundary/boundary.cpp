// SPDX-License-Identifier: MIT

#include "tessera/ui/boundary/boundary.hpp"

#include <exception>
#include <utility>

namespace tessera::ui::boundary {
namespace {

[[nodiscard]] Error registration_error(std::string code, std::string message) {
    Error error;
    error.code = std::move(code);
    error.message = std::move(message);
    error.kind = ErrorClass::Recoverable;
    return error;
}

} // namespace

BoundaryTable::BoundaryTable(RootHandler& root, ReportSink& diagnostics)
    : root_(root),
      diagnostics_(diagnostics) {}

std::expected<BoundaryId, Error> BoundaryTable::add(BoundaryConfig config, ReportSink* on_error,
                                                    SubtreeOwner* owner) {
    if (config.fallback.empty()) {
        return std::unexpected(registration_error("fallback_required",
                                                  "an error boundary requires a fallback subtree"));
    }
    if (entries_.size() >= kMaxBoundaries) {
        return std::unexpected(
            registration_error("boundary_table_full", "too many live error boundaries"));
    }
    if (config.parent != kRootBoundary && find(config.parent) == nullptr) {
        return std::unexpected(
            registration_error("parent_unknown", "parent boundary is not registered"));
    }
    if (config.component.empty()) {
        config.component = "boundary";
    }

    Entry entry;
    entry.id = next_id_++;
    entry.config = std::move(config);
    // The retry policy clamps max_auto_retries to the decided cap, so a bad prop cannot spin.
    entry.policy = RetryPolicy(entry.config.max_auto_retries);
    entry.on_error = on_error;
    entry.owner = owner;
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

std::size_t BoundaryTable::remove(BoundaryId id) {
    if (find(id) == nullptr) {
        return 0;
    }

    // Collect the boundary and every descendant before erasing anything: erasing a parent first
    // would leave children whose chain silently resolves to the root instead of being unmounted.
    std::vector<BoundaryId> doomed;
    doomed.push_back(id);
    for (const Entry& entry : entries_) {
        BoundaryId current = entry.config.parent;
        while (current != kRootBoundary) {
            if (current == id) {
                doomed.push_back(entry.id);
                break;
            }
            const Entry* parent = find(current);
            if (parent == nullptr) {
                break;
            }
            current = parent->config.parent;
        }
    }

    std::size_t removed = 0;
    for (BoundaryId target : doomed) {
        Entry* entry = find(target);
        if (entry == nullptr) {
            continue;
        }
        // Unmount cancels pending errors and releases whichever subtree is live (sections 4 and 8).
        if (entry->owner != nullptr) {
            if (entry->state.fallback_live) {
                entry->owner->dispose_subtree(entry->config.fallback);
            }
            if (!entry->state.child_detached) {
                entry->owner->dispose_subtree(entry->config.subtree);
            }
        }
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (entries_[index].id == target) {
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
        }
        ++removed;
    }
    return removed;
}

bool BoundaryTable::contains(BoundaryId id) const {
    return find(id) != nullptr;
}

const BoundaryConfig* BoundaryTable::config(BoundaryId id) const {
    const Entry* entry = find(id);
    return entry != nullptr ? &entry->config : nullptr;
}

const BoundaryState* BoundaryTable::state(BoundaryId id) const {
    const Entry* entry = find(id);
    return entry != nullptr ? &entry->state : nullptr;
}

BoundaryId BoundaryTable::catcher(BoundaryId from) const {
    BoundaryId current = from;
    while (current != kRootBoundary) {
        const Entry* entry = find(current);
        if (entry == nullptr) {
            // The owning boundary was unmounted: only the root handler is left.
            return kRootBoundary;
        }
        if (entry->state.phase == BoundaryPhase::Normal) {
            return current;
        }
        // A boundary already showing its fallback does not catch for the same child again; the
        // failure belongs to the parent (section 4, no catch loop).
        current = entry->config.parent;
    }
    return kRootBoundary;
}

void BoundaryTable::set_on_error(BoundaryId id, ReportSink* sink) {
    if (Entry* entry = find(id); entry != nullptr) {
        entry->on_error = sink;
    }
}

void BoundaryTable::set_owner(BoundaryId id, SubtreeOwner* owner) {
    if (Entry* entry = find(id); entry != nullptr) {
        entry->owner = owner;
    }
}

std::string BoundaryTable::component_of(BoundaryId id) const {
    const Entry* entry = find(id);
    return entry != nullptr ? entry->config.component : std::string("root");
}

void BoundaryTable::deliver(Entry& entry, const Report& report) {
    ReportSink* sink = entry.on_error != nullptr ? entry.on_error : &diagnostics_;
    try {
        sink->on_report(report);
    } catch (...) {
        // onError is documented not to throw. If it does, the failure must not escape the catch
        // point, so it becomes a FALLBACK_FAILED report at the root handler.
        Error error;
        error.code = "on_error_threw";
        error.message = "onError threw while handling a boundary report";
        escalate(make_report(BoundaryCode::FallbackFailed, entry.id, entry.config.component,
                             std::move(error), report.site, report.retries, report.generation));
    }
}

void BoundaryTable::escalate(const Report& report) {
    // The root handler is the last catch point; it must not throw and must not re-enter the table.
    root_.on_root_error(report);
}

BoundaryTable::Entry* BoundaryTable::find(BoundaryId id) {
    for (Entry& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const BoundaryTable::Entry* BoundaryTable::find(BoundaryId id) const {
    for (const Entry& entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace tessera::ui::boundary
