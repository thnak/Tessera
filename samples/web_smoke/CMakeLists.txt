# SPDX-License-Identifier: MIT

# The minimal web app (issue #969): Emscripten only, built by the wasm-webgpu preset. Its two
# counters are EMSCRIPTEN_KEEPALIVE, so they need no EXPORTED_FUNCTIONS entry.
add_executable(web_smoke main.cpp)
target_link_libraries(web_smoke PRIVATE tessera_runtime)
tessera_web_app(web_smoke)
# Also loadable under Node, so scripts/ci/web_smoke.mjs checks the contract without a browser. This
# option comes after the shared ones and wins; the shipped apps keep ENVIRONMENT=web.
target_link_options(web_smoke PRIVATE "SHELL:-s ENVIRONMENT=web,node")
