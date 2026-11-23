// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/types.hpp"

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which state cells and props each dynamic binding of a component reads (ADR-056 slice 1, issue
// #441). A pure analysis over the IR: it changes no emitted code. Slice 2 turns each read set into
// a dependency mask, so the one rule that matters is the failure direction: a read the analysis
// cannot see through counts as reading EVERY cell. A missed dependency is a stale UI; an extra one
// is only a re-run the hash gate already absorbs.
namespace tessera::compiler::codegen::internal {

// The cells one binding reads, as IR ids (`state::count`, `prop::title`, `anim::fade`,
// `env::theme`), sorted and unique. `all` is set when the analysis could not bound the reads;
// `cells` then lists every cell the component declares, so a consumer may use `cells` alone.
struct ReadSet {
    bool all = false;
    std::vector<std::string> cells;

    friend bool operator==(const ReadSet&, const ReadSet&) = default;
};

// What a binding is. The order is the sort order of the table.
enum class BindingKind : std::uint8_t {
    Text,         // a node's paint text, including a controlled input's value/defaultValue
    Metadata,     // `id`, `className`, `role`, `aria-*` (name = the prop name)
    Style,        // one inline style entry (name = the style property)
    Condition,    // a conditional child: the node's `condition`
    ListSource,   // the list a loop or virtual collection repeats over (the template's ordinal)
    ListItem,     // everything a list template's per-item content reads (the template's ordinal)
    InstanceProp, // one prop passed to a component instance (name = the prop name); also form state
    Attribute,    // any other non-literal prop of a node (name = the prop name)
    Effect,       // an effect's inputs (ordinal = index in `component.effects`, name = effect id)
};

struct BindingKey {
    BindingKind kind = BindingKind::Text;
    unsigned ordinal = 0; // the node's pre-order ordinal (every node counts), or the effect index
    std::string name;

    friend auto operator<=>(const BindingKey&, const BindingKey&) = default;
};

struct BindingReads {
    BindingKey key;
    ReadSet reads;
};

struct BindingReadTable {
    std::vector<BindingReads> entries; // sorted by key

    [[nodiscard]] const ReadSet* find(BindingKind kind, unsigned ordinal,
                                      std::string_view name = {}) const;
};

// The reads of one expression. `locals` are loop item/index names visible there; a reference to one
// (or to a field of one) reads nothing, because the list template's own binding carries the list
// cell.
[[nodiscard]] ReadSet expression_reads(const ir::Expr& expr, const ir::Component& component,
                                       std::span<const std::string> locals = {});

// The dependency bits one declared cell contributes (ADR-056 decision 1, ADR-051 implementation
// note #834). A cell whose declared type is exactly a declared record splits into one bit per read
// leaf path (`state::form.name`, `state::form.address.city`); every other cell is one bit (`{cell_id}`).
// A leaf is a primitive member, a `list<T>` member (a dotted ref cannot index a list), or an
// `optional<T>` member; a plain nested record recurses, so an intermediate record read expands to
// every leaf below it. `records` is the module's record type table.
[[nodiscard]] std::vector<std::string> cell_bit_ids(std::string_view cell_id,
                                                    std::string_view ir_type,
                                                    std::span<const ir::RecordType> records);

// Every bit id the component declares (`state::`, `prop::`, `anim::`, `env::`), sorted and unique.
// With the default empty record table this is exactly the old one-bit-per-cell list, which keeps
// every record-free golden byte-identical.
[[nodiscard]] std::vector<std::string> declared_cells(
    const ir::Component& component, std::span<const ir::RecordType> records = {});

// The read set of every dynamic binding of `component`, keyed with the same pre-order ordinals the
// other collectors use (`collect_dynamic_text`, `collect_dynamic_metadata`,
// `collect_dynamic_plan`). A binding whose expression is a literal is present with an empty read
// set. With the default empty record table it reports one cell id per read, as before.
[[nodiscard]] BindingReadTable analyze_binding_reads(
    const ir::Component& component, std::span<const ir::RecordType> records = {});

} // namespace tessera::compiler::codegen::internal
