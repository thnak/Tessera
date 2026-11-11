// SPDX-License-Identifier: MIT

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/click_synthesis.hpp"

#include <cmath>

namespace tessera::ui {
namespace {

float slop_for(PointerKind kind) {
    switch (kind) {
    case PointerKind::Touch:
        return ClickSynthesizer::kTouchSlop;
    case PointerKind::Pen:
        return ClickSynthesizer::kPenSlop;
    case PointerKind::Mouse:
    default:
        return -1.0f; // no slop
    }
}

std::size_t depth_of(const Node* node) {
    std::size_t depth = 0;
    for (; node != nullptr; node = node->parent()) {
        ++depth;
    }
    return depth;
}

} // namespace

void ClickSynthesizer::press(Node& target, const InputEvent& input, bool primary) {
    cancel(input.pointer_id); // a repeated press for one pointer restarts its sequence
    if (!primary || !button_can_click(input.button) || press_count_ == kMaxPresses) {
        return;
    }
    presses_[press_count_++] =
        Press{input.pointer_id, target.id(), &target, input.x, input.y, input.pointer_kind, false};
}

void ClickSynthesizer::move(const InputEvent& input) {
    for (std::size_t i = 0; i < press_count_; ++i) {
        Press& press = presses_[i];
        if (press.pointer_id != input.pointer_id) {
            continue;
        }
        const float slop = slop_for(press.kind);
        if (slop >= 0.0f && std::hypot(input.x - press.x, input.y - press.y) > slop) {
            press.exceeded_slop = true;
        }
        return;
    }
}

bool ClickSynthesizer::take(std::uint32_t pointer_id, Press& out) {
    for (std::size_t i = 0; i < press_count_; ++i) {
        if (presses_[i].pointer_id != pointer_id) {
            continue;
        }
        out = presses_[i];
        presses_[i] = presses_[--press_count_];
        presses_[press_count_] = Press{};
        return true;
    }
    return false;
}

void ClickSynthesizer::cancel(std::uint32_t pointer_id) {
    Press discarded;
    (void)take(pointer_id, discarded);
}

void ClickSynthesizer::clear() {
    presses_ = {};
    press_count_ = 0;
    space_armed_id_ = 0;
}

Node* ClickSynthesizer::common_ancestor(Node* a, Node* b) {
    if (a == nullptr || b == nullptr) {
        return nullptr;
    }
    std::size_t depth_a = depth_of(a);
    std::size_t depth_b = depth_of(b);
    while (depth_a > depth_b) {
        a = a->parent();
        --depth_a;
    }
    while (depth_b > depth_a) {
        b = b->parent();
        --depth_b;
    }
    while (a != b) {
        a = a->parent();
        b = b->parent();
        if (a == nullptr || b == nullptr) {
            return nullptr;
        }
    }
    return a;
}

bool ClickSynthesizer::keyboard_activatable(const Node& node) {
    return node.element_kind() == "Button" || node.role() == "button";
}

bool ClickSynthesizer::key(InputEventType type, KeyCode key_code, bool repeat, Modifier modifiers,
                           const Node* focused, bool consumed) {
    const bool shortcut = has_modifier(modifiers, Modifier::Control) ||
                          has_modifier(modifiers, Modifier::Alt) ||
                          has_modifier(modifiers, Modifier::Meta);
    const bool eligible =
        focused != nullptr && !consumed && !shortcut && keyboard_activatable(*focused);
    if (type == InputEventType::KeyDown) {
        if (!eligible) {
            space_armed_id_ = 0;
            return false;
        }
        if (key_code == key::kEnter) {
            return !repeat;
        }
        if (key_code == key::kSpace) {
            space_armed_id_ = focused->id();
        }
        return false;
    }
    if (type == InputEventType::KeyUp && key_code == key::kSpace) {
        const bool fire = eligible && space_armed_id_ != 0 && space_armed_id_ == focused->id();
        space_armed_id_ = 0;
        return fire;
    }
    return false;
}

} // namespace tessera::ui
