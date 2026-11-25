# SPDX-License-Identifier: MIT

# Front-end corpus measurement (spike #660, ADR-066). The core is a small library so the unit test
# and the CLI measure through the same function; the CLI only walks a directory.
#
# The pipeline stage calls `collect_source_diagnostics()` (tools/codegen/diagnose.cpp), which is the
# same entry point the preview page uses, so the measurement cannot drift from the compiler it
# measures. That is why the codegen driver sources are compiled in here rather than reimplemented.
add_library(tessera_corpus_measure_core STATIC
    measure.cpp
    measure_directory.cpp
    measure_pipeline.cpp
    report.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/diagnose.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/diagnostics_json.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/diagnostic_record.cpp)

target_include_directories(tessera_corpus_measure_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/tools/codegen)

target_link_libraries(tessera_corpus_measure_core
    PUBLIC tessera_jsx_frontend
    PRIVATE
        tessera_react_rules
        tessera_lowering
        tessera_ir
        tessera_ir_ids)

target_compile_features(tessera_corpus_measure_core PUBLIC cxx_std_23)

add_executable(tessera_corpus_measure
    main.cpp)

target_link_libraries(tessera_corpus_measure PRIVATE tessera_corpus_measure_core)
