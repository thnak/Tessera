cmake_minimum_required(VERSION 3.24)
project(pins_url_no_hash LANGUAGES NONE)

include(FetchContent)

FetchContent_Declare(
    beta
    URL https://example.invalid/beta-1.0.0.tar.gz)
