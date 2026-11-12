// SPDX-License-Identifier: MIT

#include "tessera/app/host.hpp"
#include "tessera/automation/engine_host.hpp"
#include "tessera/automation/fixture_scene.hpp"
#include "tessera/automation/manifest_scene.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/artifact_sink.hpp"
#include "tessera/harness/harness_handler.hpp"
#include "tessera/harness/headless_serve.hpp"
#include "tessera/harness/script.hpp"
#include "tessera/harness/serve.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <optional>
#include <ostream>
#include <string>

namespace tessera::harness {
namespace {

constexpr double kTickSeconds = 1.0 / 60.0;

} // namespace

int run_headless_serve(LineTransport& transport, std::string_view output_directory,
                       bool diagnostics_enabled, const std::atomic<bool>* stop, std::ostream& err,
                       const automation::AppManifest* app) {
    (void)err;
    const ScriptProfile profile;
    ui::Node root(1);
    std::optional<automation::FixtureScene> fixture;
    std::optional<automation::ManifestScene> manifest_scene;
    if (app != nullptr) {
        manifest_scene.emplace(root, *app);
    } else {
        fixture.emplace(root);
    }
    const std::string_view stylesheet = app != nullptr ? automation::ManifestScene::stylesheet()
                                                       : automation::FixtureScene::stylesheet();

    runtime::ManualClock clock;
    runtime::RuntimeLoop loop(clock);
    app::HostConfig config;
    config.width = static_cast<float>(profile.width);
    config.height = static_cast<float>(profile.height);
    config.dpi_scale = static_cast<float>(profile.scale);
    app::AppHost host(root, ui::style::compile_stylesheet(stylesheet),
                      loop, config);
    (void)host.begin_frame(0.0);
    host.end_frame();

    diagnostics::TraceRecorder trace;
    diagnostics::MetricsRegistry metrics;
    automation::EngineAutomationHost automation_host(root, host, loop);
    automation::AutomationDiagnostics hooks;
    hooks.trace = &trace;
    hooks.metrics = &metrics;
    hooks.enabled = diagnostics_enabled;
    automation_host.set_diagnostics(hooks);

    FileArtifactSink sink;
    HarnessHandlerConfig handler_config;
    handler_config.output_directory = std::string(output_directory);
    HarnessCommandHandler handler(automation_host, sink, handler_config);

    SteadySessionClock session_clock;
    ServeOptions options;
    options.stop = stop;
    options.on_tick = [&clock, &loop] {
        clock.advance(kTickSeconds);
        (void)loop.tick();
    };
    (void)serve(transport, handler, session_clock, options);
    return 0;
}

} // namespace tessera::harness
