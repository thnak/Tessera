// SPDX-License-Identifier: MIT

// Dismissal of overlays (docs/specs/overlays-and-layers-v1.md sections 4 and 8.1, issue #302):
// Escape, back, a press outside, and window blur.

#include "overlay_entry.hpp"

namespace tessera::ui {

namespace {

using overlay_detail::find_node;
using overlay_detail::within;

bool escape_allowed(const OverlaySpec& spec) noexcept {
    return spec.dismiss_on_escape.value_or(spec.layer != OverlayLayer::Toast);
}

bool outside_allowed(const OverlaySpec& spec) noexcept {
    return spec.dismiss_on_outside.value_or(spec.layer == OverlayLayer::Overlay);
}

// Stacking order: layer, then priority, then open order.
bool outranks(const OverlayHost::Entry& a, const OverlayHost::Entry& b) noexcept {
    if (a.spec.layer != b.spec.layer) {
        return a.spec.layer > b.spec.layer;
    }
    if (a.spec.priority != b.spec.priority) {
        return a.spec.priority > b.spec.priority;
    }
    return a.sequence > b.sequence;
}

} // namespace

OverlayHost::Entry* OverlayHost::below(const Entry* above) const {
    Entry* best = nullptr;
    for (const auto& candidate : entries_) {
        if (candidate->state != Entry::State::Open || candidate->closing) {
            continue;
        }
        if (above != nullptr && !outranks(*above, *candidate)) {
            continue;
        }
        if (best == nullptr || outranks(*candidate, *best)) {
            best = candidate.get();
        }
    }
    return best;
}

void OverlayHost::dismiss(Entry& entry, OverlayDismissReason reason) {
    if (entry.closing) {
        return;
    }
    if (entry.spec.on_dismiss) {
        entry.spec.on_dismiss(OverlayHandle{entry.sequence}, reason);
    }
    entry.closing = true;
}

bool OverlayHost::dismiss_top(OverlayDismissReason reason) {
    for (Entry* entry = below(nullptr); entry != nullptr; entry = below(entry)) {
        if (escape_allowed(entry->spec)) {
            dismiss(*entry, reason);
            return true;
        }
        // A dialog owns the keyboard: nothing beneath it may react.
        if (entry->spec.layer == OverlayLayer::Modal) {
            return false;
        }
    }
    return false;
}

bool OverlayHost::handle_escape() {
    return dismiss_top(OverlayDismissReason::Escape);
}

bool OverlayHost::handle_back() {
    return dismiss_top(OverlayDismissReason::Back);
}

std::size_t OverlayHost::handle_pointer_down(float x, float y) {
    const Node* const target = root_.hit_test_deep(x, y);
    if (target == nullptr) {
        return 0;
    }
    const Entry* pressed = nullptr;
    for (const auto& entry : entries_) {
        if (entry->state == Entry::State::Open && !entry->closing &&
            within(entry->wrapper, target)) {
            pressed = entry.get();
        }
    }
    // Hints and status never dismiss what sits below them.
    if (pressed != nullptr && pressed->spec.layer >= OverlayLayer::Tooltip) {
        return 0;
    }
    std::size_t dismissed = 0;
    for (Entry* entry = below(nullptr); entry != nullptr; entry = below(entry)) {
        if (entry == pressed && within(entry->content, target)) {
            break;
        }
        // The trigger of a popover toggles it itself, so a press on the anchor is not outside.
        bool on_anchor = false;
        if (entry->spec.placement &&
            entry->spec.placement->anchor.kind == OverlayAnchor::Kind::Node) {
            on_anchor = within(find_node(root_, entry->spec.placement->anchor.node), target);
        }
        if (outside_allowed(entry->spec) && !on_anchor) {
            dismiss(*entry, OverlayDismissReason::Outside);
            ++dismissed;
        }
        // A dialog is inert to everything beneath it, whatever the press hit.
        if (entry->spec.layer == OverlayLayer::Modal) {
            break;
        }
    }
    return dismissed;
}

std::size_t OverlayHost::close_transient() {
    std::size_t closed = 0;
    for (const auto& entry : entries_) {
        const OverlayLayer layer = entry->spec.layer;
        if (entry->state != Entry::State::Open || entry->closing || entry->spec.persistent ||
            (layer != OverlayLayer::Overlay && layer != OverlayLayer::Tooltip)) {
            continue;
        }
        dismiss(*entry, OverlayDismissReason::Blur);
        ++closed;
    }
    return closed;
}

} // namespace tessera::ui
