# SPDX-License-Identifier: MIT

add_library(tessera_forms STATIC
    form_async.cpp
    form_model.cpp
    form_rules.cpp)

target_include_directories(tessera_forms PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_forms PUBLIC tessera_runtime)

target_compile_features(tessera_forms PUBLIC cxx_std_23)
