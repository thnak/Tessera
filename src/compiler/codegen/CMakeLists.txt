# SPDX-License-Identifier: MIT

# First C++ codegen slice (docs/design/codegen.md): IR -> native C++ source text. Codegen reads the
# IR only (AGENTS.md non-negotiable 4), so it links tessera_ir and nothing else from the project.
add_library(tessera_codegen STATIC
    src/codegen.cpp
    src/emit_base.cpp
    src/emit_base_accessors.cpp
    src/scaffold.cpp
    src/emit_component.cpp
    src/emit_composition.cpp
    src/emit_conditional.cpp
    src/emit_controlled.cpp
    src/emit_dynamic_header.cpp
    src/emit_dynamic_plan.cpp
    src/emit_dynamic_plan_util.cpp
    src/emit_events.cpp
    src/emit_call_prop.cpp
    # emit_expr.cpp is the expression switch; naming/literal spelling and the action emitter are
    # separate TUs (issue #327: the file was 578 lines, over the 500 hard limit).
    src/emit_expr.cpp
    src/emit_expr_actions.cpp
    src/emit_expr_names.cpp
    src/emit_forms.cpp
    src/emit_layout_observe.cpp
    src/emit_schedule.cpp
    src/emit_services.cpp
    src/emit_snapshot.cpp
    src/emit_stylesheet_embed.cpp
    src/emit_theme_embed.cpp
    src/emit_types_header.cpp
    src/emit_vector.cpp
    src/form_binding.cpp
    src/emit_header.cpp
    src/emit_item_props.cpp
    src/emit_lifecycle.cpp
    src/emit_list.cpp
    # The `#line` map of issue #78: markers the ADR-040 reshaping writes become directives before
    # the file is returned. Off by default, so the committed goldens stay byte-identical.
    src/emit_line_map.cpp
    src/emit_node.cpp
    src/emit_node_attributes.cpp
    src/emit_node_behavior.cpp
    src/emit_node_support.cpp
    src/emit_props.cpp
    src/emit_virtual.cpp
    src/emit_virtual_geometry.cpp
    src/emit_virtual_scroll.cpp
    src/interpolation.cpp
    src/value_ops.cpp)

target_include_directories(tessera_codegen
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_codegen PUBLIC tessera_ir)
target_compile_features(tessera_codegen PUBLIC cxx_std_23)

# Layering rule: codegen sees the IR and its own headers, never the parser, lowering or the UI.
get_target_property(_tessera_codegen_links tessera_codegen LINK_LIBRARIES)
if (NOT _tessera_codegen_links STREQUAL "tessera_ir")
    message(FATAL_ERROR "tessera_codegen must link exactly tessera_ir, found: ${_tessera_codegen_links}")
endif ()

# The CLI driver is the only place that reads JSX and calls the front end; it is a separate target
# so the tessera_codegen library keeps its IR-only dependency (docs/guides/developer-workflow.md).
add_executable(tessera_codegen_cli
    ${CMAKE_SOURCE_DIR}/tools/codegen/main.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_source_file.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_stylesheets.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_text.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_assets.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostics_json.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostic_record.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/project_identity.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/package_manifest.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components_expr.cpp)

target_include_directories(tessera_codegen_cli PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
target_link_libraries(tessera_codegen_cli PRIVATE
    tessera_codegen
    tessera_harness
    tessera_ir_ids
    tessera_lowering
    # A `*.module.css` is scoped before it is embedded (issue #463).
    tessera_css_module
    # The CLI compiles CSS into the blobs codegen embeds (issue #81, ADR-048); the library does not.
    tessera_style)
set_target_properties(tessera_codegen_cli PROPERTIES OUTPUT_NAME tessera_codegen)

# The Emscripten build of the CLI is a Node tool, never a browser/sample-app artifact (issue #164,
# docs/design/compiler-wasm.md): the design-time preview compiles .tsx with the same front end the
# native CLI uses, so the two must behave identically.
#
#   -sENVIRONMENT=node  emits a Node loader only; there is no DOM/browser shim to link against.
#   -sNODERAWFS=1       routes <cstdio>/<filesystem> to Node's synchronous `fs` calls instead of
#                       Emscripten's in-memory MEMFS. Without it the CLI cannot open the .tsx input
#                       or write the output at all (MEMFS only holds what was preloaded), which made
#                       the Wasm compiler unusable outside a preload-manifest build.
#
# NODERAWFS must never leak into a browser artifact: it is undefined in a browser because browsers
# have no synchronous host filesystem, and it would defeat the point of a self-contained preview
# module. Both flags therefore live on this one target, not in a global link option.
if (EMSCRIPTEN)
    target_link_options(tessera_codegen_cli PRIVATE -sNODERAWFS=1 -sENVIRONMENT=node)
endif ()

# The browser diagnostics module (issue #165, P1 of docs/design/reference-equantic-ui.md section 7.4):
# the same front end, analysis, lowering, validator and diagnostics serializer the CLI uses, behind one
# C export the preview page calls with the textarea contents in memory.
#
# Host builds compile the same sources as an OBJECT library instead of the module. It has no `main`, so
# it cannot be an executable there, but compiling it natively is what keeps the entry point
# type-checked on the ordinary Windows/Linux lanes: the Wasm lane builds once per push, the host lanes
# run constantly. The sources are listed once below and shared by both branches.
set(_tessera_diagnostics_sources
    ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_entry.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/diagnose.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostics_json.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostic_record.cpp)

if (NOT EMSCRIPTEN)
    add_library(tessera_diagnostics_entry OBJECT ${_tessera_diagnostics_sources})
    target_include_directories(tessera_diagnostics_entry
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_diagnostics_entry PRIVATE
        tessera_codegen
        tessera_ir_ids
        tessera_lowering)
else ()
    # The module is a PREVIEW artifact, not a Node tool, and the flags say so:
    #   -sMODULARIZE=1 -sEXPORT_ES6=1   the page does `import createModule from './tessera_diagnostics.js'`
    #                                   with no bundler; MODULARIZE keeps the module off `window`.
    #   -sENVIRONMENT=web,node          a browser page and, for CI, the emsdk Node that drives the parity
    #                                   gate. Deliberately NOT `node` alone: the parity gate would then be
    #                                   proving an artifact the page cannot load.
    #   -sALLOW_MEMORY_GROWTH=1         the module allocates per Run (a document, a parse tree); a fixed
    #                                   heap would fail on a large file instead of growing.
    #   --no-entry                      there is no `main`: the module is a library of exports called by
    #                                   the page, not a program.
    #   -sEXPORTED_FUNCTIONS            the two exports `wasm_entry.cpp` defines, plus the malloc/free
    #                                   prefix emscripten needs to hand the page a pointer.
    # NO NODERAWFS here: a browser has no synchronous host filesystem, so it is undefined there and
    # would defeat the point of a self-contained module (docs/design/compiler-wasm.md section 2.1
    # explains why the flag must never leak between the two artifacts). This module never reads a file.
    add_executable(tessera_diagnostics_wasm ${_tessera_diagnostics_sources})
    target_include_directories(tessera_diagnostics_wasm
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_diagnostics_wasm PRIVATE
        tessera_codegen
        tessera_ir_ids
        tessera_lowering)
    set_target_properties(tessera_diagnostics_wasm PROPERTIES OUTPUT_NAME tessera_diagnostics)
    target_link_options(tessera_diagnostics_wasm PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTesseraDiagnostics
        -sENVIRONMENT=web,node
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=["_tessera_diagnostics_json","_tessera_free","_malloc","_free"]
        -sEXPORTED_RUNTIME_METHODS=["UTF8ToString","lengthBytesUTF8","stringToUTF8"])
endif ()

# The browser scene-preview module (issue #166, P2 of docs/design/reference-equantic-ui.md section
# 7.4): the same front end, lowering, evaluator, stylesheet engine and Yoga layout the native harness
# runs, behind one C export the preview page calls with the textarea contents in memory, returning the
# canonical UiSnapshot document of docs/specs/automation-ids-v1.md section 5.2.
#
# It is shaped like the diagnostics module above, and for the same reasons: host builds compile the
# sources as an OBJECT library (no `main`, so it cannot be an executable there) which keeps the entry
# point type-checked on the ordinary Windows/Linux lanes, and only an Emscripten build links them into
# a module. The sources are listed once and shared by both branches.
#
# The driver (`scene_preview.cpp`) is separate from the native CLI binary on purpose: the CLI target
# (`tessera_codegen_cli`) is what the `wasm-compiler` preset builds for P0's IR parity, and adding the
# engine to it would drag Yoga and the text stack into that artifact. The shared thing is the driver,
# not the executable.
set(_tessera_scene_preview_sources
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_evaluate.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_preview.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_prepare.cpp
    # The author-stylesheet choice (issue #390): which sheet a preview compiles, and its diagnostics.
    # One TU so `prepare_scene`, the canvas session and the input path cannot disagree.
    #
    # It scans a `*.module.css` before compiling it (issue #463), so every target that compiles this
    # list names `tessera_css_module` directly. The scanner is already reachable through
    # `tessera_lowering` (which uses it for the same reason), but the preview must not depend on
    # that: a unit that includes `css_module/module_scan.hpp` says so on its own link line.
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_stylesheet.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_build.cpp
    # The IR -> scene walk, split out of `scene_build.cpp` when conditional and repeated children
    # (issue #389) pushed that file past the size limit: one TU per node, one per child set.
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_build_walk.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_build_children.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_build_dynamic.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_layout.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_state.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_session.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_interaction.cpp)

# The persistent canvas session (#388): the tree a pointer event feeds and a frame paints. It is
# NOT in the list above because it links `scene_frame.cpp` (it paints) and the snapshot driver must
# stay free of the GPU/paint stack - the same separation `tessera_scene_frame_entry` makes below.
set(_tessera_canvas_session_sources
    ${CMAKE_SOURCE_DIR}/tools/codegen/scene_canvas_session.cpp)

# The preview's text stack (issue #387): the production FreeType/HarfBuzz/atlas path, loaded from font
# BYTES the caller supplies (the page fetches them; the CLI reads the same shipped file).
#
# Listed separately from `_tessera_scene_preview_sources` for the same reason
# `_tessera_canvas_session_sources` is: it links `tessera_gpu` (the atlas uploads page textures), and
# the P2 snapshot driver's null-RHI claim depends on that library never appearing in its link line.
# Every target that paints a frame adds this list; the snapshot targets do not.
set(_tessera_preview_text_sources
    ${CMAKE_SOURCE_DIR}/tools/codegen/preview_text.cpp)

if (NOT EMSCRIPTEN)
    # The driver as a library, so the parity test (tests/parity) can run the very same evaluation
    # and input session the module exports instead of a second copy of the stage order. The OBJECT
    # library below and the CLI both link it; linking rather than re-listing the sources is what
    # keeps "the native half" and "the Wasm half" one build of one implementation.
    add_library(tessera_scene_preview STATIC ${_tessera_scene_preview_sources})
    target_include_directories(tessera_scene_preview PUBLIC ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_preview PUBLIC
        tessera_automation_scene
        tessera_css_module
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_scene
        tessera_style)

    add_library(tessera_scene_preview_entry OBJECT
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_scene_entry.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp)
    target_include_directories(tessera_scene_preview_entry
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_preview_entry PRIVATE tessera_scene_preview)

    # The GPU half as an OBJECT library too (issue #167). `scene_frame.cpp` is the file the browser
    # module's frame entry calls, and it is deliberately *not* part of the P2 OBJECT library above:
    # that one exists to keep the snapshot driver on the null RHI, and linking tessera_gpu/paint into
    # it would quietly break the claim the P2 gate is about. Compiling it here keeps its RHI-only
    # code (no webgpu.h, no Vulkan header) type-checked on the ordinary Windows/Linux lanes.
    add_library(tessera_scene_frame_entry OBJECT
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
        ${_tessera_preview_text_sources}
        ${_tessera_scene_preview_sources})
    target_include_directories(tessera_scene_frame_entry
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_frame_entry PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_gpu
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_paint
        tessera_scene
        tessera_style)

    # `wasm_canvas_entry.cpp` is Emscripten-only in its body (it includes the WebGPU device header),
    # so the host lane cannot compile it. Its ABI and its report serializer therefore live here as a
    # plain function the Wasm build calls, and the host lane compiles *that* - the same
    # "type-check the entry on every push" rule P1 and P2 followed, adapted to a header the host
    # cannot see.
    add_library(tessera_canvas_report OBJECT
        ${CMAKE_SOURCE_DIR}/tools/codegen/canvas_report.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp
        # The ABI argument parsing (issue #444) is host-buildable on purpose: it includes no
        # Emscripten and no WebGPU header, so listing it here is what makes the host lanes
        # type-check the one file a signature change lands in. `wasm_canvas_support.cpp` (issue
        # #390) is here for the same reason - `clear_canvas` and the pointer vocabulary are pure
        # over their arguments.
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_request.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_support.cpp
        ${_tessera_canvas_session_sources})
    target_include_directories(tessera_canvas_report
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_canvas_report PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_gpu
        tessera_ir
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_paint
        tessera_rhi
        tessera_scene
        tessera_style)
    # The session paints, so it needs `scene_frame.cpp` compiled into this OBJECT library too; it is
    # deliberately not a link dependency because that target is an OBJECT library as well.
    target_sources(tessera_canvas_report PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
        ${_tessera_preview_text_sources}
        ${_tessera_scene_preview_sources})

    # The native half of the parity gate: `tessera_scene_preview --scene-json OUT FILE.tsx` prints the
    # same document the module returns, so scripts/ci/scene_snapshot_parity.py can compare two
    # artifacts instead of two implementations.
    add_executable(tessera_scene_preview_cli
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_preview_main.cpp)
    target_include_directories(tessera_scene_preview_cli
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_preview_cli PRIVATE tessera_scene_preview)
    set_target_properties(tessera_scene_preview_cli PROPERTIES OUTPUT_NAME tessera_scene_preview)

    # The GPU half (issue #167, P3 of docs/design/reference-equantic-ui.md section 7.4): the same
    # prepared scene, painted through an RHI device. Kept in its own sources list and its own
    # executable because it links `tessera_gpu` and `tessera_paint`; the snapshot driver must stay
    # free of both, so the P2 parity gate keeps proving the null-RHI claim.
    #
    #   tessera_scene_frame_cli      native: renders a PNG through Vulkan (the lavapipe reference on
    #                               the Linux lane) or records the command stream on the null backend.
    #                               The image the browser canvas is compared against comes from here.
    add_executable(tessera_scene_frame_cli
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame_main.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
        ${_tessera_preview_text_sources}
        ${_tessera_scene_preview_sources})
    target_include_directories(tessera_scene_frame_cli
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_frame_cli PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_gpu
        tessera_harness_image
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_paint
        tessera_rhi_null
        tessera_scene
        tessera_style)
    set_target_properties(tessera_scene_frame_cli PROPERTIES OUTPUT_NAME tessera_scene_frame)
    # The reference lane is a lavapipe render, so this binary must be able to open a Vulkan device;
    # a build without the Vulkan backend still produces it, and `--backend null` is then the only
    # mode (the CLI says so instead of failing silently).
    #
    # The guard is the root `TESSERA_BUILD_VULKAN` option, not `TARGET tessera_rhi_vulkan`: this
    # directory is processed before `src/rendering/backend/vulkan`, so a TARGET test here is always
    # false and the binary would silently lose its reference backend.
    if (TESSERA_BUILD_VULKAN)
        target_link_libraries(tessera_scene_frame_cli PRIVATE tessera_rhi_vulkan)
        target_compile_definitions(tessera_scene_frame_cli PRIVATE
            TESSERA_SCENE_FRAME_HAS_VULKAN=1)
    endif ()

    # The pixel comparison, split out so `scripts/ci/scene_frame_parity.py --compare` can drive the
    # repository's one tolerance implementation instead of re-deriving SSIM/deltaE in Python.
    add_executable(tessera_scene_frame_compare
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame_compare.cpp)
    target_include_directories(tessera_scene_frame_compare
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_frame_compare PRIVATE
        tessera_golden
        tessera_harness_image)
    set_target_properties(tessera_scene_frame_compare PROPERTIES
        OUTPUT_NAME tessera_scene_frame_compare)
else ()
    # Same flag set as the diagnostics module, with the one difference the artifact needs: nothing
    # here reads a file either, so NODERAWFS is absent for the same documented reason
    # (docs/design/compiler-wasm.md section 2.1).
    add_executable(tessera_scene_preview_wasm
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_scene_entry.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp
        ${_tessera_scene_preview_sources})
    target_include_directories(tessera_scene_preview_wasm
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_scene_preview_wasm PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_scene
        tessera_style)
    set_target_properties(tessera_scene_preview_wasm PROPERTIES OUTPUT_NAME tessera_scene_preview)
    target_link_options(tessera_scene_preview_wasm PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTesseraScenePreview
        -sENVIRONMENT=web,node
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=["_tessera_scene_json","_tessera_scene_json_sized","_tessera_scene_json_styled","_tessera_scene_json_styled_sized","_tessera_scene_json_folded","_tessera_scene_json_folded_styled","_tessera_input_json","_tessera_input_json_styled","_tessera_css_diagnostics_json","_tessera_free","_malloc","_free"]
        -sEXPORTED_RUNTIME_METHODS=["UTF8ToString","lengthBytesUTF8","stringToUTF8"])

    # The canvas module (issue #167, P3): the same prepared scene plus the GPU half, drawing through
    # the WebGPU device the page created. Only Emscripten can build it, because only there does
    # `tessera_webgpu` exist (`<webgpu/webgpu.h>` comes from the emdawnwebgpu port).
    #
    # It is a SEPARATE module from the P2 one on purpose, and the page loads both: the snapshot page
    # must keep working in a browser without WebGPU (design section 7.5 names that risk), and a
    # single module whose import fails would take the snapshot view down with the canvas.
    #
    # `-sEXPORTED_FUNCTIONS` mirrors the P2 ABI plus the device handover, and the two functions the
    # page needs to run a frame. No NODERAWFS: nothing here reads a file (section 2.1).
    add_executable(tessera_canvas_wasm
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_entry.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_support.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_font.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/canvas_report.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp
        # The ABI argument parsing (issue #444), split out of `wasm_canvas_entry.cpp` when the live
        # edit's `apply` export pushed it past the size limit. It is Emscripten-free and WebGPU-free
        # on purpose, so the parsing rules are type-checked on the host lanes too.
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_request.cpp
        # Painting a frame into the page's canvas texture, split from the entry for the same reason
        # and on a real boundary: these functions take the device and the tree they paint, so they
        # cannot reach the module's globals, and the entry no longer has to include the WebGPU
        # target-creation header.
        ${CMAKE_SOURCE_DIR}/tools/codegen/wasm_canvas_paint.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
        ${_tessera_preview_text_sources}
        ${_tessera_canvas_session_sources}
        ${_tessera_scene_preview_sources})
    target_include_directories(tessera_canvas_wasm
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_canvas_wasm PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_gpu
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_paint
        tessera_scene
        tessera_style
        tessera_webgpu)
    set_target_properties(tessera_canvas_wasm PROPERTIES OUTPUT_NAME tessera_canvas)
    target_link_options(tessera_canvas_wasm PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTesseraCanvas
        -sENVIRONMENT=web,node
        -sALLOW_MEMORY_GROWTH=1
        # `_tessera_canvas_apply` is the live edit's export (issue #444): the same evaluation the
        # `run` export performs, answered with `changed` and the state-carry split so an auto-apply
        # of unchanged text costs no frame and a failed one keeps the last good picture.
        -sEXPORTED_FUNCTIONS=["_tessera_canvas_frame","_tessera_canvas_run","_tessera_canvas_apply","_tessera_canvas_pointer","_tessera_canvas_draw","_tessera_canvas_reset","_tessera_canvas_set_font","_tessera_canvas_font_status","_tessera_webgpu_set_device","_tessera_free","_malloc","_free"]
        # `WebGPU` is the emdawnwebgpu port's handle table: the page needs `WebGPU.importJsDevice` and
        # `importJsQueue` to turn a JS GPUDevice/GPUQueue into the integer handles the C ABI takes.
        # `HEAPU8` is the view the page writes font bytes through (issue #387): `_malloc` returns a
        # pointer into the module's linear memory, and without the view exported the page has nowhere to
        # put the bytes - the handover fails with "no HEAPU8" rather than a wrong picture.
        -sEXPORTED_RUNTIME_METHODS=["UTF8ToString","lengthBytesUTF8","stringToUTF8","WebGPU","HEAPU8"])
endif ()

# Goldens of the generated base (`<Component>.g.hpp`) live here; they are written by the
# `--write` mode of their test and are generated from IR, never hand-edited.
#
# ADR-040 (issue #218): a migrated family has no compiled sample library any more. Its developer
# class is the hand-written `tests/unit/codegen_base/fixtures/<Component>.hpp` and the generated
# base is either the committed golden under this directory or a build-time artifact.
set(_tessera_codegen_golden_dir ${CMAKE_SOURCE_DIR}/tests/golden/codegen)

# Callback-forwarding fixture (issue #286), generated from TSX in lowering_callback_forward_tests.
# Same deliberate golden path: `lowering_callback_forward_tests --write` owns ForwardWrap.g.hpp,
# ForwardCounter.g.hpp and ForwardOuter.g.hpp. ADR-040 (#218): the developer classes are the
# hand-written tests/unit/codegen_base/fixtures/Forward*.hpp, so no sample library is compiled.
#
# Per-branch and per-item component instances (issue #231, docs/design/compiler-node-state.md):
# `codegen_instances_tests --write` owns tests/golden/codegen/instances/{Row,Page}.g.hpp.
set(_tessera_codegen_instances_golden_dir ${_tessera_codegen_golden_dir}/instances)

# Library Button fixture (issue #239), generated from packages/tessera-ui/Button.tsx and a demo that
# imports it, in library_button_lowering_tests. Same deliberate golden path:
# `library_button_lowering_tests --write` owns Button.g.hpp and ButtonDemo.g.hpp; the developer
# classes are tests/unit/codegen_base/fixtures/Button*.hpp (ADR-040, #218).

# Library SafeArea fixture (issue #223), generated from packages/tessera-ui/SafeArea.tsx and a demo
# that imports it, in library_safe_area_lowering_tests. Same deliberate golden path:
# `library_safe_area_lowering_tests --write` owns SafeArea.g.hpp, SafeAreaDemo.g.hpp and
# SafeAreaOptions.g.hpp; the developer classes are the fixtures under codegen_base/fixtures.

# onSizeChange fixture (ADR-041 slice 5), generated from the hand-built IR in
# codegen_size_change_tests. Same deliberate golden path: `codegen_size_change_tests --write` owns
# Watcher.g.hpp and WatchPage.g.hpp. The runtime test links tessera_layout for LayoutObservation.

# Compiler demo (ADR-040): the generated base `Demo.g.hpp` is written into the build directory from
# Demo.tsx by the CLI in base mode; the developer class `Demo.hpp/.cpp` is committed under
# samples/compiler-demo and compiled from there.
#
# The generation runs the target-built `tessera_codegen_cli` ON THE HOST at build time, so it is
# host-only: a cross-compiling build (Android/iOS) would try to execute a target binary. Generators
# run on the host, never the target (same rule as the embedded-asset generator); mobile builds skip
# the demo entirely and the host platforms cover it.
if (NOT CMAKE_CROSSCOMPILING)
set(_tessera_codegen_demo_dir ${CMAKE_CURRENT_BINARY_DIR}/samples/compiler-demo)
set(_tessera_codegen_demo_src ${CMAKE_SOURCE_DIR}/samples/compiler-demo)
add_custom_command(
    OUTPUT ${_tessera_codegen_demo_dir}/Demo.g.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_tessera_codegen_demo_dir}
    COMMAND $<TARGET_FILE:tessera_codegen_cli> --write
            --out ${_tessera_codegen_demo_dir} --namespace demo
            ${_tessera_codegen_demo_src}/Demo.tsx
    DEPENDS tessera_codegen_cli
            ${_tessera_codegen_demo_src}/Demo.tsx
    COMMENT "Generating the compiler demo base"
    VERBATIM)

add_library(tessera_compiler_demo STATIC ${_tessera_codegen_demo_src}/Demo.cpp
    ${_tessera_codegen_demo_dir}/Demo.g.hpp)
target_include_directories(tessera_compiler_demo PUBLIC
    ${_tessera_codegen_demo_src} ${_tessera_codegen_demo_dir})
target_link_libraries(tessera_compiler_demo PUBLIC tessera_scene tessera_text_edit
    tessera_animation)
endif ()

if (TESSERA_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(codegen_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_tests.cpp)

    target_link_libraries(codegen_tests PRIVATE
        tessera_codegen
        Threads::Threads)

    add_test(NAME codegen_generate COMMAND codegen_tests)

    # ADR-040 generated base (issue #157): shape, golden and determinism of the CRTP header.
    add_executable(codegen_base_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base_tests.cpp)
    target_link_libraries(codegen_base_tests PRIVATE tessera_codegen)
    target_compile_definitions(codegen_base_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")
    add_test(NAME codegen_base_generate COMMAND codegen_base_tests)

    # ADR-050 section 3.1 (AF5, issue #177): the `useService<T>()` lowering. The generated base
    # resolves each declared requirement once at mount and exposes `service<T>()`; an undeclared
    # service is `TESSERA_CG_SERVICE_UNDECLARED`. The goldens live in
    # tests/golden/codegen/services/, a NEW directory: every pre-existing `.g.hpp` stays
    # byte-identical because emission is conditional on the component having a requirement.
    add_executable(codegen_services_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_services_tests.cpp)
    target_link_libraries(codegen_services_tests PRIVATE tessera_codegen)
    target_compile_definitions(codegen_services_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")
    add_test(NAME codegen_services_generate COMMAND codegen_services_tests)


    # The developer class derives from the generated base and the generated thunks reach it.
    set(_tessera_codegen_base_golden_dir ${_tessera_codegen_golden_dir})
    set(_tessera_codegen_base_dev_dir ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base)
    if (EXISTS ${_tessera_codegen_base_golden_dir}/Sample.g.hpp)
        add_executable(codegen_base_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base_runtime_tests.cpp
            ${_tessera_codegen_base_dev_dir}/Sample.cpp)
        target_include_directories(codegen_base_runtime_tests PRIVATE
            ${_tessera_codegen_base_golden_dir} ${_tessera_codegen_base_dev_dir})
        target_link_libraries(codegen_base_runtime_tests PRIVATE
            tessera_scene tessera_text_edit tessera_animation)
        target_compile_features(codegen_base_runtime_tests PRIVATE cxx_std_23)
        add_test(NAME codegen_base_runtime COMMAND codegen_base_runtime_tests)

        # Compile-fail checks: a developer class without a required member, or with the wrong
        # signature, must fail to build with the contract message. The targets are excluded from
        # `all`; each test builds one and expects the static_assert text in the output.
        foreach (_variant IN ITEMS missing_member wrong_signature private_field)
            add_library(codegen_base_${_variant} OBJECT EXCLUDE_FROM_ALL
                ${_tessera_codegen_base_dev_dir}/${_variant}/instantiate.cpp)
            target_include_directories(codegen_base_${_variant} PRIVATE
                ${_tessera_codegen_base_golden_dir}
                ${_tessera_codegen_base_dev_dir}/${_variant})
            target_link_libraries(codegen_base_${_variant} PRIVATE tessera_scene)
            target_compile_features(codegen_base_${_variant} PRIVATE cxx_std_23)
        endforeach ()
        # These three cases are *negative compile* tests: each drives the whole build for one
        # fixture target, and all three drive the SAME build directory. Without a lock they
        # collide when ctest runs in parallel and one fails at random -- a different member of
        # the set each run, each passing in isolation (issue #353). One shared resource
        # serialises only these three.
        add_test(NAME codegen_base_missing_member_fails
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                    --target codegen_base_missing_member)
        set_tests_properties(codegen_base_missing_member_fails PROPERTIES
            PASS_REGULAR_EXPRESSION "TESSERA_CG_MISSING_BINDING: component .Sample. needs .void sync"
            RESOURCE_LOCK tessera_codegen_negative_build)
        # The generator tool runs on the HOST at build time, so a cross-compiling build (Android,
        # iOS) skips the fixture compile, like the compiler demo above; the host lanes cover it.
        if (NOT CMAKE_CROSSCOMPILING)
            # Every emitter family (lists, virtual collections, vector icons) compiled in base mode: a
            # build-time tool writes the base headers of the shared fixtures, and one translation unit
            # per module instantiates them against hand-written developer classes.
            add_executable(codegen_base_emit_fixtures
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base_emit_fixtures.cpp)
            target_link_libraries(codegen_base_emit_fixtures PRIVATE tessera_codegen tessera_lowering
                tessera_jsx_frontend tessera_react_rules tessera_ir)
            set(_tessera_codegen_base_fixture_dir ${CMAKE_CURRENT_BINARY_DIR}/codegen_base_fixtures)
            set(_tessera_codegen_base_fixture_headers
                ${_tessera_codegen_base_fixture_dir}/dynamic/Dynamic.g.hpp
                ${_tessera_codegen_base_fixture_dir}/dvirt/DynamicVirtual.g.hpp
                ${_tessera_codegen_base_fixture_dir}/vlist/VirtualDemo.g.hpp
                ${_tessera_codegen_base_fixture_dir}/vicon/VectorIcon.g.hpp
                ${_tessera_codegen_base_fixture_dir}/compose/Card.g.hpp
                ${_tessera_codegen_base_fixture_dir}/compose/CardPage.g.hpp
                ${_tessera_codegen_base_fixture_dir}/compose/Watcher.g.hpp
                ${_tessera_codegen_base_fixture_dir}/compose/WatchPage.g.hpp
                ${_tessera_codegen_base_fixture_dir}/demo/Field.g.hpp
                ${_tessera_codegen_base_fixture_dir}/demo/FieldPage.g.hpp
                ${_tessera_codegen_base_fixture_dir}/demo/Form.g.hpp
                ${_tessera_codegen_base_fixture_dir}/demo/FormPage.g.hpp
                ${_tessera_codegen_base_fixture_dir}/scaffold_sample/Sample.g.hpp
                ${_tessera_codegen_base_fixture_dir}/scaffold_sample/Sample.hpp
                ${_tessera_codegen_base_fixture_dir}/scaffold_sample/Sample.cpp)
            add_custom_command(
                OUTPUT ${_tessera_codegen_base_fixture_headers}
                COMMAND codegen_base_emit_fixtures ${_tessera_codegen_base_fixture_dir}
                DEPENDS codegen_base_emit_fixtures
                COMMENT "Generating ADR-040 base headers of the shared fixtures"
                VERBATIM)
            add_library(codegen_base_fixtures_compile OBJECT
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/dynamic.cpp
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/dvirt.cpp
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/vlist.cpp
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/vicon.cpp
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/compose.cpp
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/forms.cpp
                # The scaffold() stubs of the sample module: they must compile unedited.
                ${_tessera_codegen_base_fixture_dir}/scaffold_sample/Sample.cpp
                ${_tessera_codegen_base_fixture_headers})
            target_include_directories(codegen_base_fixtures_compile PRIVATE
                # Generated parents include their children's developer headers (`#include "Card.hpp"`).
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures
                ${_tessera_codegen_base_fixture_dir}/dynamic
                ${_tessera_codegen_base_fixture_dir}/dvirt
                ${_tessera_codegen_base_fixture_dir}/vlist
                ${_tessera_codegen_base_fixture_dir}/vicon
                ${_tessera_codegen_base_fixture_dir}/compose
                ${_tessera_codegen_base_fixture_dir}/demo
                ${_tessera_codegen_base_fixture_dir}/scaffold_sample)
            target_link_libraries(codegen_base_fixtures_compile PRIVATE
                tessera_scene tessera_virtual tessera_layout tessera_text_edit tessera_animation
                tessera_forms)
            target_compile_features(codegen_base_fixtures_compile PRIVATE cxx_std_23)
        endif ()
        add_test(NAME codegen_base_private_field_fails
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                    --target codegen_base_private_field)
        set_tests_properties(codegen_base_private_field_fails PROPERTIES
            PASS_REGULAR_EXPRESSION "state_count_.*private|private.*state_count_"
            RESOURCE_LOCK tessera_codegen_negative_build)
        add_test(NAME codegen_base_wrong_signature_fails
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                    --target codegen_base_wrong_signature)
        set_tests_properties(codegen_base_wrong_signature_fails PROPERTIES
            PASS_REGULAR_EXPRESSION "TESSERA_CG_MISSING_BINDING: component .Sample. needs .void on_click"
            RESOURCE_LOCK tessera_codegen_negative_build)

        # ADR-050 section 3.1 (AF5, issue #177): the resolve-cost gate. A developer class derives
        # from `services/Inbox.g.hpp`, `mount(context)` resolves each requirement once, and 1000
        # accessor reads inside a FrameScope add ZERO lookups -- asserted with AF1's audit counters
        # and paired with value checks, so a no-op accessor cannot satisfy it. The wall-clock
        # numbers behind the same path are the documented measurement in
        # docs/design/service-resolve-benchmark.md (tools/poc/service_resolve), not a second harness.
        if (EXISTS ${_tessera_codegen_golden_dir}/services/Inbox.g.hpp AND
            TARGET tessera_appfw_core)
            add_executable(codegen_services_runtime_tests
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_services_runtime_tests.cpp
                ${_tessera_codegen_base_dev_dir}/services/Inbox.cpp)
            target_include_directories(codegen_services_runtime_tests PRIVATE
                ${_tessera_codegen_golden_dir}/services
                ${_tessera_codegen_base_dev_dir}/services)
            target_link_libraries(codegen_services_runtime_tests PRIVATE
                tessera_scene tessera_appfw_core)
            target_compile_features(codegen_services_runtime_tests PRIVATE cxx_std_23)
            add_test(NAME codegen_services_runtime COMMAND codegen_services_runtime_tests)

            # Compile-fail checks, the same shape as the codegen_base_* cases and taking the same
            # resource lock: both drive the shared build directory.
            #
            #   service_undeclared   `service<Settings>()` for a valid interface the component never
            #                        declared -> TESSERA_CG_SERVICE_UNDECLARED
            #   service_not_interface `service<NotAService>()` for a type with no kServiceId ->
            #                        TESSERA_CG_SERVICE_NOT_INTERFACE
            foreach (_variant IN ITEMS undeclared not_interface)
                add_library(codegen_base_service_${_variant} OBJECT EXCLUDE_FROM_ALL
                    ${_tessera_codegen_base_dev_dir}/services/${_variant}/instantiate.cpp)
                target_include_directories(codegen_base_service_${_variant} PRIVATE
                    ${_tessera_codegen_golden_dir}/services
                    ${_tessera_codegen_base_dev_dir}/services
                    ${_tessera_codegen_base_dev_dir}/services/${_variant})
                target_link_libraries(codegen_base_service_${_variant} PRIVATE
                    tessera_scene tessera_appfw_core)
                target_compile_features(codegen_base_service_${_variant} PRIVATE cxx_std_23)
            endforeach ()
            add_test(NAME codegen_base_service_undeclared_fails
                COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                        --target codegen_base_service_undeclared)
            set_tests_properties(codegen_base_service_undeclared_fails PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "TESSERA_CG_SERVICE_UNDECLARED: component .Inbox. needs a .useService.*declaration for this service type"
                RESOURCE_LOCK tessera_codegen_negative_build)
            add_test(NAME codegen_base_service_not_interface_fails
                COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                        --target codegen_base_service_not_interface)
            set_tests_properties(codegen_base_service_not_interface_fails PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "TESSERA_CG_SERVICE_NOT_INTERFACE: component .Inbox. can only resolve a service interface"
                RESOURCE_LOCK tessera_codegen_negative_build)
        endif ()
    endif ()

    # The canvas path's native half (issue #167): prepared scene, recorded command stream, the report
    # the page parses, and the tolerance policy the pixel gate uses. Registered here rather than in
    # tests/unit/CMakeLists.txt so this lane does not edit the shared registry while other lanes work
    # in it - the same pattern src/ui/paint and tests/gpu/backends already use.
    #
    # Two translation units make one ctest case: `scene_frame_tests.cpp` covers the frame path and
    # `scene_frame_text_tests.cpp` covers text with a font (issue #387). They are split for the source
    # size limit, share `scene_frame_test_support.hpp`, and are linked into one executable so the
    # `scene_frame` case still proves the whole path in one run.
    add_executable(scene_frame_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_frame_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_frame_text_tests.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
        ${_tessera_preview_text_sources}
        ${CMAKE_SOURCE_DIR}/tools/codegen/canvas_report.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp
        ${_tessera_scene_preview_sources})
    target_include_directories(scene_frame_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen
                ${CMAKE_SOURCE_DIR}/tests/unit)
    # The shipped font's absolute path: the text cases read the very bytes the page fetches, and ctest
    # may run from any directory (the same reason `TESSERA_FONT_FIXTURE` is absolutized elsewhere).
    target_compile_definitions(scene_frame_tests PRIVATE
        TESSERA_PREVIEW_FONT_PATH="${CMAKE_SOURCE_DIR}/designer/web/public/fonts/IBMPlexSans-Latin.ttf")
    target_link_libraries(scene_frame_tests PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_golden
        tessera_gpu
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_paint
        tessera_rhi_null
        tessera_scene
        tessera_style)
    add_test(NAME scene_frame COMMAND scene_frame_tests)

    # The author stylesheet in the preview (issue #390): the built-in sheet replaced rather than
    # layered, `:root` tokens through `var()`, inline `style`/`className` through the same cascade, the
    # line/column diagnostics, and -- the reason this is a ctest case and not a manual check -- the
    # depth-bound proof that hostile CSS (nested selectors, at-rules and `@supports` conditions, a 7 KB
    # file of them) produces a diagnostic instead of a stack overflow. A stack overflow in Wasm aborts
    # the module, so this must fail here, where a person sees it, not in a browser.
    # Two translation units make one ctest case, split for the source size limit and sharing
    # `scene_stylesheet_test_support.hpp` (the same pattern `scene_frame_tests` uses):
    # `scene_stylesheet_tests.cpp` covers the sheet compiled as written and
    # `scene_stylesheet_module_tests.cpp` covers the `*.module.css` that is scanned first (issue
    # #463), so the `scene_stylesheet` case still proves the whole path in one run.
    add_executable(scene_stylesheet_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_stylesheet_tests.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_stylesheet_module_tests.cpp
        ${_tessera_scene_preview_sources})
    target_include_directories(scene_stylesheet_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(scene_stylesheet_tests PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_scene
        tessera_style)
    add_test(NAME scene_stylesheet COMMAND scene_stylesheet_tests)

    # A whole project through `evaluate_project` (the studio's project preview): the entry's component,
    # only the files and stylesheets the entry reaches, import-graph sheet order, and an error that
    # names a file instead of a default one.
    add_executable(scene_project_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_project_tests.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_evaluate.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/diagnose.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostics_json.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostic_record.cpp)
    target_include_directories(scene_project_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(scene_project_tests PRIVATE
        tessera_ir_ids
        tessera_lowering)
    add_test(NAME scene_project COMMAND scene_project_tests)

    # Conditional and repeated children in the preview (issue #389): a branch selected by the state the
    # session's scope holds, a re-evaluation after a click changes that state, one subtree per list
    # item, and the bounds that are reported rather than swallowed. Registered here rather than in
    # tests/unit/CMakeLists.txt for the same reason `scene_frame` is: this lane does not edit the
    # shared registry while other lanes work in it.
    add_executable(scene_dynamic_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_dynamic_tests.cpp
        ${_tessera_scene_preview_sources})
    target_include_directories(scene_dynamic_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(scene_dynamic_tests PRIVATE
        tessera_automation_scene
        tessera_css_module
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_scene
        tessera_style)
    add_test(NAME scene_dynamic COMMAND scene_dynamic_tests)

    # The studio's foldable preview (issue #408): a simulated `FoldInfo` reaches the same metrics and
    # safe-area path the app runs, a posture change is one layout pass, and the preview's SafeArea
    # box equals a real `AppHost`'s for the same snapshot. Registered here rather than in
    # tests/unit/CMakeLists.txt for the same reason `scene_frame` is.
    add_executable(scene_fold_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_fold_tests.cpp
        ${_tessera_scene_preview_sources})
    target_include_directories(scene_fold_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(scene_fold_tests PRIVATE
        tessera_app
        tessera_automation_scene
        tessera_css_module
        tessera_hal_metrics
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_scene
        tessera_runtime
        tessera_scene
        tessera_style)
    add_test(NAME scene_fold COMMAND scene_fold_tests)

    # The persistent canvas session (issue #388): pointer input into one evaluation, the
    # `:hover`/`:active` path through the stylesheet engine, and the on-demand frame accounting.
    # Registered here rather than in tests/unit/CMakeLists.txt for the same reason `scene_frame` is:
    # this lane does not edit the shared registry while other lanes work in it.
    #
    # Two executables, split by responsibility (the sources share `scene_canvas_test_support.hpp`):
    #   scene_canvas_session  the session's lifecycle + the frame contract (a static scene draws zero)
    #   scene_canvas_pointer  the pointer path into the style engine + click synthesis
    foreach (_canvas_test IN ITEMS scene_canvas_session scene_canvas_pointer)
        add_executable(${_canvas_test}_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/${_canvas_test}_tests.cpp
            ${CMAKE_SOURCE_DIR}/tools/codegen/canvas_report.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp
            ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
            ${_tessera_preview_text_sources}
            ${_tessera_canvas_session_sources}
            ${_tessera_scene_preview_sources})
        target_include_directories(${_canvas_test}_tests
            PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
        target_link_libraries(${_canvas_test}_tests PRIVATE
            tessera_automation_scene
            tessera_css_module
            tessera_gpu
            tessera_ir_eval
            tessera_ir_ids
            tessera_layout
            tessera_lowering
            tessera_paint
            tessera_rhi
            tessera_rhi_null
            tessera_scene
            tessera_style)
        add_test(NAME ${_canvas_test} COMMAND ${_canvas_test}_tests)
    endforeach ()

    # A resize is a layout pass, not a re-run (issue #436): `@media` re-resolves for the new
    # viewport, a size that crosses no breakpoint does not rebuild the cascade (the session's
    # generation counter), the resize carries state/hover/focus, its frame equals a cold evaluation
    # at the same size (snapshot digest), and a real `AppHost` resolves the same fixture the same
    # way. Registered here rather than in tests/unit/CMakeLists.txt for the same reason
    # `scene_frame` is: this lane does not edit the shared registry while other lanes work in it.
    add_executable(scene_viewport_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/scene_viewport_tests.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/canvas_report.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/css_diagnostics_report.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/scene_frame.cpp
        ${_tessera_preview_text_sources}
        ${_tessera_canvas_session_sources}
        ${_tessera_scene_preview_sources})
    target_include_directories(scene_viewport_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen
                ${CMAKE_SOURCE_DIR}/tests/unit)
    target_link_libraries(scene_viewport_tests PRIVATE
        tessera_app
        tessera_automation_scene
        tessera_css_module
        tessera_gpu
        tessera_ir_eval
        tessera_ir_ids
        tessera_layout
        tessera_lowering
        tessera_paint
        tessera_rhi
        tessera_rhi_null
        tessera_scene
        tessera_style)
    add_test(NAME scene_viewport COMMAND scene_viewport_tests)

    add_executable(codegen_layering_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_layering_tests.cpp)

    target_link_libraries(codegen_layering_tests PRIVATE tessera_codegen)

    target_compile_definitions(codegen_layering_tests PRIVATE
        TESSERA_COMPILER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/compiler")

    add_test(NAME codegen_layering COMMAND codegen_layering_tests)

    # Embedded stylesheet blobs (issue #81, ADR-048): the `_stylesheets.hpp` file codegen emits for a
    # module with blobs, the warning/error matrix, and the CLI path that compiles CSS and feeds them.
    # The CLI part needs subprocess execution and skips where the platform does not have it.
    add_executable(codegen_stylesheet_embed_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_stylesheet_embed_tests.cpp)

    target_link_libraries(codegen_stylesheet_embed_tests PRIVATE
        tessera_codegen
        tessera_style)

    if (CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        target_compile_definitions(codegen_stylesheet_embed_tests PRIVATE TESSERA_NO_SUBPROCESS=1)
    endif ()

    add_test(NAME codegen_stylesheet_embed
        COMMAND codegen_stylesheet_embed_tests $<TARGET_FILE:tessera_codegen_cli>)

    # CSS module scoping of the embedded blob (issue #463): a `*.module.css` must compile into the
    # blob as its scoped rewrite, while a plain sheet in the same run keeps its authored bytes. Its
    # own executable because `codegen_stylesheet_embed_tests` is at the 500-line file size limit; the
    # test name keeps that prefix so `-R codegen_stylesheet_embed` runs both.
    add_executable(codegen_stylesheet_embed_module_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_stylesheet_embed_module_tests.cpp)

    target_link_libraries(codegen_stylesheet_embed_module_tests PRIVATE
        tessera_codegen
        tessera_style)

    if (CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        target_compile_definitions(codegen_stylesheet_embed_module_tests PRIVATE TESSERA_NO_SUBPROCESS=1)
    endif ()

    add_test(NAME codegen_stylesheet_embed_module
        COMMAND codegen_stylesheet_embed_module_tests $<TARGET_FILE:tessera_codegen_cli>)

    # Embedded theme asset (issue #81, ADR-048): the `<module>_theme.hpp` file codegen emits, its
    # determinism, and the CLI path that parses `assets.theme` and fails with the typed code on a
    # broken asset. The CLI part needs subprocess execution.
    add_executable(codegen_theme_embed_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_theme_embed_tests.cpp)

    target_link_libraries(codegen_theme_embed_tests PRIVATE
        tessera_codegen
        tessera_style)

    if (CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        target_compile_definitions(codegen_theme_embed_tests PRIVATE TESSERA_NO_SUBPROCESS=1)
    endif ()

    add_test(NAME codegen_theme_embed
        COMMAND codegen_theme_embed_tests $<TARGET_FILE:tessera_codegen_cli>)

    # End-to-end proof: build the generated sample and inspect the attached metadata, children,
    # focusability and text through the real scene API (no GPU).
    add_executable(codegen_runtime_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_runtime_tests.cpp
        ${_tessera_codegen_base_dev_dir}/Sample.cpp)

    target_include_directories(codegen_runtime_tests PRIVATE
        ${_tessera_codegen_golden_dir} ${_tessera_codegen_base_dev_dir})
    target_link_libraries(codegen_runtime_tests PRIVATE
        tessera_scene
        tessera_text_edit
        tessera_animation
        tessera_style)
    target_compile_features(codegen_runtime_tests PRIVATE cxx_std_23)

    add_test(NAME codegen_runtime COMMAND codegen_runtime_tests)

    # Scheduled updates (issue #5): setters schedule, the flush runs update() once.
    add_executable(codegen_schedule_runtime_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_schedule_runtime_tests.cpp
        ${_tessera_codegen_base_dev_dir}/Sample.cpp)

    target_include_directories(codegen_schedule_runtime_tests PRIVATE
        ${_tessera_codegen_golden_dir} ${_tessera_codegen_base_dev_dir})
    target_link_libraries(codegen_schedule_runtime_tests PRIVATE
        tessera_scene
        tessera_text_edit
        tessera_animation
        tessera_runtime)
    target_compile_features(codegen_schedule_runtime_tests PRIVATE cxx_std_23)

    add_test(NAME codegen_schedule_runtime COMMAND codegen_schedule_runtime_tests)

    # Conditional + keyed-list codegen: goldens from hand-built IR (the `.tsx` front end cannot
    # author a list type yet) and a runtime test over the compiled generated component.
    add_executable(codegen_dynamic_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_dynamic_tests.cpp)

    target_link_libraries(codegen_dynamic_tests PRIVATE tessera_codegen)

    target_compile_definitions(codegen_dynamic_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_dynamic COMMAND codegen_dynamic_tests)

    # The developer class (tests/unit/codegen_base/fixtures/Dynamic.hpp) over the committed base.
    if (EXISTS ${_tessera_codegen_golden_dir}/Dynamic.g.hpp)
        add_executable(codegen_dynamic_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_dynamic_runtime_tests.cpp)

        target_include_directories(codegen_dynamic_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(codegen_dynamic_runtime_tests PRIVATE tessera_scene)
        target_compile_features(codegen_dynamic_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME codegen_dynamic_runtime COMMAND codegen_dynamic_runtime_tests)
    endif ()

    # Component composition and the static `children` slot: goldens from hand-built IR plus a
    # runtime test over the compiled generated components (a nested instance keeps its own state).
    add_executable(codegen_composition_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_composition_tests.cpp)

    target_link_libraries(codegen_composition_tests PRIVATE tessera_codegen)

    target_compile_definitions(codegen_composition_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_composition COMMAND codegen_composition_tests)

    # The developer classes (tests/unit/codegen_base/fixtures/{Card,CardPage}.hpp) over the committed
    # bases, generated from the hand-built IR in codegen_composition_tests.
    if (EXISTS ${_tessera_codegen_golden_dir}/CardPage.g.hpp AND EXISTS
        ${_tessera_codegen_golden_dir}/Card.g.hpp)
        add_executable(codegen_composition_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_composition_runtime_tests.cpp)

        target_include_directories(codegen_composition_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(codegen_composition_runtime_tests PRIVATE tessera_scene)
        target_compile_features(codegen_composition_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME codegen_composition_runtime COMMAND codegen_composition_runtime_tests)
    endif ()

    if (EXISTS ${_tessera_codegen_golden_dir}/ForwardWrap.g.hpp AND EXISTS ${_tessera_codegen_golden_dir}/ForwardOuter.g.hpp)
        # ADR-040 (#218): the developer classes (tests/unit/codegen_base/fixtures/Forward*.hpp) over
        # the committed bases, generated from the TSX in lowering_callback_forward_tests.
        add_executable(codegen_callback_forward_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_callback_forward_runtime_tests.cpp)

        target_include_directories(codegen_callback_forward_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(codegen_callback_forward_runtime_tests PRIVATE
            tessera_scene
            tessera_text_edit
            tessera_animation
            tessera_style)
        target_compile_features(codegen_callback_forward_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME codegen_callback_forward_runtime
            COMMAND codegen_callback_forward_runtime_tests)
    endif ()

    add_executable(codegen_instances_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_instances_tests.cpp)

    target_link_libraries(codegen_instances_tests PRIVATE
        tessera_codegen
        tessera_ir)

    target_include_directories(codegen_instances_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/unit)

    target_compile_definitions(codegen_instances_tests PRIVATE
        TESSERA_CODEGEN_INSTANCES_GOLDEN_DIR="${_tessera_codegen_instances_golden_dir}")

    add_test(NAME codegen_instances COMMAND codegen_instances_tests)

    # ADR-040 (#218): the developer classes in tests/unit/codegen_base/fixtures/instances over the
    # committed bases, generated from the hand-built IR in codegen_instances_tests.
    if (EXISTS ${_tessera_codegen_instances_golden_dir}/Page.g.hpp AND EXISTS
        ${_tessera_codegen_instances_golden_dir}/Row.g.hpp)
        add_executable(codegen_instances_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_instances_runtime_tests.cpp)

        target_include_directories(codegen_instances_runtime_tests PRIVATE
            ${_tessera_codegen_instances_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/instances)
        target_link_libraries(codegen_instances_runtime_tests PRIVATE tessera_scene)
        target_compile_features(codegen_instances_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME codegen_instances_runtime COMMAND codegen_instances_runtime_tests)

        # #440: how many update() calls one state change costs. The same committed Page/Row bases
        # under a counting developer class for Row (fixtures/update_count).
        add_executable(codegen_update_fanout_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_update_fanout_tests.cpp)

        target_include_directories(codegen_update_fanout_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/update_count
            ${_tessera_codegen_instances_golden_dir})
        target_link_libraries(codegen_update_fanout_tests PRIVATE tessera_scene)
        target_compile_features(codegen_update_fanout_tests PRIVATE cxx_std_23)

        add_test(NAME codegen_update_fanout COMMAND codegen_update_fanout_tests)
    endif ()

    if (EXISTS ${_tessera_codegen_golden_dir}/Button.g.hpp AND EXISTS
        ${_tessera_codegen_golden_dir}/ButtonDemo.g.hpp)
        # ADR-040 (#218): the developer classes (tests/unit/codegen_base/fixtures/Button*.hpp) over
        # the committed bases, generated from packages/tessera-ui/Button.tsx by
        # library_button_lowering_tests.
        add_executable(library_button_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/library_button_runtime_tests.cpp)

        target_include_directories(library_button_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(library_button_runtime_tests PRIVATE
            tessera_scene
            tessera_text_edit
            tessera_animation
            tessera_style)
        target_compile_features(library_button_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME library_button_runtime COMMAND library_button_runtime_tests)
    endif ()

    if (EXISTS ${_tessera_codegen_golden_dir}/SafeArea.g.hpp AND
        EXISTS ${_tessera_codegen_golden_dir}/SafeAreaDemo.g.hpp AND TARGET tessera_app)
        # ADR-040 (#218): the developer classes (tests/unit/codegen_base/fixtures/SafeArea*.hpp)
        # over the committed bases, generated from packages/tessera-ui/SafeArea.tsx by
        # library_safe_area_lowering_tests.
        add_executable(library_safe_area_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/library_safe_area_runtime_tests.cpp)

        target_include_directories(library_safe_area_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_compile_definitions(library_safe_area_runtime_tests PRIVATE
            TESSERA_SAFE_AREA_CSS="${CMAKE_SOURCE_DIR}/packages/tessera-ui/safe-area.css")
        target_link_libraries(library_safe_area_runtime_tests PRIVATE
            tessera_app
            tessera_runtime
            tessera_scene
            tessera_text_edit
            tessera_animation
            tessera_style)
        target_compile_features(library_safe_area_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME library_safe_area_runtime COMMAND library_safe_area_runtime_tests)
    endif ()

    add_executable(codegen_size_change_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_size_change_tests.cpp)

    target_link_libraries(codegen_size_change_tests PRIVATE
        tessera_codegen)

    target_compile_definitions(codegen_size_change_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_size_change COMMAND codegen_size_change_tests)

    # ADR-040 (#218): the developer classes in tests/unit/codegen_base/fixtures over the committed
    # bases, generated from the hand-built IR in codegen_size_change_tests.
    if (EXISTS ${_tessera_codegen_golden_dir}/Watcher.g.hpp AND EXISTS
        ${_tessera_codegen_golden_dir}/WatchPage.g.hpp)
        add_executable(codegen_size_change_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_size_change_runtime_tests.cpp)

        target_include_directories(codegen_size_change_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures/watch)
        target_link_libraries(codegen_size_change_runtime_tests PRIVATE
            tessera_scene
            tessera_layout)
        target_compile_features(codegen_size_change_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME codegen_size_change_runtime COMMAND codegen_size_change_runtime_tests)
    endif ()

    # VirtualList/VirtualGrid item templates: goldens from hand-built IR (the `.tsx` front end
    # cannot author a virtual collection yet) and an integration test that scripts a scroll through
    # the `tessera_virtual` geometry and checks the realized item range.
    add_executable(codegen_virtual_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_tests.cpp)

    target_link_libraries(codegen_virtual_tests PRIVATE
        tessera_codegen)

    target_compile_definitions(codegen_virtual_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_virtual COMMAND codegen_virtual_tests)

    # Dynamic virtual geometry: the re-seed path, the measurement-invalidation rule and the
    # literal-only structural props (one virtual collection geometry, not the whole template).
    add_executable(codegen_virtual_geometry_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_geometry_tests.cpp)

    target_link_libraries(codegen_virtual_geometry_tests PRIVATE
        tessera_codegen)

    add_test(NAME codegen_virtual_geometry COMMAND codegen_virtual_geometry_tests)

    # The sample library owns the committed golden: `codegen_virtual_tests --write` regenerates
    # tests/golden/codegen/VirtualDemo.*; the guard keeps a fresh worktree configuring before the
    # golden exists.
    # ADR-040 (#218): the developer class in tests/unit/codegen_base/fixtures over the committed
    # golden base. One developer class per component, shared by the three runtime suites.
    if (EXISTS ${_tessera_codegen_golden_dir}/VirtualDemo.g.hpp)
        foreach (_virtual_runtime
                 codegen_virtual_runtime_tests
                 codegen_virtual_scroll_tests
                 codegen_virtual_geometry_runtime_tests)
            add_executable(${_virtual_runtime}
                ${CMAKE_SOURCE_DIR}/tests/unit/${_virtual_runtime}.cpp)
            target_include_directories(${_virtual_runtime} PRIVATE
                ${_tessera_codegen_golden_dir}
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
            target_link_libraries(${_virtual_runtime} PRIVATE
                tessera_scene
                tessera_virtual)
            target_compile_features(${_virtual_runtime} PRIVATE cxx_std_23)
        endforeach ()

        add_test(NAME codegen_virtual_runtime COMMAND codegen_virtual_runtime_tests)

        # The generated scroll entry point: geometry ownership, overscanned window,
        # measurement feedback/re-anchor, and the edge/throttle `onEndReached` contract.
        add_test(NAME codegen_virtual_scroll COMMAND codegen_virtual_scroll_tests)

        # Dynamic geometry at runtime: the state/prop-driven re-seed path (once per change, not per
        # frame), the measurement-invalidation rule, the re-anchor and its clamp, and the untouched
        # `onEndReached` edge/throttle state.
        add_test(NAME codegen_virtual_geometry_runtime
            COMMAND codegen_virtual_geometry_runtime_tests)
    endif ()

    # Prop-driven structural options on virtual collections: `orientation`/`columns`/`overscan`
    # (and `columnWidth`) re-read their component prop at sync time, so a prop change re-realizes
    # the window. `codegen_dynamic_virtual_tests --write` regenerates
    # tests/golden/codegen/dynamic/DynamicVirtual.g.hpp. The literal golden must not move.
    add_executable(codegen_dynamic_virtual_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_dynamic_virtual_tests.cpp)

    target_link_libraries(codegen_dynamic_virtual_tests PRIVATE tessera_codegen)

    target_compile_definitions(codegen_dynamic_virtual_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_dynamic_virtual_options COMMAND codegen_dynamic_virtual_tests)

    # The developer class (tests/unit/codegen_base/fixtures/DynamicVirtual.hpp) over the committed
    # base, so the same translation unit drives the runtime window behavior.
    if (EXISTS ${_tessera_codegen_golden_dir}/dynamic/DynamicVirtual.g.hpp)
        target_compile_definitions(codegen_dynamic_virtual_tests PRIVATE
            TESSERA_DYNAMIC_VIRTUAL_SAMPLE=1)
        target_include_directories(codegen_dynamic_virtual_tests
            PRIVATE
                ${_tessera_codegen_golden_dir}/dynamic
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(codegen_dynamic_virtual_tests PRIVATE
            tessera_scene
            tessera_virtual)
    endif ()

    # `Path` -> `PaintKind::Vector`: codegen synthesizes an SVG document from the literal props,
    # embeds it as a generated `AssetBundle` and names that asset in the payload. The golden under
    # `vector/` is written by `codegen_vector_payload_tests --write`.
    add_executable(codegen_vector_payload_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_vector_payload_tests.cpp)

    target_link_libraries(codegen_vector_payload_tests PRIVATE
        tessera_codegen
        tessera_assets
        tessera_asset_svg)

    target_compile_definitions(codegen_vector_payload_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_vector_payload COMMAND codegen_vector_payload_tests)

    # The developer class (tests/unit/codegen_base/fixtures/VectorIcon.hpp) over the committed base.
    if (EXISTS ${_tessera_codegen_golden_dir}/vector/VectorIcon.g.hpp)
        target_compile_definitions(codegen_vector_payload_tests PRIVATE
            TESSERA_VECTOR_SAMPLE=1)
        target_include_directories(codegen_vector_payload_tests
            PRIVATE
                ${_tessera_codegen_golden_dir}/vector
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(codegen_vector_payload_tests PRIVATE tessera_scene)
    endif ()

    # CPU paint golden for a synthesized payload: the generated component -> payload asset name ->
    # generated bundle -> SVG decode -> ThorVG raster -> textured blit, compared with a committed
    # baseline at tolerance 0. Skips cleanly without ThorVG.
    if (TARGET codegen_base_fixtures_compile AND TESSERA_ENABLE_THORVG)
        add_executable(codegen_vector_paint_golden_tests
            ${CMAKE_SOURCE_DIR}/tests/gpu/codegen_vector_paint_golden_tests.cpp)
        target_include_directories(codegen_vector_paint_golden_tests
            PRIVATE
                ${PROJECT_SOURCE_DIR}/tools/poc/batched2d
                ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures
                ${_tessera_codegen_base_fixture_dir}/vicon)
        # The generated base and asset bundle are written by the fixture tool's custom command.
        add_dependencies(codegen_vector_paint_golden_tests codegen_base_fixtures_compile)
        target_link_libraries(codegen_vector_paint_golden_tests PRIVATE
            tessera_paint
            tessera_scene
            tessera_software
            tessera_path
            tessera_asset_svg
            tessera_golden
            tessera_stb)
        add_test(NAME codegen_vector_paint_golden
            COMMAND codegen_vector_paint_golden_tests
                ${PROJECT_SOURCE_DIR}/tests/golden/baseline/codegen/vector_payload_cpu.png)
    endif ()

    # `Field` as a library component on the ordinary component path: a framework import names the
    # built-in component, lowering emits an ordinary `Component` node with the documented prop
    # records, and the static Children slot carries the caller's widget.
    add_executable(forms_lowering_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/forms_lowering_tests.cpp)

    target_link_libraries(forms_lowering_tests PRIVATE
        tessera_lowering
        tessera_jsx_frontend
        tessera_react_rules
        tessera_ir)

    add_test(NAME forms_lowering COMMAND forms_lowering_tests)

    # Focusable `View`/`Text` (issue #226): tabIndex/autoFocus/key and focus events through lowering,
    # codegen and the headless input router, plus the role and name diagnostics.
    add_executable(focusable_view_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/focusable_view_tests.cpp)

    target_link_libraries(focusable_view_tests PRIVATE
        tessera_codegen
        tessera_lowering
        tessera_jsx_frontend
        tessera_react_rules
        tessera_ir
        tessera_scene)

    add_test(NAME focusable_view COMMAND focusable_view_tests)

    # Guarded handler statements (issue #236): `guard && setter(...)` through lowering, the evaluator
    # and codegen.
    add_executable(guarded_handler_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/guarded_handler_tests.cpp)

    target_link_libraries(guarded_handler_tests PRIVATE
        tessera_codegen
        tessera_lowering
        tessera_jsx_frontend
        tessera_react_rules
        tessera_ir
        tessera_ir_eval
        tessera_scene)

    add_test(NAME guarded_handler COMMAND guarded_handler_tests)

    # Behavior primitives (issue #236): packages/tessera-ui/primitives/*.tsx through lowering, codegen
    # and the IR evaluator.
    add_executable(primitives_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/primitives_tests.cpp)

    target_link_libraries(primitives_tests PRIVATE
        tessera_codegen
        tessera_lowering
        tessera_jsx_frontend
        tessera_react_rules
        tessera_ir
        tessera_ir_eval)

    target_compile_definitions(primitives_tests PRIVATE
        TESSERA_PRIMITIVES_DIR="${CMAKE_SOURCE_DIR}/packages/tessera-ui/primitives")

    add_test(NAME primitives COMMAND primitives_tests)

    # `Field` codegen: the real built-in Field IR declaration plus a form with two instances, the prop
    # matrix (value and callback props), the static Children slot, the golden and determinism.
    add_executable(forms_codegen_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/forms_codegen_tests.cpp)

    target_link_libraries(forms_codegen_tests PRIVATE tessera_codegen tessera_lowering)

    target_compile_definitions(forms_codegen_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME forms_codegen COMMAND forms_codegen_tests)

    # The developer classes (tests/unit/codegen_base/fixtures/{Field,Form,FieldPage,FormPage}.hpp)
    # over the committed bases, generated from the library IR by
    # `forms_codegen_tests --write`.
    if (EXISTS ${_tessera_codegen_golden_dir}/FormPage.g.hpp AND EXISTS
        ${_tessera_codegen_golden_dir}/FieldPage.g.hpp)
        add_executable(forms_codegen_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/forms_codegen_runtime_tests.cpp)

        target_include_directories(forms_codegen_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir}
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_base/fixtures)
        target_link_libraries(forms_codegen_runtime_tests PRIVATE
            tessera_forms
            tessera_runtime
            tessera_scene)
        target_compile_features(forms_codegen_runtime_tests PRIVATE cxx_std_23)

        add_test(NAME forms_codegen_runtime COMMAND forms_codegen_runtime_tests)
    endif ()

    # `Form` binding rejection matrix: explicit controller-owned Field props, malformed seeds,
    # duplicate names, unsupported validators and nested Forms are rejected loudly.
    add_executable(forms_form_codegen_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/forms_form_codegen_tests.cpp)

    target_link_libraries(forms_form_codegen_tests PRIVATE
        tessera_codegen
        tessera_lowering)

    target_compile_definitions(forms_form_codegen_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME forms_form_codegen COMMAND forms_form_codegen_tests)

    # The machine-readable diagnostics document (issue #165, docs/specs/compiler-diagnostics-v1.md):
    # the CLI's --diagnostics-json output, its span fidelity against the printed diagnostic, and the
    # document's determinism. In process, no subprocess: it runs the real `cli::run()`, so it needs
    # no %TEMP% write and works in a confined sandbox.
    add_executable(codegen_diagnostics_json_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_diagnostics_json_tests.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostics_json.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/diagnostic_record.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/diagnose.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_source_file.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_stylesheets.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_text.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate_assets.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/project_identity.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/package_manifest.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components_expr.cpp)

    target_include_directories(codegen_diagnostics_json_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)

    target_link_libraries(codegen_diagnostics_json_tests PRIVATE
        tessera_codegen
        tessera_harness
        tessera_ir_ids
        tessera_lowering
        tessera_css_module
        tessera_style)

    # The scratch directory lives under the build tree, never %TEMP%: a confined sandbox denies a
    # child process writing there, and keeping it here also makes a failed run inspectable.
    add_test(NAME codegen_diagnostics_json
        COMMAND codegen_diagnostics_json_tests
                ${CMAKE_CURRENT_BINARY_DIR}/codegen_diagnostics_json_tests-work)

    add_executable(codegen_cli_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_cli_tests.cpp)

    target_link_libraries(codegen_cli_tests PRIVATE tessera_codegen)

    # iOS/tvOS/watchOS do not expose system(); the CLI tests skip there (printed reason).
    if (CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        target_compile_definitions(codegen_cli_tests PRIVATE TESSERA_NO_SUBPROCESS=1)
    endif ()

    add_test(NAME codegen_cli
        COMMAND codegen_cli_tests ${_tessera_codegen_demo_src} $<TARGET_FILE:tessera_codegen_cli>)

    # The demo target only exists on host builds (see the cross-compiling guard above).
    if (NOT CMAKE_CROSSCOMPILING)
        add_test(NAME sample_compile
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tessera_compiler_demo)
        # This test drives the build directory, like the codegen_base_* negatives above: without the
        # lock a parallel ctest run has two `--build` processes in one tree and one of them loses
        # (the failure mode #353 fixed for the negatives). It never failed alone, only under
        # `--parallel`.
        set_tests_properties(sample_compile PROPERTIES
            RESOURCE_LOCK tessera_codegen_negative_build)

        # `#line` mapping to TSX (issue #78): the directives (default off, so the goldens above do
        # not move) and one real negative compile whose diagnostics must name TSX lines, not lines
        # of the generated base. Kept in its own file so this list stays readable.
        include(${CMAKE_CURRENT_SOURCE_DIR}/line_map_tests.cmake)
    endif ()
endif ()
