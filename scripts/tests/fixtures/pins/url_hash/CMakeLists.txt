cmake_minimum_required(VERSION 3.24)
project(pins_url_hash LANGUAGES NONE)

include(FetchContent)

FetchContent_Declare(
    beta
    URL https://example.invalid/beta-1.0.0.tar.gz
    URL_HASH SHA256=fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210)
