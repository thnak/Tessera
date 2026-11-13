// SPDX-License-Identifier: MIT

// ADR-040 generated base, runtime side (issue #157, slice 1): a hand-written developer class
// derives from `gen::SampleBase<Sample>`; the generated thunks reach its members through static
// dispatch, the accessors read the generated state, and the contract concept holds.

#include "Sample.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <string_view>
#include <type_traits>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static_assert(demo::gen::SampleContract<demo::Sample>,
              "the developer class satisfies the generated contract concept");
static_assert(std::is_base_of_v<demo::gen::SampleBase<demo::Sample>, demo::Sample>);
static_assert(std::is_final_v<demo::Sample>);
static_assert(!std::is_polymorphic_v<demo::Sample>, "static dispatch: no vtable");

} // namespace

int main() {
    demo::Sample sample;
    tessera::ui::Node root(1000);
    sample.build(root);
    check(root.children().size() == 1, "build() attaches the generated tree");

    // Effect binding: mount() runs bind::sync once through the thunk.
    sample.mount();
    check(sample.syncs == 1, "mount() reaches the developer sync() through the generated thunk");
    check(sample.last_count_seen == 0, "the developer member reads state through its accessor");

    // Event binding: a real click on the generated Button runs the node adapter, which reaches the
    // private thunk through the friend Thunks struct and then the developer's on_click().
    tessera::ui::Node& button = *root.children()[0]->children()[0];
    tessera::ui::Event click;
    click.type = tessera::ui::EventType::Click;
    check(button.run_event_bindings(click) == 1, "the Button has the generated click binding");
    check(sample.clicks == 1, "the on_click thunk reaches the developer member");
    check(sample.count() == 1, "set_count() wrote the cell and the accessor reads it back");

    // A dependency change re-runs the effect: cleanup first, then the effect again.
    sample.update();
    check(sample.cleanups == 1 && sample.syncs == 2,
          "update() re-runs the effect with its cleanup through the thunks");

    sample.unmount();
    check(sample.cleanups == 2, "unmount() runs the cleanup through its thunk");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen base runtime tests passed\n";
    return 0;
}
