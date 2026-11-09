cmake_minimum_required(VERSION 3.24)
project(pins_clean LANGUAGES NONE)

include(FetchContent)

# Commented-out declarations must be ignored by the checker:
# FetchContent_Declare(commented GIT_REPOSITORY https://example.invalid/c.git GIT_TAG main)

FetchContent_Declare(
    alpha
    URL https://example.invalid/alpha-1.2.3.tar.gz
    URL_HASH SHA256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(
    delta
    GIT_REPOSITORY https://example.invalid/delta.git
    GIT_TAG 0123456789abcdef0123456789abcdef01234567)

FetchContent_Declare(
    gamma
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gamma")

FetchContent_MakeAvailable(alpha)
