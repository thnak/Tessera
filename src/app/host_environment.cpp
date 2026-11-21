// SPDX-License-Identifier: MIT
//
// The host's half of `useBreakpoint()` (docs/specs/jsx-subset-v1.md section 7.1, issue #692): the
// size class is computed from the same `MediaContext` the `@media` evaluator reads, and pushed to
// every registered consumer after a size change. Split out of `host.cpp`, which is at its budget.

#include "host_impl.hpp"

namespace tessera::app {

ui::responsive::EnvironmentSet& AppHost::environment_values() {
    return impl_->environment;
}

ui::responsive::SizeClass AppHost::size_class() const {
    return ui::responsive::size_class_for(impl_->media_context);
}

void AppHost::Impl::push_size_class() {
    // The class comes from the media context, not from `config.width` directly: the context is what
    // the stylesheet's `@media` rules resolve against, so the hook and a breakpoint rule cannot
    // disagree about which side of 600 px the window is on.
    const ui::responsive::SizeClass next = ui::responsive::size_class_for(media_context);
    if (next == pushed_size_class) {
        return; // a resize inside one class costs nothing: no walk, no setter, no re-render
    }
    pushed_size_class = next;
    (void)environment.push("breakpoint", ui::responsive::size_class_name(next));
}

} // namespace tessera::app
