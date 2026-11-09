# Tessera license-compliance, SBOM and distributable-package rules.
#
# Always (plain configure / cmake --install):
#   Installs LICENSE, THIRD_PARTY_NOTICES.md, THIRD_PARTY_LICENSES.txt and sbom.spdx.json under
#   ${CMAKE_INSTALL_DATADIR}/tessera/licenses. Configure fails loudly if any is missing. The files
#   are generated from docs/specs/dependencies.md and THIRD_PARTY_NOTICES.md by
#   scripts/gen_license_bundle.py and scripts/gen_sbom.py.
#
# Opt-in packaging (TESSERA_PACKAGE_EXECUTABLES not empty):
#   Adds a CPack TGZ archive (plus ZIP on Windows) named
#   tessera-<version>-<os>-<arch> whose top-level directory contains exactly the explicit
#   executable set, the four compliance files, PACKAGE-INFO.txt, RELEASE-NOTES.txt and a sorted
#   SHA256SUMS. CPack packages a staged directory (not the install tree), so unrelated install rules
#   (fetched dependency headers, install-tree copies) cannot leak into the archive. The CPack config
#   is only generated when the option is set, so a plain configure has no packaging side effects.
#
# The include line is owned by the root CMakeLists.txt owner and is already wired at the end of
# the root CMakeLists.txt:
#
#     include(cmake/tessera_packaging.cmake)
#
# The module is standalone so it can also be exercised without touching the root file:
#
#     cmake -S . -B build/packaging-check \
#       -DCMAKE_PROJECT_INCLUDE=cmake/tessera_packaging.cmake \
#       -DTESSERA_BUILD_VULKAN=OFF -DTESSERA_BUILD_POC=OFF
#     cmake --install build/packaging-check --prefix <temp-prefix>

include(GNUInstallDirs)

set(TESSERA_PACKAGING_SOURCE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.."
    CACHE PATH "Repository root that provides LICENSE, THIRD_PARTY_NOTICES.md and generated files")
set(TESSERA_LICENSE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/tessera/licenses")

set(_tessera_license_files
    "LICENSE"
    "THIRD_PARTY_NOTICES.md"
    "THIRD_PARTY_LICENSES.txt"
    "sbom.spdx.json")

foreach(_tessera_license_file IN LISTS _tessera_license_files)
    set(_tessera_license_path "${TESSERA_PACKAGING_SOURCE_ROOT}/${_tessera_license_file}")
    if(NOT EXISTS "${_tessera_license_path}")
        message(FATAL_ERROR
            "tessera_packaging: required license file is missing: ${_tessera_license_path}\n"
            "Generate THIRD_PARTY_LICENSES.txt with "
            "`python -B scripts/gen_license_bundle.py --write`, sbom.spdx.json with "
            "`python -B scripts/gen_sbom.py --write`, and restore the other files before "
            "configuring a release build.")
    endif()
    install(FILES "${_tessera_license_path}"
        DESTINATION "${TESSERA_LICENSE_INSTALL_DIR}"
        COMPONENT licenses)
endforeach()

message(STATUS "Tessera: license and SBOM files install to ${TESSERA_LICENSE_INSTALL_DIR}")

# ---------------------------------------------------------------------------------------------
# Optional distributable archive. Empty by default so a plain configure/install is unchanged.
# ---------------------------------------------------------------------------------------------
set(TESSERA_PACKAGE_EXECUTABLES
    ""
    CACHE STRING
    "Semicolon-separated built executable targets to ship in the CPack archive (e.g. merger_tests)")
set(TESSERA_PACKAGE_COMMIT
    ""
    CACHE STRING "Commit id written to PACKAGE-INFO.txt (default: git HEAD)")
set(TESSERA_PACKAGE_OS
    ""
    CACHE STRING "OS label in the package name (default: windows/linux/macos)")
set(TESSERA_PACKAGE_ARCH
    ""
    CACHE STRING "Architecture label in the package name (default: x86_64/arm64)")

if(TESSERA_PACKAGE_EXECUTABLES)
    foreach(_tessera_package_target IN LISTS TESSERA_PACKAGE_EXECUTABLES)
        if(NOT TARGET "${_tessera_package_target}")
            message(FATAL_ERROR
                "tessera_packaging: TESSERA_PACKAGE_EXECUTABLES lists "
                "'${_tessera_package_target}', but no such target exists. Fix the list or turn "
                "the corresponding build option on.")
        endif()
    endforeach()

    if(NOT TESSERA_PACKAGE_OS)
        if(WIN32)
            set(_tessera_package_os "windows")
        elseif(APPLE)
            set(_tessera_package_os "macos")
        else()
            set(_tessera_package_os "linux")
        endif()
    else()
        set(_tessera_package_os "${TESSERA_PACKAGE_OS}")
    endif()

    if(NOT TESSERA_PACKAGE_ARCH)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _tessera_package_arch)
        if(_tessera_package_arch MATCHES "^(x86_64|amd64)$")
            set(_tessera_package_arch "x86_64")
        elseif(_tessera_package_arch MATCHES "^(aarch64|arm64)$")
            set(_tessera_package_arch "arm64")
        endif()
    else()
        set(_tessera_package_arch "${TESSERA_PACKAGE_ARCH}")
    endif()
    set(_tessera_package_platform "${_tessera_package_os}-${_tessera_package_arch}")

    if(TESSERA_PACKAGE_COMMIT)
        set(_tessera_package_commit "${TESSERA_PACKAGE_COMMIT}")
    else()
        execute_process(
            COMMAND git rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${TESSERA_PACKAGING_SOURCE_ROOT}"
            OUTPUT_VARIABLE _tessera_package_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _tessera_package_git_result)
        if(NOT _tessera_package_git_result EQUAL 0 OR NOT _tessera_package_commit)
            set(_tessera_package_commit "unknown")
        endif()
    endif()

    if(TESSERA_STATIC_RUNTIME)
        set(_tessera_package_static_runtime "ON")
    else()
        set(_tessera_package_static_runtime "OFF")
    endif()
    string(REPLACE ";" ", " _tessera_package_executable_list
        "${TESSERA_PACKAGE_EXECUTABLES}")

    set(_tessera_package_stage "${CMAKE_CURRENT_BINARY_DIR}/tessera-package-root")
    set(_tessera_package_info "${CMAKE_CURRENT_BINARY_DIR}/tessera-package-info/PACKAGE-INFO.txt")
    set(_tessera_package_release_notes
        "${CMAKE_CURRENT_BINARY_DIR}/tessera-package-info/RELEASE-NOTES.txt")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/package-info.txt.in"
        "${_tessera_package_info}"
        @ONLY
        NEWLINE_STYLE LF)
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/release-notes.txt.in"
        "${_tessera_package_release_notes}"
        @ONLY
        NEWLINE_STYLE LF)

    # Stage exactly the files that ship: executable(s), the four compliance files and INFO. CPack
    # packages this directory instead of the install tree, so fetched dependency headers/libraries
    # and install-tree copies cannot leak into the archive.
    set(_tessera_package_executable_genexes)
    foreach(_tessera_package_target IN LISTS TESSERA_PACKAGE_EXECUTABLES)
        list(APPEND _tessera_package_executable_genexes
            "$<TARGET_FILE:${_tessera_package_target}>")
    endforeach()
    set(_tessera_package_license_paths)
    foreach(_tessera_license_file IN LISTS _tessera_license_files)
        list(APPEND _tessera_package_license_paths
            "${TESSERA_PACKAGING_SOURCE_ROOT}/${_tessera_license_file}")
    endforeach()

    add_custom_target(tessera-package-stage
        COMMAND ${CMAKE_COMMAND}
            "-DEXECUTABLE_FILES=${_tessera_package_executable_genexes}"
            "-DLICENSE_FILES=${_tessera_package_license_paths}"
            "-DINFO_FILE=${_tessera_package_info}"
            "-DRELEASE_NOTES_FILE=${_tessera_package_release_notes}"
            "-DSTAGE_DIR=${_tessera_package_stage}"
            -P "${CMAKE_CURRENT_LIST_DIR}/stage-package.cmake"
        DEPENDS ${TESSERA_PACKAGE_EXECUTABLES}
        VERBATIM)

    # Reproducible archives: CPack/libarchive take entry mtimes from SOURCE_DATE_EPOCH and ZIP DOS
    # timestamps from the writer's timezone. CI exports SOURCE_DATE_EPOCH=315532800 and TZ=UTC
    # before cpack; `scripts/check_reproducible.py --package` builds twice and requires
    # byte-identical archives.
    set(CPACK_GENERATOR "TGZ")
    if(WIN32)
        list(APPEND CPACK_GENERATOR "ZIP")
    endif()
    set(CPACK_PACKAGE_NAME "tessera")
    set(CPACK_PACKAGE_VENDOR "Tessera")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Tessera runtime package")
    set(CPACK_PACKAGE_FILE_NAME "tessera-${PROJECT_VERSION}-${_tessera_package_platform}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_ARCHIVE_THREADS 1)
    set(CPACK_SOURCE_GENERATOR "")
    # Package the staged directory only: no CPack-driven install tree, so no preinstall/all build
    # and no unrelated install rules in the archive.
    set(CPACK_INSTALL_CMAKE_PROJECTS "")
    set(CPACK_INSTALLED_DIRECTORIES "${_tessera_package_stage};.")
    include(CPack)
    if(TARGET package)
        add_dependencies(package tessera-package-stage)
    endif()
    message(STATUS
        "Tessera: CPack archive target 'package' -> "
        "tessera-${PROJECT_VERSION}-${_tessera_package_platform}.tar.gz"
        " (executables: ${_tessera_package_executable_list})")
endif()

unset(_tessera_license_file)
unset(_tessera_license_path)
