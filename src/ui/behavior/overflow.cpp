// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/overflow.hpp"
#include "tessera/ui/layout/layout_read_model.hpp"
#include "tessera/ui/scene/node.hpp"

#include <optional>

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `reserved`, `gap`.
constexpr std::string_view kOptionNames[] = {"reserved", "gap"};

double option_number(const ArgsView& args, std::string_view name, double fallback) {
    const ArgValue* value = args.find(name, kOptionNames);
    if (value == nullptr || value->kind != ArgValue::Kind::Number) {
        return fallback;
    }
    return value->number;
}

} // namespace

void Overflow::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    model_ = services.layout;
    reserved_ = option_number(args, "reserved", 0.0);
    gap_ = option_number(args, "gap", 0.0);
    measure();
}

void Overflow::update(const ArgsView& args) {
    if (target_ == nullptr) {
        return;
    }
    reserved_ = option_number(args, "reserved", 0.0);
    gap_ = option_number(args, "gap", 0.0);
    measure();
}

void Overflow::detach() {
    target_ = nullptr;
    model_ = nullptr;
    reserved_ = 0.0;
    gap_ = 0.0;
    outputs_ = {};
}

std::span<const OutputValue> Overflow::outputs() const {
    return outputs_;
}

void Overflow::measure() {
    outputs_[0].kind = OutputValue::Kind::Number;
    outputs_[1].kind = OutputValue::Kind::Bool;
    if (target_ == nullptr || model_ == nullptr) {
        // No measurement available: report that nothing fits rather than claiming everything does.
        // A component that renders `count` items then renders none, which is the safe direction for
        // a first frame (the same rule `size_class_for` applies to an unmeasured width).
        outputs_[0].number = 0.0;
        outputs_[1].boolean = true;
        return;
    }
    const std::optional<layout::LayoutInfo> own = model_->find(*target_);
    if (!own.has_value()) {
        outputs_[0].number = 0.0;
        outputs_[1].boolean = true;
        return;
    }
    // The width the items may use: the target's content box minus the reserved trailing slot.
    const double available = static_cast<double>(own->content_box.width) - reserved_;
    double used = 0.0;
    std::size_t count = 0;
    for (const auto& child : target_->children()) {
        const std::optional<layout::LayoutInfo> box = model_->find(*child);
        if (!box.has_value()) {
            // A child the last pass did not measure (added since) cannot be counted: counting it
            // would claim a width nobody measured.
            break;
        }
        const double width = static_cast<double>(box->border_box.width);
        const double next = used + (count == 0 ? 0.0 : gap_) + width;
        if (next > available) {
            break;
        }
        used = next;
        ++count;
    }
    outputs_[0].number = static_cast<double>(count);
    outputs_[1].boolean = count < target_->children().size();
}

} // namespace tessera::ui::behavior
