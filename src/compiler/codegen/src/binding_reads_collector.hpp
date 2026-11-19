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
    ReadCollector(const ir::Component& component, std::span<const std::string> locals);

    // A null expression is malformed IR; it reads everything rather than nothing.
    void add(const ir::Expr* expr);
    void add_ref_id(const std::string& id);
    [[nodiscard]] ReadSet finish() const;

private:
    const ir::Component& component_;
    std::vector<std::string> locals_;
    std::vector<std::string> cells_;
    bool all_ = false;
};

} // namespace tessera::compiler::codegen::internal
