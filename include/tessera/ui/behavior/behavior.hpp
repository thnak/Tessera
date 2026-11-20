// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tessera::ui {
class InputRouter;
}

// The behavior seam (ADR-064 item 7; docs/design/behavior-hooks.md section 5,
// docs/specs/behavior-hooks-v1.md section 8).
//
// A behavior is engine code, not generated code: a TSX `useFocusTrap(...)` call lowers to an IR
// `behaviors` entry, and the generated base only attaches, updates and detaches the instance. That
// keeps the rules testable headlessly through the scene and the manual clock, and keeps a behavior
// out of the code generator's hot path.
//
// The contract is deliberately small:
//
//   attach(Targets, Args)  every target ref holds a node; may allocate; registers listeners; may
//                          not invoke a callback.
//   update(Args)           the evaluated value options changed; no allocation; may not invoke a
//                          callback.
//   detach()               before the target node is destroyed, when a ref becomes empty, and at
//                          unmount; removes every registration; idempotent.
//   outputs()              the current outputs, read by the component after each dispatch.
//
// A behavior never sees a window handle or a swapchain (ADR-020) and reads time only through the
// runtime clock interface, so a `ManualClock` test is deterministic.
namespace tessera::ui::behavior {

// One target a behavior attaches to: the node a `useRef` resolved to. A behavior with two targets
// (a Popover takes `trigger` and `floating`) receives them in manifest order.
struct Targets {
    std::span<Node* const> nodes;

    [[nodiscard]] bool all_attached() const {
        for (const Node* node : nodes) {
            if (node == nullptr) {
                return false;
            }
        }
        return true;
    }
};

// The engine services a behavior may read. A behavior never holds a second pointer to the scene: it
// is handed the dispatch that owns the interaction state, so a detached node cannot be read and the
// behavior cannot outlive the scene it belongs to.
struct Services {
    // The scene's normalized interaction state (`hovered()`, `active()`, keyboard focus). Null in a
    // build that has no scene, which a behavior must treat as "nothing is pressed".
    InputRouter* router = nullptr;
};

// One evaluated option value. The manifest declares each field's type, so a behavior reads the
// field it expects and ignores the rest; a closed token list is a string.
struct ArgValue {
    enum class Kind : std::uint8_t {
        Bool,
        Number,
        String,
        Handler,
    };

    Kind kind = Kind::Bool;
    bool boolean = false;
    double number = 0.0;
    std::string_view text;
    // A handler arg is an id the generated base resolves to a thunk; a behavior only forwards it to
    // the dispatch that invokes it, so it never calls one itself.
    std::uint32_t handler = 0;
};

// The evaluated options of one update, in manifest order.
struct ArgsView {
    std::span<const ArgValue> values;

    [[nodiscard]] const ArgValue* find(std::string_view name,
                                       std::span<const std::string_view> names) const {
        for (std::size_t index = 0; index < names.size() && index < values.size(); ++index) {
            if (names[index] == name) {
                return &values[index];
            }
        }
        return nullptr;
    }
};

// One output value. The manifest declares the type, so the generated base reads the field it
// expects; before the first attach and after detach an output holds its manifest default.
struct OutputValue {
    enum class Kind : std::uint8_t {
        Bool,
        Number,
        String,
    };

    Kind kind = Kind::Bool;
    bool boolean = false;
    double number = 0.0;
    std::string_view text;
};

// The engine interface every behavior implements. Instances live in bounded slots counted from the
// IR (as `useEffect` slots do, `component_lifecycle.hpp`), so mounting, updating and unmounting a
// component allocate nothing after the first attach.
class Behavior {
public:
    virtual ~Behavior() = default;

    Behavior(const Behavior&) = delete;
    Behavior& operator=(const Behavior&) = delete;

    // Called when every target ref holds a node. May allocate; may not invoke a callback.
    virtual void attach(const Targets& targets, const Services& services, const ArgsView& args) = 0;
    // Called when the hash of the evaluated value options changed. No allocation; may not invoke a
    // callback.
    virtual void update(const ArgsView& args) = 0;
    // Called before the target node is destroyed, when a ref becomes empty, and at unmount. Removes
    // every registration. Idempotent.
    virtual void detach() = 0;
    // The current outputs, in manifest order.
    [[nodiscard]] virtual std::span<const OutputValue> outputs() const = 0;

protected:
    Behavior() = default;
};

} // namespace tessera::ui::behavior
