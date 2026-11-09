# SPDX-License-Identifier: MIT
#
# Stage the tessera package root for CPack (invoked by the tessera-package-stage target).
#
# Copies exactly the explicit executable set, the compliance files and PACKAGE-INFO.txt into
# STAGE_DIR. CPack packages STAGE_DIR via CPACK_INSTALLED_DIRECTORIES, so unrelated install rules
# never reach the archive.

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

foreach(_tessera_stage_file IN LISTS _tessera_stage_files)
    if(NOT EXISTS "${_tessera_stage_file}")
        message(FATAL_ERROR
            "stage-package.cmake: expected file is missing: ${_tessera_stage_file}\n"
            "Build the package executable targets before running tessera-package-stage.")
    endif()
endforeach()

file(COPY ${_tessera_stage_files} DESTINATION "${STAGE_DIR}")
