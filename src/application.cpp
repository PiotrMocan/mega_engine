#include "application.hpp"

#include "vulkan_common.hpp"

#include <cstring>
#include <chrono>
#include <cstdio>
#include <iostream>

namespace vcr {

Application::~Application() {
    shutdown();
}

int Application::run(const int argc, char** argv) {
    const auto parsed = parse_cli_options(argc, argv);
    if (!parsed) {
        std::cerr << "error: " << parsed.error << "\n\n"
                  << cli_usage(argc > 0 ? argv[0] : "vulkan_cube");
        return 1;
    }
    if (parsed.options.show_help) {
        std::cout << cli_usage(argc > 0 ? argv[0] : "vulkan_cube");
        return 0;
    }
    options_ = parsed.options;
    initialize(parsed.options);
    main_loop();
    shutdown();
    return exit_code_;
}

void Application::initialize(const CliOptions& options) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fatal(SDL_GetError());
    }
    window_ = SDL_CreateWindow(
        "Vulkan Cube Benchmark",
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        fatal(SDL_GetError());
    }

    context_.initialize(window_, options.validation);
    VkPresentModeKHR preferred_present = VK_PRESENT_MODE_IMMEDIATE_KHR;
    switch (options.present) {
    case PresentPreference::automatic:
        // Auto is deliberately uncapped. If IMMEDIATE is unavailable, the
        // swapchain falls back to MAILBOX and finally to guaranteed FIFO.
        preferred_present = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    case PresentPreference::immediate:
        preferred_present = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    case PresentPreference::mailbox:
        preferred_present = VK_PRESENT_MODE_MAILBOX_KHR;
        break;
    case PresentPreference::fifo:
        preferred_present = VK_PRESENT_MODE_FIFO_KHR;
        break;
    }
    renderer_.initialize(
        context_,
        window_,
        preferred_present,
        options.initial_mode,
        options.instances,
        options.gpu_workload_iterations);
    last_title_update_ = std::chrono::steady_clock::now();
    if (options.benchmark) {
        benchmark_.configure(
            options.benchmark_warmup_seconds,
            options.benchmark_duration_seconds);
        benchmark_.start();
    }
    running_ = true;
}

void Application::main_loop() {
    using Clock = std::chrono::steady_clock;
    while (running_) {
        const auto frame_begin = Clock::now();
        if (options_.benchmark) {
            const auto tick = benchmark_.tick(frame_begin);
            if (tick.measurement_began) {
                renderer_.reset_metrics();
                std::cout << "Benchmark measurement started ("
                          << options_.benchmark_duration_seconds
                          << " s)\n" << std::flush;
            }
            if (tick.completed) {
                finish_benchmark();
                running_ = false;
                break;
            }
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            process_event(event);
        }
        if (!running_) {
            break;
        }

        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
        if (pixel_width > 0 && pixel_height > 0) {
            FrameTimings timings{};
            const bool rendered = renderer_.draw_frame(timings);
            if (rendered) {
                timings.cpu_frame_ms =
                    std::chrono::duration<double, std::milli>(
                        Clock::now() - frame_begin).count();
                renderer_.record_metrics(timings);
                update_window_title();
            }
        } else {
            SDL_WaitEventTimeout(nullptr, 50);
        }
    }
}

void Application::process_event(const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT) {
        running_ = false;
        return;
    }
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_RESIZED) {
        renderer_.request_resize();
        return;
    }
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }
    switch (event.key.key) {
    case SDLK_ESCAPE:
        running_ = false;
        break;
    case SDLK_SPACE:
        renderer_.toggle_rotation_pause();
        break;
    case SDLK_V:
        renderer_.toggle_vsync();
        renderer_.reset_metrics();
        break;
    case SDLK_F:
        fullscreen_ = !fullscreen_;
        if (!SDL_SetWindowFullscreen(window_, fullscreen_)) {
            std::cerr << "Unable to toggle fullscreen: " << SDL_GetError() << '\n';
            fullscreen_ = !fullscreen_;
        }
        break;
    case SDLK_R:
        renderer_.reset_metrics();
        break;
    case SDLK_P:
        renderer_.print_metrics();
        break;
    case SDLK_1:
        renderer_.set_mode(RunMode::minimal);
        renderer_.reset_metrics();
        break;
    case SDLK_2:
        renderer_.set_mode(RunMode::instancing);
        renderer_.reset_metrics();
        break;
    case SDLK_3:
        renderer_.set_mode(RunMode::gpu_saturation);
        renderer_.reset_metrics();
        break;
    case SDLK_UP:
        renderer_.adjust_load(1);
        renderer_.reset_metrics();
        break;
    case SDLK_DOWN:
        renderer_.adjust_load(-1);
        renderer_.reset_metrics();
        break;
    default:
        break;
    }
}

void Application::update_window_title() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (now - last_title_update_ < std::chrono::milliseconds(250)) {
        return;
    }
    last_title_update_ = now;
    const auto stats = renderer_.metrics_snapshot();
    char title[512]{};
    std::snprintf(
        title,
        sizeof(title),
        "Vulkan Cube | %.0f FPS | avg %.3f ms | CPU %.3f ms | "
        "GPU %.3f ms | %u inst | %llu tris | %s/%u | %s",
        stats.one_second_average_fps,
        stats.average.cpu_frame_ms,
        stats.latest.cpu_frame_ms,
        stats.latest.gpu_frame_ms,
        renderer_.instance_count(),
        static_cast<unsigned long long>(renderer_.triangle_count()),
        to_string(renderer_.mode()).data(),
        renderer_.gpu_workload_iterations(),
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()));
    SDL_SetWindowTitle(window_, title);
}

void Application::finish_benchmark() {
    renderer_.print_metrics();
    const auto stats = renderer_.metrics_snapshot();
    const auto extent = renderer_.swapchain().extent();
    char resolution[64]{};
    std::snprintf(
        resolution,
        sizeof(resolution),
        "%ux%u",
        extent.width,
        extent.height);
    const auto& properties = context_.properties();
    const auto result = make_benchmark_result(
        properties.deviceName,
        std::to_string(properties.driverVersion),
        resolution,
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()),
        renderer_.instance_count(),
        renderer_.gpu_workload_iterations(),
        stats);
    std::string error;
    if (!write_benchmark_json("benchmark.json", result, &error)) {
        std::cerr << "Unable to write benchmark.json: " << error << '\n';
        exit_code_ = 1;
        return;
    }
    std::cout << "\nBenchmark result written to benchmark.json\n"
              << benchmark_result_to_json(result) << std::flush;
}

void Application::shutdown() noexcept {
    running_ = false;
    renderer_.shutdown();
    context_.shutdown();
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

} // namespace vcr
