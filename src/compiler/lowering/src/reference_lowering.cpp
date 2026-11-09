// SPDX-License-Identifier: MIT

// Reference lowering: identifiers and member chains become IR ref ids through NameResolver
// (docs/design/jsx-ir-lowering.md section 9, slice 5; docs/specs/ir-v1.md section 4).

#include "expression_lowering_internal.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kUnsupported = "TESSERA_IR_UNSUPPORTED";
constexpr std::string_view kUnresolved = "TESSERA_IR_UNRESOLVED";
constexpr std::string_view kMalformed = "TESSERA_IR_MALFORMED";

enum class ChainStatus {
    Ok,
    Index,       // `a[i]`: no IR form outside the list loop
    Malformed,   // a missing name or operand
    Unsupported, // a member base that cannot become a ref id (a call, a literal, ...)
};

ChainStatus append_chain(const jsx::Expr& expr, std::string& out) {
    switch (expr.kind) {
    case jsx::ExprKind::Identifier:
        if (expr.text.empty()) {
            return ChainStatus::Malformed;
        }
        if (!out.empty()) {
            out += '.';
        }
        out += expr.text;
        return ChainStatus::Ok;
    case jsx::ExprKind::Member:
        if (expr.text == "[]") {
            return ChainStatus::Index;
        }
        if (expr.text.empty() || expr.operands.size() != 1 || !expr.operands[0]) {
            return ChainStatus::Malformed;
        }
        {
            const ChainStatus status = append_chain(*expr.operands[0], out);
            if (status != ChainStatus::Ok) {
                return status;
            }
        }
        out += '.';
        out += expr.text;
        return ChainStatus::Ok;
    default:
        return ChainStatus::Unsupported;
    }
}

ir::ExprPtr resolve_name(LoweringState& state, std::string_view name, jsx::Span span) {
    if (state.context.names == nullptr) {
        fail(state, kUnresolved,
             "`" + std::string(name) +
                 "` cannot be resolved: no name resolver is configured for this expression",
             span);
        return nullptr;
    }
    std::optional<std::string> id = state.context.names->resolve(name);
    if (!id || id->empty()) {
        fail(state, kUnresolved,
             "`" + std::string(name) +
                 "` is not a prop, state, ref, animated value or local; the type and hook passes "
                 "that supply those names are not implemented yet (slice 4)",
             span);
        return nullptr;
    }
    return ir::make_ref(std::move(*id));
}

} // namespace

ir::ExprPtr lower_reference(LoweringState& state, const jsx::Expr& expr) {
    std::string chain;
    const ChainStatus status = append_chain(expr, chain);
    if (status == ChainStatus::Index) {
        fail(state, kUnsupported,
             "indexing `a[i]` is not an IR expression; V1 accepts it only where the list form "
             "defines it (jsx-subset-v1.md sections 8.2 and 10.5)",
             expr.span, "use `.map` in JSX for lists");
        return nullptr;
    }
    if (status == ChainStatus::Malformed) {
        fail(state, kMalformed, "a reference is missing a name or an operand", expr.span);
        return nullptr;
    }
    if (status != ChainStatus::Ok) {
        fail(state, kUnsupported,
             "member access is an IR ref only on a prop object, a record-typed state or a loop "
             "item (jsx-subset-v1.md section 8.2)",
             expr.span);
        return nullptr;
    }
    return resolve_name(state, chain, expr.span);
}

} // namespace tessera::compiler::lowering::internal
