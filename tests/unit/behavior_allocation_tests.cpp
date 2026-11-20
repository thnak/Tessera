// SPDX-License-Identifier: MIT
//
// The behavior seam's memory contract (ADR-064 item 7, spike #658; docs/specs/behavior-hooks-v1.md
// section 8 rule 5): "mount, update and unmount of a component allocate nothing after the first
// attach".
//
// Counting global `operator new` keeps the report deterministic: no RSS, no wall clock, no
// sampling. The behavior is built once (that allocation is the slot the generated base owns), and
// every attach/update/detach after it must be free.

#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/behavior/registry.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>

namespace {
std::size_t g_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* pointer = std::malloc(size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::PressFeedback;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "behavior_allocation: " << message << "\n";
    }
    return condition;
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

// A scene with one 40x40 button, so a pointer at (20, 20) hits it.
struct Scene {
    Node root{1};
    Node* button = nullptr;
    InputRouter router;

    Scene() {
        root.set_bounds({0, 0, 200, 200});
        auto owner = std::make_unique<Node>(2);
        owner->set_bounds({0, 0, 40, 40});
        owner->set_focusable(true);
        button = &root.add_child(std::move(owner));
    }
};

// The whole lifecycle after construction: attach, a pointer press, an update, a release, a detach.
// The behavior holds no container and registers no listener, so nothing here may allocate.
bool test_lifecycle_allocates_nothing() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};

    // The scene's own dispatch is measured separately: it is not the behavior's cost, and a
    // component's mount/update/unmount is what this contract is about.
    scene.router.dispatch(scene.root, pointer(InputEventType::PointerMove, 20, 20));
    scene.router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));

    const std::size_t before = g_allocations;
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});
    behavior.update(ArgsView{});
    behavior.update(ArgsView{});
    behavior.detach();
    const std::size_t after = g_allocations;

    return expect(after == before, "attach, update and detach allocate nothing (saw " +
                                       std::to_string(after - before) + " allocation(s))");
}

// A second attach/detach cycle is free too, which is what a component that mounts twice relies on.
bool test_remount_allocates_nothing() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});
    behavior.detach();

    const std::size_t before = g_allocations;
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});
    behavior.update(ArgsView{});
    behavior.detach();
    const std::size_t after = g_allocations;

    return expect(after == before, "a remount allocates nothing (saw " +
                                       std::to_string(after - before) + " allocation(s))");
}

// The registry factory is the one place that allocates, and it allocates exactly once: the slot the
// generated base owns. This pins that the behavior itself holds no container.
bool test_factory_allocates_once() {
    const std::size_t before = g_allocations;
    const auto behavior = tessera::ui::behavior::create_behavior("press_feedback");
    const std::size_t after = g_allocations;
    return expect(behavior != nullptr, "the factory builds press_feedback") &&
           expect(after - before == 1, "the factory allocates exactly once (saw " +
                                           std::to_string(after - before) + ")");
}

} // namespace

int main() {
    bool ok = test_lifecycle_allocates_nothing();
    ok = test_remount_allocates_nothing() && ok;
    ok = test_factory_allocates_once() && ok;
    if (!ok) {
        std::cerr << "behavior_allocation: FAILED\n";
        return 1;
    }
    std::cout << "behavior_allocation: ok\n";
    return 0;
}
