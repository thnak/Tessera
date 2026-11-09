# SPDX-License-Identifier: MIT
#
# Stage the tessera package root for CPack (invoked by the tessera-package-stage target).
#
# Copies exactly the explicit executable set, the compliance files, PACKAGE-INFO.txt and
# RELEASE-NOTES.txt into STAGE_DIR, then writes a sorted SHA256SUMS over every staged file except
# itself. CPack packages STAGE_DIR via CPACK_INSTALLED_DIRECTORIES, so unrelated install rules never
# reach the archive. Everything written here is deterministic: sorted names, LF, two-space
# separators, no timestamps or absolute paths.

if(NOT DEFINED STAGE_DIR OR STAGE_DIR STREQUAL "")
    message(FATAL_ERROR "stage-package.cmake: STAGE_DIR is not set")
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")
file(MAKE_DIRECTORY "${STAGE_DIR}")

set(_tessera_stage_files ${EXECUTABLE_FILES} ${LICENSE_FILES})
if(DEFINED INFO_FILE AND EXISTS "${INFO_FILE}")
    list(APPEND _tessera_stage_files "${INFO_FILE}")
else()
    message(FATAL_ERROR "stage-package.cmake: PACKAGE-INFO.txt is missing: ${INFO_FILE}")
endif()
if(DEFINED RELEASE_NOTES_FILE AND EXISTS "${RELEASE_NOTES_FILE}")
    list(APPEND _tessera_stage_files "${RELEASE_NOTES_FILE}")
else()
    message(FATAL_ERROR "stage-package.cmake: RELEASE-NOTES.txt is missing: ${RELEASE_NOTES_FILE}")
endif()

foreach(_tessera_stage_file IN LISTS _tessera_stage_files)
    if(NOT EXISTS "${_tessera_stage_file}")
        message(FATAL_ERROR
            "stage-package.cmake: expected file is missing: ${_tessera_stage_file}\n"
            "Build the package executable targets before running tessera-package-stage.")
    endif()
endforeach()

file(COPY ${_tessera_stage_files} DESTINATION "${STAGE_DIR}")

# SHA256SUMS covers every shipped file except itself. Sorted by relative path, one
# "<sha256>  <file>" line each (the sha256sum format, with two spaces).
set(_tessera_checksum_names)
foreach(_tessera_stage_file IN LISTS _tessera_stage_files)
    get_filename_component(_tessera_stage_name "${_tessera_stage_file}" NAME)
    if(_tessera_stage_name IN_LIST _tessera_checksum_names)
        message(FATAL_ERROR
            "stage-package.cmake: duplicate staged file name '${_tessera_stage_name}'")
    endif()
    list(APPEND _tessera_checksum_names "${_tessera_stage_name}")
endforeach()
list(SORT _tessera_checksum_names)

set(_tessera_checksum_lines "")
foreach(_tessera_stage_name IN LISTS _tessera_checksum_names)
    file(SHA256 "${STAGE_DIR}/${_tessera_stage_name}" _tessera_stage_hash)
    string(APPEND _tessera_checksum_lines
        "${_tessera_stage_hash}  ${_tessera_stage_name}\n")
endforeach()
# CMake writes text files in the platform newline mode; SHA256SUMS must be LF so the
# Linux/macOS `sha256sum -c` verification also works on archives built on Windows.
set(_tessera_checksum_template "${STAGE_DIR}/SHA256SUMS.template")
file(WRITE "${_tessera_checksum_template}" "${_tessera_checksum_lines}")
configure_file("${_tessera_checksum_template}" "${STAGE_DIR}/SHA256SUMS"
    @ONLY
    NEWLINE_STYLE LF)
file(REMOVE "${_tessera_checksum_template}")
