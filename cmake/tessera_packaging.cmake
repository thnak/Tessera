# SPDX-License-Identifier: MIT
#
# Tessera license-compliance and SBOM install rules.
#
# Installs the project license, third-party notices, the generated license bundle and the
# generated SPDX SBOM next to the application under ${CMAKE_INSTALL_DATADIR}/tessera/licenses
# (normally share/tessera/licenses). The bundle and SBOM are generated from
# docs/specs/dependencies.md and THIRD_PARTY_NOTICES.md by scripts/gen_license_bundle.py and
# scripts/gen_sbom.py; configure fails loudly if any required file is missing.
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
unset(_tessera_license_file)
unset(_tessera_license_path)

message(STATUS "Tessera: license and SBOM files install to ${TESSERA_LICENSE_INSTALL_DIR}")
