// SPDX-License-Identifier: MIT

// Shared helpers for the lowering_module_* tests: build one or more parsed and analysed files and
// lower them with the module-level API (slice 9).

#pragma once

#include "jsx_test_util.hpp"
#include "lowering_nodes_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lowering_module_test {

using namespace jsx_test;
using lowering_nodes_test::expr_text;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace rules = tessera::compiler::rules;
namespace lowering = tessera::compiler::lowering;

// Owns one file's text, parse, map and analysis. Not movable: the SourceMap points into `text`.
struct FileFixture {
    FileFixture(std::string path, std::string text)
        : file(std::move(path)),
          source(std::move(text)),
          parsed(jsx::parse_module(source)),
          map(source),
          analyzed(rules::analyze_module(*parsed.root, map)) {}
    FileFixture(const FileFixture&) = delete;
    FileFixture& operator=(const FileFixture&) = delete;

    [[nodiscard]] lowering::ModuleSource module_source() const {
        return lowering::ModuleSource{file, parsed.root.get(), &analyzed, &map};
    }

    std::string file;
    std::string source;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    rules::AnalyzedModule analyzed;
};

inline std::unique_ptr<FileFixture> parse_file(std::string path, std::string text) {
    auto fixture = std::make_unique<FileFixture>(std::move(path), std::move(text));
    check(fixture->parsed.ok(), "fixture parses cleanly: " + codes(fixture->parsed));
    check(fixture->analyzed.diagnostics.empty(),
          "fixture analyses cleanly: " +
              (fixture->analyzed.diagnostics.empty()
                   ? std::string()
                   : std::string(fixture->analyzed.diagnostics[0].message)));
    return fixture;
}

inline std::unique_ptr<FileFixture> parse_file_raw(std::string path, std::string text) {
    auto fixture = std::make_unique<FileFixture>(std::move(path), std::move(text));
    check(fixture->parsed.root != nullptr, "the raw fixture parses");
    return fixture;
}

// A project of files, in the order given (the API sorts by path internally).
struct Project {
    std::vector<std::unique_ptr<FileFixture>> fixtures;

    void add(std::unique_ptr<FileFixture> fixture) {
        fixtures.push_back(std::move(fixture));
    }

    [[nodiscard]] std::vector<lowering::ModuleSource> sources() const {
        std::vector<lowering::ModuleSource> result;
        result.reserve(fixtures.size());
        for (const std::unique_ptr<FileFixture>& fixture : fixtures) {
            result.push_back(fixture->module_source());
        }
        return result;
    }

    [[nodiscard]] lowering::ModuleLoweringResult lower(std::string_view module_name) const {
        const std::vector<lowering::ModuleSource> list = sources();
        return lowering::lower_project(list, module_name);
    }
};

inline std::string strip_trailing_newline(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

inline const ir::Component* find_component(const lowering::LoweredModule& module,
                                           std::string_view name) {
    for (const ir::Component& component : module.components) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

// One component's full JSON, one line strip, for determinism comparisons.
inline std::string describe_component(const ir::Component& component) {
    return strip_trailing_newline(ir::to_text(ir::to_json(component)));
}

inline std::string describe(const lowering::LoweredModule& module) {
    std::string out = "module " + module.name + "\n";
    for (const ir::Component& component : module.components) {
        out += describe_component(component) + "\n";
    }
    for (const ir::Binding& binding : module.bindings) {
        out += strip_trailing_newline(ir::to_text(ir::to_json(binding))) + "\n";
    }
    return out;
}

template<typename Value>
inline std::string first_code(const std::expected<Value, lowering::NodeLoweringErrors>& result) {
    if (result || result.error().empty()) {
        return {};
    }
    return std::string(result.error().front().code);
}

inline std::string
first_message(const std::expected<lowering::LoweredModule, lowering::NodeLoweringErrors>& result) {
    if (result || result.error().empty()) {
        return {};
    }
    return result.error().front().message;
}

} // namespace lowering_module_test
