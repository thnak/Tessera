// SPDX-License-Identifier: MIT

#include "fmt_run.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    if (!args.empty() && args.front() == "fmt") {
        args.erase(args.begin());
    }
    return tessera::fmt_cli::run_fmt(args, std::cout, std::cerr);
}
