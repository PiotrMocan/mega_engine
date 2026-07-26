#include "cli_options.hpp"
#include "performance_metrics.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iterator>
#include <string>

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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
