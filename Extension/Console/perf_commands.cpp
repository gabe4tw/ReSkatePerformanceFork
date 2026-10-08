#include "Extension/Console/commands.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Settings/engine_tweaks.h"
#include "Extension/Settings/job_spin.h"
#include "Extension/UI/ui_pointer_skip.h"
#include "Engine/Core/Log/logging.h"
#include <chrono>
#include <format>
#include <thread>

// The performance profiler's console commands (Engine/Core/Profiling/profiler.h). Everything but
// the saved HUD switch runs on the overlay's thread: the profiler is thread-safe.
namespace dingosdk::console {
namespace {
std::string narrow(const std::filesystem::path& path) {
    std::string result;
    for (const auto c : path.wstring()) result.push_back(c < 128 ? static_cast<char>(c) : '?');
    return result;
}
void print_summary(const Output& out, std::size_t zone_limit = 12) {
    if (!profiler::active()) {
        out("The profiler is off: `perf hud on` or `perf window on` starts it (figures follow a second later).");
        return;
    }
    const auto s = profiler::summary();
    if (!s || s->seconds <= 0) { out("No figures yet; try again in a second."); return; }
    out(std::format("Client update: {:.1f}/s, every {:.1f} ms (longest {:.1f}); client frame {:.2f} ms (longest {:.1f}); "
        "ReSkate tick {:.2f} ms (longest {:.1f}) + hooks {:.2f} ms per frame",
        s->client_updates_per_second, s->client_interval_average_ms, s->client_interval_longest_ms,
        s->client_frame_average_ms, s->client_frame_longest_ms, s->client_own_average_ms,
        s->client_own_longest_ms, s->hooks_milliseconds_per_update));
    out(std::format("Frames: {:.1f}/s, {:.2f} ms (longest {:.1f}); process CPU {:.0f}% of {} cores",
        s->presents_per_second, s->present_interval_average_ms, s->present_interval_longest_ms,
        s->process_cpu_percent, s->cores));
    out("ReSkate zones (busiest first):");
    std::size_t shown{};
    for (const auto& zone : s->zones) {
        if (shown++ == zone_limit) break;
        out(std::format("  {:<48} {:7.2f} ms/s  {:7.0f}/s  {:8.1f} us avg  {:6.2f} ms longest", zone.name,
            zone.milliseconds_per_second, zone.calls_per_second, zone.average_microseconds, zone.longest_milliseconds));
    }
    out("Threads (100% = one core):");
    shown = 0;
    for (const auto& thread : s->threads) {
        if (shown++ == 10) break;
        out(std::format("  {:>6} {:<26} {:5.1f}%", thread.id, thread.label, thread.cpu_percent));
    }
}
void print_report(const Output& out) {
    const auto r = profiler::sample_report();
    if (!r || (!r->running && !r->samples && r->error.empty())) { out("No sample yet: `perf sample` takes one."); return; }
    if (r->running) { out(std::format("Sampling {}: {:.0f}%, {} samples so far.", r->target, r->progress * 100, r->samples)); return; }
    if (!r->error.empty()) out("error: " + r->error);
    if (r->spike_ms > 0) {
        if (!r->spikes) {
            out(std::format("No frame took longer than {:.0f} ms in {:.0f} s. Play normally during `perf spikes`, or try a lower "
                "threshold.", r->spike_ms, r->seconds));
            return;
        }
        out(std::format("{} frames took longer than {:.0f} ms (longest {:.0f} ms). Only what ran during them is counted below.",
            r->spikes, r->spike_ms, r->spike_longest_ms));
    }
    if (!r->samples) return;
    out(std::format("{} (thread {}): {} samples over {:.1f} s, busy {:.1f}%.", r->target, r->thread_id, r->samples,
        r->seconds, r->busy_percent));
    for (const auto& thread : r->threads)
        out(std::format("  {:5.1f}%  thread {} {}", thread.cpu_percent, thread.id, thread.label));
    out("  Self  Total  Function");
    std::size_t shown{};
    for (const auto& f : r->functions) {
        if (shown++ == 20) break;
        out(std::format("{:5.1f}% {:5.1f}%  {}", 100.0 * f.self / r->samples, 100.0 * f.total / r->samples, f.name));
    }
    out("Report: " + narrow(r->report));
    out("Flame graph (open at speedscope.app): " + narrow(r->folded));
}
}

void register_perf_commands(Commands& registry) {
    // The HUD starts off every launch; these turn it on for the session.
    auto toggle = action("perf", "Show or hide the performance HUD", Group::console);
    toggle.execution = Execution::local;
    toggle.run = [](const Model&, const Values&, const Output& out) {
        const bool on = !profiler::hud();
        profiler::set_hud(on);
        out(on ? "Performance HUD on. `perf window on` opens the profiler; `perf sample` takes a stack sample."
               : "Performance HUD off.");
    };
    registry.add(std::move(toggle));

    auto hud = variable("perf hud", "Performance HUD: client update, frame time, ReSkate's cost, CPU per thread",
        Group::console, argument("on|off", Type::boolean));
    hud.execution = Execution::local;
    hud.inspect = [](const Model&) { return boolean_state(true, profiler::hud()); };
    hud.run = [](const Model&, const Values& args, const Output& out) {
        const bool on = std::get<bool>(args[0]);
        profiler::set_hud(on);
        out(on ? "Performance HUD on." : "Performance HUD off.");
    };
    registry.add(std::move(hud));

    auto window = variable("perf window", "The profiler window (shown while the menu or console is open)",
        Group::console, argument("on|off", Type::boolean));
    window.execution = Execution::local;
    window.inspect = [](const Model&) { return boolean_state(true, profiler::window()); };
    window.run = [](const Model&, const Values& args, const Output& out) {
        profiler::set_window(std::get<bool>(args[0]));
        out(profiler::window() ? "Profiler window on: it shows while the menu or console is open." : "Profiler window off.");
    };
    registry.add(std::move(window));

    auto seconds = argument("seconds", Type::number, true);
    seconds.minimum = 0.5;
    seconds.maximum = 60;
    auto target = argument("client|present|all|<thread id>", Type::text, true);
    target.complete = [](const Model&, auto) { return std::vector<std::string>{"client", "present", "all"}; };
    auto rate = argument("rate", Type::unsigned_integer, true);
    rate.minimum = 50;
    rate.maximum = 4000;
    auto sample = action("perf sample", "Sample stacks: the client update thread (default, 10 s at 1000 Hz), present, all busy threads, or a thread id",
        Group::console, {seconds, target, rate});
    sample.execution = Execution::local;
    sample.run = [](const Model&, const Values& args, const Output& out) {
        profiler::SampleRequest request;
        if (args.size() > 0) request.seconds = std::get<double>(args[0]);
        if (args.size() > 1) {
            const auto& which = std::get<std::string>(args[1]);
            if (equal(which, "client")) request.target = profiler::Target::client;
            else if (equal(which, "present")) request.target = profiler::Target::present;
            else if (equal(which, "all")) { request.target = profiler::Target::all; request.rate = 250; }
            else {
                request.target = profiler::Target::thread;
                try { request.thread_id = static_cast<std::uint32_t>(std::stoul(which, nullptr, 0)); }
                catch (...) { out("error: The target is client, present, all or a thread id."); return; }
            }
        }
        if (args.size() > 2) request.rate = static_cast<unsigned>(std::get<std::uint64_t>(args[2]));
        std::string error;
        if (!profiler::start_sampling(request, error)) { out("error: " + error); return; }
        out(std::format("Sampling for {:.1f} s at {} Hz. `perf report` shows the result; the files go next to ReSkate.log.",
            request.seconds, request.rate));
    };
    registry.add(std::move(sample));

    // Hitches: what every busy thread was doing during the frames that took too long, and nothing else.
    auto spike_seconds = argument("seconds", Type::number, true);
    spike_seconds.minimum = 5;
    spike_seconds.maximum = 180;
    auto spike_threshold = argument("milliseconds", Type::number, true);
    spike_threshold.minimum = 20;
    spike_threshold.maximum = 2000;
    auto spikes = action("perf spikes",
        "Find what causes hitches: sample all busy threads (default 90 s) and report only frames longer than a threshold (default 80 ms)",
        Group::console, {spike_seconds, spike_threshold});
    spikes.execution = Execution::local;
    spikes.run = [](const Model&, const Values& args, const Output& out) {
        profiler::SampleRequest request;
        request.target = profiler::Target::all;
        request.rate = 100; // 10 ms apart: a 100 ms hitch still gets ten samples per busy thread
        request.seconds = args.size() > 0 ? std::get<double>(args[0]) : 90;
        request.spike_ms = args.size() > 1 ? std::get<double>(args[1]) : 80;
        std::string error;
        if (!profiler::start_sampling(request, error)) { out("error: " + error); return; }
        out(std::format("Watching for frames over {:.0f} ms for {:.0f} s. Close the console and play normally; "
            "`perf report` shows the result.", request.spike_ms, request.seconds));
    };
    registry.add(std::move(spikes));

    auto stop = action("perf stop", "Stop the running sample early (it still reports)", Group::console);
    stop.execution = Execution::local;
    stop.run = [](const Model&, const Values&, const Output& out) {
        profiler::stop_sampling();
        out("Stopping the sample.");
    };
    registry.add(std::move(stop));

    auto report = action("perf report", "The last sample's busiest functions and its report files", Group::console);
    report.execution = Execution::local;
    report.run = [](const Model&, const Values&, const Output& out) { print_report(out); };
    registry.add(std::move(report));

    auto microseconds = argument("microseconds", Type::number);
    microseconds.minimum = 0;
    microseconds.maximum = 5000;
    auto spin = variable("perf jobspin", "How long idle engine job threads poll for work before sleeping (ReSkate 50 us, stock 250 us)",
        Group::engine, microseconds);
    spin.execution = Execution::local;
    spin.inspect = [](const Model&) {
        const auto value = job_spin::microseconds();
        return State{value.has_value(), value ? std::optional<std::string>(std::format("{:.0f}", *value)) : std::nullopt,
                     "The job system's spin setting is unavailable for this build.",
                     std::format("ReSkate default {:.0f} us; stock {:.0f} us", job_spin::default_microseconds,
                         job_spin::stock_microseconds()),
                     value && *value != job_spin::default_microseconds};
    };
    spin.run = [](const Model&, const Values& args, const Output& out) {
        const double wanted = std::get<double>(args[0]);
        if (!job_spin::set_microseconds(wanted)) { out("error: The job system's spin setting is unavailable for this build."); return; }
        out(std::format("Job threads now poll for {:.0f} us before sleeping (ReSkate default {:.0f}, stock {:.0f}; not saved).",
            wanted, job_spin::default_microseconds, job_spin::stock_microseconds()));
    };
    spin.reset = [](const Model&, const Output& out) {
        if (!job_spin::set_microseconds(job_spin::default_microseconds)) { out("error: The job system's spin setting is unavailable."); return; }
        out(std::format("Job threads poll for {:.0f} us again.", job_spin::default_microseconds));
    };
    registry.add(std::move(spin));

    auto pointer = variable("perf uipointer", "Skip the UI's per-frame mouse hit-test while the game shows no cursor (on by default)",
        Group::engine, argument("on|off", Type::boolean));
    pointer.execution = Execution::local;
    pointer.inspect = [](const Model&) { return boolean_state(true, ui_pointer::skip(), {}, ui_pointer::status()); };
    pointer.run = [](const Model&, const Values& args, const Output& out) {
        ui_pointer::set_skip(std::get<bool>(args[0]));
        out(ui_pointer::status());
    };
    registry.add(std::move(pointer));

    // Engine tweaks (Extension/Settings/engine_tweaks.h): mainsleep and dirtyskip start on, the
    // others off; nothing is saved. Measure each against stock with `perf sample 8 all`.
    const auto tweak_variable = [&](std::string name, std::string description, engine_tweaks::Tweak tweak, Argument value) {
        auto entry = variable(std::move(name), std::move(description), Group::engine, std::move(value));
        entry.execution = Execution::local;
        const bool numeric = tweak == engine_tweaks::Tweak::gi || tweak == engine_tweaks::Tweak::job_wake;
        entry.inspect = [tweak, numeric](const Model&) {
            const auto now = engine_tweaks::value(tweak);
            const bool changed = now != engine_tweaks::default_value(tweak);
            if (!numeric) {
                auto state = boolean_state(engine_tweaks::available(tweak), now > 0, engine_tweaks::status(tweak), engine_tweaks::status(tweak));
                state.overridden = changed;
                return state;
            }
            return State{engine_tweaks::available(tweak), std::format("{:.0f}", now), engine_tweaks::status(tweak),
                         engine_tweaks::status(tweak), changed};
        };
        entry.run = [tweak, numeric](const Model&, const Values& args, const Output& out) {
            const double wanted = numeric ? std::get<double>(args[0]) : (std::get<bool>(args[0]) ? 1.0 : 0.0);
            std::string error;
            if (!engine_tweaks::set(tweak, wanted, error)) { out("error: " + error); return; }
            out(engine_tweaks::status(tweak));
        };
        entry.reset = [tweak](const Model&, const Output& out) {
            std::string error;
            if (!engine_tweaks::set(tweak, engine_tweaks::default_value(tweak), error)) { out("error: " + error); return; }
            out(engine_tweaks::status(tweak));
        };
        registry.add(std::move(entry));
    };
    auto updates = argument("updates per second", Type::number);
    updates.minimum = 0;
    updates.maximum = 1000;
    tweak_variable("perf gi", "Throttle Enlighten GI's per-frame update to this rate (0 = every frame, stock; try 60)",
        engine_tweaks::Tweak::gi, updates);
    tweak_variable("perf meshtree", "Cull meshes through the octree the game ships off (MeshCullTreeEnabled)",
        engine_tweaks::Tweak::mesh_tree, argument("on|off", Type::boolean));
    auto wait = argument("microseconds", Type::number);
    wait.minimum = 0;
    wait.maximum = 1000;
    tweak_variable("perf jobwake", "Idle job workers wait this long where a push wakes them cheaply, and pushes prefer them (0 = stock; measured to cost more CPU than it saves)",
        engine_tweaks::Tweak::job_wake, wait);
    tweak_variable("perf mainsleep", "Pace the main loop on a high-resolution timer with a 0.25 ms spin instead of a 1 ms one (on by default)",
        engine_tweaks::Tweak::main_sleep, argument("on|off", Type::boolean));
    tweak_variable("perf dirtyskip", "Skip the per-section spinlock that clears dirty bits when none are set (on by default)",
        engine_tweaks::Tweak::dirty_skip, argument("on|off", Type::boolean));
    auto tweaks = action("perf tweaks", "What each engine tweak is doing (perf gi, meshtree, jobwake, mainsleep, dirtyskip)", Group::console);
    tweaks.execution = Execution::local;
    tweaks.run = [](const Model&, const Values&, const Output& out) {
        for (const auto tweak : {engine_tweaks::Tweak::gi, engine_tweaks::Tweak::mesh_tree, engine_tweaks::Tweak::job_wake,
                 engine_tweaks::Tweak::main_sleep, engine_tweaks::Tweak::dirty_skip})
            out(engine_tweaks::status(tweak));
    };
    registry.add(std::move(tweaks));

    auto status = action("perf status", "Client update and frame timing, ReSkate's zones, CPU per thread", Group::console);
    status.execution = Execution::local;
    status.run = [](const Model&, const Values&, const Output& out) { print_summary(out); };
    registry.add(std::move(status));

    // The console and menu cost frames themselves (drawing, the mouse hit-test), so `perf status`
    // typed into the console measures them too. This writes the same report to ReSkate.log a while
    // later, with every zone, once the console is closed and the game is being played.
    auto delay = argument("seconds", Type::number, true);
    delay.minimum = 3;
    delay.maximum = 120;
    auto later = action("perf log", "Write `perf status` (every zone) to ReSkate.log after a delay (default 10 s), "
        "so it measures play with the console closed", Group::console, {delay});
    later.execution = Execution::local;
    later.run = [](const Model&, const Values& args, const Output& out) {
        const double log_delay = args.empty() ? 10.0 : std::get<double>(args[0]);
        // The figures need the profiler running; the window keeps it running without drawing
        // anything while the menu and console are closed.
        const bool started = !profiler::active();
        if (started) profiler::set_window(true);
        try {
            std::thread([log_delay, started] {
                std::this_thread::sleep_for(std::chrono::duration<double>(log_delay));
                Output log;
                log.write = [](const std::string& line) {
                    logging::write(logging::Level::info, logging::Channel::diagnostics, line);
                };
                log("perf log: the figures for the last second");
                print_summary(log, 64);
                if (started) profiler::set_window(false);
            }).detach();
        } catch (...) {
            if (started) profiler::set_window(false);
            out("error: Cannot start the timer.");
            return;
        }
        out(std::format("In {:.0f} s the report goes to ReSkate.log. Close the console and skate normally until then.", log_delay));
    };
    registry.add(std::move(later));
}
} // namespace dingosdk::console
