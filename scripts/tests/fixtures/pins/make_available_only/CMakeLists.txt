cmake_minimum_required(VERSION 3.24)
project(pins_make_available_only LANGUAGES NONE)

include(FetchContent)

FetchContent_MakeAvailable(ghost)
