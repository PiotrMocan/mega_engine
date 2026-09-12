#include "asset_browser.hpp"
#include "editor_scene_tools.hpp"
#include "cli_options.hpp"
#include "fly_camera.hpp"
#include "mcp_control.hpp"
#include "performance_metrics.hpp"
#include "png_encoder.hpp"
#include "scene_document.hpp"
#include "scene_io.hpp"
#include "temporal_coordinates.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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
        vcr::BenchmarkFrameSamples samples;
        samples.prepare(10'000U);
        for (std::size_t index = 0; index < 10'000U; ++index) {
            samples.record(index < 100U ? 100.0 : 1.0);
        }
        expect(samples.complete() && samples.size() == 10'000U,
               "full benchmark sample interval retained");
        expect(close_to(samples.one_percent_low_fps(), 10.0, 0.001),
               "early hitches survive past the rolling window");
        samples.record(1.0);
        expect(!samples.complete() && samples.size() == 10'000U &&
                   samples.one_percent_low_fps() == 0.0,
               "sample overflow invalidates instead of truncating percentile");
        samples.prepare(4U);
        samples.record(2.0);
        samples.record(4.0);
        expect(samples.complete() && samples.size() == 2U &&
                   close_to(samples.one_percent_low_fps(), 250.0, 0.001),
               "sample reset and sub-100-frame percentile");
        samples.record(std::numeric_limits<double>::quiet_NaN());
        expect(!samples.complete(), "invalid benchmark sample is rejected");
    }
    {
        constexpr std::uint32_t width = 1920U;
        constexpr std::uint32_t height = 1080U;
        const auto scale = vcr::ndc_motion_to_pixel_scale(width, height);
        // A quarter-width and half-height move in pixels corresponds to
        // NDC deltas 0.5 and 1.0, not UV deltas of those sizes.
        expect(close_to(0.5F * scale.x, 480.0, 0.001) &&
                   close_to(1.0F * scale.y, 540.0, 0.001),
               "FSR motion vectors convert from NDC to pixels");
        const vcr::TemporalOffset jitter{0.25F, -0.375F};
        auto projection = glm::perspective(
            glm::radians(65.0F), float(width) / float(height), 0.1F, 500.0F);
        projection[1][1] *= -1.0F;
        const glm::vec4 point{0.7F, 0.2F, -5.0F, 1.0F};
        const glm::vec4 before = projection * point;
        const auto offset = vcr::vulkan_projection_jitter(jitter, width, height);
        projection[2][0] += offset.x;
        projection[2][1] += offset.y;
        const glm::vec4 after = projection * point;
        const auto expected_pixels = vcr::vulkan_pixel_jitter(jitter);
        expect(close_to((after.x / after.w - before.x / before.w) * scale.x,
                        expected_pixels.x, 0.0001) &&
                   close_to((after.y / after.w - before.y / before.w) * scale.y,
                            expected_pixels.y, 0.0001),
               "FSR jitter matches actual Vulkan viewport pixel displacement");
    }
    {
        {
            const char *lighting_args[]{"vcr",   "--lighting",    "dynamic", "--gi",
                                        "voxel", "--time-of-day", "0",       "--day-cycle",
                                        "off",   "--day-length",  "120",     "--gi-triangle-budget",
                                        "1024"};
            const auto parsed =
                vcr::parse_cli_options(static_cast<int>(std::size(lighting_args)), lighting_args);
            expect(static_cast<bool>(parsed) && parsed.options.lighting.hour == 0 &&
                       !parsed.options.lighting.cycle &&
                       parsed.options.lighting.triangle_budget == 1024,
                   "dynamic lighting CLI controls parse");
            for (const char *bad :
                 {"24", "-1", "nan", "inf", "9999999999999999999999999999999999999"}) {
                const char *args[]{"vcr", "--time-of-day", bad};
                expect(!vcr::parse_cli_options(3, args), "invalid day hour rejected");
            }
            for (const char *bad : {"1023", "262145", "1024.5", "nan", "-1"}) {
                const char *args[]{"vcr", "--gi-triangle-budget", bad};
                expect(!vcr::parse_cli_options(3, args),
                       "invalid geometry budget rejected before cast");
            }
            for (const char *size : {"1024", "2048", "4096", "8192"}) {
                const char *args[]{"vcr", "--shadow-resolution", size};
                const auto shadow = vcr::parse_cli_options(3, args);
                expect(static_cast<bool>(shadow) &&
                           shadow.options.lighting.shadow_resolution == std::strtoul(size, nullptr, 10),
                       "shadow atlas resolution parses without losing the requested size");
            }
            for (const char *bad : {"0", "512", "1023", "2049", "16384", "2048.5",
                                    "-1", "nan", "4294967296"}) {
                const char *args[]{"vcr", "--shadow-resolution", bad};
                expect(!vcr::parse_cli_options(3, args), "invalid shadow resolution rejected");
            }
        }
        const char *arguments[]{"vulkan_cube"};
        const auto parsed =
            vcr::parse_cli_options(static_cast<int>(std::size(arguments)), arguments);
        expect(
            static_cast<bool>(parsed) &&
                parsed.options.width == 1920 &&
                parsed.options.height == 1080,
            "default drawable resolution is Full HD");
    }

    {
        vcr::PbrMaterial material{
            .base_color = glm::vec4(
                -1.0F,
                0.4F,
                2.0F,
                std::numeric_limits<float>::quiet_NaN()),
            .metallic = 4.0F,
            .roughness = 0.0F,
            .ambient_occlusion = -2.0F,
            .reserved = 7.0F,
        };
        material = vcr::sanitize_pbr_material(material);
        expect(
            material.base_color ==
                    glm::vec4(0.0F, 0.4F, 1.0F, 1.0F) &&
                material.metallic == 1.0F &&
                material.roughness ==
                    vcr::minimum_pbr_roughness &&
                material.ambient_occlusion == 0.0F &&
                material.reserved == 0.0F,
            "PBR material factors are finite and bounded");
    }

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
        result.mode = "scene";
        result.measurement_seconds = 15.0;
        result.percentile_samples = 42;
        result.average_gpu_workload_ms = vcr::unavailable_timing_ms;
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
        expect(json.find("\"percentile_scope\": \"full_measurement\"") != std::string::npos &&
                   json.find("\"average_gpu_workload_ms\": null") != std::string::npos &&
                   json.find("\"mode\": \"scene\"") != std::string::npos,
               "benchmark exposes sample scope, workload and unavailable timings");
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

    {
        expect(
            vcr::valid_scene_asset_path(
                "models/tree/tree.vcrasset.json") &&
                vcr::valid_scene_asset_path(
                    "import/"
                    "\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB5"
                    "\xD0\xB2\xD0\xBE.vcrasset.json"),
            "scene model asset paths accept bounded relative UTF-8 paths");
        std::string embedded_nul = "model.vcrasset.json";
        embedded_nul.push_back('\0');
        embedded_nul += "hidden";
        std::string invalid_utf8 = "model/";
        invalid_utf8.append("\xC0\xAF", 2U);
        invalid_utf8 += ".vcrasset.json";
        expect(
            !vcr::valid_scene_asset_path(
                "../model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "/model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "C:/model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "folder\\model.vcrasset.json") &&
                !vcr::valid_scene_asset_path(
                    "model.obj") &&
                !vcr::valid_scene_asset_path(embedded_nul) &&
                !vcr::valid_scene_asset_path(invalid_utf8),
            "scene model asset paths reject escape and malformed values");

        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        const vcr::SceneNodeId child = scene.create_node(
            "Child", vcr::SceneNodeKind::folder, root);
        const vcr::SceneNodeId cube = scene.create_node(
            "Cube", vcr::SceneNodeKind::cube, child);
        expect(
            scene.valid(root) && scene.valid(child) &&
                scene.valid(cube) && scene.size() == 3U,
            "scene creates stable hierarchical nodes");
        expect(
            !scene.reparent(root, cube),
            "scene rejects a hierarchy cycle");
        expect(
            !scene.create_node(
                std::string(
                    vcr::max_scene_node_name_bytes + 1U,
                    'x'),
                vcr::SceneNodeKind::folder),
            "scene rejects an unbounded node name");
        const std::uint64_t render_revision =
            scene.content_revision();
        const std::uint64_t hierarchy_revision =
            scene.structure_revision();
        expect(
            scene.rename_node(cube, "Renamed cube") &&
                scene.content_revision() == render_revision &&
                scene.structure_revision() ==
                    hierarchy_revision + 1U,
            "scene rename invalidates hierarchy without GPU content");

        std::vector<vcr::SceneHierarchyRow> rows;
        rows.reserve(16U);
        scene.build_hierarchy_rows(rows);
        expect(
            rows.size() == 3U &&
                rows[0].depth == 0U &&
                rows[1].depth == 1U &&
                rows[2].depth == 2U,
            "scene hierarchy cache is iterative preorder");

        expect(
            scene.destroy_subtree(child) &&
                !scene.valid(child) &&
                !scene.valid(cube) &&
                scene.valid(root) &&
                scene.size() == 1U,
            "scene subtree deletion invalidates stale handles");
        const vcr::SceneNodeId reused = scene.create_node(
            "Reused", vcr::SceneNodeKind::cube, root);
        expect(
            reused.index == child.index &&
                reused.generation != child.generation &&
                !scene.valid(child),
            "scene slot reuse increments generation");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        static_cast<void>(scene.create_node(
            "Terrain", vcr::SceneNodeKind::terrain, root));
        static_cast<void>(scene.create_node(
            "Sky", vcr::SceneNodeKind::environment, root));
        static_cast<void>(scene.create_node(
            "Sun", vcr::SceneNodeKind::directional_light, root));
        static_cast<void>(scene.create_node(
            "Cube", vcr::SceneNodeKind::cube, root));
        const vcr::SceneNodeId model = scene.create_node(
            "Model", vcr::SceneNodeKind::model, root);
        if (auto model_node = scene.node(model)) {
            model_node->get().asset_path =
                "models/tree/tree.vcrasset.json";
            model_node->get().transform.translation =
                glm::vec3(2.0F, 3.0F, -4.0F);
            scene.mark_content_changed();
        }
        std::vector<vcr::SceneGpuInstance> instances;
        std::vector<vcr::SceneModelInstance> models;
        const vcr::SceneRenderView visible =
            scene.build_render_view(instances, models);
        expect(
            visible.cubes.size() == 1U &&
                visible.models.size() == 1U &&
                visible.models[0].asset_path ==
                    "models/tree/tree.vcrasset.json" &&
                visible.models[0].model_row_0.w == 2.0F &&
                visible.models[0].model_row_1.w == 3.0F &&
                visible.models[0].model_row_2.w == -4.0F &&
                visible.terrain_visible &&
                visible.sky_visible &&
                visible.sun_visible,
            "scene render view exposes visible component flags");
        if (auto root_node = scene.node(root)) {
            root_node->get().visible = false;
            scene.mark_content_changed();
        }
        const vcr::SceneRenderView hidden =
            scene.build_render_view(instances, models);
        expect(
            hidden.cubes.empty() &&
                hidden.models.empty() &&
                !hidden.terrain_visible &&
                !hidden.sky_visible &&
                !hidden.sun_visible,
            "hidden parent suppresses renderable descendants");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId parent = scene.create_node(
            "Scaled parent", vcr::SceneNodeKind::folder);
        const vcr::SceneNodeId cube = scene.create_node(
            "Rotated child", vcr::SceneNodeKind::cube, parent);
        auto parent_node = scene.node(parent);
        auto cube_node = scene.node(cube);
        expect(
            parent_node.has_value() && cube_node.has_value(),
            "scene transform test nodes exist");
        if (parent_node && cube_node) {
            parent_node->get().transform.translation =
                glm::vec3(3.0F, -2.0F, 5.0F);
            parent_node->get().transform.rotation_degrees =
                glm::vec3(12.0F, 31.0F, -7.0F);
            parent_node->get().transform.scale =
                glm::vec3(2.0F, 3.0F, 0.5F);
            cube_node->get().transform.translation =
                glm::vec3(-4.0F, 1.0F, 2.0F);
            cube_node->get().transform.rotation_degrees =
                glm::vec3(27.0F, -19.0F, 43.0F);
            cube_node->get().transform.scale =
                glm::vec3(0.75F, 1.25F, 1.5F);
            scene.mark_content_changed();
        }

        std::vector<vcr::SceneGpuInstance> instances;
        std::vector<vcr::SceneModelInstance> models;
        instances.reserve(1U);
        const vcr::SceneRenderView snapshot =
            scene.build_render_view(instances, models);
        expect(
            snapshot.cubes.size() == 1U,
            "scene transform emits one cube");
        if (snapshot.cubes.size() == 1U &&
            parent_node && cube_node) {
            const auto local = [](const vcr::SceneTransform& value) {
                return glm::translate(
                           glm::mat4(1.0F),
                           value.translation) *
                       glm::mat4_cast(glm::quat(glm::radians(
                           value.rotation_degrees))) *
                       glm::scale(
                           glm::mat4(1.0F), value.scale);
            };
            const glm::mat4 expected =
                local(parent_node->get().transform) *
                local(cube_node->get().transform);
            const vcr::SceneGpuInstance& packed =
                snapshot.cubes.front();
            const std::array rows{
                packed.model_row_0,
                packed.model_row_1,
                packed.model_row_2,
                glm::vec4(0.0F, 0.0F, 0.0F, 1.0F),
            };
            bool matches = true;
            for (glm::length_t column = 0;
                 column < 4;
                 ++column) {
                for (glm::length_t row = 0;
                     row < 4;
                     ++row) {
                    matches =
                        matches &&
                        close_to(
                            rows[static_cast<std::size_t>(row)]
                                [column],
                            expected[column][row],
                            0.0001);
                }
            }
            expect(
                matches,
                "hierarchical affine transform preserves shear");
        }
    }

    {
        vcr::SceneDocument scene;
        vcr::SceneNodeId parent = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        constexpr std::uint32_t depth = 10'000U;
        for (std::uint32_t index = 1U; index < depth; ++index) {
            parent = scene.create_node(
                "Node", vcr::SceneNodeKind::folder, parent);
        }
        std::vector<vcr::SceneHierarchyRow> rows;
        rows.reserve(depth);
        scene.build_hierarchy_rows(rows);
        expect(
            rows.size() == depth &&
                rows.back().depth == depth - 1U,
            "deep hierarchy traversal does not recurse");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Root", vcr::SceneNodeKind::folder);
        for (std::uint32_t index = 1U;
             index < vcr::max_scene_nodes;
             ++index) {
            const vcr::SceneNodeId node = scene.create_node(
                "Node", vcr::SceneNodeKind::folder, root);
            if (!node) {
                break;
            }
        }
        expect(
            scene.size() == vcr::max_scene_nodes &&
                !scene.create_node(
                    "Overflow",
                    vcr::SceneNodeKind::folder),
            "scene node capacity is strictly bounded");
        expect(
            scene.destroy_subtree(root) &&
                scene.size() == 0U,
            "maximum-size subtree destruction is iterative");
    }

    {
        vcr::SceneDocument scene;
        const vcr::SceneNodeId root = scene.create_node(
            "Landscape", vcr::SceneNodeKind::folder);
        std::string error;
        const vcr::SceneNodeId ground = vcr::create_flat_ground(
            scene,
            vcr::FlatGroundOptions{
                .name = "Playable Ground",
                .parent = root,
                .center_x = 4.0F,
                .height = -2.0F,
                .center_z = -6.0F,
                .width = 80.0F,
                .depth = 40.0F,
            },
            error);
        const auto ground_node = scene.node(ground);
        expect(
            ground && error.empty() && ground_node &&
                ground_node->get().kind == vcr::SceneNodeKind::ground &&
                close_to(ground_node->get().transform.translation.y, -2.0, 0.0001) &&
                close_to(ground_node->get().transform.scale.x, 40.0, 0.0001) &&
                close_to(ground_node->get().transform.scale.z, 20.0, 0.0001),
            "flat terrain tool creates an explicitly sized ground plane");

        vcr::ModelScatterOptions scatter{
            .asset_path = "plants/fern/fern.vcrasset.json",
            .name_prefix = "Fern",
            .parent = root,
            .base_transform = vcr::SceneTransform{
                .translation = glm::vec3(0.25F, 0.5F, -0.25F),
                .rotation_degrees = glm::vec3(0.0F, 15.0F, 0.0F),
                .scale = glm::vec3(0.5F),
            },
            .center_x = 3.0F,
            .height = -2.0F,
            .center_z = -4.0F,
            .width = 20.0F,
            .depth = 12.0F,
            .minimum_scale = 0.75F,
            .maximum_scale = 1.25F,
            .count = 32U,
            .seed = 42U,
        };
        std::vector<vcr::SceneNodeId> first;
        expect(
            vcr::scatter_model_instances(scene, scatter, first, error) &&
                error.empty() && first.size() == scatter.count,
            "model scatter creates the requested bounded batch");
        std::vector<vcr::SceneTransform> first_transforms;
        first_transforms.reserve(first.size());
        for (const vcr::SceneNodeId id : first) {
            const auto node = scene.node(id);
            if (node) {
                first_transforms.push_back(node->get().transform);
            }
        }

        vcr::SceneDocument repeat;
        const vcr::SceneNodeId repeat_root = repeat.create_node(
            "Landscape", vcr::SceneNodeKind::folder);
        scatter.parent = repeat_root;
        std::vector<vcr::SceneNodeId> second;
        expect(
            vcr::scatter_model_instances(repeat, scatter, second, error) &&
                second.size() == first_transforms.size(),
            "scatter can be reproduced in a fresh scene");
        bool deterministic = second.size() == first_transforms.size();
        for (std::size_t index = 0U;
             deterministic && index < second.size(); ++index) {
            const auto node = repeat.node(second[index]);
            deterministic = node &&
                node->get().transform == first_transforms[index] &&
                node->get().asset_path == scatter.asset_path;
        }
        expect(
            deterministic,
            "scatter seed produces stable transforms independent of the STL RNG");

        const std::uint32_t size_before_invalid = repeat.size();
        scatter.minimum_scale = 2.0F;
        scatter.maximum_scale = 1.0F;
        expect(
            !vcr::scatter_model_instances(repeat, scatter, second, error) &&
                repeat.size() == size_before_invalid && second.empty(),
            "invalid scatter is rejected without a partial scene mutation");
    }

    {
        std::error_code filesystem_error;
        const std::filesystem::path temporary_root =
            std::filesystem::temp_directory_path(filesystem_error) /
            ("vcr_asset_browser_" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()));
        expect(
            !filesystem_error &&
                std::filesystem::create_directories(
                    temporary_root / "folder",
                    filesystem_error) &&
                !filesystem_error,
            "asset browser test directory is created");
        {
            std::ofstream image(
                temporary_root / "image.PNG",
                std::ios::binary);
            image.put('x');
        }
        {
            std::ofstream shader(
                temporary_root / "shader.vert",
                std::ios::binary);
            shader.put('x');
        }
        {
            std::ofstream nested(
                temporary_root / "folder" / "nested.glb",
                std::ios::binary);
            nested.put('x');
        }

        vcr::AssetBrowser browser;
        std::string error;
        expect(
            browser.initialize(temporary_root, error) &&
                error.empty() &&
                browser.entries().size() == 3U &&
                browser.entries().front().kind ==
                    vcr::AssetEntryKind::directory &&
                !browser.truncated(),
            "asset browser enumerates a bounded sorted project root");
        expect(
            !browser.navigate_to("..", error) &&
                browser.current_directory().empty() &&
                browser.entries().size() == 3U,
            "asset browser rejects root escape without changing state");
        expect(
            browser.navigate_to("folder", error) &&
                browser.current_directory() ==
                    std::filesystem::path("folder") &&
                browser.entries().size() == 1U &&
                browser.entries().front().kind ==
                    vcr::AssetEntryKind::model,
            "asset browser navigates into project assets");
        const std::size_t stable_entry_count =
            browser.entries().size();
        expect(
            !browser.navigate_to("missing", error) &&
                browser.current_directory() ==
                    std::filesystem::path("folder") &&
                browser.entries().size() == stable_entry_count,
            "failed asset navigation is transactional");
        expect(
            browser.navigate_up(error) &&
                browser.current_directory().empty(),
            "asset browser navigates to its root");

        vcr::SceneDocument source_scene;
        const vcr::SceneNodeId scene_root =
            source_scene.create_node(
                "Root", vcr::SceneNodeKind::folder);
        const vcr::SceneNodeId scene_cube =
            source_scene.create_node(
                "Cube", vcr::SceneNodeKind::cube, scene_root);
        const vcr::SceneNodeId scene_model =
            source_scene.create_node(
                "Imported tree",
                vcr::SceneNodeKind::model,
                scene_root);
        if (auto cube = source_scene.node(scene_cube)) {
            cube->get().transform.translation =
                glm::vec3(3.0F, 2.0F, -7.0F);
            cube->get().transform.rotation_degrees =
                glm::vec3(10.0F, 20.0F, 30.0F);
            cube->get().transform.scale =
                glm::vec3(2.0F, 0.5F, 4.0F);
            cube->get().material = vcr::PbrMaterial{
                .base_color =
                    glm::vec4(0.2F, 0.4F, 0.8F, 1.0F),
                .metallic = 0.75F,
                .roughness = 0.24F,
                .ambient_occlusion = 0.9F,
            };
            source_scene.mark_content_changed();
        }
        if (auto model = source_scene.node(scene_model)) {
            model->get().asset_path =
                "tree_stump_01_4k/"
                "tree_stump_01_4k.vcrasset.json";
            model->get().transform.translation =
                glm::vec3(-2.0F, 0.0F, -5.0F);
            source_scene.mark_content_changed();
        }
        const std::filesystem::path scene_file =
            temporary_root / "roundtrip.scene.json";
        expect(
            vcr::save_scene_file(
                source_scene, scene_file, error),
            "scene JSON saves through a temporary file");
        vcr::SceneDocument loaded_scene;
        expect(
            vcr::load_scene_file(
                scene_file, loaded_scene, error) &&
                loaded_scene.size() == source_scene.size(),
            "scene JSON loads transactionally");
        std::vector<vcr::SceneNodeRecord> source_records;
        std::vector<vcr::SceneNodeRecord> loaded_records;
        source_scene.build_node_records(source_records);
        loaded_scene.build_node_records(loaded_records);
        expect(
            loaded_records.size() == 3U &&
                source_records.size() == loaded_records.size() &&
                loaded_records[1].parent_record_index == 0U &&
                loaded_records[1].value.name == "Cube" &&
                loaded_records[2].value.kind ==
                    vcr::SceneNodeKind::model &&
                loaded_records[2].value.asset_path ==
                    "tree_stump_01_4k/"
                    "tree_stump_01_4k.vcrasset.json" &&
                close_to(
                    loaded_records[1].value.transform.scale.z,
                    4.0,
                    0.0001) &&
                close_to(
                    loaded_records[1].value.material.metallic,
                    0.75,
                    0.0001) &&
                close_to(
                    loaded_records[1].value.material.roughness,
                    0.24,
                    0.0001) &&
                close_to(
                    loaded_records[1]
                        .value.material.ambient_occlusion,
                    0.9,
                    0.0001),
            "scene JSON preserves hierarchy, model asset and PBR values");

        vcr::SceneDocument invalid_model_scene;
        static_cast<void>(invalid_model_scene.create_node(
            "Missing asset",
            vcr::SceneNodeKind::model));
        expect(
            !vcr::save_scene_file(
                invalid_model_scene,
                temporary_root / "invalid-model.scene.json",
                error),
            "scene save rejects a model without an asset path");

        const std::filesystem::path legacy_scene_file =
            temporary_root / "legacy.scene.json";
        {
            std::ofstream legacy(legacy_scene_file);
            legacy
                << "{\"format\":\"vulkan_cube_scene\","
                   "\"version\":1,\"nodes\":[{"
                   "\"name\":\"Legacy\",\"kind\":\"cube\","
                   "\"parent\":null,\"visible\":true,"
                   "\"expanded\":true,\"transform\":{"
                   "\"translation\":[0,0,0],"
                   "\"rotation_degrees\":[0,0,0],"
                   "\"scale\":[1,1,1]},"
                   "\"color\":[0.2,0.4,0.8,1]}]}";
        }
        vcr::SceneDocument legacy_scene;
        expect(
            vcr::load_scene_file(
                legacy_scene_file, legacy_scene, error),
            "legacy scene JSON without PBR data remains supported");
        std::vector<vcr::SceneNodeRecord> legacy_records;
        legacy_scene.build_node_records(legacy_records);
        expect(
            legacy_records.size() == 1U &&
                close_to(
                    legacy_records[0].value.material.base_color.b,
                    0.8,
                    0.0001) &&
                close_to(
                    legacy_records[0].value.material.metallic,
                    0.0,
                    0.0001) &&
                close_to(
                    legacy_records[0].value.material.roughness,
                    0.65,
                    0.0001) &&
                close_to(
                    legacy_records[0]
                        .value.material.ambient_occlusion,
                    1.0,
                    0.0001),
            "legacy scene JSON receives safe PBR defaults");

        const std::filesystem::path invalid_scene_file =
            temporary_root / "invalid.scene.json";
        {
            std::ofstream invalid(invalid_scene_file);
            invalid
                << "{\"format\":\"vulkan_cube_scene\","
                   "\"version\":1,\"nodes\":[{"
                   "\"name\":\"Invalid\",\"kind\":\"cube\","
                   "\"parent\":1,\"visible\":true,"
                   "\"expanded\":true,\"transform\":{"
                   "\"translation\":[0,0,0],"
                   "\"rotation_degrees\":[0,0,0],"
                   "\"scale\":[1,1,1]},"
                   "\"color\":[1,1,1,1]}]}";
        }
        vcr::SceneDocument unchanged =
            vcr::SceneDocument::make_default();
        const std::uint32_t unchanged_size = unchanged.size();
        expect(
            !vcr::load_scene_file(
                invalid_scene_file, unchanged, error) &&
                unchanged.size() == unchanged_size,
            "invalid scene load leaves the current document unchanged");

        const std::filesystem::path escaping_asset_scene_file =
            temporary_root / "escaping-asset.scene.json";
        {
            std::ofstream invalid(escaping_asset_scene_file);
            invalid
                << "{\"format\":\"vulkan_cube_scene\","
                   "\"version\":1,\"nodes\":[{"
                   "\"name\":\"Escaping\",\"kind\":\"model\","
                   "\"asset_path\":\"../outside.vcrasset.json\","
                   "\"parent\":null,\"visible\":true,"
                   "\"expanded\":true,\"transform\":{"
                   "\"translation\":[0,0,0],"
                   "\"rotation_degrees\":[0,0,0],"
                   "\"scale\":[1,1,1]},"
                   "\"color\":[1,1,1,1]}]}";
        }
        expect(
            !vcr::load_scene_file(
                escaping_asset_scene_file, unchanged, error) &&
                unchanged.size() == unchanged_size,
            "scene load rejects an asset-root escape transactionally");

        filesystem_error.clear();
        static_cast<void>(std::filesystem::remove_all(
            temporary_root, filesystem_error));
        expect(
            !filesystem_error,
            "asset browser test directory is removed");
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
