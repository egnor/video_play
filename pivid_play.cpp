// Simple command line tool to exercise video decoding and playback.

#include <cmath>
#include <fstream>
#include <numeric>
#include <thread>

#include <CLI/App.hpp>
#include <CLI/Config.hpp>
#include <CLI/Formatter.hpp>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/log.h>
}

#include "display_output.h"
#include "frame_loader.h"
#include "frame_player.h"
#include "logging_policy.h"
#include "media_decoder.h"
#include "script_data.h"
#include "script_runner.h"

namespace pivid {

namespace {

std::shared_ptr<log::logger> const& main_logger() {
    static const auto logger = make_logger("main");
    return logger;
}

std::unique_ptr<DisplayDriver> find_driver(std::string const& dev_arg) {
    fmt::print("=== Video drivers ===\n");
    std::optional<DisplayDriverListing> found;
    for (auto const& d : list_display_drivers(global_system())) {
        auto const text = debug(d);
        if (!found && text.find(dev_arg) != std::string::npos)
            found = d;
        fmt::print("{} {}\n", (found == d) ? "=>" : "  ", debug(d));
    }
    fmt::print("\n");

    if (!found) throw std::runtime_error("No matching device");
    return open_display_driver(global_system(), found->dev_file);
}

void set_kernel_debug(bool enable) {
    auto const debug_file = "/sys/module/drm/parameters/debug";
    auto const debug_stat = global_system()->stat(debug_file).ex(debug_file);
    if ((debug_stat.st_mode & 022) == 0 && debug_stat.st_uid == 0) {
        if (!enable) return;  // No permissions, assume disabled
        std::vector<std::string> argv = {"sudo", "chmod", "go+rw", debug_file};
        fmt::print("!!! Running:");
        for (auto const& arg : argv) fmt::print(" {}", arg);
        fmt::print("\n");
        fflush(stdout);
        auto const pid = global_system()->spawn(argv[0], argv).ex(argv[0]);
        auto const ex = global_system()->wait(P_PID, pid, WEXITED).ex(argv[0]);
        if (ex.si_status) throw(std::runtime_error("Kernel debug chmod error"));
    }

    auto const fd = global_system()->open(debug_file, O_WRONLY).ex(debug_file);
    auto const val = fmt::format("0x{:x}", enable ? 0x3DF : 0);
    fmt::print("Kernel debug: Writing {} to {}\n\n", val, debug_file);
    fd->write(val.data(), val.size()).ex(debug_file);
}

Script make_script(
    std::string const& media_file,
    std::string const& overlay_file,
    std::string const& screen_arg,
    XY<int> mode_xy,
    int mode_hz,
    double start_arg,
    double buffer_arg,
    double overlay_opacity_arg
) {
    Script script;
    script.time_is_relative = true;

    auto *screen = &script.screens.try_emplace(screen_arg).first->second;
    screen->display_mode = mode_xy;
    screen->display_hz = mode_hz;

    if (!media_file.empty()) {
        ScriptLayer* layer = &screen->layers.emplace_back();
        layer->media.file = media_file;
        layer->media.buffer = buffer_arg;
        layer->media.play.segments.push_back(
            linear_segment({0, 1e12}, {start_arg, 1e12 + start_arg})
        );
    }

    if (!overlay_file.empty()) {
        ScriptLayer* layer = &screen->layers.emplace_back();
        layer->media.file = overlay_file;
        layer->media.play.segments.push_back(constant_segment({0, 1e12}, 0));
        layer->opacity.segments.push_back(
            constant_segment({0, 1e12}, overlay_opacity_arg)
        );
    }

    auto const run_start = global_system()->system_time();
    fix_script_time(run_start, &script);
    main_logger()->info("Play start: {}", format_date_time(run_start));
    return script;
}

Script load_script(std::string const& script_file) {
    auto const logger = main_logger();
    auto const sys = global_system();

    logger->info("Loading script: {}", script_file);

    std::ifstream ifs;
    ifs.exceptions(~std::ifstream::goodbit);
    ifs.open(script_file, std::ios::binary);
    std::string const text(
        (std::istreambuf_iterator<char>(ifs)),
        (std::istreambuf_iterator<char>())
    );

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text);
    } catch (nlohmann::json::parse_error const& je) {
        throw_with_nested(std::invalid_argument(je.what()));
    }

    auto script = json.get<Script>();
    if (script.time_is_relative) {
        auto const run_start = sys->system_time();
        fix_script_time(run_start, &script);
        logger->info("Script start: {}", format_date_time(run_start));
    }

    return script;
}

bool layer_is_done(ScriptLayer const& layer, ScriptStatus const& status) {
    Interval const future{status.update_time, 1e12};
    auto const bounds = layer.media.play.range(future).bounds();
    if (bounds.empty() || bounds.end <= 0) return true;
    auto const media_it = status.media_eof.find(layer.media.file);
    if (media_it == status.media_eof.end()) return false;
    return (bounds.begin >= media_it->second);
}

bool script_is_done(Script const& script, ScriptStatus const& status) {
    for (auto const& [connector, screen] : script.screens) {
        for (auto const& layer : screen.layers) {
            if (!layer_is_done(layer, status))
                return false;
        }
    }
    return true;
}

void run_script(std::shared_ptr<DisplayDriver> driver, Script const& script) {
    auto const logger = main_logger();
    auto const sys = global_system();

    ASSERT(script.main_loop_hz > 0);
    double const loop_period = 1.0 / script.main_loop_hz;
    double next_time = 0.0;

    ScriptContext context = {};
    context.driver = std::move(driver);
    auto const runner = make_script_runner(context);
    for (;;) {
        double const now = sys->system_time();
        next_time = std::clamp(next_time, now, now + loop_period);
        sys->sleep_for(next_time - now);
        next_time += loop_period;

        auto const status = runner->update(script);
        if (script_is_done(script, status)) {
            logger->info("All media done playing");
            break;
        }
    }
}

}  // namespace

// Main program, parses flags and calls the decoder loop.
extern "C" int main(int const argc, char const* const* const argv) {
    double buffer_arg = 0.1;
    std::string dev_arg;
    std::string screen_arg = "*";
    std::string log_arg;
    std::string media_arg;
    std::string overlay_arg;
    std::string script_arg;
    XY<int> mode_arg = {0, 0};
    int mode_hz_arg = 0;
    double overlay_opacity_arg = 1.0;
    double start_arg = -0.2;
    bool debug_libav = false;
    bool debug_kernel = false;

    CLI::App app("Decode and show a media file");
    app.add_option("--buffer", buffer_arg, "Seconds of readahead");
    app.add_option("--dev", dev_arg, "DRM driver /dev file or hardware path");
    app.add_option("--log", log_arg, "Log level/configuration");
    app.add_option("--mode_x", mode_arg.x, "Video pixels per line");
    app.add_option("--mode_y", mode_arg.y, "Video scan lines");
    app.add_option("--mode_hz", mode_hz_arg, "Video refresh rate");
    app.add_option("--overlay", overlay_arg, "Image file to overlay");
    app.add_option("--overlay_opacity", overlay_opacity_arg, "Overlay alpha");
    app.add_option("--screen", screen_arg, "Video output connector");
    app.add_option("--start", start_arg, "Seconds into media to start");
    app.add_flag("--debug_libav", debug_libav, "Enable libav* debug logs");
    app.add_flag("--debug_kernel", debug_kernel, "Enable kernel DRM debugging");

    auto input = app.add_option_group("Input")->require_option(0, 1);
    input->add_option("--media", media_arg, "Media file to play");
    input->add_option("--script", script_arg, "Script file to play");

    CLI11_PARSE(app, argc, argv);

    configure_logging(log_arg);
    if (debug_libav) av_log_set_level(AV_LOG_DEBUG);
    pivid::set_kernel_debug(debug_kernel);

    try {
        std::shared_ptr const driver = find_driver(dev_arg);
        if (!script_arg.empty()) {
            auto const script = load_script(script_arg);
            run_script(driver, script);
        } else {
            auto const script = make_script(
                media_arg, overlay_arg, screen_arg, mode_arg, mode_hz_arg,
                start_arg, buffer_arg, overlay_opacity_arg
            );

            run_script(driver, script);
        }
    } catch (std::exception const& e) {
        main_logger()->critical("{}", e.what());
    }

    fmt::print("Done!\n\n");
    return 0;
}

}  // namespace pivid
