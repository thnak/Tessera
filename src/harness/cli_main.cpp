// SPDX-License-Identifier: MIT

#include "tessera/harness/cli.hpp"

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    if (argc > 1) {
        args.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
    }
    return tessera::harness::run_cli(args, std::cout, std::cerr);
}
