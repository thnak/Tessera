// SPDX-License-Identifier: MIT

#include "tessera/ui/text/edit_history.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tessera::ui::text {
namespace {

bool mergeable(EditCommitKind kind) {
    return kind == EditCommitKind::Typing || kind == EditCommitKind::Delete;
}

std::size_t snapshot_bytes(const EditSnapshot& snapshot) {
    return snapshot.text.size();
}

} // namespace

EditHistory::EditHistory(EditHistoryConfig config)
    : config_(config) {
    if (config_.max_entries == 0 || config_.max_bytes == 0) {
        throw std::invalid_argument("edit history bounds must be greater than zero");
    }
    if (!std::isfinite(config_.coalesce_seconds) || config_.coalesce_seconds < 0.0) {
        throw std::invalid_argument(
            "edit history coalescing window must be finite and non-negative");
    }
}

void EditHistory::record_commit(const EditSnapshot& before, EditCommitKind kind) {
    // A new edit invalidates the redo tail: the future that was undone no longer exists.
    redo_.clear();
    redo_bytes_ = 0;

    const bool merges = has_last_commit_ && mergeable(kind) && kind == last_kind_ &&
                        now_ - last_commit_seconds_ <= config_.coalesce_seconds;
    has_last_commit_ = true;
    last_commit_seconds_ = now_;
    last_kind_ = kind;
    if (merges) {
        // Keep the run's first undo target: the step already exists, this commit joins it.
        return;
    }
    push(undo_, undo_bytes_, before);
    trim();
}

std::optional<EditSnapshot> EditHistory::undo(const EditSnapshot& current) {
    if (undo_.empty()) {
        return std::nullopt;
    }
    Entry target = undo_.back();
    undo_.pop_back();
    undo_bytes_ -= target.bytes;
    push(redo_, redo_bytes_, current);
    // The next edit must start a fresh undo step instead of merging into the pre-undo run.
    has_last_commit_ = false;
    last_kind_ = EditCommitKind::Other;
    trim();
    return target.snapshot;
}

std::optional<EditSnapshot> EditHistory::redo(const EditSnapshot& current) {
    if (redo_.empty()) {
        return std::nullopt;
    }
    Entry target = redo_.back();
    redo_.pop_back();
    redo_bytes_ -= target.bytes;
    push(undo_, undo_bytes_, current);
    has_last_commit_ = false;
    last_kind_ = EditCommitKind::Other;
    trim();
    return target.snapshot;
}

void EditHistory::break_coalescing() {
    has_last_commit_ = false;
    last_kind_ = EditCommitKind::Other;
}

void EditHistory::clear() {
    undo_.clear();
    redo_.clear();
    undo_bytes_ = 0;
    redo_bytes_ = 0;
    has_last_commit_ = false;
    last_kind_ = EditCommitKind::Other;
    now_ = 0.0;
    last_commit_seconds_ = 0.0;
}

void EditHistory::tick(double now_seconds) {
    if (!std::isfinite(now_seconds)) {
        throw std::invalid_argument("edit history time must be finite");
    }
    if (now_seconds > now_) {
        now_ = now_seconds;
    }
}

void EditHistory::push(std::vector<Entry>& stack, std::size_t& bytes,
                       const EditSnapshot& snapshot) {
    const std::size_t size = snapshot_bytes(snapshot);
    stack.push_back(Entry{snapshot, size});
    bytes += size;
}

void EditHistory::trim() {
    // Entries are evicted oldest-first from the undo stack, then from the redo front. The newest
    // entry of each non-empty stack is always kept, so a single oversized snapshot still works.
    while (!undo_.empty() &&
           (undo_.size() + redo_.size() > config_.max_entries ||
            (undo_bytes_ + redo_bytes_ > config_.max_bytes && undo_.size() + redo_.size() > 1))) {
        undo_bytes_ -= undo_.front().bytes;
        undo_.erase(undo_.begin());
    }
    while (!redo_.empty() &&
           (undo_.size() + redo_.size() > config_.max_entries ||
            (undo_bytes_ + redo_bytes_ > config_.max_bytes && undo_.size() + redo_.size() > 1))) {
        redo_bytes_ -= redo_.front().bytes;
        redo_.erase(redo_.begin());
    }
}

} // namespace tessera::ui::text
