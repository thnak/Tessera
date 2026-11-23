// SPDX-License-Identifier: MIT

#pragma once

#include "binding_reads.hpp"

#include <span>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {

// Accumulates the reads of several expressions into one set. Shared by the expression walk and the
// component walk, which merges every expression of a list template into one binding.
class ReadCollector {
public:
    // `records` is the module's record type table (ADR-051 implementation note #834). With it, a
    // read of a record-typed cell expands to the leaf-path bits the read actually touches; without
    // it (the default) one bit per cell is recorded, as before.
    ReadCollector(const ir::Component& component, std::span<const std::string> locals,
                  std::span<const ir::RecordType> records = {});

    // A null expression is malformed IR; it reads everything rather than nothing.
    void add(const ir::Expr* expr);
    void add_ref_id(const std::string& id);
    [[nodiscard]] ReadSet finish() const;

private:
    // Records the bits an expression reads. A dotted id names a member path: with the record table
    // the touched leaves are recorded (`state::form.name`), without it the base cell only. A bare
    // cell read records every leaf bit its type declares.
    void add_bits(std::string_view id);

    const ir::Component& component_;
    std::vector<std::string> locals_;
    std::span<const ir::RecordType> records_;
    std::vector<std::string> cells_;
    bool all_ = false;
};

} // namespace tessera::compiler::codegen::internal
