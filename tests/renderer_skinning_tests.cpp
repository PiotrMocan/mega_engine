#include "cooked_model_asset.hpp"
#include "exposure.hpp"
#include "fly_camera.hpp"
#include "renderer.hpp"
#include "scene_document.hpp"
#include "skeletal_animation.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Json = nlohmann::json;

struct SdlLifetime final {
    SdlLifetime() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            vcr::fatal(SDL_GetError());
        }
    }
    ~SdlLifetime() noexcept {
        SDL_Quit();
    }
    SdlLifetime(const SdlLifetime&) = delete;
    SdlLifetime& operator=(const SdlLifetime&) = delete;
};

struct WindowDeleter final {
    void operator()(SDL_Window* const window) const noexcept {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
};

struct RendererGuard final {
    vcr::Renderer& renderer;
    vcr::VulkanContext& context;
    explicit RendererGuard(vcr::Renderer& renderer_ref, vcr::VulkanContext& context_ref) noexcept
        : renderer(renderer_ref), context(context_ref) {}
    ~RendererGuard() noexcept {
        renderer.shutdown();
        context.shutdown();
    }
    RendererGuard(const RendererGuard&) = delete;
    RendererGuard& operator=(const RendererGuard&) = delete;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t attempt = 0U; attempt < 128U; ++attempt) {
            const std::filesystem::path candidate =
                base / ("vcr_renderer_skinning_tests_" + std::to_string(token) + "_" +
                        std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) {
                return;
            }
        }
    }

    ~TemporaryDirectory() noexcept {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error("Assertion failed: " + std::string(message));
    }
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value));
    append_u32(output, static_cast<std::uint32_t>(value >> 32U));
}

void append_f32(std::vector<std::byte>& output, const float value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

struct TestVertex {
    std::array<float, 3U> position{};
    std::array<float, 3U> normal{};
    std::array<float, 2U> texcoord{};
    std::array<float, 4U> tangent{};
};

struct TestSubmesh {
    std::uint64_t first_index = 0U;
    std::uint64_t index_count = 0U;
    std::uint32_t material_index = 0U;
};

[[nodiscard]] std::vector<std::byte> make_mesh_bytes(
    const std::span<const TestVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::span<const TestSubmesh> submeshes) {
    std::vector<std::byte> output;
    constexpr std::array<char, 8U> magic{'V', 'C', 'R', 'M', 'E', 'S', 'H', '\0'};
    for (const char ch : magic) {
        output.push_back(static_cast<std::byte>(ch));
    }
    append_u32(output, vcr::cooked_mesh_binary_version);
    append_u32(output, vcr::cooked_mesh_vertex_stride);
    append_u64(output, vertices.size());
    append_u64(output, indices.size());
    append_u32(output, static_cast<std::uint32_t>(submeshes.size()));
    append_u32(output, 0U);
    for (const TestSubmesh& submesh : submeshes) {
        append_u64(output, submesh.first_index);
        append_u64(output, submesh.index_count);
        append_u32(output, submesh.material_index);
        append_u32(output, 0U);
    }
    for (const TestVertex& vertex : vertices) {
        for (const float value : vertex.position) append_f32(output, value);
        for (const float value : vertex.normal) append_f32(output, value);
        for (const float value : vertex.texcoord) append_f32(output, value);
        for (const float value : vertex.tangent) append_f32(output, value);
    }
    for (const std::uint32_t index : indices) {
        append_u32(output, index);
    }
    return output;
}

[[nodiscard]] bool write_bytes(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_json(const std::filesystem::path& path, const Json& json) {
    const std::string text = json.dump(2);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

void add_quad_patch(
    std::vector<TestVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    std::vector<vcr::CookedSkinVertex>& skin_vertices,
    const float half_extent,
    const float z_offset,
    const std::uint16_t bone_index) {
    const std::uint32_t base_vertex = static_cast<std::uint32_t>(vertices.size());
    constexpr std::uint32_t grid = 4U;
    for (std::uint32_t row = 0U; row <= grid; ++row) {
        const float v = static_cast<float>(row) / static_cast<float>(grid);
        const float y = -half_extent + 2.0F * half_extent * v;
        for (std::uint32_t col = 0U; col <= grid; ++col) {
            const float u = static_cast<float>(col) / static_cast<float>(grid);
            const float x = -half_extent + 2.0F * half_extent * u;
            vertices.push_back(TestVertex{
                .position = {x, y, z_offset},
                .normal = {0.0F, 0.0F, 1.0F},
                .texcoord = {u, v},
                .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
            });
            vcr::CookedSkinVertex sv{};
            sv.joints = {bone_index, 0U, 0U, 0U};
            sv.weights = {1.0F, 0.0F, 0.0F, 0.0F};
            skin_vertices.push_back(sv);
        }
    }
    const std::uint32_t stride = grid + 1U;
    for (std::uint32_t row = 0U; row < grid; ++row) {
        for (std::uint32_t col = 0U; col < grid; ++col) {
            const std::uint32_t i00 = base_vertex + row * stride + col;
            const std::uint32_t i10 = i00 + 1U;
            const std::uint32_t i01 = i00 + stride;
            const std::uint32_t i11 = i01 + 1U;
            indices.push_back(i00);
            indices.push_back(i10);
            indices.push_back(i11);
            indices.push_back(i00);
            indices.push_back(i11);
            indices.push_back(i01);
        }
    }
}

void write_skinned_hero_asset(
    const std::filesystem::path& asset_root,
    std::uint32_t& out_vertex_count) {
    const std::filesystem::path dir = asset_root / "hero";
    std::filesystem::create_directories(dir / "mesh");
    std::filesystem::create_directories(dir / "materials");

    std::vector<TestVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<vcr::CookedSkinVertex> skin_vertices;
    // Patch 0 (25 verts, 96 indices) on bone 0 ("root"), Patch 1 (25 verts, 96 indices) on bone 1 ("limb").
    // Total 50 vertices / 192 indices (>= 96 so prepare_model_lods generates distinct multi-LOD chains).
    add_quad_patch(vertices, indices, skin_vertices, 0.15F, 0.0F, 0U);
    add_quad_patch(vertices, indices, skin_vertices, 0.85F, 0.05F, 1U);
    out_vertex_count = static_cast<std::uint32_t>(vertices.size());

    const std::array<TestSubmesh, 1U> submeshes{{
        TestSubmesh{
            .first_index = 0U,
            .index_count = indices.size(),
            .material_index = 0U,
        },
    }};

    vcr::CookedSkeletonAsset skeleton{};
    skeleton.root_motion_bone_index = 0;
    skeleton.bones.push_back(vcr::SkeletonBone{
        .name = "root",
        .parent_index = -1,
        .inverse_bind_matrix = glm::mat4(1.0F),
    });
    skeleton.bones.push_back(vcr::SkeletonBone{
        .name = "limb",
        .parent_index = 0,
        .inverse_bind_matrix = glm::mat4(1.0F),
    });

    std::string skin_err;
    const bool skin_ok =
        vcr::write_cooked_skin_file(dir / "hero.vcrskin", skeleton, skin_vertices, skin_err);
    require(skin_ok, "write_cooked_skin_file succeeds: " + skin_err);

    require(
        write_bytes(dir / "mesh" / "hero.vcrmesh", make_mesh_bytes(vertices, indices, submeshes)),
        "write hero.vcrmesh");

    const Json material{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "material"},
        {"name", "HeroRed"},
        {"double_sided", true},
        {"pbr",
         {
             {"base_color", {1.0F, 0.06F, 0.04F, 1.0F}},
             {"metallic", 0.0F},
             {"roughness", 0.5F},
             {"ambient_occlusion", 1.0F},
         }},
        {"textures", Json::object()},
    };
    require(
        write_json(dir / "materials" / "0_painted.vcrmaterial.json", material),
        "write hero material");

    // Deliberately omit authored "lods" so prepare_model_lods preserves skinning data
    // AND generates multi-LOD lod_count[0..3] > 0.
    const Json metadata{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "model"},
        {"name", "hero"},
        {"mesh", "mesh/hero.vcrmesh"},
        {"skeleton", "hero.vcrskin"},
        {"geometry",
         {
             {"vertex_count", vertices.size()},
             {"index_count", indices.size()},
             {"index_type", "uint32"},
             {"primitive", "triangles"},
             {"vertex_stride", vcr::cooked_mesh_vertex_stride},
         }},
        {"materials", {"materials/0_painted.vcrmaterial.json"}},
        {"submeshes",
         {
             {
                 {"name", "hero_submesh"},
                 {"first_index", 0U},
                 {"index_count", indices.size()},
                 {"material_index", 0U},
                 {"material", "materials/0_painted.vcrmaterial.json"},
             },
         }},
    };
    require(write_json(dir / "hero.vcrasset.json", metadata), "write hero.vcrasset.json");
}

[[nodiscard]] vcr::TransformRows make_rows(
    const glm::vec3 translation,
    const glm::vec3 scale = glm::vec3(1.0F)) noexcept {
    return vcr::TransformRows{
        .x = glm::vec4(scale.x, 0.0F, 0.0F, translation.x),
        .y = glm::vec4(0.0F, scale.y, 0.0F, translation.y),
        .z = glm::vec4(0.0F, 0.0F, scale.z, translation.z),
    };
}

[[nodiscard]] std::uint64_t sum_green_dominance(
    const vcr::ScreenshotCapture& shot,
    const std::uint32_t x0,
    const std::uint32_t y0,
    const std::uint32_t x1,
    const std::uint32_t y1) noexcept {
    std::uint64_t total = 0U;
    const std::uint32_t cx1 = std::min(x1, shot.width);
    const std::uint32_t cy1 = std::min(y1, shot.height);
    for (std::uint32_t y = y0; y < cy1; ++y) {
        for (std::uint32_t x = x0; x < cx1; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * shot.width + x) * 4U;
            const int r = static_cast<int>(shot.rgba[idx + 0U]);
            const int g = static_cast<int>(shot.rgba[idx + 1U]);
            const int b = static_cast<int>(shot.rgba[idx + 2U]);
            const int dom = g - std::max(r, b);
            if (dom > 12) {
                total += static_cast<std::uint64_t>(dom);
            }
        }
    }
    return total;
}

[[nodiscard]] std::uint64_t sum_red_dominance(
    const vcr::ScreenshotCapture& shot,
    const std::uint32_t x0,
    const std::uint32_t y0,
    const std::uint32_t x1,
    const std::uint32_t y1) noexcept {
    std::uint64_t total = 0U;
    const std::uint32_t cx1 = std::min(x1, shot.width);
    const std::uint32_t cy1 = std::min(y1, shot.height);
    for (std::uint32_t y = y0; y < cy1; ++y) {
        for (std::uint32_t x = x0; x < cx1; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * shot.width + x) * 4U;
            const int r = static_cast<int>(shot.rgba[idx + 0U]);
            const int g = static_cast<int>(shot.rgba[idx + 1U]);
            const int b = static_cast<int>(shot.rgba[idx + 2U]);
            const int dom = r - std::max(g, b);
            if (dom > 12) {
                total += static_cast<std::uint64_t>(dom);
            }
        }
    }
    return total;
}

void run() {
    TemporaryDirectory temp_assets;
    require(!temp_assets.path().empty(), "temporary asset directory created");

    std::uint32_t hero_vertex_count = 0U;
    write_skinned_hero_asset(temp_assets.path(), hero_vertex_count);
    require(hero_vertex_count == 50U, "hero asset has 50 vertices");

    SdlLifetime sdl;
    std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
        "Renderer Skinning Regression Test",
        320,
        240,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
    require(window != nullptr, std::string("SDL_CreateWindow failed: ") + SDL_GetError());

    vcr::VulkanContext context;
    vcr::Renderer renderer;
    RendererGuard guard{renderer, context};

    context.initialize(window.get(), vcr::default_validation_enabled);
    const vcr::LightingSettings lighting{
        .dynamic = true,
        .voxel_gi = false,
        .cycle = false,
        .hour = 14.0,
        .ray_tracing = vcr::RayTracingMode::off,
    };
    renderer.initialize(
        context,
        window.get(),
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        vcr::RunMode::scene,
        64U,
        64U,
        vcr::AntiAliasingSettings{.method = vcr::AntiAliasingMethod::off},
        true,
        false,
        true,
        lighting);
    require(renderer.set_local_light_backend(vcr::LocalLightBackend::forward),
            "set forward local light backend");
    require(renderer.set_exposure_settings(vcr::ExposureSettings{}),
            "set fixed exposure settings");

    const vcr::FlyCamera camera(
        glm::vec3(0.0F, 0.0F, 0.0F),
        -1.57079632679F,
        0.0F);
    const glm::mat4 view_matrix = camera.view_matrix();

    auto pump_events = [&renderer]() {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                renderer.request_resize();
            }
        }
    };

    auto render_frames = [&](const std::size_t count) {
        std::size_t drawn = 0U;
        for (std::size_t attempt = 0U; attempt < count + 8U && drawn < count; ++attempt) {
            pump_events();
            vcr::FrameTimings timings{};
            if (renderer.draw_frame(timings, view_matrix, nullptr)) {
                ++drawn;
            }
        }
        require(drawn == count, "rendered requested frame count");
    };

    if (renderer.output_extent().width == 0U || renderer.output_extent().height == 0U) {
        SDL_ShowWindow(window.get());
        pump_events();
        renderer.request_resize();
        render_frames(2U);
    }
    require(renderer.output_extent().width > 0U && renderer.output_extent().height > 0U,
            "swapchain output extent is non-zero");

    auto capture_frame = [&]() {
        require(renderer.request_screenshot(), "request_screenshot");
        for (std::size_t attempt = 0U; attempt < 8U; ++attempt) {
            pump_events();
            vcr::FrameTimings timings{};
            static_cast<void>(renderer.draw_frame(timings, view_matrix, nullptr));
            if (auto shot = renderer.take_completed_screenshot()) {
                require(static_cast<bool>(*shot), "screenshot capture succeeded: " + shot->error);
                return *shot;
            }
        }
        throw std::runtime_error("Timed out waiting for completed screenshot");
    };

    // Build authored scene.
    vcr::SceneDocument doc;
    static_cast<void>(doc.create_node("Sky", vcr::SceneNodeKind::environment));
    static_cast<void>(doc.create_node("Sun", vcr::SceneNodeKind::directional_light));

    const vcr::SceneNodeId key_light = doc.create_node("KeyLight", vcr::SceneNodeKind::point_light);
    if (auto node = doc.node(key_light)) {
        node->get().transform.translation = glm::vec3(0.0F, 0.0F, -1.5F);
        node->get().light = vcr::PunctualLight{
            .color = glm::vec3(1.0F, 1.0F, 1.0F),
            .intensity_candela = 800.0F,
            .range = 30.0F,
        };
    }

    const vcr::SceneNodeId ground = doc.create_node("Ground", vcr::SceneNodeKind::ground);
    if (auto node = doc.node(ground)) {
        node->get().transform.translation = glm::vec3(0.0F, -10.0F, -10.0F);
        node->get().transform.scale = glm::vec3(20.0F, 1.0F, 20.0F);
        node->get().material.base_color = glm::vec4(0.08F, 0.08F, 0.1F, 1.0F);
        node->get().material.roughness = 0.9F;
    }

    const glm::vec3 mover_t0_pos(-3.0F, 1.8F, -6.0F);
    const glm::vec3 mover_scale(0.9F, 0.9F, 0.9F);
    const vcr::SceneNodeId mover = doc.create_node("Mover", vcr::SceneNodeKind::cube);
    if (auto node = doc.node(mover)) {
        node->get().transform.translation = mover_t0_pos;
        node->get().transform.scale = mover_scale;
        node->get().material = vcr::PbrMaterial{
            .base_color = glm::vec4(0.04F, 1.0F, 0.04F, 1.0F),
            .metallic = 0.0F,
            .roughness = 0.5F,
            .ambient_occlusion = 1.0F,
        };
    }

    auto add_character = [&](std::string name, const glm::vec3 pos) {
        const vcr::SceneNodeId id = doc.create_node(std::move(name), vcr::SceneNodeKind::model);
        if (auto node = doc.node(id)) {
            node->get().asset_path = "hero/hero.vcrasset.json";
            node->get().transform.translation = pos;
        }
        return id;
    };

    const vcr::SceneNodeId char_0 = add_character("Char0", glm::vec3(-1.4F, 0.0F, -6.0F));
    const vcr::SceneNodeId char_1 = add_character("Char1", glm::vec3(+1.4F, 0.0F, -6.0F));
    const vcr::SceneNodeId char_2 = add_character("Char2", glm::vec3(0.0F, -2.2F, -25.0F));

    std::vector<vcr::SceneGpuInstance> cube_instances;
    std::vector<vcr::SceneModelInstance> model_instances;
    std::vector<vcr::ScenePunctualLight> light_instances;
    const vcr::SceneRenderView authored_view =
        doc.build_render_view(cube_instances, model_instances, light_instances);

    const vcr::TransformRows t0 = make_rows(mover_t0_pos, mover_scale);
    const vcr::TransformRows t1 = make_rows(glm::vec3(-3.0F, 1.8F, -6.0F), mover_scale);
    const vcr::TransformRows t2 = make_rows(glm::vec3(+3.0F, 1.8F, -6.0F), mover_scale);
    const vcr::TransformRows t2b = make_rows(glm::vec3(+2.8F, 1.8F, -6.0F), mover_scale);
    const vcr::TransformRows t3 = make_rows(glm::vec3(0.0F, 1.8F, -6.0F), mover_scale);
    const vcr::TransformRows t4 = make_rows(glm::vec3(-2.2F, 1.8F, -6.0F), mover_scale);
    const vcr::TransformRows t5 = make_rows(glm::vec3(+2.2F, 1.8F, -6.0F), mover_scale);

    const auto make_pose = [](const glm::vec3 limb_offset) {
        return std::array<glm::mat4, 2U>{
            glm::mat4(1.0F),
            glm::translate(glm::mat4(1.0F), limb_offset),
        };
    };

    std::vector<vcr::VisibilitySettings::Backend> backends{
        vcr::VisibilitySettings::Backend::cpu,
    };
    if (renderer.gpu_visibility_available()) {
        backends.push_back(vcr::VisibilitySettings::Backend::gpu);
    } else {
        std::cout << "Warning: GPU visibility not supported on this device; running CPU backend only.\n";
    }

    for (const vcr::VisibilitySettings::Backend backend : backends) {
        const bool is_gpu = backend == vcr::VisibilitySettings::Backend::gpu;
        std::cout << "Testing visibility backend: " << (is_gpu ? "gpu" : "cpu") << '\n';

        renderer.clear_skinned_model_poses();
        renderer.invalidate_editor_scene();

        vcr::VisibilitySettings vis{};
        vis.frustum = true;
        vis.occlusion = is_gpu;
        vis.lod = true;
        vis.dynamic_shadows = true;
        vis.backend = backend;
        vis.coarse_front_to_back = true;
        renderer.set_visibility_settings(vis);

        std::string scene_err;
        const bool scene_ok =
            renderer.update_editor_scene(authored_view, temp_assets.path(), scene_err);
        require(scene_ok, "update_editor_scene succeeds: " + scene_err);
        require(!renderer.editor_scene_needs_sync(), "editor_scene_needs_sync is false");

        render_frames(3U);
        require(std::string_view(renderer.visibility_backend()) == (is_gpu ? "gpu" : "cpu"),
                "visibility_backend matches requested backend");

        // A. Pre-pose static LOD state & baseline screenshot.
        {
            const auto d2 = renderer.inspect_rendered_node(char_2);
            require(d2.has_model_draw && !d2.uses_skinned_pool && d2.draw_vertex_offset == 0,
                    "pre-pose char_2 is unskinned with offset 0");
            if (is_gpu) {
                require(d2.has_visibility_group && d2.group_force_lod0 == 0U &&
                            d2.group_lod_counts.y > 0U && d2.indirect_index_counts[1] > 0U,
                        "pre-pose GPU visibility group has multi-LOD chains for char_2");
            }
            require(renderer.set_runtime_transform(mover, t1), "pre-pose set_runtime_transform(mover, t1)");
            render_frames(2U);
            const auto dm = renderer.inspect_rendered_node(mover);
            require(dm.in_runtime_transforms && dm.in_scene_packet && dm.packet_current == t1,
                    "pre-pose mover packet_current == t1");
        }
        const vcr::ScreenshotCapture shot_t1 = capture_frame();

        // B. First skin activation on 3 instances + independent poses + live rigid transform T2.
        const auto pose_up = make_pose(glm::vec3(0.0F, +1.5F, 0.0F));
        const auto pose_down = make_pose(glm::vec3(0.0F, -1.5F, 0.0F));
        const auto pose_side = make_pose(glm::vec3(+0.8F, 0.0F, 0.0F));
        renderer.set_skinned_model_pose(char_0, "hero/hero.vcrasset.json", pose_up);
        renderer.set_skinned_model_pose(char_1, "hero/hero.vcrasset.json", pose_down);
        renderer.set_skinned_model_pose(char_2, "hero/hero.vcrasset.json", pose_side);

        // Must preserve runtime_transforms_ and editor_scene_revision_!
        require(!renderer.editor_scene_needs_sync(),
                "skin activation must not trigger full editor_scene_needs_sync");
        require(renderer.set_runtime_transform(mover, t2),
                "set_runtime_transform(mover, t2) succeeds immediately after first skinning");

        render_frames(4U);
        require(renderer.skinning_active_node_count() == 3U, "3 skinned slots active");

        const auto dm_t2 = renderer.inspect_rendered_node(mover);
        require(dm_t2.in_runtime_transforms && dm_t2.in_scene_packet &&
                    dm_t2.packet_current == t2 && dm_t2.packet_previous == t2,
                "mover packet_current and packet_previous converged to T2");

        const auto d0_b = renderer.inspect_rendered_node(char_0);
        const auto d1_b = renderer.inspect_rendered_node(char_1);
        const auto d2_b = renderer.inspect_rendered_node(char_2);
        require(d0_b.uses_skinned_pool && d0_b.draw_vertex_offset == 0,
                "char_0 uses skinned pool offset 0");
        require(d1_b.uses_skinned_pool &&
                    d1_b.draw_vertex_offset == static_cast<std::int32_t>(hero_vertex_count),
                "char_1 uses skinned pool offset 50");
        require(d2_b.uses_skinned_pool &&
                    d2_b.draw_vertex_offset == static_cast<std::int32_t>(hero_vertex_count * 2U),
                "char_2 uses skinned pool offset 100");
        if (is_gpu) {
            const std::array<vcr::Renderer::RenderedNodeDiagnostics, 3U> diags{d0_b, d1_b, d2_b};
            for (std::size_t idx = 0U; idx < diags.size(); ++idx) {
                const auto expected_offset =
                    static_cast<std::int32_t>(idx * hero_vertex_count);
                require(diags[idx].has_visibility_group &&
                            diags[idx].group_vertex_offset == expected_offset &&
                            diags[idx].indirect_vertex_offsets[0] == expected_offset &&
                            diags[idx].group_force_lod0 == 1U &&
                            diags[idx].group_lod_counts.y == 0U &&
                            diags[idx].group_lod_counts.z == 0U &&
                            diags[idx].group_lod_counts.w == 0U &&
                            diags[idx].indirect_index_counts[1] == 0U &&
                            diags[idx].indirect_index_counts[2] == 0U &&
                            diags[idx].indirect_index_counts[3] == 0U,
                        "GPU visibility group & indirect commands match skinned LOD0-only slot");
            }
            const auto stats = renderer.visibility_statistics();
            require(stats.lod_instances[1] == 0U && stats.lod_instances[2] == 0U &&
                        stats.lod_instances[3] == 0U,
                    "skinned models never fall into LOD > 0 in GPU visibility stats");
        }

        const vcr::ScreenshotCapture shot_t2 = capture_frame();
        const std::uint32_t w = shot_t2.width;
        const std::uint32_t h = shot_t2.height;
        require(shot_t1.width == w && shot_t1.height == h, "matching screenshot extents");

        const std::uint64_t t1_green_left = sum_green_dominance(shot_t1, 0U, 0U, w / 3U, h / 2U);
        const std::uint64_t t1_green_right =
            sum_green_dominance(shot_t1, (2U * w) / 3U, 0U, w, h / 2U);
        const std::uint64_t t2_green_left = sum_green_dominance(shot_t2, 0U, 0U, w / 3U, h / 2U);
        const std::uint64_t t2_green_right =
            sum_green_dominance(shot_t2, (2U * w) / 3U, 0U, w, h / 2U);
        require(t1_green_left > 5000U && t1_green_right < t1_green_left / 10U,
                "shot_t1 renders mover in upper-left");
        require(t2_green_right > 5000U && t2_green_left < t2_green_right / 10U,
                "shot_t2 renders mover in upper-right after skinning activation");

        const std::uint64_t c0_red_upper =
            sum_red_dominance(shot_t2, (w * 25U) / 100U, 0U, (w * 46U) / 100U, h / 2U);
        const std::uint64_t c0_red_lower =
            sum_red_dominance(shot_t2, (w * 25U) / 100U, h / 2U, (w * 46U) / 100U, h);
        const std::uint64_t c1_red_upper =
            sum_red_dominance(shot_t2, (w * 54U) / 100U, 0U, (w * 75U) / 100U, h / 2U);
        const std::uint64_t c1_red_lower =
            sum_red_dominance(shot_t2, (w * 54U) / 100U, h / 2U, (w * 75U) / 100U, h);
        require(c0_red_upper > 5000U && c0_red_upper > c0_red_lower * 4U,
                "char_0 limb shifted upward (+Y) in rendered pixels");
        require(c1_red_lower > 5000U && c1_red_lower > c1_red_upper * 4U,
                "char_1 limb shifted downward (-Y) using its own skinned vertex offset");

        // C. Steady-state pose animation across 2+ frames without revision churn.
        const std::uint64_t skin_rev_before = renderer.skinned_draw_revision();
        const std::uint64_t gpu_rev_before = renderer.gpu_scene_revision();
        for (int step = 0; step < 3; ++step) {
            const float delta = 0.05F * static_cast<float>(step + 1);
            renderer.set_skinned_model_pose(
                char_0, "hero/hero.vcrasset.json", make_pose(glm::vec3(0.0F, 1.5F - delta, 0.0F)));
            renderer.set_skinned_model_pose(
                char_1, "hero/hero.vcrasset.json", make_pose(glm::vec3(0.0F, -1.5F + delta, 0.0F)));
            require(renderer.set_runtime_transform(mover, t2b),
                    "steady-state runtime transform update succeeds");
            render_frames(1U);
        }
        require(renderer.skinned_draw_revision() == skin_rev_before,
                "steady-state pose updates do not bump skinned_draw_revision");
        require(renderer.gpu_scene_revision() == gpu_rev_before,
                "steady-state pose updates do not bump gpu_scene_revision");

        // D. Middle slot removal & compaction (char_1 removed -> char_2 compacts 100 -> 50).
        renderer.remove_skinned_model_pose(char_1);
        require(renderer.set_runtime_transform(mover, t3),
                "set_runtime_transform(mover, t3) succeeds after remove_skinned_model_pose");
        render_frames(3U);

        require(renderer.skinning_active_node_count() == 2U, "2 skinned slots remain after removal");
        require(renderer.inspect_rendered_node(mover).packet_current == t3,
                "mover packet_current == t3 after compaction");
        const auto d0_d = renderer.inspect_rendered_node(char_0);
        const auto d1_d = renderer.inspect_rendered_node(char_1);
        const auto d2_d = renderer.inspect_rendered_node(char_2);
        require(d0_d.uses_skinned_pool && d0_d.draw_vertex_offset == 0,
                "char_0 remains at offset 0");
        require(!d1_d.uses_skinned_pool && d1_d.draw_vertex_offset == 0,
                "removed char_1 reverts to unskinned pool");
        require(d2_d.uses_skinned_pool &&
                    d2_d.draw_vertex_offset == static_cast<std::int32_t>(hero_vertex_count),
                "char_2 compacts from offset 100 to 50");
        if (is_gpu) {
            require(d1_d.group_force_lod0 == 0U && d1_d.group_lod_counts.y > 0U &&
                        d1_d.indirect_index_counts[1] > 0U,
                    "removed char_1 restores multi-LOD GPU visibility group");
            require(d2_d.group_vertex_offset == static_cast<std::int32_t>(hero_vertex_count) &&
                        d2_d.indirect_vertex_offsets[0] ==
                            static_cast<std::int32_t>(hero_vertex_count) &&
                        d2_d.group_force_lod0 == 1U,
                    "compacted char_2 updates GPU visibility group and indirect baseVertex to 50");
        }

        // E. Pause / Resume.
        render_frames(3U);
        const auto dm_pause = renderer.inspect_rendered_node(mover);
        require(dm_pause.packet_current == t3 && dm_pause.packet_previous == t3,
                "pause preserves live transform T3 instead of snapping to authored T0");

        require(renderer.set_runtime_transform(mover, t4), "resume set_runtime_transform(mover, t4)");
        renderer.set_skinned_model_pose(char_0, "hero/hero.vcrasset.json", pose_up);
        renderer.set_skinned_model_pose(char_2, "hero/hero.vcrasset.json", pose_side);
        render_frames(2U);
        require(renderer.inspect_rendered_node(mover).packet_current == t4,
                "resume advances mover to T4");

        // F. clear_skinned_model_poses vs Editor Stop (invalidate_editor_scene).
        renderer.clear_skinned_model_poses();
        require(renderer.skinning_active_node_count() == 0U,
                "clear_skinned_model_poses clears all active slots");
        require(renderer.set_runtime_transform(mover, t5),
                "clear_skinned_model_poses preserves runtime transforms for T5");
        render_frames(2U);
        require(renderer.inspect_rendered_node(mover).packet_current == t5,
                "mover renders T5 after clear_skinned_model_poses");
        for (const vcr::SceneNodeId cid : {char_0, char_1, char_2}) {
            const auto dc = renderer.inspect_rendered_node(cid);
            require(!dc.uses_skinned_pool && dc.draw_vertex_offset == 0,
                    "cleared character draws revert to unskinned offset 0");
            if (is_gpu) {
                require(dc.group_force_lod0 == 0U && dc.group_vertex_offset == 0,
                        "cleared character GPU group reverts to unskinned");
            }
        }

        renderer.invalidate_editor_scene();
        require(renderer.editor_scene_needs_sync(),
                "invalidate_editor_scene sets editor_scene_needs_sync");
        std::string stop_err;
        const bool stop_ok =
            renderer.update_editor_scene(authored_view, temp_assets.path(), stop_err);
        require(stop_ok, "editor Stop rebuilds authored scene: " + stop_err);
        render_frames(2U);
        require(renderer.inspect_rendered_node(mover).packet_current == t0,
                "editor Stop restores authored transform T0");
    }

    // G. Forward transparent dielectric glass blending (no Bayer screen-door checkerboard).
    {
        const auto write_quad_model = [&](const std::string_view model_name,
                                          const std::string_view material_name,
                                          const float half_extent,
                                          const std::array<float, 4U>& base_color,
                                          const float roughness,
                                          const std::string_view alpha_mode,
                                          const bool is_glass,
                                          const bool double_sided) {
            const std::filesystem::path dir = temp_assets.path() / model_name;
            std::filesystem::create_directories(dir / "mesh");
            std::filesystem::create_directories(dir / "materials");

            std::vector<TestVertex> vertices;
            std::vector<std::uint32_t> indices;
            std::vector<vcr::CookedSkinVertex> unused_skin;
            add_quad_patch(vertices, indices, unused_skin, half_extent, 0.0F, 0U);
            const std::array<TestSubmesh, 1U> submeshes{{
                TestSubmesh{
                    .first_index = 0U,
                    .index_count = indices.size(),
                    .material_index = 0U,
                },
            }};
            require(
                write_bytes(dir / "mesh" / "quad.vcrmesh", make_mesh_bytes(vertices, indices, submeshes)),
                "write static quad vcrmesh");

            const Json material{
                {"schema", "vcr.asset"},
                {"version", 1U},
                {"kind", "material"},
                {"name", material_name},
                {"alpha_mode", alpha_mode},
                {"glass", is_glass},
                {"double_sided", double_sided},
                {"pbr",
                 {
                     {"base_color", {base_color[0], base_color[1], base_color[2], base_color[3]}},
                     {"metallic", 0.0F},
                     {"roughness", roughness},
                     {"ambient_occlusion", 1.0F},
                 }},
                {"textures", Json::object()},
            };
            require(
                write_json(dir / "materials" / "0_surface.vcrmaterial.json", material),
                "write static quad material");

            const Json metadata{
                {"schema", "vcr.asset"},
                {"version", 1U},
                {"kind", "model"},
                {"name", model_name},
                {"mesh", "mesh/quad.vcrmesh"},
                {"generate_lods", false},
                {"geometry",
                 {
                     {"vertex_count", vertices.size()},
                     {"index_count", indices.size()},
                     {"index_type", "uint32"},
                     {"primitive", "triangles"},
                     {"vertex_stride", vcr::cooked_mesh_vertex_stride},
                 }},
                {"materials", {"materials/0_surface.vcrmaterial.json"}},
                {"submeshes",
                 {
                     {
                         {"name", "quad_submesh"},
                         {"first_index", 0U},
                         {"index_count", indices.size()},
                         {"material_index", 0U},
                         {"material", "materials/0_surface.vcrmaterial.json"},
                     },
                 }},
            };
            require(
                write_json(dir / (std::string(model_name) + ".vcrasset.json"), metadata),
                "write static quad vcrasset.json");
        };

        write_quad_model(
            "opaque_bg",
            "OpaqueRed",
            2.0F,
            {1.0F, 0.05F, 0.03F, 1.0F},
            0.8F,
            "opaque",
            false,
            true);
        write_quad_model(
            "blue_glass",
            "BlueGlass",
            2.5F,
            {0.08F, 0.55F, 0.95F, 0.35F},
            0.06F,
            "blend",
            true,
            true);

        const auto build_glass_scene = [&](const bool include_glass) {
            vcr::SceneDocument glass_doc;
            static_cast<void>(glass_doc.create_node("Sky", vcr::SceneNodeKind::environment));
            static_cast<void>(glass_doc.create_node("Sun", vcr::SceneNodeKind::directional_light));
            const vcr::SceneNodeId light_id =
                glass_doc.create_node("KeyLight", vcr::SceneNodeKind::point_light);
            if (auto node = glass_doc.node(light_id)) {
                node->get().transform.translation = glm::vec3(0.0F, 0.0F, -1.5F);
                node->get().light = vcr::PunctualLight{
                    .color = glm::vec3(1.0F, 1.0F, 1.0F),
                    .intensity_candela = 900.0F,
                    .range = 30.0F,
                };
            }
            const vcr::SceneNodeId bg_id =
                glass_doc.create_node("BgQuad", vcr::SceneNodeKind::model);
            if (auto node = glass_doc.node(bg_id)) {
                node->get().asset_path = "opaque_bg/opaque_bg.vcrasset.json";
                node->get().transform.translation = glm::vec3(0.0F, 0.0F, -5.5F);
            }
            if (include_glass) {
                const vcr::SceneNodeId glass_id =
                    glass_doc.create_node("GlassQuad", vcr::SceneNodeKind::model);
                if (auto node = glass_doc.node(glass_id)) {
                    node->get().asset_path = "blue_glass/blue_glass.vcrasset.json";
                    node->get().transform.translation = glm::vec3(0.0F, 0.0F, -4.0F);
                }
            }
            return glass_doc;
        };

        struct PatchMetrics final {
            double mean_r = 0.0;
            double mean_g = 0.0;
            double mean_b = 0.0;
            double mean_neighbor_diff = 0.0;
        };
        const auto measure_center_patch = [](const vcr::ScreenshotCapture& shot) {
            const std::uint32_t cx = shot.width / 2U;
            const std::uint32_t cy = shot.height / 2U;
            const std::uint32_t x0 = cx > 8U ? cx - 8U : 0U;
            const std::uint32_t y0 = cy > 8U ? cy - 8U : 0U;
            const std::uint32_t x1 = std::min(shot.width, x0 + 16U);
            const std::uint32_t y1 = std::min(shot.height, y0 + 16U);
            std::uint64_t sum_r = 0U;
            std::uint64_t sum_g = 0U;
            std::uint64_t sum_b = 0U;
            std::uint64_t sample_count = 0U;
            std::uint64_t diff_sum = 0U;
            std::uint64_t diff_pairs = 0U;
            const auto pixel_idx = [&](const std::uint32_t x, const std::uint32_t y) {
                return (static_cast<std::size_t>(y) * shot.width + x) * 4U;
            };
            const auto abs_u8 = [](const std::uint8_t a, const std::uint8_t b) noexcept {
                return static_cast<std::uint64_t>(a > b ? (a - b) : (b - a));
            };
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const std::size_t idx = pixel_idx(x, y);
                    sum_r += shot.rgba[idx + 0U];
                    sum_g += shot.rgba[idx + 1U];
                    sum_b += shot.rgba[idx + 2U];
                    ++sample_count;
                    if (x + 1U < x1) {
                        const std::size_t right = pixel_idx(x + 1U, y);
                        diff_sum += abs_u8(shot.rgba[idx + 0U], shot.rgba[right + 0U]);
                        diff_sum += abs_u8(shot.rgba[idx + 1U], shot.rgba[right + 1U]);
                        diff_sum += abs_u8(shot.rgba[idx + 2U], shot.rgba[right + 2U]);
                        diff_pairs += 3U;
                    }
                    if (y + 1U < y1) {
                        const std::size_t down = pixel_idx(x, y + 1U);
                        diff_sum += abs_u8(shot.rgba[idx + 0U], shot.rgba[down + 0U]);
                        diff_sum += abs_u8(shot.rgba[idx + 1U], shot.rgba[down + 1U]);
                        diff_sum += abs_u8(shot.rgba[idx + 2U], shot.rgba[down + 2U]);
                        diff_pairs += 3U;
                    }
                }
            }
            require(sample_count > 0U && diff_pairs > 0U, "center patch is non-empty");
            return PatchMetrics{
                .mean_r = static_cast<double>(sum_r) / static_cast<double>(sample_count),
                .mean_g = static_cast<double>(sum_g) / static_cast<double>(sample_count),
                .mean_b = static_cast<double>(sum_b) / static_cast<double>(sample_count),
                .mean_neighbor_diff =
                    static_cast<double>(diff_sum) / static_cast<double>(diff_pairs),
            };
        };

        for (const vcr::VisibilitySettings::Backend backend : backends) {
            const bool is_gpu = backend == vcr::VisibilitySettings::Backend::gpu;
            renderer.invalidate_editor_scene();
            vcr::VisibilitySettings vis{};
            vis.frustum = true;
            vis.occlusion = is_gpu;
            vis.lod = true;
            vis.dynamic_shadows = true;
            vis.backend = backend;
            vis.coarse_front_to_back = true;
            renderer.set_visibility_settings(vis);

            std::vector<vcr::SceneGpuInstance> cubes_bg;
            std::vector<vcr::SceneModelInstance> models_bg;
            std::vector<vcr::ScenePunctualLight> lights_bg;
            vcr::SceneDocument doc_bg = build_glass_scene(false);
            const vcr::SceneRenderView view_bg =
                doc_bg.build_render_view(cubes_bg, models_bg, lights_bg);
            std::string sync_err;
            require(renderer.update_editor_scene(view_bg, temp_assets.path(), sync_err),
                    "sync opaque background scene: " + sync_err);
            render_frames(3U);
            const vcr::ScreenshotCapture shot_bg = capture_frame();
            const PatchMetrics m_bg = measure_center_patch(shot_bg);
            require(m_bg.mean_r > 80.0 && m_bg.mean_r > m_bg.mean_b + 30.0,
                    "opaque background quad renders bright red before glass overlay");

            std::vector<vcr::SceneGpuInstance> cubes_glass;
            std::vector<vcr::SceneModelInstance> models_glass;
            std::vector<vcr::ScenePunctualLight> lights_glass;
            vcr::SceneDocument doc_glass = build_glass_scene(true);
            const vcr::SceneRenderView view_glass =
                doc_glass.build_render_view(cubes_glass, models_glass, lights_glass);
            require(renderer.update_editor_scene(view_glass, temp_assets.path(), sync_err),
                    "sync transparent glass scene: " + sync_err);
            render_frames(3U);
            const vcr::ScreenshotCapture shot_glass = capture_frame();
            const PatchMetrics m_glass = measure_center_patch(shot_glass);
            require(m_glass.mean_r > 35.0 && m_glass.mean_r < m_bg.mean_r,
                    "dielectric glass transmits attenuated red background behind it");
            require(m_glass.mean_b > m_bg.mean_b + 12.0 && m_glass.mean_g > m_bg.mean_g + 8.0,
                    "dielectric glass contributes cyan-blue tint/reflection over background");
            require(m_glass.mean_neighbor_diff < 6.0,
                    "transparent glass blends smoothly without Bayer screen-door checkerboard");
        }
    }

    std::cout << "vcr_renderer_skinning_tests passed all checks\n";
}

} // namespace

int main() {
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vcr_renderer_skinning_tests failed: " << error.what() << '\n';
        return 1;
    }
}
