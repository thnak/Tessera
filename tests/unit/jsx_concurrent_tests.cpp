// SPDX-License-Identifier: MIT

// Independent parses on different threads must give exactly the results of the same parses run
// one after another. Each parse call owns all of its state (see "Concurrency" in
// docs/design/jsx-frontend.md); this test shows determinism under concurrency, it cannot show the
// absence of data races (no TSan on the development machine).

#include "jsx_test_util.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

enum class Entry { Module, Element, Expression, Type };

struct Case {
    Entry entry;
    std::string source;
};

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out.append(text);
    }
    return out;
}

template<typename Result> void append_result(std::string& out, const Result& result) {
    for (const auto& diagnostic : result.diagnostics) {
        out += format_diagnostic(diagnostic);
        out += '\n';
    }
    out += "comments=" + std::to_string(result.comments.size());
    out += result.diagnostics_truncated ? " truncated" : "";
    out += '\n';
}

// Everything observable about one parse as text: tree dump, diagnostics, comment count.
std::string fingerprint(const Case& test, const std::string& source) {
    std::string out;
    switch (test.entry) {
    case Entry::Module: {
        const auto result = parse_module(source);
        out = result.root ? dump(*result.root) : "<null>";
        append_result(out, result);
        break;
    }
    case Entry::Element: {
        const auto result = parse_jsx_element(source);
        out = result.root ? dump(*result.root) : "<null>";
        append_result(out, result);
        break;
    }
    case Entry::Expression: {
        const auto result = parse_expression(source);
        out = result.root ? dump(*result.root) : "<null>";
        append_result(out, result);
        break;
    }
    case Entry::Type: {
        const auto result = parse_type(source);
        out = result.root ? dump(*result.root) : "<null>";
        append_result(out, result);
        break;
    }
    }
    return out;
}

std::vector<Case> make_cases() {
    std::vector<Case> cases;
    const std::string counter = "import { useState, useEffect, View, Text } from \"tessera\";\n"
                                "type P = { initial: int };\n"
                                "// Note: bind::on_x\n"
                                "export function Counter({ initial = 0 }: P) {\n"
                                "  const [n, setN] = useState<Array<int>>(initial);\n"
                                "  useEffect(() => { go(n); return () => { stop(); }; }, [n]);\n"
                                "  return (<View a=\"1\" {...p}>{`t${n}`} &amp; {rows.map((r) => "
                                "(<Row key={r.id}/>))}</View>);\n"
                                "}\n";
    cases.push_back({Entry::Module, counter});
    cases.push_back({Entry::Module, counter + "function Broken() { const = ; return <X/>; }\n" +
                                        "function After() { return <A/>; }\n"});
    cases.push_back({Entry::Module, "function a() { let x = 1; }\nconst k = 1\nclass Z {}\n"});
    cases.push_back({Entry::Module, repeat("const = 1;\n", 400)});
    cases.push_back({Entry::Module, "function A() { return " + repeat("<A>", 3000)});
    cases.push_back({Entry::Element, "<View a=\"1\" b={x == y}>\n  text &foo; <div/>\n</View>"});
    cases.push_back({Entry::Element, "<A><B></C></A>"});
    cases.push_back({Entry::Element, "<A b=\"unterminated/>"});
    cases.push_back({Entry::Element, "<A>" + repeat("&bogus;", 500) + "</A>"});
    cases.push_back({Entry::Element, std::string("<A>\xFF\xC3(</A>")});
    cases.push_back({Entry::Expression, "a ? b : c ? d : (e + f) * g"});
    cases.push_back({Entry::Expression, "useState<Array<int>>(x) ?? y"});
    cases.push_back({Entry::Expression, repeat("(", 4000) + "1" + repeat(")", 4000)});
    cases.push_back({Entry::Expression, "a" + repeat("+a", 500)});
    cases.push_back({Entry::Type, "{ a: int; b?: Row[] } | null"});
    cases.push_back({Entry::Type, "Map<string, number> & any"});
    cases.push_back({Entry::Type, repeat("A<", 2000)});
    return cases;
}

void test_parallel_parses_match_sequential_parses() {
    const std::vector<Case> cases = make_cases();

    std::vector<std::string> expected;
    for (const auto& test : cases) {
        expected.push_back(fingerprint(test, test.source));
    }
    // The inputs must exercise both outcomes, or the comparison proves little.
    check(expected[0].find("<null>") == std::string::npos &&
              expected[0].find("error[") == std::string::npos,
          "the first input is clean");
    check(expected[1].find("error[") != std::string::npos, "the second input has diagnostics");
    check(expected[4].find("LIMIT_DEPTH") != std::string::npos, "an input hits a limit");

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kRounds = 3;
    // Even threads read the one shared buffer, odd threads read private copies.
    std::vector<std::string> shared;
    for (const auto& test : cases) {
        shared.push_back(test.source);
    }

    std::vector<std::vector<std::string>> mismatches(kThreads);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::vector<std::string> own;
            if (t % 2 == 1) {
                own = shared;
            }
            const std::vector<std::string>& sources = t % 2 == 1 ? own : shared;
            for (std::size_t round = 0; round < kRounds; ++round) {
                // Different threads start at different inputs so they overlap on different work.
                for (std::size_t i = 0; i < cases.size(); ++i) {
                    const std::size_t index = (i + t * 3 + round) % cases.size();
                    if (fingerprint(cases[index], sources[index]) != expected[index]) {
                        mismatches[t].push_back("thread " + std::to_string(t) + " input " +
                                                std::to_string(index));
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& list : mismatches) {
        for (const auto& message : list) {
            check(false, "parallel result differs from the sequential result: " + message);
        }
    }
}

} // namespace

int main() {
    test_parallel_parses_match_sequential_parses();
    return finish("jsx concurrent tests");
}
