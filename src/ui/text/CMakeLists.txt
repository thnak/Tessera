# SPDX-License-Identifier: MIT

add_library(tessera_text STATIC
    font_face.cpp
    font_metrics.cpp
    freetype_metrics.cpp
    text_measure.cpp
    text_measure_cache.cpp)

target_include_directories(tessera_text PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# FreeType stays private: public text headers must not include third-party headers.
target_link_libraries(tessera_text PUBLIC tessera_style PRIVATE tessera_freetype)

target_compile_features(tessera_text PUBLIC cxx_std_23)
