// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

struct InputDispatchResult {
    bool dispatched = false;
    EventType type = EventType::PointerMove;
    Node* target = nullptr;
    // True when a handler set `prevent_default` and the dispatcher therefore skipped its default
    // action (currently only Tab focus traversal).
    bool default_prevented = false;
    // True when the default action actually moved focus (Tab/Shift+Tab).
    bool focus_moved = false;
};

InputDispatchResult dispatch_input(Node& root, const InputEvent& input);

class InputRouter {
public:
    InputDispatchResult dispatch(Node& root, const InputEvent& input);
    void reset();

    [[nodiscard]] Node* captured() const {
        return captured_;
    }

    [[nodiscard]] FocusManager& focus_manager() {
        return focus_manager_;
    }
    [[nodiscard]] const FocusManager& focus_manager() const {
        return focus_manager_;
    }

private:
    Node* captured_ = nullptr;
    FocusManager focus_manager_;
};

} // namespace tessera::ui
