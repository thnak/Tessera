# SPDX-License-Identifier: MIT

# The minimal renderer-backed web app (issue #1013): the web app driver (WebGPU device, canvas
# surface, WebHost frame step) drawing through the batched 2D renderer. Emscripten only, built by the
# wasm-webgpu preset. Its counters are EMSCRIPTEN_KEEPALIVE, so they need no EXPORTED_FUNCTIONS entry.
add_executable(web_render main.cpp)
target_link_libraries(web_render PRIVATE tessera_runtime tessera_web_gpu tessera_gpu)
tessera_web_app(web_render)
# Also loadable under Node (no WebGPU there, so it runs the host contract without a renderer), so
# scripts/ci/web_smoke.mjs checks it like web_smoke; the shipped page keeps ENVIRONMENT=web.
target_link_options(web_render PRIVATE "SHELL:-s ENVIRONMENT=web,node")
