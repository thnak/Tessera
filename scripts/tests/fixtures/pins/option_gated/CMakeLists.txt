cmake_minimum_required(VERSION 3.24)
project(pins_option_gated LANGUAGES NONE)

include(FetchContent)

if(FALSE)
    FetchContent_Declare(
        beta
        GIT_REPOSITORY https://example.invalid/beta.git
        GIT_TAG main)
endif()
