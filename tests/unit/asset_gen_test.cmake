# SPDX-License-Identifier: MIT
#
# Asset generator checks: byte-identical output across two runs from different working directories,
# a checked-in golden for the generated header/source, and loud rejection of duplicate names and
# invalid names. Regenerate the golden deliberately with:
#
#   cmake -DGENERATOR=<tessera_asset_gen> -DMANIFEST=<icons.manifest> \
#         -DDUPLICATE_MANIFEST=<duplicate.manifest> -DINVALID_MANIFEST=<invalid_name.manifest> \
#         -DGOLDEN_DIR=<repo>/tests/golden/assets -DWORK=<build>/asset_generator \
#         -DWRITE_GOLDEN=ON -P tests/unit/asset_gen_test.cmake

foreach(required GENERATOR MANIFEST DUPLICATE_MANIFEST INVALID_MANIFEST GOLDEN_DIR WORK)
    if (NOT DEFINED ${required})
        message(FATAL_ERROR "asset_gen_test: missing -D${required}=...")
    endif()
endforeach()

function(run_generator out_dir manifest name_space working_dir)
    file(REMOVE_RECURSE "${out_dir}")
    file(MAKE_DIRECTORY "${out_dir}")
    execute_process(
        COMMAND "${GENERATOR}" --manifest "${manifest}" --namespace "${name_space}"
                --header "${out_dir}/assets.hpp" --source "${out_dir}/assets.cpp"
        WORKING_DIRECTORY "${working_dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "tessera_asset_gen failed (${result}): ${output}${error}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}/cwd-a" "${WORK}/cwd-b")
run_generator("${WORK}/a" "${MANIFEST}" "tessera::ui::assets::golden" "${WORK}/cwd-a")
run_generator("${WORK}/b" "${MANIFEST}" "tessera::ui::assets::golden" "${WORK}/cwd-b")

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
    COMMAND "${GENERATOR}" --manifest "${DUPLICATE_MANIFEST}" --namespace test::duplicate
            --header "${WORK}/duplicate.hpp" --source "${WORK}/duplicate.cpp"
    RESULT_VARIABLE duplicate_result
    OUTPUT_VARIABLE duplicate_output
    ERROR_VARIABLE duplicate_error)
if (duplicate_result EQUAL 0)
    message(FATAL_ERROR "asset_gen_test: a duplicate asset name was accepted")
endif()

execute_process(
    COMMAND "${GENERATOR}" --manifest "${INVALID_MANIFEST}" --namespace test::invalid
            --header "${WORK}/invalid.hpp" --source "${WORK}/invalid.cpp"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error)
if (invalid_result EQUAL 0)
    message(FATAL_ERROR "asset_gen_test: an invalid asset name was accepted")
endif()

message(STATUS "asset_gen_test: ok")
