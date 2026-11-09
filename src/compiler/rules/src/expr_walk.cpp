// SPDX-License-Identifier: MIT

#include "expr_walk.hpp"

#include <algorithm>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;

class ReadCollector {
public:
    explicit ReadCollector(const ReadOptions& options)
        : options_(options) {}

    void operator()(const Expr& expr) {
        switch (expr.kind) {
        case ExprKind::Identifier:
            if (!shadowed(expr.text)) {
                reads_.push_back({expr.text, expr.span});
            }
            return;
        case ExprKind::Member:
            if (is_props_access(expr)) {
                reads_.push_back({std::string(options_.props_name) + "." + expr.text, expr.span});
                return;
            }
            break;
        case ExprKind::Call:
            visit_call(expr);
            return;
        case ExprKind::Arrow:
            visit_arrow(expr);
            return;
        default:
            break;
        }
        for_each_child(expr, *this);
    }

    std::vector<Read> take() {
        return std::move(reads_);
    }

private:
    [[nodiscard]] bool shadowed(const std::string& name) const {
        return std::find(shadow_.begin(), shadow_.end(), name) != shadow_.end();
    }

    [[nodiscard]] bool is_props_access(const Expr& member) const {
        return !options_.props_name.empty() && !member.operands.empty() &&
               member.operands[0]->kind == ExprKind::Identifier &&
               member.operands[0]->text == options_.props_name &&
               !shadowed(member.operands[0]->text) && member.text != "[]";
    }

    void visit_call(const Expr& call) {
        const Expr& callee = *call.operands[0];
        const bool skipped = !options_.include_callees &&
                             (callee.kind == ExprKind::Identifier ||
                              (callee.kind == ExprKind::Member && callee.text == "set"));
        if (!skipped) {
            (*this)(callee);
        }
        for (std::size_t i = 1; i < call.operands.size(); ++i) {
            (*this)(*call.operands[i]);
        }
    }

    void visit_arrow(const Expr& arrow) {
        const std::size_t before = shadow_.size();
        shadow_.insert(shadow_.end(), arrow.params.begin(), arrow.params.end());
        for (const auto& operand : arrow.operands) {
            (*this)(*operand);
        }
        shadow_.resize(before);
    }

    const ReadOptions& options_;
    std::vector<std::string> shadow_;
    std::vector<Read> reads_;
};

} // namespace

std::vector<Read> collect_reads(const Expr& expr, const ReadOptions& options) {
    ReadCollector collector(options);
    collector(expr);
    return collector.take();
}

} // namespace tessera::compiler::rules
