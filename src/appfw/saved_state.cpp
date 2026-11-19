// SPDX-License-Identifier: MIT
//
// The saved-state record and its Suspend-time recorder (docs/specs/studio-device-profiles-v1.md
// section 4 and section 9 item 1, issue #445). See include/tessera/appfw/saved_state.hpp for the
// rule.

#include "tessera/appfw/saved_state.hpp"

#include <utility>

namespace tessera::appfw {
namespace {

// The kind a cell's value has, as the P4 rule's type check: a list's kind is its element's, and an
// empty list has no element to compare, so it matches any list cell.
bool same_kind(const ui::StateCell& saved, const ui::StateCell& target) {
    if (saved.is_list != target.is_list) {
        return false;
    }
    if (!saved.is_list) {
        return saved.value.index() == target.value.index();
    }
    if (saved.items.empty() || target.items.empty()) {
        return true;
    }
    return saved.items.front().index() == target.items.front().index();
}

} // namespace

void SavedStateRecord::capture(std::string_view component_id, ui::StateSnapshot cells) {
    for (SavedComponent& component : components_) {
        if (component.component_id == component_id) {
            component.cells = std::move(cells);
            return;
        }
    }
    components_.push_back(SavedComponent{std::string(component_id), std::move(cells)});
}

SavedStateReport SavedStateRecord::restore(std::string_view component_id,
                                           ui::StateSnapshot& target) const {
    SavedStateReport report;
    const SavedComponent* saved = nullptr;
    for (const SavedComponent& component : components_) {
        if (component.component_id == component_id) {
            saved = &component;
            break;
        }
    }
    for (ui::StateCell& cell : target) {
        const ui::StateCell* match = nullptr;
        if (saved != nullptr) {
            for (const ui::StateCell& candidate : saved->cells) {
                if (candidate.name == cell.name) {
                    match = &candidate;
                    break;
                }
            }
        }
        if (match == nullptr) {
            ++report.missing;
        } else if (!same_kind(*match, cell)) {
            ++report.kind_mismatch;
        } else {
            cell = *match;
            ++report.restored;
        }
    }
    return report;
}

bool SavedStateRecorder::add_source(std::string component_id, SnapshotFn snapshot, void* user) {
    if (component_id.empty() || snapshot == nullptr) {
        return false;
    }
    sources_.push_back(Source{std::move(component_id), snapshot, user});
    return true;
}

bool SavedStateRecorder::on_event(hal::OsEventType type) {
    if (type != hal::OsEventType::Suspend) {
        return false;
    }
    for (const Source& source : sources_) {
        record_->capture(source.component_id, source.snapshot(source.user));
    }
    ++captures_;
    return true;
}

} // namespace tessera::appfw
