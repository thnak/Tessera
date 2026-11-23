// SPDX-License-Identifier: MIT

#pragma once

#include "binding_reads.hpp"
#include "codegen_internal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The dirty mask of a generated component (ADR-056 slice 2): one bit per state cell, animated value
// and prop, one dependency mask per binding (from `analyze_binding_reads`), and the helpers that
// gate a binding on `dirty_.intersects(mask)`. The runtime half is
// `include/tessera/ui/scene/dirty_mask.hpp`.
namespace tessera::compiler::codegen::internal {

struct DirtyPlan {
    std::vector<std::string> cells; // declared cell ids, sorted; a cell's bit is its index here
    BindingReadTable reads;

    // The mask width: one bit per cell plus the "everything" bit. Every dependency mask carries
    // that last bit, so a first build or a hand-called update() (which set every bit) reaches even
    // a binding that reads no cell, while a setter (which sets only its own bit) does not.
    [[nodiscard]] std::size_t bits() const;
    // `tessera::ui::DirtyMask<N>`.
    [[nodiscard]] std::string type() const;
    // The cell's bit index, or the cell count when the id is not a declared cell.
    [[nodiscard]] std::size_t bit(std::string_view cell_id) const;
    // True when the analysis recorded the binding (an unrecorded one is evaluated ungated).
    [[nodiscard]] bool has(BindingKind kind, unsigned ordinal, std::string_view name = {}) const;
    // The `kDirty_...` namespace constant holding the binding's dependency mask.
    [[nodiscard]] std::string mask_name(BindingKind kind, unsigned ordinal,
                                        std::string_view name = {}) const;
};

// `records` is the module's record type table: a record-typed cell takes one bit per read leaf
// path (ADR-051 implementation note #834), everything else one bit per cell. Empty keeps the
// one-bit-per-cell layout every record-free golden already has.
[[nodiscard]] DirtyPlan make_dirty_plan(const ir::Component& component,
                                        std::span<const ir::RecordType> records = {});

// `state::count` -> `kBit_state_count_`: the public static constant holding the cell's bit.
[[nodiscard]] std::string dirty_bit_name(std::string_view cell_id);

// Header: the public bit constants, the private mask member and the dirty-related methods.
void emit_dirty_declarations(CppWriter& writer, const DirtyPlan& dirty);

// Source: the `kDirty_...` constants (inside an anonymous namespace, so the base reshaping moves
// them into the detail namespace).
void emit_dirty_mask_constants(CppWriter& writer, const DirtyPlan& dirty);

// Source: `note_dirty`, `mark_all_dirty`.
void emit_dirty_definitions(CppWriter& writer, const ir::Component& component);

// Opens `if (dirty_.intersects(<mask>)) {` plus the test trace line and returns true; returns false
// and writes nothing when the binding is not in the table (it then runs ungated, which is always
// correct). `label` is the trace id, for example `text:2`.
bool open_dirty_gate(CppWriter& writer, const DirtyPlan& dirty, BindingKind kind, unsigned ordinal,
                     std::string_view name, std::string_view label);
void close_dirty_gate(CppWriter& writer, bool opened);

} // namespace tessera::compiler::codegen::internal
