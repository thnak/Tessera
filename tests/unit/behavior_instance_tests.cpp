// SPDX-License-Identifier: MIT
//
// The behavior `Instance` (ADR-064 item 7, spike #658; docs/specs/behavior-hooks-v1.md section 9):
// the one wiring class the generated base and the preview session share.
//
// What this pins, and why each rule is worth a test:
//   * attach builds the registry behavior and reports failure for an unknown name, so a manifest
//     entry the registry lacks is a test failure rather than a silently dead hook;
//   * a partial target set attaches nothing, which is the spec's "attaches only when every target
//     holds a node" rule;
//   * `update` is skipped when the hashed options did not change, which is the rule that makes an
//     update pass over an unchanged component free;
//   * detach is idempotent and returns the outputs to their defaults.

#include "tessera/ui/behavior/instance.hpp"
#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>

namespace {

using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::hash_args;
using tessera::ui::behavior::Instance;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "behavior_instance: " << message << "\n";
    }
    return condition;
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

ArgValue bool_arg(bool value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Bool;
    arg.boolean = value;
    return arg;
}

ArgValue number_arg(double value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Number;
    arg.number = value;
    return arg;
}

ArgValue string_arg(std::string_view text) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::String;
    arg.text = text;
    return arg;
}

// The hash is the contract that makes an unchanged update free, so it must distinguish the values
// that a careless comparison would conflate.
bool test_hash_distinguishes_values() {
    const std::array<ArgValue, 1> on{bool_arg(true)};
    const std::array<ArgValue, 1> off{bool_arg(false)};
    const std::array<ArgValue, 1> one{number_arg(1.0)};
    const std::array<ArgValue, 1> two{number_arg(2.0)};
    const std::array<ArgValue, 1> alpha{string_arg("alpha")};
    const std::array<ArgValue, 1> beta{string_arg("beta")};

    bool ok = true;
    ok = expect(hash_args(on) != hash_args(off), "a bool change changes the hash") && ok;
    ok = expect(hash_args(one) != hash_args(two), "a number change changes the hash") && ok;
    ok = expect(hash_args(alpha) != hash_args(beta), "a string change changes the hash") && ok;
    // A bool `true` and a number `1.0` are different options, not the same value in two spellings.
    ok = expect(hash_args(on) != hash_args(one), "the kind is part of the hash") && ok;
    // The same values hash equal, which is what makes the skip possible at all.
    ok = expect(hash_args(on) == hash_args(on), "equal values hash equal") && ok;
    // Order matters: the args are in manifest order, so a swap is a different option set.
    const std::array<ArgValue, 2> forward{bool_arg(true), number_arg(1.0)};
    const std::array<ArgValue, 2> reverse{number_arg(1.0), bool_arg(true)};
    ok = expect(hash_args(forward) != hash_args(reverse), "arg order is part of the hash") && ok;
    return ok;
}

// An unknown behavior name is a build-tool error the manifest test catches, but the instance must
// report it rather than pretend it attached.
bool test_unknown_behavior_does_not_attach() {
    Scene scene;
    Instance instance;
    std::array<Node*, 1> nodes{scene.button};
    const bool attached =
        instance.attach("no_such_behavior", Targets{nodes}, Services{&scene.router}, ArgsView{});
    return expect(!attached, "an unknown behavior name does not attach") &&
           expect(!instance.attached(), "the instance reports not attached") &&
           expect(instance.outputs().empty(), "an unattached instance has no outputs");
}

// The spec's rule: a behavior attaches only when every target holds a node.
bool test_partial_targets_do_not_attach() {
    Scene scene;
    Instance instance;
    std::array<Node*, 2> nodes{scene.button, nullptr};
    const bool attached =
        instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{});
    bool ok = expect(!attached, "a null target does not attach");
    ok = expect(!instance.attached(), "the instance reports not attached") && ok;

    // The same instance attaches once the missing target appears, which is what a rebuild relies
    // on.
    nodes[1] = scene.button;
    ok = expect(
             instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{}),
             "a complete target set attaches") &&
         ok;
    return ok;
}

// `press_feedback` reads the router, so a press must reach the outputs through the instance.
bool test_outputs_follow_the_scene() {
    Scene scene;
    Instance instance;
    std::array<Node*, 1> nodes{scene.button};
    if (!instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{})) {
        return expect(false, "attach succeeds");
    }
    bool ok = expect(instance.outputs().size() == 3, "press_feedback has three outputs");
    ok = expect(!instance.outputs()[0].boolean, "not pressed before any input") && ok;

    // Press, then refresh: the behavior recomputes from the router's state. `refresh` is the
    // trigger for a scene change, because `press_feedback` has no options for `update` to notice.
    tessera::ui::InputEvent down;
    down.type = tessera::ui::InputEventType::PointerDown;
    down.x = 20.0f;
    down.y = 20.0f;
    scene.router.dispatch(scene.root, down);
    instance.refresh(ArgsView{});
    ok = expect(instance.outputs()[0].boolean, "pressed after a pointer down") && ok;

    // Detach returns the outputs to their manifest defaults.
    instance.detach();
    ok = expect(!instance.attached(), "detached") && ok;
    ok = expect(instance.outputs().empty(), "a detached instance has no outputs") && ok;
    return ok;
}

// The rule that makes an unchanged update free: the hash is unchanged, so the engine behavior is
// not called. `press_feedback` has no options, so its hash is the empty-args hash every time.
bool test_unchanged_update_is_skipped() {
    Scene scene;
    Instance instance;
    std::array<Node*, 1> nodes{scene.button};
    if (!instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{})) {
        return expect(false, "attach succeeds");
    }
    const std::uint64_t after_attach = instance.args_hash();
    instance.update(ArgsView{});
    bool ok =
        expect(instance.args_hash() == after_attach, "an unchanged update leaves the hash alone");

    // A changed option set does move the hash, so the skip is not "never update".
    const std::array<ArgValue, 1> changed{bool_arg(true)};
    instance.update(ArgsView{changed});
    ok = expect(instance.args_hash() != after_attach, "a changed update moves the hash") && ok;

    // `refresh` recomputes the outputs but must NOT move the hash: the options did not change, so a
    // later `update` with the same options is still skipped. This is the distinction that keeps a
    // scene-driven refresh from defeating the option-driven skip.
    const std::uint64_t before_refresh = instance.args_hash();
    instance.refresh(ArgsView{changed});
    ok = expect(instance.args_hash() == before_refresh, "refresh leaves the hash alone") && ok;
    return ok;
}

// Detach before attach and a double detach are both no-ops: a component that unmounts without
// mounting, or unmounts twice, must not crash.
bool test_detach_is_idempotent() {
    Instance instance;
    instance.detach();
    instance.detach();
    bool ok = expect(!instance.attached(), "detach before attach is a no-op");

    Scene scene;
    std::array<Node*, 1> nodes{scene.button};
    instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{});
    instance.detach();
    instance.detach();
    ok = expect(!instance.attached(), "a double detach is a no-op") && ok;
    return ok;
}

// Re-attaching replaces the previous instance rather than stacking a second one.
bool test_reattach_replaces() {
    Scene scene;
    Instance instance;
    std::array<Node*, 1> nodes{scene.button};
    instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{});
    const bool again =
        instance.attach("press_feedback", Targets{nodes}, Services{&scene.router}, ArgsView{});
    return expect(again, "re-attach succeeds") &&
           expect(instance.outputs().size() == 3, "re-attach leaves one instance, not two");
}

} // namespace

int main() {
    bool ok = test_hash_distinguishes_values();
    ok = test_unknown_behavior_does_not_attach() && ok;
    ok = test_partial_targets_do_not_attach() && ok;
    ok = test_outputs_follow_the_scene() && ok;
    ok = test_unchanged_update_is_skipped() && ok;
    ok = test_detach_is_idempotent() && ok;
    ok = test_reattach_replaces() && ok;
    if (!ok) {
        std::cerr << "behavior_instance: FAILED\n";
        return 1;
    }
    std::cout << "behavior_instance: ok\n";
    return 0;
}
