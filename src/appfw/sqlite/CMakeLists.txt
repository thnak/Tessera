# SPDX-License-Identifier: MIT
#
# Opt-in module `tessera_appfw_storage_sqlite` (issue #837, docs/design/storage-sqlite.md). Included
# from src/appfw/CMakeLists.txt only when -DTESSERA_APPFW_SQLITE=ON. Nothing else links it, so an app
# that does not adopt it carries none of the engine.
#
# The SQLite amalgamation is pinned by archive hash and fetched at configure time (the FreeType
# precedent in third_party/CMakeLists.txt) instead of committing a 9 MB generated file. A machine
# without network access points TESSERA_SQLITE_SOURCE_DIR at an unpacked amalgamation.

set(TESSERA_SQLITE_VERSION "3.54.1")
set(TESSERA_SQLITE_URL "https://sqlite.org/2026/sqlite-amalgamation-3540100.zip")
set(TESSERA_SQLITE_SHA256 "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9")

if (TESSERA_SQLITE_SOURCE_DIR)
    set(_tessera_sqlite_dir "${TESSERA_SQLITE_SOURCE_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(
        sqlite3
        URL ${TESSERA_SQLITE_URL}
        URL_HASH SHA256=${TESSERA_SQLITE_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sqlite3)
    set(_tessera_sqlite_dir "${sqlite3_SOURCE_DIR}")
endif()

add_library(tessera_sqlite3 STATIC ${_tessera_sqlite_dir}/sqlite3.c)
target_include_directories(tessera_sqlite3 PUBLIC ${_tessera_sqlite_dir})
# Features the module does not use are compiled out; the connection is serialized by Database, and
# no extension loading means no dlopen.
target_compile_definitions(tessera_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_USE_ALLOCA)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(tessera_sqlite3 PUBLIC Threads::Threads)
if (MSVC)
    target_compile_options(tessera_sqlite3 PRIVATE /W0)
else()
    target_compile_options(tessera_sqlite3 PRIVATE -w)
    target_link_libraries(tessera_sqlite3 PUBLIC m)
endif()

add_library(tessera_appfw_storage_sqlite STATIC
    sql_connection.cpp
    sql_database.cpp
    sql_value.cpp)
target_include_directories(tessera_appfw_storage_sqlite PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(tessera_appfw_storage_sqlite PUBLIC cxx_std_23)
# Layering (open question 4): this module depends on `storage` and the runtime executor seam, never
# the reverse; tessera_sqlite3 stays private so no SQLite header reaches a consumer.
target_link_libraries(tessera_appfw_storage_sqlite
    PUBLIC tessera_appfw_storage tessera_runtime
    PRIVATE tessera_sqlite3)

if (TESSERA_BUILD_TESTS)
    add_executable(appfw_sql_tests ${PROJECT_SOURCE_DIR}/tests/unit/appfw_sql_tests.cpp)
    target_link_libraries(appfw_sql_tests PRIVATE tessera_appfw_storage_sqlite tessera_runtime)
    target_compile_definitions(appfw_sql_tests PRIVATE
        TESSERA_SQL_SCRATCH_DIR="${PROJECT_BINARY_DIR}/sql-scratch")
    add_test(NAME appfw_sql COMMAND appfw_sql_tests)
endif()
