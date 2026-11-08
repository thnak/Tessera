// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

struct InputDispatchResult {
    bool dispatched = false;
    EventType type = EventType::PointerMove;
    Node* target = nullptr;
};

InputDispatchResult dispatch_input(Node& root, const InputEvent& input);

class InputRouter {
public:
    InputDispatchResult dispatch(Node& root, const InputEvent& input);
    void reset();

    [[nodiscard]] Node* captured() const {
        return captured_;
    }

private:
    Node* captured_ = nullptr;
};

} // namespace tessera::ui
