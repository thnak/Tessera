#SPDX - License - Identifier : MIT

#Device / render - scale quantization and the per -                                                \
    window scale state(design / window - resize.md §2.3).
add_library(tessera_scale STATIC render_scale.cpp)

    target_include_directories(tessera_scale PUBLIC ${PROJECT_SOURCE_DIR} / include)

        target_compile_features(tessera_scale PUBLIC cxx_std_23)
