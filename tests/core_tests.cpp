#include "cli_options.hpp"
#include "fly_camera.hpp"
#include "performance_metrics.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

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
        expect(!parsed.options.validation, "no-validation flag");
    }

    {
        const char* arguments[]{"vulkan_cube", "--width", "0"};
        const auto parsed = vcr::parse_cli_options(
            static_cast<int>(std::size(arguments)), arguments);
        expect(!static_cast<bool>(parsed), "zero width is rejected");
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
        result.frames_rendered = 42;
        const std::string json = vcr::benchmark_result_to_json(result);
        expect(
            json.find("GPU \\\"quoted\\\"") != std::string::npos,
            "JSON string escaping");
        expect(
            json.find("\"frames_rendered\": 42") != std::string::npos,
            "JSON integer field");
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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
