// SPDX-License-Identifier: MIT
//
// The saved-state record for process death and relaunch (docs/specs/studio-device-profiles-v1.md
// section 4 last row and section 9 item 1, issue #445).
//
// Working choice of the spec (revisable): the state cells are captured **by the P4 rule** of the
// state-carrying re-evaluation (tools/codegen/scene_state.hpp) rather than through an app-declared
// `onSaveInstanceState` hook. The rule is applied at the engine's own level, where a component is
// its id and its `ui::StateSnapshot`:
//
//   A cell is restored when, and only when, the relaunched component has the SAME component id,
//   declares a cell with the SAME name (the IR id, `state::count`) and the SAME value kind (a
//   scalar of the same alternative, or a list of the same element alternative). Everything else
//   starts at its initializer.
//
// The kind check stands in for the IR type: a cell's value alternative is generated from it, so an
// `int` is never poured into a `float` cell. Initializer expressions are not part of the key, as in
// the P4 rule.
//
// When it is captured: at `Suspend`, the last event a process is guaranteed to see before the OS may
// kill it (Android's `onSaveInstanceState` moment). `SavedStateRecorder::on_event` does that on the
// normalized event, so the capture happens on the same path the app sees. A hot resume never reads
// the record (startup-api-v1.md Hot Resume: no snapshot read), and an unhandled Back, which closes
// the app, discards it (the spec's rule 1: the state is discarded).
//
// Capture copies the snapshots, so it belongs at checkpoints and never on the frame path.

#pragma once

#include "tessera/hal/os_event.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

// How a restore went, counted per cell of the target snapshot so a test asserts the split.
struct SavedStateReport {
    // Cells that took the saved value.
    std::size_t restored = 0;
    // Target cells the record had no value for (new component, new cell): they keep their
    // initializer.
    std::size_t missing = 0;
    // Target cells the record named but with a different value kind: kept at their initializer.
    std::size_t kind_mismatch = 0;
};

// One saved component: its id and the cells captured from it.
struct SavedComponent {
    std::string component_id;
    ui::StateSnapshot cells;
};

// The saved-state record: plain data a host keeps across a process restart (or a studio session
// restart) and hands to the relaunch.
class SavedStateRecord {
public:
    // Replaces what is saved for `component_id`. A second capture of the same id overwrites the
    // first, so the record holds the state at the latest `Suspend`.
    void capture(std::string_view component_id, ui::StateSnapshot cells);

    // Applies the record to `target`, the freshly initialized snapshot of the relaunched
    // component: every cell the P4 rule matches takes the saved value. Never adds or removes a
    // cell, so the component's declared shape stays the source of truth.
    SavedStateReport restore(std::string_view component_id, ui::StateSnapshot& target) const;

    [[nodiscard]] bool empty() const noexcept {
        return components_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return components_.size();
    }
    [[nodiscard]] const std::vector<SavedComponent>& components() const noexcept {
        return components_;
    }
    void clear() noexcept {
        components_.clear();
    }

private:
    std::vector<SavedComponent> components_;
};

// Captures component snapshots into a record when a `Suspend` is observed, and discards the record
// when the app is closed. A component registers a plain function pointer plus context, so the
// recorder never holds a std::function target.
class SavedStateRecorder {
public:
    using SnapshotFn = ui::StateSnapshot (*)(void* user);

    explicit SavedStateRecorder(SavedStateRecord& record) : record_(&record) {}

    // Registers one component's snapshot source. The caller keeps `user` alive for the recorder's
    // life. Returns false for an empty id or a null function.
    bool add_source(std::string component_id, SnapshotFn snapshot, void* user);

    // Feeds one normalized OS event. `Suspend` captures every source into the record; any other
    // event is ignored. Returns true when a capture ran.
    bool on_event(hal::OsEventType type);

    // The app was closed (an unhandled Back): the saved state is discarded.
    void discard() noexcept {
        record_->clear();
    }

    [[nodiscard]] std::size_t captures() const noexcept {
        return captures_;
    }

private:
    struct Source {
        std::string component_id;
        SnapshotFn snapshot = nullptr;
        void* user = nullptr;
    };

    SavedStateRecord* record_;
    std::vector<Source> sources_;
    std::size_t captures_ = 0;
};

} // namespace tessera::appfw
