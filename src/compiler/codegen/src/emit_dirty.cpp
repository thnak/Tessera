// SPDX-License-Identifier: MIT

#include "emit_dirty.hpp"

#include <algorithm>
#include <cctype>
#include <span>

namespace tessera::compiler::codegen::internal {
namespace {

std::string_view kind_tag(BindingKind kind) {
    switch (kind) {
    case BindingKind::Text:
        return "text";
    case BindingKind::Metadata:
        return "meta";
    case BindingKind::Style:
        return "style";
    case BindingKind::Condition:
        return "cond";
    case BindingKind::ListSource:
        return "source";
    case BindingKind::ListItem:
        return "list";
    case BindingKind::InstanceProp:
        return "prop";
    case BindingKind::Attribute:
        return "attr";
    case BindingKind::Effect:
        return "effect";
    }
    return "binding";
}

// The kinds a generated binding gates on. The others (attributes, effects) are recorded by the
// analysis but evaluated elsewhere, ungated.
bool gated_kind(BindingKind kind) {
    return kind == BindingKind::Text || kind == BindingKind::Metadata ||
           kind == BindingKind::Style || kind == BindingKind::Condition ||
           kind == BindingKind::ListItem || kind == BindingKind::InstanceProp;
}

std::string identifier(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
    }
    return out;
}

} // namespace

std::size_t DirtyPlan::bits() const {
    return cells.size() + 1; // the last bit is the "everything" bit every mask carries
}

std::string DirtyPlan::type() const {
    return "tessera::ui::DirtyMask<" + std::to_string(bits()) + ">";
}

std::size_t DirtyPlan::bit(std::string_view cell_id) const {
    const auto it = std::find(cells.begin(), cells.end(), cell_id);
    return static_cast<std::size_t>(it - cells.begin());
}

bool DirtyPlan::has(BindingKind kind, unsigned ordinal, std::string_view name) const {
    return reads.find(kind, ordinal, name) != nullptr;
}

std::string DirtyPlan::mask_name(BindingKind kind, unsigned ordinal, std::string_view name) const {
    std::string out = "kDirty_" + std::string(kind_tag(kind)) + "_" + std::to_string(ordinal);
    if (!name.empty()) {
        out += "_" + identifier(name);
    }
    return out;
}

DirtyPlan make_dirty_plan(const ir::Component& component, std::span<const ir::RecordType> records) {
    DirtyPlan plan;
    plan.cells = declared_cells(component, records);
    plan.reads = analyze_binding_reads(component, records);
    return plan;
}

std::string dirty_bit_name(std::string_view cell_id) {
    // A record leaf path (`state::form.address.city`) would otherwise leave a `.` in the C++
    // identifier; one `_` per dot keeps it a plain name (`kBit_state_form_address_city_`).
    std::string out = "kBit_" + member_name(cell_id);
    for (char& c : out) {
        if (c == '.' || c == ':') {
            c = '_';
        }
    }
    return out;
}

void emit_dirty_declarations(CppWriter& writer, const DirtyPlan& dirty) {
    writer.line(
        "// ADR-056 dirty mask: one bit per state cell, animated value and prop. A setter sets");
    writer.line(
        "// its cell's bit; apply_state() re-evaluates a binding only when a cell it reads is");
    writer.line("// dirty. The bit constants are public so an owner can mark a child's prop.");
    for (std::size_t i = 0; i < dirty.cells.size(); ++i) {
        writer.line("static constexpr std::size_t " + dirty_bit_name(dirty.cells[i]) + " = " +
                    std::to_string(i) + ";");
    }
    writer.line(dirty.type() + " dirty_ = " + dirty.type() + "::all();");
    writer.line("// Marks one cell dirty without scheduling (the caller schedules).");
    writer.line("void note_dirty(std::size_t bit);");
    writer.line("// Marks every cell dirty: the next apply_state() re-evaluates every binding.");
    writer.line("void mark_all_dirty();");
}

void emit_dirty_mask_constants(CppWriter& writer, const DirtyPlan& dirty) {
    bool opened = false;
    for (const BindingReads& entry : dirty.reads.entries) {
        if (!gated_kind(entry.key.kind)) {
            continue;
        }
        if (!opened) {
            writer.open("namespace");
            opened = true;
        }
        std::string bits;
        std::string cells;
        for (const std::string& cell : entry.reads.cells) {
            bits += (bits.empty() ? "" : ", ") + std::to_string(dirty.bit(cell));
            cells += (cells.empty() ? "" : " ") + cell;
        }
        bits += (bits.empty() ? "" : ", ") + std::to_string(dirty.cells.size());
        writer.line("// " + std::string(kind_tag(entry.key.kind)) +
                    " binding of node::" + std::to_string(entry.key.ordinal) +
                    (entry.key.name.empty() ? "" : " `" + entry.key.name + "`") +
                    " reads: " + (cells.empty() ? "nothing" : cells) +
                    (entry.reads.all ? " (unbounded)" : "") + ".");
        writer.line("[[maybe_unused]] constexpr " + dirty.type() + " " +
                    dirty.mask_name(entry.key.kind, entry.key.ordinal, entry.key.name) + " = " +
                    dirty.type() + "::of({" + bits + "});");
    }
    if (opened) {
        writer.close("");
        writer.line("");
    }
}

void emit_dirty_definitions(CppWriter& writer, const ir::Component& component) {
    writer.open("void " + component.name + "::note_dirty(std::size_t bit)");
    writer.line("dirty_.set(bit);");
    writer.close("");
    writer.line("");
    writer.open("void " + component.name + "::mark_all_dirty()");
    writer.line("dirty_.set_all();");
    writer.close("");
    writer.line("");
}

bool open_dirty_gate(CppWriter& writer, const DirtyPlan& dirty, BindingKind kind, unsigned ordinal,
                     std::string_view name, std::string_view label) {
    if (!dirty.has(kind, ordinal, name)) {
        return false;
    }
    writer.open("if (dirty_.intersects(" + dirty.mask_name(kind, ordinal, name) + "))");
    writer.line("TESSERA_BINDING_RAN(this, \"" + std::string(label) + "\");");
    return true;
}

void close_dirty_gate(CppWriter& writer, bool opened) {
    if (opened) {
        writer.close("");
    }
}

} // namespace tessera::compiler::codegen::internal
