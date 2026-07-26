#include "cli_options.hpp"
#include "fly_camera.hpp"
#include "mcp_control.hpp"
#include "performance_metrics.hpp"
#include "png_encoder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

#include <glm/geometric.hpp>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool close_to(const double value, const double expected, const double epsilon) {
    return std::abs(value - expected) <= epsilon;
}

} // namespace

int main() {
    {
        const char* arguments[]{
            "vulkan_cube",
            "--benchmark",
            "--width=1920",
            "--height",
            "1080",
            "--instances",
            "10000",
            "--present",
            "immediate",
            "--aa",
            "fsr3",
            "--fsr-quality=balanced",
            "--smaa-quality",
            "ultra",
            "--no-validation",
        };
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(static_cast<bool>(parsed), "valid CLI parses");
        expect(parsed.options.benchmark, "benchmark flag");
        expect(parsed.options.width == 1920, "width value");
        expect(parsed.options.height == 1080, "height value");
        expect(parsed.options.instances == 10'000, "instance value");
        expect(
            parsed.options.initial_mode == vcr::RunMode::instancing,
            "instances infer instancing mode");
        expect(
            parsed.options.present == vcr::PresentPreference::immediate,
            "present preference");
        expect(
            parsed.options.anti_aliasing.method ==
                    vcr::AntiAliasingMethod::fsr3 &&
                parsed.options.anti_aliasing.fsr3_quality ==
                    vcr::Fsr3Quality::balanced &&
                parsed.options.anti_aliasing.smaa_quality ==
                    vcr::SmaaQuality::ultra,
            "anti-aliasing CLI values");
        expect(!parsed.options.validation, "no-validation flag");
    }

    {
        const char* arguments[]{"vulkan_cube", "--mcp-stdio"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(
            static_cast<bool>(parsed) && parsed.options.mcp_stdio,
            "MCP stdio flag");
    }

    {
        const char* arguments[]{"vulkan_cube", "--width", "0"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(!static_cast<bool>(parsed), "zero width is rejected");
    }

    {
        const char* arguments[]{"vulkan_cube", "--aa", "txaa"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(
            !static_cast<bool>(parsed),
            "unknown anti-aliasing method is rejected");
    }

    {
        expect(
            vcr::adjust_quality(vcr::SmaaQuality::low, -1) ==
                    vcr::SmaaQuality::low &&
                vcr::adjust_quality(vcr::SmaaQuality::high, 1) ==
                    vcr::SmaaQuality::ultra,
            "SMAA quality adjustment is bounded");
        expect(
            vcr::adjust_quality(
                vcr::Fsr3Quality::ultra_performance, 1) ==
                    vcr::Fsr3Quality::ultra_performance &&
                close_to(
                    vcr::fsr3_scale(vcr::Fsr3Quality::performance),
                    2.0,
                    0.0001),
            "FSR 3 quality adjustment and scale");
        expect(
            vcr::to_token(vcr::AntiAliasingMethod::fsr3) == "fsr3",
            "anti-aliasing method has a stable machine token");
    }

    {
        const char* arguments[]{"vulkan_cube", "--mode", "minimal"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(
            static_cast<bool>(parsed) &&
                parsed.options.initial_mode == vcr::RunMode::scene,
            "legacy minimal mode selects the game scene");
    }

    {
        const auto interactive_order = vcr::present_preference_order(
            vcr::PresentPreference::automatic, false);
        expect(
            interactive_order.size == 3 &&
                interactive_order.values[0] ==
                    vcr::PresentPreference::immediate &&
                interactive_order.values[1] ==
                    vcr::PresentPreference::mailbox &&
                interactive_order.values[2] ==
                    vcr::PresentPreference::fifo,
            "automatic present mode is uncapped");
    }

    {
        vcr::PerformanceMetrics metrics;
        vcr::FrameTimings timings{};
        timings.cpu_frame_ms = 10.0;
        timings.fence_wait_ms = 1.0;
        timings.gpu_frame_ms = 4.0;
        for (int index = 0; index < 100; ++index) {
            metrics.record_frame(timings);
        }
        const auto snapshot = metrics.snapshot();
        expect(snapshot.frames_rendered == 100, "frame count");
        expect(close_to(snapshot.average_fps, 100.0, 0.001), "average FPS");
        expect(
            close_to(snapshot.one_percent_low_fps, 100.0, 0.001),
            "one-percent-low FPS");
        expect(
            close_to(snapshot.average.gpu_frame_ms, 4.0, 0.001),
            "GPU average");
        expect(
            close_to(snapshot.average.fence_wait_ms, 1.0, 0.001),
            "fence average");
    }

    {
        using namespace std::chrono_literals;
        vcr::BenchmarkController controller(1.0, 2.0);
        const auto start = vcr::BenchmarkController::TimePoint{};
        controller.start(start);
        auto tick = controller.tick(start + 500ms);
        expect(
            tick.phase == vcr::BenchmarkPhase::warmup,
            "benchmark warmup phase");
        tick = controller.tick(start + 1s);
        expect(tick.measurement_began, "measurement transition");
        tick = controller.tick(start + 3s);
        expect(tick.completed, "benchmark completion");
    }

    {
        vcr::BenchmarkResult result{};
        result.gpu = "GPU \"quoted\"";
        result.render_resolution = "1280x720";
        result.anti_aliasing = "FSR 3.1.4";
        result.anti_aliasing_quality = "quality";
        result.frames_rendered = 42;
        const std::string json = vcr::benchmark_result_to_json(result);
        expect(
            json.find("GPU \\\"quoted\\\"") != std::string::npos,
            "JSON string escaping");
        expect(
            json.find("\"frames_rendered\": 42") != std::string::npos,
            "JSON integer field");
        expect(
            json.find("\"anti_aliasing\": \"FSR 3.1.4\"") !=
                    std::string::npos &&
                json.find("\"render_resolution\": \"1280x720\"") !=
                    std::string::npos,
            "benchmark records anti-aliasing and render resolution");
    }

    {
        vcr::FlyCamera camera(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        const glm::vec3 forward = camera.forward();
        expect(
            close_to(forward.x, 0.0, 0.0001) &&
                close_to(forward.y, 0.0, 0.0001) &&
                close_to(forward.z, -1.0, 0.0001),
            "camera initially faces negative Z");

        vcr::FlyCameraInput input{};
        input.forward = true;
        camera.update(input, 0.05F);
        expect(
            close_to(camera.position().z, -0.3, 0.0001),
            "camera forward movement uses configured speed");

        vcr::FlyCamera boosted(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        input.boost = true;
        boosted.update(input, 0.05F);
        expect(
            close_to(boosted.position().z, -1.2, 0.0001),
            "camera shift boost multiplies movement speed");
    }

    {
        vcr::FlyCamera forward_only(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        vcr::FlyCamera diagonal(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        vcr::FlyCameraInput input{};
        input.forward = true;
        forward_only.update(input, 0.05F);
        input.right = true;
        diagonal.update(input, 0.05F);
        expect(
            close_to(
                glm::length(forward_only.position()),
                glm::length(diagonal.position()),
                0.0001),
            "camera diagonal movement is normalized");

        const glm::vec3 before = diagonal.forward();
        input = {};
        input.mouse_delta_x = 100.0F;
        diagonal.update(input, 0.0F);
        expect(
            glm::length(diagonal.forward() - before) > 0.01F,
            "mouse look is independent of frame delta");
    }

    {
        vcr::FlyCamera camera(
            glm::vec3(0.0F),
            -1.57079632679F,
            0.0F);
        vcr::FlyCameraInput input{};
        input.forward = true;
        input.backward = true;
        input.left = true;
        input.right = true;
        input.up = true;
        input.down = true;
        camera.update(input, 0.05F);
        expect(
            glm::length(camera.position()) < 0.0001F,
            "opposing camera inputs cancel");

        input = {};
        input.forward = true;
        camera.update(input, 1.0F);
        expect(
            close_to(camera.position().z, -0.3, 0.0001),
            "camera delta is clamped after a stall");
        const glm::vec3 position = camera.position();
        camera.update(input, -1.0F);
        camera.update(
            input, std::numeric_limits<float>::quiet_NaN());
        expect(
            glm::length(camera.position() - position) < 0.0001F,
            "invalid camera deltas do not move the camera");

        input = {};
        input.mouse_delta_y = 100'000.0F;
        camera.update(input, 0.0F);
        expect(
            camera.forward().y < -0.99F,
            "camera pitch is clamped below the singularity");
    }

    {
        vcr::McpControlPlane control;
        vcr::McpCommand submitted{
            .token = 42,
            .type = vcr::McpCommandType::render_set_instances,
            .value = 10'000,
        };
        expect(
            control.submit(submitted),
            "MCP command queue accepts a command");
        vcr::McpCommand received{};
        expect(
            control.try_next_command(received) &&
                received.token == 42 &&
                received.value == 10'000,
            "MCP command queue preserves typed payload");
        expect(
            control.complete(vcr::McpCommandResult{
                .token = 42,
                .success = true,
                .message = "ok",
                .structured_json = "{\"ok\":true}",
            }),
            "MCP result queue accepts a response");
        vcr::McpCommandResult result{};
        expect(
            control.try_next_result(result) &&
                result.token == 42 &&
                result.success &&
                result.structured_json == "{\"ok\":true}",
            "MCP result queue preserves ownership and ordering");
    }

    {
        constexpr std::uint64_t command_count = 100'000;
        vcr::McpControlPlane control;
        std::atomic<bool> producer_failed{false};
        std::jthread producer([&]() {
            for (std::uint64_t token = 1;
                 token <= command_count;
                 ++token) {
                vcr::McpCommand command{
                    .token = token,
                    .type = vcr::McpCommandType::debug_get_state,
                };
                while (!control.submit(command)) {
                    std::this_thread::yield();
                }
                vcr::McpCommandResult result{};
                while (!control.try_next_result(result)) {
                    std::this_thread::yield();
                }
                if (!result.success || result.token != token) {
                    producer_failed.store(
                        true, std::memory_order_relaxed);
                    return;
                }
            }
        });

        bool ordering_failed = false;
        for (std::uint64_t expected = 1;
             expected <= command_count;
             ++expected) {
            vcr::McpCommand command{};
            while (!control.try_next_command(command)) {
                std::this_thread::yield();
            }
            if (command.token != expected) {
                ordering_failed = true;
            }
            vcr::McpCommandResult result{
                .token = command.token,
                .success = true,
                .message = "ok",
                .structured_json = "{\"ok\":true}",
            };
            while (!control.complete(std::move(result))) {
                std::this_thread::yield();
            }
        }
        producer.join();
        expect(
            !ordering_failed &&
                !producer_failed.load(std::memory_order_relaxed),
            "MCP SPSC queues preserve ordering under concurrent stress");
    }

    {
        constexpr std::array<std::uint8_t, 8> pixels{
            255U, 0U, 0U, 255U,
            0U, 255U, 0U, 255U,
        };
        const auto png = vcr::encode_png_rgba(pixels, 2, 1);
        expect(static_cast<bool>(png), "PNG encoder accepts RGBA8 pixels");
        constexpr std::array<std::uint8_t, 8> signature{
            0x89U, 0x50U, 0x4eU, 0x47U,
            0x0dU, 0x0aU, 0x1aU, 0x0aU,
        };
        expect(
            png.bytes.size() > 33U &&
                std::equal(
                    signature.begin(),
                    signature.end(),
                    png.bytes.begin()),
            "PNG signature");
        expect(
            png.bytes.size() > 24U &&
                png.bytes[12] == 'I' &&
                png.bytes[13] == 'H' &&
                png.bytes[14] == 'D' &&
                png.bytes[15] == 'R' &&
                png.bytes[19] == 2U &&
                png.bytes[23] == 1U &&
                png.bytes[24] == 8U &&
                png.bytes[25] == 6U,
            "PNG IHDR dimensions and RGBA color type");
        const auto invalid = vcr::encode_png_rgba(pixels, 0, 1);
        expect(
            !static_cast<bool>(invalid),
            "PNG encoder rejects zero dimensions");
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
