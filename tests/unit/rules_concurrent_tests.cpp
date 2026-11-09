// SPDX-License-Identifier: MIT

// Analysing different modules on different threads must give exactly the diagnostics, symbols and
// roles of analysing them one after another: analyze_module keeps all state per call. Like
// jsx_concurrent this shows determinism, not the absence of data races.

#include "rules_test_util.hpp"

#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace rules_test;

std::string fingerprint(const std::string& source) {
    const Fixture fixture(source);
    std::string out = describe_all(fixture);
    for (const auto& analysis : fixture.analyzed.components) {
        out += (analysis.ok ? "ok " : "failed ") + symbol_text(analysis) + "| " +
               role_text(analysis) + "\n";
    }
    return out;
}

std::vector<std::string> sources() {
    return {
        component("  const [n, setN] = useState<int>(0);\n"
                  "  useEffect(() => { log(n); return () => { stop(); }; }, [n]);\n"),
        component("  const t = useTheme();\n"),
        component("  const [n, setN] = useState<int>(0);\n"
                  "  useEffect(() => { log(n); }, []);\n"),
        component("  const f = () => { go(); };\n"),
        component("  const [n, setN] = flag ? useState<int>(0) : 1;\n"),
        component("  useEffect(() => { return 1; }, []);\n"),
    };
}

} // namespace

int main() {
    const std::vector<std::string> inputs = sources();
    std::vector<std::string> expected;
    for (const auto& source : inputs) {
        expected.push_back(fingerprint(source));
    }
    check(!expected[1].empty() && expected[1] != expected[0], "the fixtures differ");

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kRounds = 50;
    std::vector<std::vector<std::string>> actual(kThreads);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (std::size_t round = 0; round < kRounds; ++round) {
                for (const auto& source : inputs) {
                    actual[t].push_back(fingerprint(source));
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (std::size_t t = 0; t < kThreads; ++t) {
        check(actual[t].size() == kRounds * inputs.size(), "every thread finished its rounds");
        for (std::size_t i = 0; i < actual[t].size(); ++i) {
            check_eq(actual[t][i], expected[i % inputs.size()],
                     "thread " + std::to_string(t) + " result " + std::to_string(i) +
                         " equals the sequential result");
        }
    }
    return finish("rules concurrent tests");
}
