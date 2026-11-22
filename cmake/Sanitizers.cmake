# SPDX-License-Identifier: MIT
#
# Sanitizer lanes (issue #702, docs/specs/memory-model-v1.md section 10).
#
# Included from the root CMakeLists.txt before any target is defined, so the flags apply uniformly
# to Tessera and to the third-party targets (FreeType, ThorVG, Yoga, ...) that a sanitizer lane must
# also instrument. With TESSERA_SANITIZERS=OFF - the default - this module adds nothing at all, so
# the default build is exactly what it was before the option existed.
#
# Values: OFF, address, undefined, thread, or a comma list of them ("address,undefined"). `address`
# and `thread` are mutually exclusive: ASan and TSan cannot share a process, and clang rejects the
# combination at link time. The `asan` and `tsan` presets in CMakePresets.json are the two lanes.
#
# The runtime behaviour is deliberately NOT baked in here. ASAN_OPTIONS, LSAN_OPTIONS,
# UBSAN_OPTIONS and TSAN_OPTIONS belong to the caller (the nightly workflow, or a developer), so the
# same binary can run with detect_leaks=1 in CI and without it locally. See
# docs/guides/testing.md section 6.

set(TESSERA_SANITIZERS "OFF" CACHE STRING
    "Sanitizers to build with: OFF, address, undefined, thread, or a comma list")

string(STRIP "${TESSERA_SANITIZERS}" TESSERA_SANITIZERS_STRIPPED)
string(TOUPPER "${TESSERA_SANITIZERS_STRIPPED}" TESSERA_SANITIZERS_UPPER)

# The HAS_* probes are read by tessera_allow_allocator_override() below, which every allocation
# test calls unconditionally, so they must exist even when no sanitizer is enabled. -1 is "absent",
# the same value list(FIND) returns for a miss.
set(TESSERA_SANITIZER_HAS_ADDRESS -1)
set(TESSERA_SANITIZER_HAS_UNDEFINED -1)
set(TESSERA_SANITIZER_HAS_THREAD -1)

if (TESSERA_SANITIZERS_STRIPPED STREQUAL "" OR TESSERA_SANITIZERS_UPPER STREQUAL "OFF")
    set(TESSERA_SANITIZER_LIST "")
else()
    string(TOLOWER "${TESSERA_SANITIZERS_STRIPPED}" TESSERA_SANITIZERS_LOWER)
    string(REPLACE "," ";" TESSERA_SANITIZER_LIST "${TESSERA_SANITIZERS_LOWER}")
    list(TRANSFORM TESSERA_SANITIZER_LIST STRIP)
    list(REMOVE_ITEM TESSERA_SANITIZER_LIST "")
endif()

if (TESSERA_SANITIZER_LIST)
    # MSVC has no -fsanitize. Failing here is better than silently building an uninstrumented binary
    # that a lane would then trust: the lanes are Linux/clang only (issue #702).
    if (MSVC)
        message(FATAL_ERROR
            "TESSERA_SANITIZERS=${TESSERA_SANITIZERS} is not supported with MSVC; the sanitizer "
            "lanes are Linux/clang only (CMakePresets.json presets `asan` and `tsan`)")
    endif()

    foreach (sanitizer IN LISTS TESSERA_SANITIZER_LIST)
        if (NOT sanitizer MATCHES "^(address|undefined|thread)$")
            message(FATAL_ERROR
                "TESSERA_SANITIZERS: unknown sanitizer '${sanitizer}' (expected address, "
                "undefined or thread)")
        endif()
    endforeach()

    list(FIND TESSERA_SANITIZER_LIST "address" TESSERA_SANITIZER_HAS_ADDRESS)
    list(FIND TESSERA_SANITIZER_LIST "undefined" TESSERA_SANITIZER_HAS_UNDEFINED)
    list(FIND TESSERA_SANITIZER_LIST "thread" TESSERA_SANITIZER_HAS_THREAD)
    if (TESSERA_SANITIZER_HAS_ADDRESS GREATER -1 AND TESSERA_SANITIZER_HAS_THREAD GREATER -1)
        message(FATAL_ERROR
            "TESSERA_SANITIZERS: 'address' and 'thread' cannot be combined; ASan and TSan do not "
            "share a process. Build them as the two presets `asan` and `tsan`.")
    endif()

    list(REMOVE_DUPLICATES TESSERA_SANITIZER_LIST)
    list(JOIN TESSERA_SANITIZER_LIST "," TESSERA_SANITIZER_VALUE)

    # -fno-omit-frame-pointer keeps the frame chain usable, so a report names the caller instead of
    # "??". -fno-sanitize-recover=all turns a UBSan finding into a failure instead of a warning
    # printed once and ignored: the lane has to go red, which is the whole point of it. A TSan-only
    # build leaves it off, because the `tsan` preset is specified without it and TSAN_OPTIONS=
    # halt_on_error=1 is the runtime equivalent the lane sets.
    set(TESSERA_SANITIZER_COMPILE_OPTIONS
        "-fsanitize=${TESSERA_SANITIZER_VALUE}" -fno-omit-frame-pointer)
    if (TESSERA_SANITIZER_HAS_ADDRESS GREATER -1 OR TESSERA_SANITIZER_HAS_UNDEFINED GREATER -1)
        list(APPEND TESSERA_SANITIZER_COMPILE_OPTIONS -fno-sanitize-recover=all)
    endif()

    add_compile_options(${TESSERA_SANITIZER_COMPILE_OPTIONS})
    add_link_options("-fsanitize=${TESSERA_SANITIZER_VALUE}")

    # The warning policy is untouched: TESSERA_WARNINGS_AS_ERRORS keeps working exactly as it does
    # in every other lane, so a sanitizer build cannot quietly become a warnings-tolerant build.
    message(STATUS
        "Tessera: sanitizers ON (-fsanitize=${TESSERA_SANITIZER_VALUE}); runtime options "
        "(ASAN_OPTIONS/LSAN_OPTIONS/UBSAN_OPTIONS/TSAN_OPTIONS) are set by the caller")
endif()

# Targets that replace the global allocation functions (the allocation-counting tests and probes)
# must call this. It is a no-op unless the build is a TSan build, so a caller writes it
# unconditionally and every other lane is byte-for-byte what it was.
#
# Why it is needed: clang links `libclang_rt.tsan_cxx` into every TSan executable, and that archive
# is a single object, `tsan_new_delete.cpp.o`, whose only job is to define the global `operator
# new`/`delete` family as *strong* symbols (ASan's equivalent is weak, which is why ASan links the
# same tests without complaint). A test that defines its own `operator new` therefore collides:
#
#   multiple definition of `operator new(unsigned long)';
#   libclang_rt.tsan_cxx-x86_64.a(tsan_new_delete.cpp.o): first defined here
#
# `-fno-sanitize-link-c++-runtime` drops that archive from the link. It is the narrow fix, not a
# workaround: the archive contains nothing but the allocator wrappers, so TSan loses no checking
# ability. TSan intercepts `malloc`/`free` itself, so heap races and use-after-free are still
# reported, and the test's own `operator new` (which calls `std::malloc`) is still instrumented.
# Verified on the `tsan` preset: a heap race and a use-after-free both still fail the run, and the
# allocation counter still counts.
function(tessera_allow_allocator_override target)
    if (TESSERA_SANITIZER_HAS_THREAD GREATER -1)
        target_link_options(${target} PRIVATE -fno-sanitize-link-c++-runtime)
    endif()
endfunction()
