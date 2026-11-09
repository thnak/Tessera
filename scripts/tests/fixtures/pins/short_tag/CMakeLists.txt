cmake_minimum_required(VERSION 3.24)
project(pins_short_tag LANGUAGES NONE)

include(FetchContent)

FetchContent_Declare(
    beta
    GIT_REPOSITORY https://example.invalid/beta.git
    GIT_TAG v1.2.3)
