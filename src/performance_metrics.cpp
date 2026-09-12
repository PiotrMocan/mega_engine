#include "performance_metrics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <stdexcept>

namespace vcr {
namespace {

[[nodiscard]] bool is_available(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] double seconds_between(
    BenchmarkController::TimePoint first,
    BenchmarkController::TimePoint second) noexcept {
    return std::chrono::duration<double>(second - first).count();
}

[[nodiscard]] double clamp_elapsed(double value, double maximum) noexcept {
    return std::clamp(value, 0.0, maximum);
}

void append_json_string(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (character < 0x20) {
                output.append("\\u00");
                output.push_back(hex[(character >> 4U) & 0x0fU]);
                output.push_back(hex[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_json_number(std::string& output, double value) {
    if (!std::isfinite(value) || value < 0.0) {
        value = 0.0;
    }

    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (error == std::errc{}) {
        output.append(buffer.data(), end);
    } else {
        output.push_back('0');
    }
}

void append_json_u64(std::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) {
        output.append(buffer.data(), end);
    } else {
        output.push_back('0');
    }
}

void append_key(std::string& output, std::string_view key) {
    output.append("  ");
    append_json_string(output, key);
    output.append(": ");
}

} // namespace

void PerformanceMetrics::reset() noexcept {
    window_size_ = 0;
    next_sample_ = 0;
    sums_.fill(0.0);
    counts_.fill(0);
    latest_ = {};
    frames_rendered_ = 0;
}

void PerformanceMetrics::accumulate(
    TimingIndex index, double value) noexcept {
    if (!is_available(value) ||
        (index == TimingIndex::cpu_frame && value == 0.0)) {
        return;
    }
    const std::size_t position = static_cast<std::size_t>(index);
    sums_[position] += value;
    ++counts_[position];
}

double PerformanceMetrics::average(TimingIndex index) const noexcept {
    const std::size_t position = static_cast<std::size_t>(index);
    return counts_[position] == 0
        ? unavailable_timing_ms
        : sums_[position] / static_cast<double>(counts_[position]);
}

void PerformanceMetrics::record_frame(const FrameTimings& timings) noexcept {
    ++frames_rendered_;
    latest_ = timings;

    accumulate(TimingIndex::cpu_frame, timings.cpu_frame_ms);
    accumulate(TimingIndex::command_record, timings.command_record_ms);
    accumulate(TimingIndex::fence_wait, timings.fence_wait_ms);
    accumulate(TimingIndex::acquire, timings.acquire_ms);
    accumulate(TimingIndex::queue_submit, timings.queue_submit_ms);
    accumulate(TimingIndex::present, timings.present_ms);
    accumulate(TimingIndex::gpu_frame, timings.gpu_frame_ms);
    accumulate(TimingIndex::gpu_geometry, timings.gpu_geometry_ms);
    accumulate(TimingIndex::gpu_workload, timings.gpu_workload_ms);
    accumulate(TimingIndex::gpu_shadow, timings.gpu_shadow_ms);
    accumulate(TimingIndex::gpu_voxel_geometry, timings.gpu_voxel_geometry_ms);
    accumulate(TimingIndex::gpu_voxel_light, timings.gpu_voxel_light_ms);
    accumulate(TimingIndex::gpu_visibility, timings.gpu_visibility_ms);
    accumulate(TimingIndex::scene_prepare, timings.scene_prepare_ms);

    if (std::isfinite(timings.cpu_frame_ms) &&
        timings.cpu_frame_ms > 0.0) {
        frame_times_ms_[next_sample_] = timings.cpu_frame_ms;
        next_sample_ = (next_sample_ + 1) % frame_window_capacity;
        window_size_ = std::min(
            window_size_ + std::size_t{1}, frame_window_capacity);
    }
}

PerformanceSnapshot PerformanceMetrics::snapshot(const bool include_percentile) const noexcept {
    PerformanceSnapshot result{};
    result.frames_rendered = frames_rendered_;
    result.timed_frames =
        counts_[static_cast<std::size_t>(TimingIndex::cpu_frame)];
    result.samples_in_window = window_size_;
    result.latest = latest_;

    result.average.cpu_frame_ms = average(TimingIndex::cpu_frame);
    result.average.command_record_ms = average(TimingIndex::command_record);
    result.average.fence_wait_ms = average(TimingIndex::fence_wait);
    result.average.acquire_ms = average(TimingIndex::acquire);
    result.average.queue_submit_ms = average(TimingIndex::queue_submit);
    result.average.present_ms = average(TimingIndex::present);
    result.average.gpu_frame_ms = average(TimingIndex::gpu_frame);
    result.average.gpu_geometry_ms = average(TimingIndex::gpu_geometry);
    result.average.gpu_workload_ms = average(TimingIndex::gpu_workload);
    result.average.gpu_shadow_ms = average(TimingIndex::gpu_shadow);
    result.average.gpu_voxel_geometry_ms = average(TimingIndex::gpu_voxel_geometry);
    result.average.gpu_voxel_light_ms = average(TimingIndex::gpu_voxel_light);
    result.average.gpu_visibility_ms = average(TimingIndex::gpu_visibility);
    result.average.scene_prepare_ms = average(TimingIndex::scene_prepare);

    if (window_size_ == 0) {
        return result;
    }

    const std::size_t newest =
        (next_sample_ + frame_window_capacity - 1) % frame_window_capacity;
    const double latest_frame_ms = frame_times_ms_[newest];
    result.instantaneous_fps =
        latest_frame_ms > 0.0 ? 1000.0 / latest_frame_ms : 0.0;

    double one_second_ms = 0.0;
    std::size_t one_second_frames = 0;
    for (std::size_t offset = 0; offset < window_size_; ++offset) {
        const std::size_t sample =
            (newest + frame_window_capacity - offset) %
            frame_window_capacity;
        one_second_ms += frame_times_ms_[sample];
        ++one_second_frames;
        if (one_second_ms >= 1000.0) {
            break;
        }
    }
    if (one_second_ms > 0.0) {
        result.one_second_average_fps =
            static_cast<double>(one_second_frames) * 1000.0 / one_second_ms;
    }

    const double total_frame_ms =
        sums_[static_cast<std::size_t>(TimingIndex::cpu_frame)];
    if (total_frame_ms > 0.0) {
        result.average_fps =
            static_cast<double>(result.timed_frames) * 1000.0 /
            total_frame_ms;
    }

    if (!include_percentile) {
        return result;
    }

    // Common real-time convention: 1% low FPS is the reciprocal of the mean
    // duration of the slowest 1% of frames in the bounded rolling window.
    const std::size_t slow_frame_count =
        std::max<std::size_t>(1, (window_size_ + 99) / 100);

    // At capacity, one percent is at most twenty values. Keeping only those
    // values avoids a temporary 2000-element array and makes the no-allocation
    // behavior independent of the standard library's selection algorithm.
    constexpr std::size_t maximum_slow_frames =
        (frame_window_capacity + 99) / 100;
    std::array<double, maximum_slow_frames> slowest_frame_times{};
    std::size_t retained_slow_frames = 0;
    for (std::size_t index = 0; index < window_size_; ++index) {
        const double candidate = frame_times_ms_[index];
        if (retained_slow_frames < slow_frame_count) {
            slowest_frame_times[retained_slow_frames++] = candidate;
            continue;
        }

        std::size_t fastest_retained = 0;
        for (std::size_t retained = 1;
             retained < retained_slow_frames;
             ++retained) {
            if (slowest_frame_times[retained] <
                slowest_frame_times[fastest_retained]) {
                fastest_retained = retained;
            }
        }
        if (candidate > slowest_frame_times[fastest_retained]) {
            slowest_frame_times[fastest_retained] = candidate;
        }
    }

    double slow_frame_total_ms = 0.0;
    for (std::size_t index = 0; index < slow_frame_count; ++index) {
        slow_frame_total_ms += slowest_frame_times[index];
    }
    if (slow_frame_total_ms > 0.0) {
        result.one_percent_low_fps =
            static_cast<double>(slow_frame_count) * 1000.0 /
            slow_frame_total_ms;
    }

    return result;
}

void BenchmarkFrameSamples::prepare(const std::size_t capacity) {
    if (capacity == 0U || capacity > maximum_samples) {
        throw std::invalid_argument("Invalid benchmark sample capacity.");
    }
    samples_.resize(capacity);
    count_ = 0;
    incomplete_ = false;
}

void BenchmarkFrameSamples::record(const double frame_ms) noexcept {
    if (!std::isfinite(frame_ms) || frame_ms <= 0.0 ||
        count_ >= samples_.size()) {
        incomplete_ = true;
        return;
    }
    samples_[count_++] = frame_ms;
}

double BenchmarkFrameSamples::one_percent_low_fps() {
    if (incomplete_ || count_ == 0U) {
        return 0.0;
    }
    // Only invoked after measurement has stopped. No work is added to the
    // frame loop beyond one bounded append.
    std::sort(samples_.begin(), samples_.begin() + count_, std::greater<double>{});
    const std::size_t count = (count_ + 99U) / 100U;
    double total = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        total += samples_[index];
    }
    return static_cast<double>(count) * 1000.0 / total;
}

BenchmarkController::BenchmarkController(
    double warmup_seconds, double measurement_seconds) noexcept {
    configure(warmup_seconds, measurement_seconds);
}

void BenchmarkController::configure(
    double warmup_seconds, double measurement_seconds) noexcept {
    warmup_seconds_ =
        std::isfinite(warmup_seconds) && warmup_seconds >= 0.0
        ? warmup_seconds
        : 0.0;
    measurement_seconds_ =
        std::isfinite(measurement_seconds) && measurement_seconds > 0.0
        ? measurement_seconds
        : 15.0;
    phase_ = BenchmarkPhase::idle;
    measurement_begin_pending_ = false;
    completion_pending_ = false;
}

void BenchmarkController::start(TimePoint now) noexcept {
    start_time_ = now;
    phase_ = warmup_seconds_ > 0.0
        ? BenchmarkPhase::warmup
        : BenchmarkPhase::measurement;
    measurement_begin_pending_ =
        phase_ == BenchmarkPhase::measurement;
    completion_pending_ = false;
}

BenchmarkTick BenchmarkController::tick(TimePoint now) noexcept {
    BenchmarkTick result{};

    if (phase_ == BenchmarkPhase::idle) {
        return result;
    }

    const double total_elapsed =
        std::max(0.0, seconds_between(start_time_, now));

    if (phase_ == BenchmarkPhase::warmup &&
        total_elapsed >= warmup_seconds_) {
        phase_ = BenchmarkPhase::measurement;
        measurement_begin_pending_ = true;
    }

    if (phase_ == BenchmarkPhase::measurement &&
        total_elapsed >= warmup_seconds_ + measurement_seconds_) {
        phase_ = BenchmarkPhase::complete;
        completion_pending_ = true;
    }

    result.phase = phase_;
    result.measurement_began = measurement_begin_pending_;
    result.completed = completion_pending_;
    measurement_begin_pending_ = false;
    completion_pending_ = false;

    switch (phase_) {
    case BenchmarkPhase::idle:
        break;
    case BenchmarkPhase::warmup:
        result.phase_elapsed_seconds =
            clamp_elapsed(total_elapsed, warmup_seconds_);
        break;
    case BenchmarkPhase::measurement:
        result.phase_elapsed_seconds = clamp_elapsed(
            total_elapsed - warmup_seconds_, measurement_seconds_);
        result.measurement_elapsed_seconds = result.phase_elapsed_seconds;
        break;
    case BenchmarkPhase::complete:
        result.phase_elapsed_seconds = measurement_seconds_;
        result.measurement_elapsed_seconds = measurement_seconds_;
        break;
    }
    return result;
}

BenchmarkResult make_benchmark_result(
    std::string gpu,
    std::string driver,
    std::string resolution,
    std::string render_resolution,
    std::string present_mode,
    std::string anti_aliasing,
    std::string anti_aliasing_quality,
    std::uint32_t instances,
    std::uint32_t gpu_workload_iterations,
    const PerformanceSnapshot& snapshot) {
    BenchmarkResult result{};
    result.gpu = std::move(gpu);
    result.driver = std::move(driver);
    result.resolution = std::move(resolution);
    result.render_resolution = std::move(render_resolution);
    result.present_mode = std::move(present_mode);
    result.anti_aliasing = std::move(anti_aliasing);
    result.anti_aliasing_quality = std::move(anti_aliasing_quality);
    result.instances = instances;
    result.gpu_workload_iterations = gpu_workload_iterations;
    result.average_fps = snapshot.average_fps;
    result.one_percent_low_fps = snapshot.one_percent_low_fps;
    result.average_cpu_frame_ms =
        is_available(snapshot.average.cpu_frame_ms)
        ? snapshot.average.cpu_frame_ms
        : 0.0;
    result.average_gpu_frame_ms =
        is_available(snapshot.average.gpu_frame_ms)
        ? snapshot.average.gpu_frame_ms
        : unavailable_timing_ms;
    result.average_fence_wait_ms =
        is_available(snapshot.average.fence_wait_ms)
        ? snapshot.average.fence_wait_ms
        : 0.0;
    result.frames_rendered = snapshot.frames_rendered;
    result.average_acquire_ms = snapshot.average.acquire_ms;
    result.average_command_record_ms = snapshot.average.command_record_ms;
    result.average_queue_submit_ms = snapshot.average.queue_submit_ms;
    result.average_present_ms = snapshot.average.present_ms;
    result.average_gpu_geometry_ms = snapshot.average.gpu_geometry_ms;
    result.average_gpu_workload_ms = snapshot.average.gpu_workload_ms;
    result.average_gpu_shadow_ms = snapshot.average.gpu_shadow_ms;
    result.average_gpu_voxel_geometry_ms = snapshot.average.gpu_voxel_geometry_ms;
    result.average_gpu_voxel_light_ms = snapshot.average.gpu_voxel_light_ms;
    result.average_gpu_visibility_ms = snapshot.average.gpu_visibility_ms;
    result.average_scene_prepare_ms = snapshot.average.scene_prepare_ms;
    return result;
}

std::string benchmark_result_to_json(const BenchmarkResult& result) {
    std::string output;
    output.reserve(
        544 + result.gpu.size() + result.driver.size() +
        result.resolution.size() + result.render_resolution.size() +
        result.present_mode.size() + result.anti_aliasing.size() +
        result.anti_aliasing_quality.size());
    output.append("{\n");

    const auto timing = [&output](const char* key, const double value) {
        append_key(output, key);
        if (is_available(value)) {
            append_json_number(output, value);
        } else {
            output.append("null");
        }
        output.append(",\n");
    };
    append_key(output, "schema_version");
    append_json_u64(output, 2U);
    output.append(",\n");
    append_key(output, "mode");
    append_json_string(output, result.mode);
    output.append(",\n");
    append_key(output, "build_type");
    append_json_string(output, result.build_type);
    output.append(",\n");
    append_key(output, "triangles");
    append_json_u64(output, result.triangles);
    output.append(",\n");
    append_key(output, "measurement_seconds");
    append_json_number(output, result.measurement_seconds);
    output.append(",\n");
    append_key(output, "percentile_samples");
    append_json_u64(output, result.percentile_samples);
    output.append(",\n");
    append_key(output, "percentile_scope");
    append_json_string(output, result.percentile_samples > 0U
                                  ? "full_measurement" : "rolling_window");
    output.append(",\n");
    timing("average_acquire_ms", result.average_acquire_ms);
    timing("average_command_record_ms", result.average_command_record_ms);
    timing("average_queue_submit_ms", result.average_queue_submit_ms);
    timing("average_present_ms", result.average_present_ms);
    timing("average_gpu_geometry_ms", result.average_gpu_geometry_ms);
    timing("average_gpu_workload_ms", result.average_gpu_workload_ms);
    timing("average_gpu_shadow_ms", result.average_gpu_shadow_ms);
    timing("average_gpu_voxel_geometry_ms", result.average_gpu_voxel_geometry_ms);
    timing("average_gpu_voxel_light_ms", result.average_gpu_voxel_light_ms);
    timing("average_gpu_visibility_ms", result.average_gpu_visibility_ms);
    append_key(output, "visibility_backend");
    append_json_string(output, result.visibility_backend);
    output += ",\n";
    timing("average_scene_prepare_ms", result.average_scene_prepare_ms);
    const auto integer = [&](std::string_view key, std::uint64_t value) {
        append_key(output, key);
        append_json_u64(output, value);
        output += ",\n";
    };
    integer("visible_instances", result.visible_instances);
    integer("source_instances", result.source_instances);
    integer("scene_count", result.scene_count);
    integer("terrain_lod", result.terrain_lod);
    timing("camera_z", result.camera_z);
    timing("scene_scale", result.scene_scale);
    append_key(output, "scene_asset");
    append_json_string(output, result.scene_asset);
    output += ",\n";
    integer("frustum_rejected", result.frustum_rejected);
    integer("occlusion_rejected", result.occlusion_rejected);
    integer("lod0_instances", result.lod_instances[0]);
    integer("lod1_instances", result.lod_instances[1]);
    integer("lod2_instances", result.lod_instances[2]);
    const auto boolean = [&](std::string_view key, bool value) {
        append_key(output, key);
        output += value ? "true,\n" : "false,\n";
    };
    boolean("frustum_culling", result.frustum_culling);
    boolean("occlusion_culling", result.occlusion_culling);
    boolean("lod", result.lod);
    boolean("dynamic_shadows", result.dynamic_shadows);
    boolean("animated", result.animated);
    boolean("animated_camera", result.animated_camera);
    boolean("scene_walls", result.scene_walls);
    boolean("dynamic_lighting", result.lighting.dynamic);
    boolean("voxel_gi", result.lighting.voxel_gi);
    boolean("day_cycle", result.lighting.cycle);
    boolean("lighting_room", result.lighting_room);
    timing("time_of_day_end", result.lighting.hour);
    timing("day_length_seconds", result.lighting.day_seconds);
    timing("gi_extent", result.lighting.gi_extent);
    timing("gi_strength", result.lighting.gi_strength);
    timing("gi_update_hz", result.lighting.gi_update_hz);
    integer("gi_triangle_budget", result.lighting.triangle_budget);
    integer("gi_geometry_updates_total", result.lighting_statistics.geometry_updates);
    integer("gi_radiance_updates_total", result.lighting_statistics.radiance_updates);
    integer("gi_pending_bricks", result.lighting_statistics.pending_bricks);
    timing("gi_oldest_geometry_ms", result.lighting_statistics.oldest_geometry_ms);
    integer("gi_mesh_bytes", result.lighting_statistics.mesh_bytes);

    append_key(output, "gpu");
    append_json_string(output, result.gpu);
    output.append(",\n");
    append_key(output, "driver");
    append_json_string(output, result.driver);
    output.append(",\n");
    append_key(output, "resolution");
    append_json_string(output, result.resolution);
    output.append(",\n");
    append_key(output, "render_resolution");
    append_json_string(output, result.render_resolution);
    output.append(",\n");
    append_key(output, "present_mode");
    append_json_string(output, result.present_mode);
    output.append(",\n");
    append_key(output, "anti_aliasing");
    append_json_string(output, result.anti_aliasing);
    output.append(",\n");
    append_key(output, "anti_aliasing_quality");
    append_json_string(output, result.anti_aliasing_quality);
    output.append(",\n");
    append_key(output, "instances");
    append_json_u64(output, result.instances);
    output.append(",\n");
    append_key(output, "gpu_workload_iterations");
    append_json_u64(output, result.gpu_workload_iterations);
    output.append(",\n");
    append_key(output, "average_fps");
    append_json_number(output, result.average_fps);
    output.append(",\n");
    append_key(output, "one_percent_low_fps");
    append_json_number(output, result.one_percent_low_fps);
    output.append(",\n");
    append_key(output, "average_cpu_frame_ms");
    append_json_number(output, result.average_cpu_frame_ms);
    output.append(",\n");
    timing("average_gpu_frame_ms", result.average_gpu_frame_ms);
    append_key(output, "average_fence_wait_ms");
    append_json_number(output, result.average_fence_wait_ms);
    output.append(",\n");
    append_key(output, "frames_rendered");
    append_json_u64(output, result.frames_rendered);
    output.append("\n}\n");
    return output;
}

bool write_benchmark_json(
    const std::filesystem::path& path,
    const BenchmarkResult& result,
    std::string* error) noexcept {
    if (error != nullptr) {
        error->clear();
    }

    try {
        const std::string json = benchmark_result_to_json(result);
        std::ofstream output{
            path, std::ios::binary | std::ios::out | std::ios::trunc};
        if (!output) {
            if (error != nullptr) {
                *error = "failed to open benchmark output: ";
                error->append(path.string());
            }
            return false;
        }

        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.flush();
        if (!output) {
            if (error != nullptr) {
                *error = "failed to write benchmark output: ";
                error->append(path.string());
            }
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        try {
            if (error != nullptr) {
                *error = "failed to write benchmark output: ";
                error->append(exception.what());
            }
        } catch (...) {
            // Reporting an allocation failure must not allocate again.
            if (error != nullptr) { error->clear(); }
        }
        return false;
    } catch (...) {
        try {
            if (error != nullptr) {
                *error = "failed to write benchmark output";
            }
        } catch (...) {
            if (error != nullptr) { error->clear(); }
        }
        return false;
    }
}

} // namespace vcr
