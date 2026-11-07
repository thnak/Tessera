# SPDX-License-Identifier: MIT

add_executable(merger_poc
    main.cpp)

target_link_libraries(merger_poc PRIVATE tessera_merger)
