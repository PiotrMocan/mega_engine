#include "application.hpp"

#include "vulkan_common.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace vcr {

void SdlWindowDeleter::operator()(SDL_Window* const window) const noexcept {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

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
    window_.reset(SDL_CreateWindow(
        "Vulkan Scene",
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        SDL_WINDOW_VULKAN |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!window_) {
        fatal(SDL_GetError());
    }

    context_.initialize(window_.get(), options.validation);
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
        window_.get(),
        preferred_present,
        options.initial_mode,
        options.instances,
        options.gpu_workload_iterations);
    const auto now = std::chrono::steady_clock::now();
    last_camera_tick_ = now;
    last_title_update_ = now;
    if (options.benchmark) {
        benchmark_.configure(
            options.benchmark_warmup_seconds,
            options.benchmark_duration_seconds);
        benchmark_.start();
    }
    running_ = true;
    if (!options.benchmark) {
        set_mouse_capture(true);
        std::cout
            << "Controls: mouse look, WASD move, Shift boost, "
               "Space/E up, Ctrl/Q down, Esc release/exit\n";
    }
}

void Application::main_loop() {
    using Clock = std::chrono::steady_clock;
    while (running_) {
        const auto frame_begin = Clock::now();
        const float camera_delta_seconds = static_cast<float>(
            std::chrono::duration<double>(
                frame_begin - last_camera_tick_).count());
        last_camera_tick_ = frame_begin;
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
        update_camera(camera_delta_seconds);

        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(
            window_.get(), &pixel_width, &pixel_height);
        if (pixel_width > 0 && pixel_height > 0) {
            FrameTimings timings{};
            const glm::mat4 view = camera_.view_matrix();
            const bool rendered = renderer_.draw_frame(timings, view);
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
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        set_mouse_capture(false);
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (mouse_captured_ && !options_.benchmark) {
            pending_mouse_delta_x_ += event.motion.xrel;
            pending_mouse_delta_y_ += event.motion.yrel;
        }
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (!options_.benchmark &&
            event.button.button == SDL_BUTTON_LEFT &&
            !mouse_captured_) {
            set_mouse_capture(true);
        }
        return;
    }
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }
    switch (event.key.key) {
    case SDLK_ESCAPE:
        if (mouse_captured_ && !options_.benchmark) {
            set_mouse_capture(false);
        } else {
            running_ = false;
        }
        break;
    case SDLK_V:
        renderer_.toggle_vsync();
        renderer_.reset_metrics();
        break;
    case SDLK_F:
        fullscreen_ = !fullscreen_;
        if (!SDL_SetWindowFullscreen(window_.get(), fullscreen_)) {
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
        renderer_.set_mode(RunMode::scene);
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

void Application::set_mouse_capture(const bool enabled) noexcept {
    if (!window_ || mouse_captured_ == enabled) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(window_.get(), enabled)) {
        std::cerr << "Unable to change relative mouse mode: "
                  << SDL_GetError() << '\n';
        return;
    }
    mouse_captured_ = enabled;
    pending_mouse_delta_x_ = 0.0F;
    pending_mouse_delta_y_ = 0.0F;
}

void Application::update_camera(const float delta_seconds) noexcept {
    FlyCameraInput input{};
    if (mouse_captured_ && !options_.benchmark) {
        input.mouse_delta_x = pending_mouse_delta_x_;
        input.mouse_delta_y = pending_mouse_delta_y_;

        int key_count = 0;
        const bool* const keys = SDL_GetKeyboardState(&key_count);
        const auto key_down = [keys, key_count](const SDL_Scancode scancode) {
            const int index = static_cast<int>(scancode);
            return keys != nullptr && index >= 0 && index < key_count &&
                   keys[index];
        };
        input.forward = key_down(SDL_SCANCODE_W);
        input.backward = key_down(SDL_SCANCODE_S);
        input.left = key_down(SDL_SCANCODE_A);
        input.right = key_down(SDL_SCANCODE_D);
        input.up =
            key_down(SDL_SCANCODE_SPACE) || key_down(SDL_SCANCODE_E);
        input.down =
            key_down(SDL_SCANCODE_LCTRL) ||
            key_down(SDL_SCANCODE_RCTRL) ||
            key_down(SDL_SCANCODE_Q);
        input.boost =
            key_down(SDL_SCANCODE_LSHIFT) ||
            key_down(SDL_SCANCODE_RSHIFT);
    }
    pending_mouse_delta_x_ = 0.0F;
    pending_mouse_delta_y_ = 0.0F;
    camera_.update(input, delta_seconds);
}

void Application::update_window_title() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (now - last_title_update_ < std::chrono::milliseconds(250)) {
        return;
    }
    last_title_update_ = now;
    const auto stats = renderer_.metrics_snapshot();
    const glm::vec3 camera_position = camera_.position();
    char title[512]{};
    std::snprintf(
        title,
        sizeof(title),
        "Vulkan Scene | %.0f FPS | avg %.3f ms | CPU %.3f ms | "
        "GPU %.3f ms | cam %.1f %.1f %.1f | %u inst | "
        "%llu tris | %s/%u | %s",
        stats.one_second_average_fps,
        stats.average.cpu_frame_ms,
        stats.latest.cpu_frame_ms,
        stats.latest.gpu_frame_ms,
        static_cast<double>(camera_position.x),
        static_cast<double>(camera_position.y),
        static_cast<double>(camera_position.z),
        renderer_.instance_count(),
        static_cast<unsigned long long>(renderer_.triangle_count()),
        to_string(renderer_.mode()).data(),
        renderer_.gpu_workload_iterations(),
        Swapchain::present_mode_name(renderer_.swapchain().present_mode()));
    SDL_SetWindowTitle(window_.get(), title);
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
    set_mouse_capture(false);
    renderer_.shutdown();
    context_.shutdown();
    window_.reset();
    SDL_Quit();
}

} // namespace vcr
