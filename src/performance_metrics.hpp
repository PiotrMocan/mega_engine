#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vcr {

inline constexpr double unavailable_timing_ms = -1.0;

struct FrameTimings {
    // Full wall-clock CPU duration of one renderer-loop iteration. FPS metrics
    // are derived from this value.
    double cpu_frame_ms = unavailable_timing_ms;
    double command_record_ms = unavailable_timing_ms;
    double fence_wait_ms = unavailable_timing_ms;
    double acquire_ms = unavailable_timing_ms;
    double queue_submit_ms = unavailable_timing_ms;
    double present_ms = unavailable_timing_ms;

    // GPU values may describe an earlier frame whose fence just completed.
    // Leave them negative while timestamp-query data is not yet available.
    double gpu_frame_ms = unavailable_timing_ms;
    double gpu_geometry_ms = unavailable_timing_ms;
    double gpu_workload_ms = unavailable_timing_ms;
};

struct PerformanceSnapshot {
    std::uint64_t frames_rendered = 0;
    std::uint64_t timed_frames = 0;
    std::size_t samples_in_window = 0;

    double instantaneous_fps = 0.0;
    double one_second_average_fps = 0.0;
    double average_fps = 0.0;
    double one_percent_low_fps = 0.0;

    FrameTimings latest{};
    FrameTimings average{};
};

// Single-writer metrics accumulator. record_frame() is O(1), noexcept, and
// performs no allocation. The bounded percentile calculation is deferred to
// snapshot(), which also performs no heap allocation.
class PerformanceMetrics {
public:
    static constexpr std::size_t frame_window_capacity = 2000;

    PerformanceMetrics() noexcept = default;

    void reset() noexcept;
    void record_frame(const FrameTimings& timings) noexcept;

    [[nodiscard]] PerformanceSnapshot snapshot(
        bool include_percentile = true) const noexcept;
    [[nodiscard]] std::uint64_t frames_rendered() const noexcept {
        return frames_rendered_;
    }

private:
    enum class TimingIndex : std::size_t {
        cpu_frame,
        command_record,
        fence_wait,
        acquire,
        queue_submit,
        present,
        gpu_frame,
        gpu_geometry,
        gpu_workload,
        count,
    };

    static constexpr std::size_t timing_count =
        static_cast<std::size_t>(TimingIndex::count);

    void accumulate(TimingIndex index, double value) noexcept;
    [[nodiscard]] double average(TimingIndex index) const noexcept;

    std::array<double, frame_window_capacity> frame_times_ms_{};
    std::size_t window_size_ = 0;
    std::size_t next_sample_ = 0;

    std::array<double, timing_count> sums_{};
    std::array<std::uint64_t, timing_count> counts_{};
    FrameTimings latest_{};
    std::uint64_t frames_rendered_ = 0;
};

// Exact full-run percentile samples; allocation is confined to prepare().
// Overflow invalidates the result instead of silently dropping early hitches.
class BenchmarkFrameSamples final {
public:
    static constexpr std::size_t maximum_samples = 1'000'000;
    void prepare(std::size_t capacity = maximum_samples);
    void record(double frame_ms) noexcept;
    [[nodiscard]] bool complete() const noexcept { return !incomplete_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] double one_percent_low_fps();

private:
    std::vector<double> samples_{};
    std::size_t count_ = 0;
    bool incomplete_ = false;
};

enum class BenchmarkPhase : std::uint8_t {
    idle,
    warmup,
    measurement,
    complete,
};

struct BenchmarkTick {
    BenchmarkPhase phase = BenchmarkPhase::idle;
    bool measurement_began = false;
    bool completed = false;
    double phase_elapsed_seconds = 0.0;
    double measurement_elapsed_seconds = 0.0;
};

// The controller owns no renderer state. On measurement_began, reset the
// PerformanceMetrics instance; while phase==measurement, feed it samples; on
// completed, serialize its snapshot.
class BenchmarkController {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit BenchmarkController(
        double warmup_seconds = 5.0,
        double measurement_seconds = 15.0) noexcept;

    void configure(
        double warmup_seconds, double measurement_seconds) noexcept;
    void start(TimePoint now = Clock::now()) noexcept;
    [[nodiscard]] BenchmarkTick tick(TimePoint now = Clock::now()) noexcept;
    [[nodiscard]] BenchmarkTick update(TimePoint now = Clock::now()) noexcept {
        return tick(now);
    }

    [[nodiscard]] BenchmarkPhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] bool is_measuring() const noexcept {
        return phase_ == BenchmarkPhase::measurement;
    }
    [[nodiscard]] double warmup_seconds() const noexcept {
        return warmup_seconds_;
    }
    [[nodiscard]] double measurement_seconds() const noexcept {
        return measurement_seconds_;
    }

private:
    double warmup_seconds_ = 5.0;
    double measurement_seconds_ = 15.0;
    TimePoint start_time_{};
    BenchmarkPhase phase_ = BenchmarkPhase::idle;
    bool measurement_begin_pending_ = false;
    bool completion_pending_ = false;
};

struct BenchmarkResult {
    std::string gpu;
    std::string driver;
    std::string resolution;
    std::string render_resolution;
    std::string present_mode;
    std::string anti_aliasing;
    std::string anti_aliasing_quality;
    std::string mode;
    std::string build_type;
    std::uint64_t triangles = 0;
    std::uint32_t instances = 0;
    std::uint32_t gpu_workload_iterations = 0;
    double average_fps = 0.0;
    double one_percent_low_fps = 0.0;
    double average_cpu_frame_ms = 0.0;
    double average_gpu_frame_ms = 0.0;
    double average_fence_wait_ms = 0.0;
    double average_acquire_ms = unavailable_timing_ms;
    double average_command_record_ms = unavailable_timing_ms;
    double average_queue_submit_ms = unavailable_timing_ms;
    double average_present_ms = unavailable_timing_ms;
    double average_gpu_geometry_ms = unavailable_timing_ms;
    double average_gpu_workload_ms = unavailable_timing_ms;
    double measurement_seconds = 0.0;
    std::uint64_t percentile_samples = 0;
    std::uint64_t frames_rendered = 0;
};

[[nodiscard]] BenchmarkResult make_benchmark_result(
    std::string gpu,
    std::string driver,
    std::string resolution,
    std::string render_resolution,
    std::string present_mode,
    std::string anti_aliasing,
    std::string anti_aliasing_quality,
    std::uint32_t instances,
    std::uint32_t gpu_workload_iterations,
    const PerformanceSnapshot& snapshot);

// Serialization is intentionally outside the render loop and uses no JSON
// dependency. Strings are escaped and non-finite measurements become 0.
[[nodiscard]] std::string benchmark_result_to_json(
    const BenchmarkResult& result);
[[nodiscard]] inline std::string to_json(const BenchmarkResult& result) {
    return benchmark_result_to_json(result);
}

[[nodiscard]] bool write_benchmark_json(
    const std::filesystem::path& path,
    const BenchmarkResult& result,
    std::string* error = nullptr) noexcept;

} // namespace vcr
