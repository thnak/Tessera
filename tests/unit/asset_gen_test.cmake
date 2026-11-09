# SPDX-License-Identifier: MIT
#
# Asset embedding checks: byte-identical output across two runs from different working directories,
# a checked-in golden for the generated header/source, and loud rejection of duplicate names and
# invalid names. Generation runs as a CMake script on the build host (never a target binary), so
# this test also covers the cross-compiling mechanism. Regenerate the golden deliberately with:
#
#   cmake -DEMBED_SCRIPT=<repo>/cmake/embed_assets.cmake -DMANIFEST=<icons.manifest> \
#         -DDUPLICATE_MANIFEST=<duplicate.manifest> -DINVALID_MANIFEST=<invalid_name.manifest> \
#         -DGOLDEN_DIR=<repo>/tests/golden/assets -DWORK=<build>/asset_generator \
#         -DWRITE_GOLDEN=ON -P tests/unit/asset_gen_test.cmake

foreach(required EMBED_SCRIPT MANIFEST DUPLICATE_MANIFEST INVALID_MANIFEST GOLDEN_DIR WORK)
    if (NOT DEFINED ${required})
        message(FATAL_ERROR "asset_gen_test: missing -D${required}=...")
    endif()
endforeach()

function(run_embed out_dir manifest name_space working_dir)
    file(REMOVE_RECURSE "${out_dir}")
    file(MAKE_DIRECTORY "${out_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DMANIFEST=${manifest}"
                "-DNAMESPACE=${name_space}"
                "-DHEADER=${out_dir}/assets.hpp"
                "-DSOURCE=${out_dir}/assets.cpp"
                -P "${EMBED_SCRIPT}"
        WORKING_DIRECTORY "${working_dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "embed_assets failed (${result}): ${output}${error}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}/cwd-a" "${WORK}/cwd-b")
run_embed("${WORK}/a" "${MANIFEST}" "tessera::ui::assets::golden" "${WORK}/cwd-a")
run_embed("${WORK}/b" "${MANIFEST}" "tessera::ui::assets::golden" "${WORK}/cwd-b")

foreach(file assets.hpp assets.cpp)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/a/${file}" "${WORK}/b/${file}"
        RESULT_VARIABLE compare_result)
    if (NOT compare_result EQUAL 0)
        message(FATAL_ERROR "asset_gen_test: ${file} differs between runs/working directories")
    endif()
endforeach()

if (WRITE_GOLDEN)
    file(COPY "${WORK}/a/assets.hpp" "${WORK}/a/assets.cpp" DESTINATION "${GOLDEN_DIR}")
    message(STATUS "asset_gen_test: golden rewritten in ${GOLDEN_DIR}")
else()
    foreach(file assets.hpp assets.cpp)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/a/${file}"
                    "${GOLDEN_DIR}/${file}"
            RESULT_VARIABLE compare_result)
        if (NOT compare_result EQUAL 0)
            message(FATAL_ERROR
                "asset_gen_test: ${file} does not match the checked-in golden "
                "(regenerate deliberately with -DWRITE_GOLDEN=ON)")
        endif()
    endforeach()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${DUPLICATE_MANIFEST}"
            "-DNAMESPACE=test::duplicate"
            "-DHEADER=${WORK}/duplicate.hpp" "-DSOURCE=${WORK}/duplicate.cpp"
            -P "${EMBED_SCRIPT}"
    RESULT_VARIABLE duplicate_result
    OUTPUT_VARIABLE duplicate_output
    ERROR_VARIABLE duplicate_error)
if (duplicate_result EQUAL 0)
    message(FATAL_ERROR "asset_gen_test: a duplicate asset name was accepted")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${INVALID_MANIFEST}"
            "-DNAMESPACE=test::invalid"
            "-DHEADER=${WORK}/invalid.hpp" "-DSOURCE=${WORK}/invalid.cpp"
            -P "${EMBED_SCRIPT}"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error)
if (invalid_result EQUAL 0)
    message(FATAL_ERROR "asset_gen_test: an invalid asset name was accepted")
endif()

message(STATUS "asset_gen_test: ok")
