// SPDX-License-Identifier: MIT
//
// OS-scheduled background jobs (issue #1043): the fake scheduler playing the OS, and the headless
// application host running a job under a budget. Deterministic: a `ManualClock`, a hand-drained
// `CompletionQueue`, no wall clock, thread or OS scheduler.

#include "appfw_task_scope_test_support.hpp"
#include "tessera/appfw/application.hpp"
#include "tessera/appfw/background_job_fake.hpp"
#include "tessera/appfw/background_job_host.hpp"
#include "tessera/appfw/service_runner.hpp"

#include <string>

namespace {

using namespace appfw_task_test;
using appfw::FakeJobScheduler;
using appfw::JobOutcome;
using Wake = FakeJobScheduler::WakeResult;

constexpr auto kBrokenId = appfw::service_id("tessera.test.BrokenJobService");

struct BrokenService final : appfw::Service {
    runtime::Task<appfw::ServiceStatus> start(appfw::StartContext& ctx) override {
        return run_start(ctx);
    }
    runtime::Task<void> stop(appfw::StopContext& ctx) override {
        return run_stop(ctx);
    }
    static runtime::Task<appfw::ServiceStatus> run_start(appfw::StartContext) {
        co_return appfw::ServiceStatus::Failed;
    }
    static runtime::Task<void> run_stop(appfw::StopContext) {
        co_return;
    }
};

// A job whose work is `hops` queue hops, then `finish(result)`. `hops < 0` never finishes.
class ScriptedJob final : public appfw::BackgroundJob {
public:
    ScriptedJob(std::string id, runtime::CompletionQueue& queue, int hops,
                appfw::JobResult result = appfw::JobResult::Success)
        : id_(std::move(id)),
          queue_(&queue),
          hops_(hops),
          result_(result) {}

    std::string_view id() const override {
        return id_;
    }
    appfw::JobConstraints constraints() const override {
        return {true, false, 0.0};
    }
    bool start(appfw::JobContext& context) override {
        ++starts;
        if (refuse) {
            return false;
        }
        context.set_checkpoint([this] { ++checkpoints; });
        return context.scope().spawn(work(context.scope().context(), *this, context));
    }

    int starts = 0;
    int checkpoints = 0;
    int completions = 0;
    bool refuse = false;

private:
    static runtime::Task<void> work(runtime::TaskContext task_context, ScriptedJob& self,
                                    appfw::JobContext& context) {
        if (self.hops_ < 0) {
            co_await ParkForever{};
        }
        for (int step = 0; step < self.hops_; ++step) {
            co_await QueueHop{self.queue_};
            if (task_context.token.cancelled()) {
                co_return;
            }
        }
        ++self.completions;
        context.finish(self.result_);
    }

    std::string id_;
    runtime::CompletionQueue* queue_ = nullptr;
    int hops_ = 0;
    appfw::JobResult result_ = appfw::JobResult::Success;
};

struct Host {
    explicit Host(bool broken = false)
        : runner(clock, queue, nullptr) {
        if (broken) {
            expect(runner.add(appfw::ServiceRegistration{kBrokenId, {}, true}, bad).has_value(),
                   "the broken service registers");
        }
        expect(runner.prepare().has_value(), "the runner prepares");
    }

    // One loop iteration: drain, then tick, which is what a real headless loop does.
    void step(double dt = 0.0) {
        clock.advance(dt);
        pump(queue);
        runner.tick();
        host.tick();
        pump(queue);
    }
    void run_to_done(int limit = 64, double dt = 0.0) {
        for (int i = 0; i < limit && !host.done(); ++i) {
            step(dt);
        }
    }

    runtime::ManualClock clock;
    runtime::CompletionQueue queue{256};
    appfw::ServiceRunner runner;
    BrokenService bad;
    appfw::Application application{runner};
    appfw::HeadlessJobHost host{application, clock, queue, {2.0, 0.5, 8}};
};

bool scheduler_plays_the_os() {
    bool ok = true;
    FakeJobScheduler os;
    ok = expect(os.wake("sync") == Wake::UnknownId, "nothing scheduled, nothing to wake") && ok;
    ok = expect(os.schedule("sync", {true, true, 60.0}), "schedules") && ok;
    ok = expect(os.entries().size() == 1, "one entry") && ok;
    ok = expect(os.wake("sync") == Wake::TooEarly, "the delay has not elapsed") && ok;
    os.set_now(61.0);
    os.set_device(false, false);
    ok = expect(os.wake("sync") == Wake::NoNetwork, "constraint: network") && ok;
    os.set_device(true, false);
    ok = expect(os.wake("sync") == Wake::NotCharging, "constraint: charging") && ok;
    os.set_device(true, true);
    ok = expect(os.wake("sync") == Wake::Woken, "all constraints hold") && ok;
    ok = expect(os.entries().empty(), "a woken entry is one-shot") && ok;

    ok = expect(os.schedule("a", {}) && os.schedule("a", {true, false, 0.0}) &&
                    os.entries().size() == 1,
                "the same id replaces") &&
         ok;
    ok = expect(os.cancel("a") && !os.cancel("a"), "cancel reports removal once") && ok;
    os.set_refuse(true);
    ok = expect(!os.schedule("b", {}) && os.entries().empty(), "a refusing platform is a value") &&
         ok;
    ok = expect(!FakeJobScheduler{}.schedule("", {}), "an empty id is refused") && ok;
    return ok;
}

bool job_runs_to_success_and_services_stop() {
    bool ok = true;
    Host f;
    ScriptedJob job("sync", f.queue, 3);
    ok = expect(f.host.add(job) && !f.host.add(job), "a duplicate id is refused") && ok;
    ok = expect(!f.host.begin("missing", 30.0), "unknown id") && ok;
    ok = expect(f.host.begin("sync", 30.0), "begins") && ok;
    ok = expect(f.host.budget_seconds() == 28.0, "budget is the OS budget minus the margin") && ok;
    ok = expect(!f.host.begin("sync", 30.0), "one run at a time") && ok;
    f.run_to_done();
    ok = expect(f.host.done() && f.host.outcome() == JobOutcome::Success, "success") && ok;
    ok = expect(job.starts == 1 && job.completions == 1 && job.checkpoints == 0,
                "ran once, finished, no checkpoint needed") &&
         ok;
    ok =
        expect(f.application.state() == appfw::AppState::Terminated, "services were stopped") && ok;
    return ok;
}

bool finish_results_map_to_outcomes() {
    bool ok = true;
    for (const auto [result, expected] :
         {std::pair{appfw::JobResult::Retry, JobOutcome::RetryRequested},
          std::pair{appfw::JobResult::Failed, JobOutcome::Failed}}) {
        Host f;
        ScriptedJob job("j", f.queue, 1, result);
        (void)f.host.add(job);
        (void)f.host.begin("j", 10.0);
        f.run_to_done();
        ok = expect(f.host.outcome() == expected, "result maps to outcome") && ok;
    }
    return ok;
}

bool budget_expiry_checkpoints_then_cancels() {
    bool ok = true;
    Host f;
    ScriptedJob job("slow", f.queue, -1);
    (void)f.host.add(job);
    (void)f.host.begin("slow", 12.0);
    f.run_to_done(8, 0.0);
    ok =
        expect(f.host.phase() == appfw::JobHostPhase::Running, "still running inside the budget") &&
        ok;
    f.step(9.0);
    ok =
        expect(f.host.phase() == appfw::JobHostPhase::Running, "budget is 10 s, 9 s elapsed") && ok;
    f.step(1.0);
    ok = expect(f.host.outcome() == JobOutcome::BudgetExpired && f.host.checkpoint_ran() &&
                    job.checkpoints == 1,
                "at the budget: checkpoint once, then expiry") &&
         ok;
    f.run_to_done(8, 1.0);
    ok = expect(f.host.done(), "the host finishes after the grace") &&
         expect(f.host.scope_timed_out(), "a parked child never drains: the grace times out") && ok;
    ok = expect(f.application.state() == appfw::AppState::Terminated,
                "services still stop after a timed-out scope") &&
         ok;
    return ok;
}

bool os_stop_checkpoints() {
    bool ok = true;
    Host f;
    ScriptedJob job("slow", f.queue, 1000);
    (void)f.host.add(job);
    (void)f.host.begin("slow", 60.0);
    f.host.request_stop();
    ok = expect(f.host.outcome() == JobOutcome::Pending, "stop before Running is a no-op") && ok;
    for (int i = 0; i < 32 && f.host.phase() != appfw::JobHostPhase::Running; ++i) {
        f.step();
    }
    ok = expect(f.host.phase() == appfw::JobHostPhase::Running, "reached Running") && ok;
    f.host.request_stop();
    f.run_to_done(64, 0.1);
    ok = expect(f.host.outcome() == JobOutcome::Stopped && job.checkpoints == 1,
                "OS stop: checkpoint then Stopped") &&
         ok;
    ok = expect(f.host.done() && !f.host.scope_timed_out(),
                "a cooperative child drains inside the grace") &&
         ok;
    return ok;
}

bool refusals_end_as_not_started() {
    bool ok = true;
    {
        Host f(true);
        ScriptedJob job("j", f.queue, 1);
        (void)f.host.add(job);
        (void)f.host.begin("j", 10.0);
        f.run_to_done();
        ok = expect(f.host.done() && f.host.outcome() == JobOutcome::NotStarted && job.starts == 0,
                    "a required service failure never starts the job") &&
             ok;
    }
    {
        Host f;
        ScriptedJob job("j", f.queue, 1);
        job.refuse = true;
        (void)f.host.add(job);
        (void)f.host.begin("j", 10.0);
        f.run_to_done();
        ok = expect(f.host.done() && f.host.outcome() == JobOutcome::NotStarted && job.starts == 1,
                    "start() refusing is NotStarted") &&
             ok;
    }
    {
        Host f;
        ScriptedJob job("j", f.queue, 1);
        (void)f.host.add(job);
        ok = expect(f.host.begin("j", 1.5) && f.host.done() &&
                        f.host.outcome() == JobOutcome::BudgetExpired && job.starts == 0 &&
                        !f.application.launched(),
                    "a budget eaten by the margin launches nothing") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = scheduler_plays_the_os() && ok;
    ok = job_runs_to_success_and_services_stop() && ok;
    ok = finish_results_map_to_outcomes() && ok;
    ok = budget_expiry_checkpoints_then_cancels() && ok;
    ok = os_stop_checkpoints() && ok;
    ok = refusals_end_as_not_started() && ok;
    if (!ok || appfw_task_test::failures != 0) {
        return 1;
    }
    std::cout << "appfw_background_job: ok\n";
    return 0;
}
