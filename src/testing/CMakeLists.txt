# SPDX-License-Identifier: MIT
#
# `tessera_testing`: app-level test support (issue #454, ADR-050). It is the app-level counterpart of
# the seams that already existed one layer down (`ManualClock`, `ManualIoExecutor`, `FakeNet`,
# `FakeShell`, `SyntheticEventInjector`, the headless `AppHost`, the automation snapshot).
#
# **No shipping target links this, and that is the point.** The issue's acceptance says "the target
# adds no dependency to production binaries"; keeping it as its own static library is what makes that
# checkable in a build graph rather than only in review. An app's test binary links it; the app links
# `tessera_app` and `tessera_appfw_*` and never sees it. The two executables under
# `tests/unit/testing/` are its only in-tree consumers.
#
# It is defined unconditionally, like `tessera_app`, because a library nobody links costs nothing to
# build and a conditional target would make "does a shipping binary link it" depend on a configure
# flag. The rule that matters is the *dependency direction*: nothing under `src/app`, `src/appfw` or
# `samples/` names this target, and adding such a link would be the change that breaks the promise.
#
# What it links is what it composes: the app host and its frame pipeline (`tessera_app`), the layer-1
# service contract and the fakes that ship with the modules they fake (`tessera_appfw_core`,
# `tessera_appfw_net`, `tessera_appfw_shell`, `tessera_appfw_storage`), the deterministic layer-0
# seams (`tessera_runtime`), the synthetic OS event injector and the normalized metrics
# (`tessera_hal_metrics`), the input pump the window driver feeds (`tessera_input_pump`), and the
# scene->snapshot walk (`tessera_automation_scene`). It deliberately does NOT link a window backend or
# a GPU backend: that is what lets the same test binary run on every lane, including lavapipe and the
# Wasm lane.
add_library(tessera_testing STATIC
    test_app.cpp)

target_include_directories(tessera_testing PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_testing PUBLIC cxx_std_23)

target_link_libraries(tessera_testing
    PUBLIC
        # The app under test: the frame loop, its pipeline and the scene/style types a component
        # builds into.
        tessera_app
        # The service contract, the runner and `ServiceContext`, plus the fakes that ship with the
        # modules they fake.
        tessera_appfw_core
        tessera_appfw_net
        tessera_appfw_shell
        tessera_appfw_storage
        # The snapshot walk, which is what a structural assertion compares instead of pixels.
        tessera_automation_scene
        # The synthetic OS-event injector and the normalized `WindowMetrics` it carries. The metrics
        # target is the platform-neutral one, so linking it cannot pull an OS window backend in.
        tessera_hal_metrics
        # The pump the window driver feeds: `tap`/`type_text` dispatch through the production router.
        tessera_input_pump
        # `ManualClock`, `CompletionQueue`, `ManualIoExecutor`, `TimerQueue`, `RuntimeLoop`.
        tessera_runtime
        # `ui::style::compile_stylesheet`, for the `Config::stylesheet` field.
        tessera_style)
