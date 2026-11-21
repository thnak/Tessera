// SPDX-License-Identifier: MIT
//
// `scroll_lock` (ADR-064 item 7, issue #633): stop the background from scrolling while a modal
// layer is open.
//
// The engine owns the mechanism (a node with its scroll axes clear is not a scroll container, so a
// wheel chains past it); this behavior owns the hook's half: the reference count that lets two
// nested layers lock the same page without unlocking each other, and the restore of the axes the
// node had before the first lock.

#include "tessera/ui/behavior/scroll_lock.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::ScrollLock;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "scroll_lock: " << message << "\n";
        ++failures;
    }
}

// A page that scrolls on both axes, with a dialog inside it.
struct Scene {
    Node root{1};
    Node* page = nullptr;
    Node* dialog = nullptr;

    Scene() {
        root.set_bounds({0, 0, 400, 400});
        auto body = std::make_unique<Node>(2);
        body->set_bounds({0, 0, 400, 400});
        body->set_scrollable_x(true);
        body->set_scrollable_y(true);
        page = &root.add_child(std::move(body));

        auto box = std::make_unique<Node>(3);
        box->set_bounds({0, 50, 200, 200});
        dialog = &page->add_child(std::move(box));
    }
};

ArgValue bool_arg(bool value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Bool;
    arg.boolean = value;
    return arg;
}

std::array<ArgValue, 1> options(bool active) {
    return {bool_arg(active)};
}

void test_attach_locks_the_target() {
    Scene scene;
    ScrollLock lock;
    auto args = options(true);
    lock.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
    expect(!scene.page->scrollable_x(), "a locked page does not scroll horizontally");
    expect(!scene.page->scrollable_y(), "a locked page does not scroll vertically");
    expect(lock.target() == scene.page, "the behavior reports its target");
    const auto outputs = lock.outputs();
    expect(outputs.size() == 1 && outputs[0].kind == OutputValue::Kind::Bool && outputs[0].boolean,
           "the `locked` output reports the lock is held");
    expect(ScrollLock::lock_count(*scene.page) == 1, "one reference is held");
}

void test_inactive_lock_marks_nothing() {
    Scene scene;
    ScrollLock lock;
    auto args = options(false);
    lock.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
    expect(scene.page->scrollable_x() && scene.page->scrollable_y(),
           "an inactive lock leaves the page scrollable");
    expect(!lock.outputs()[0].boolean, "the output reports the lock is not held");
    expect(ScrollLock::lock_count(*scene.page) == 0, "no reference is held");
}

void test_update_toggles_the_lock() {
    Scene scene;
    ScrollLock lock;
    auto on = options(true);
    lock.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{on});
    expect(!scene.page->scrollable_y(), "the lock starts held");

    auto off = options(false);
    lock.update(ArgsView{off});
    expect(scene.page->scrollable_y(), "an update releases the lock");
    expect(!lock.outputs()[0].boolean, "the output follows the option");

    lock.update(ArgsView{on});
    expect(!scene.page->scrollable_y(), "an update re-acquires the lock");
}

// The rule the reference count exists for: two nested layers lock the same page, and the inner one
// closing must not unlock it.
void test_nested_locks_are_reference_counted() {
    Scene scene;
    ScrollLock outer;
    ScrollLock inner;
    auto args = options(true);
    outer.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
    inner.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
    expect(ScrollLock::lock_count(*scene.page) == 2, "two references are held");

    inner.detach();
    expect(!scene.page->scrollable_y(),
           "releasing the inner lock leaves the page locked for the outer one");
    expect(ScrollLock::lock_count(*scene.page) == 1, "one reference remains");

    outer.detach();
    expect(scene.page->scrollable_y(), "releasing the last lock restores the page");
    expect(ScrollLock::lock_count(*scene.page) == 0, "no reference remains");
}

// The axes are restored to what they were, not assumed to be both on.
void test_restores_the_original_axes() {
    Scene scene;
    scene.page->set_scrollable_x(false); // a page that only scrolls vertically
    ScrollLock lock;
    auto args = options(true);
    lock.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
    expect(!scene.page->scrollable_y(), "the vertical axis is locked");
    lock.detach();
    expect(!scene.page->scrollable_x(), "the horizontal axis stays off, as it was");
    expect(scene.page->scrollable_y(), "the vertical axis is restored");
}

void test_detach_is_idempotent() {
    Scene scene;
    ScrollLock lock;
    auto args = options(true);
    lock.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
    lock.detach();
    expect(scene.page->scrollable_y(), "the first detach restores the page");
    lock.detach();
    expect(scene.page->scrollable_y(), "a second detach does not touch the page again");
    expect(ScrollLock::lock_count(*scene.page) == 0, "no reference is left behind");
}

void test_missing_target_attaches_nothing() {
    Scene scene;
    ScrollLock lock;
    auto args = options(true);
    lock.attach(Targets{std::array<Node*, 1>{nullptr}}, Services{}, ArgsView{args});
    expect(lock.target() == nullptr, "a behavior with an unresolved target attaches nothing");
    expect(scene.page->scrollable_y(), "and locks no node");
}

// A behavior destroyed without an explicit detach must not leave the page unscrollable.
void test_destructor_releases_the_lock() {
    Scene scene;
    {
        ScrollLock lock;
        auto args = options(true);
        lock.attach(Targets{std::array<Node*, 1>{scene.page}}, Services{}, ArgsView{args});
        expect(!scene.page->scrollable_y(), "the lock is held inside the scope");
    }
    expect(scene.page->scrollable_y(), "the destructor releases the lock");
    expect(ScrollLock::lock_count(*scene.page) == 0, "no reference is left behind");
}

} // namespace

int main() {
    test_attach_locks_the_target();
    test_inactive_lock_marks_nothing();
    test_update_toggles_the_lock();
    test_nested_locks_are_reference_counted();
    test_restores_the_original_axes();
    test_detach_is_idempotent();
    test_missing_target_attaches_nothing();
    test_destructor_releases_the_lock();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
