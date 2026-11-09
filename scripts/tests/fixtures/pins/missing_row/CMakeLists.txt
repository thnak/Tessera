cmake_minimum_required(VERSION 3.24)
project(pins_missing_row LANGUAGES NONE)

include(FetchContent)

FetchContent_Declare(
    newdep
    GIT_REPOSITORY https://example.invalid/newdep.git
    GIT_TAG 0123456789abcdef0123456789abcdef01234567)

FetchContent_MakeAvailable(newdep)
