#pragma once

#include "asset_preparation.hpp"
#include "cli_options.hpp"
#include "device_object.hpp"
#include "depth_prepass.hpp"
#include "lod_transition.hpp"
#include "frame_context.hpp"
#include "fsr3_upscaler.hpp"
#include "exposure.hpp"
#include "fog.hpp"
#include "color_grading.hpp"
#include "subsurface.hpp"
#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"
#include "lighting.hpp"
#include "performance_metrics.hpp"
#include "runtime_scene.hpp"
#include "bounds_hierarchy.hpp"
#include "swapchain.hpp"
#include "upload_batch.hpp"
#include "vulkan_context.hpp"

#include <SDL3/SDL.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if VCR_EDITOR_ENABLED
#include "cooked_model_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "scene_document.hpp"
struct ImDrawData;
#endif

namespace vcr {

struct ScreenshotCapture {
    std::vector<std::uint8_t> rgba;
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty() && !rgba.empty();
    }
};

class Renderer final {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void initialize(VulkanContext &context, SDL_Window *window,
                    VkPresentModeKHR preferred_present_mode, RunMode initial_mode,
                    std::uint32_t initial_instances, std::uint32_t initial_gpu_workload,
                    AntiAliasingSettings initial_anti_aliasing, bool enable_screenshot_capture,
                    bool enable_editor_output = false, bool enable_authored_scene = false,
                    LightingSettings lighting = {}, RenderFixture fixture = RenderFixture::none);
    [[nodiscard]] bool draw_frame(
        FrameTimings& timings,
        const glm::mat4& view_matrix
#if VCR_EDITOR_ENABLED
        ,
        ImDrawData* editor_draw_data = nullptr
#endif
    );
    void record_metrics(const FrameTimings& timings) noexcept {
        metrics_.record_frame(timings);
    }
    [[nodiscard]] const LightingSettings &lighting_settings() const noexcept {
        return lighting_settings_;
    }
    [[nodiscard]] const LightingStatistics &lighting_statistics() const noexcept {
        return lighting_statistics_;
    }
    [[nodiscard]] std::uint32_t local_light_count() const noexcept {
        return static_cast<std::uint32_t>(local_lights_.size());
    }
    [[nodiscard]] bool set_local_light_backend(LocalLightBackend backend) noexcept {
        if (backend != LocalLightBackend::off && backend != LocalLightBackend::forward &&
            backend != LocalLightBackend::clustered && backend != LocalLightBackend::automatic)
            return false;
        if (local_light_backend_ != backend) {
            local_light_backend_ = backend;
            ++local_light_revision_;
            if (local_light_revision_ == 0U) ++local_light_revision_;
        }
        return true;
    }
    [[nodiscard]] LocalLightBackend local_light_backend() const noexcept {
        return local_light_backend_;
    }
    [[nodiscard]] LocalLightBackend local_light_backend_actual() const noexcept {
        if (local_light_backend_ == LocalLightBackend::off || local_lights_.empty())
            return LocalLightBackend::off;
        if (local_light_backend_ == LocalLightBackend::automatic)
            return local_lights_.size() >= 64U
                ? LocalLightBackend::clustered
                : LocalLightBackend::forward;
        return local_light_backend_;
    }
    [[nodiscard]] const LocalLightStatistics& local_light_statistics() const noexcept {
        return local_light_statistics_;
    }
    [[nodiscard]] bool gi_provisioned() const noexcept { return gi_provisioned_; }
    [[nodiscard]] bool set_lighting_settings(LightingSettings settings) noexcept;
    [[nodiscard]] ToneMapper tone_mapper() const noexcept { return tone_mapper_; }
    [[nodiscard]] DepthPrepass depth_prepass() const noexcept { return depth_prepass_; }
    [[nodiscard]] LodFadePattern lod_fade_pattern() const noexcept { return lod_fade_pattern_; }
    [[nodiscard]] float lod_fade_seconds() const noexcept { return lod_fade_seconds_; }
    [[nodiscard]] LodFadePattern lod_fade_actual() const noexcept {
#if VCR_EDITOR_ENABLED
        return authored_scene_enabled_ && mode_ != RunMode::instancing && visibility_settings_.lod
            ? lod_fade_pattern_ : LodFadePattern::off;
#else
        return LodFadePattern::off;
#endif
    }
    [[nodiscard]] bool set_lod_fade(LodFadePattern pattern, float seconds) noexcept;
    [[nodiscard]] bool set_depth_prepass(DepthPrepass mode) noexcept {
        if (mode != DepthPrepass::off && mode != DepthPrepass::partial &&
            mode != DepthPrepass::full) return false;
        depth_prepass_ = mode;
        return true;
    }
    // Display mapping runs after temporal reconstruction, so switching it does
    // not invalidate scene-linear history or touch any in-flight resources.
    [[nodiscard]] bool set_tone_mapper(ToneMapper mapper) noexcept {
        if (mapper != ToneMapper::reinhard && mapper != ToneMapper::gt7) return false;
        tone_mapper_ = mapper;
        return true;
    }
    [[nodiscard]] const ExposureSettings& exposure_settings() const noexcept {
        return exposure_settings_;
    }
    [[nodiscard]] bool set_exposure_settings(ExposureSettings settings) noexcept {
        if (!valid_exposure_settings(settings)) return false;
        if (exposure_settings_ == settings) return true;
        exposure_settings_ = settings;
        exposure_reset_pending_ = true;
        return true;
    }
    [[nodiscard]] ExposureMode exposure_mode_actual() const noexcept {
        return lighting_settings_.dynamic && mode_ != RunMode::instancing
            ? exposure_settings_.mode : ExposureMode::fixed;
    }
    [[nodiscard]] const ExposureGpuReadback& exposure_state() const noexcept {
        return exposure_readback_;
    }
    void request_exposure_reset() noexcept { exposure_reset_pending_ = true; }
    [[nodiscard]] ColorGrading color_grading() const noexcept {
        return color_grading_;
    }
    [[nodiscard]] ColorGrading color_grading_actual() const noexcept {
        return lighting_settings_.dynamic && mode_ != RunMode::instancing
            ? color_grading_ : ColorGrading::off;
    }
    [[nodiscard]] ColorGradingLutSize color_grading_lut_size() const noexcept {
        return color_grading_lut_size_;
    }
    [[nodiscard]] bool set_color_grading(
        ColorGrading grading,
        ColorGradingLutSize size) noexcept {
        if ((grading != ColorGrading::off && grading != ColorGrading::trilinear &&
             grading != ColorGrading::tetrahedral) ||
            (size != ColorGradingLutSize::size_32 &&
             size != ColorGradingLutSize::size_64)) return false;
        if (color_grading_ == grading && color_grading_lut_size_ == size) return true;
        color_grading_ = grading;
        color_grading_lut_size_ = size;
        return true;
    }
    [[nodiscard]] bool set_subsurface_mode(SubsurfaceMode mode) noexcept;
    [[nodiscard]] SubsurfaceMode subsurface_mode() const noexcept {
        return subsurface_mode_;
    }
    [[nodiscard]] SubsurfaceMode subsurface_mode_actual() const noexcept {
        return subsurface_mode_ == SubsurfaceMode::full_resolution &&
                   subsurface_resources_provisioned_ &&
                   mode_ != RunMode::instancing
            ? SubsurfaceMode::full_resolution
            : SubsurfaceMode::off;
    }
    [[nodiscard]] bool subsurface_resources_provisioned() const noexcept {
        return subsurface_resources_provisioned_;
    }
    [[nodiscard]] std::uint64_t subsurface_resident_bytes() const noexcept;
    [[nodiscard]] std::uint32_t subsurface_active_material_count() const noexcept;
    [[nodiscard]] std::uint32_t shadow_resolution() const noexcept { return shadow_map_.width(); }
    [[nodiscard]] std::string_view shadow_depth_format() const noexcept {
        return shadow_map_format_ == VK_FORMAT_D32_SFLOAT ? "d32"
             : shadow_map_format_ == VK_FORMAT_D16_UNORM ? "d16" : "unavailable";
    }
    [[nodiscard]] bool shadow_linear_filter() const noexcept { return shadow_map_linear_; }
    [[nodiscard]] bool supports_shadow_resolution(std::uint32_t resolution) const noexcept {
        return valid_shadow_resolution(resolution) && resolution <= max_shadow_resolution_;
    }
    void set_lighting_paused(bool paused) noexcept { lighting_paused_ = paused; }
    void request_resize() noexcept { resize_requested_ = true; }
    void toggle_vsync();
    void set_present_mode(PresentPreference preference);
    void set_vsync(bool enabled);
    void reset_metrics() noexcept { metrics_.reset(); }
    void print_metrics() const;
    void set_mode(RunMode mode) noexcept;
    void set_instance_count(std::uint32_t count) noexcept;
    void set_gpu_workload_iterations(std::uint32_t iterations) noexcept;
    void adjust_load(int direction) noexcept;
    void cycle_anti_aliasing();
    [[nodiscard]] bool set_anti_aliasing(AntiAliasingMethod method) noexcept;
    [[nodiscard]] bool set_smaa_mode(SmaaMode mode) noexcept;
    [[nodiscard]] bool set_motion_blur_settings(
        MotionBlurSettings settings) noexcept;
    [[nodiscard]] bool set_fog_settings(const FogSettings& settings) noexcept;
    [[nodiscard]] const FogSettings& fog_settings() const noexcept { return fog_settings_; }
    [[nodiscard]] bool fog_supported() const noexcept { return fog_supported_; }
    [[nodiscard]] bool fog_active() const noexcept {
        return fog_supported_ && fog_settings_.enabled &&
            fog_settings_.density > 0.0F && fog_settings_.maximum_opacity > 0.0F &&
            mode_ != RunMode::instancing;
    }
    [[nodiscard]] MotionBlurSettings motion_blur_settings() const noexcept {
        return motion_blur_settings_;
    }
    [[nodiscard]] MotionBlurMode motion_blur_mode_actual() const noexcept {
        return mode_ == RunMode::instancing
            ? MotionBlurMode::off : motion_blur_settings_.mode;
    }
    void adjust_anti_aliasing_quality(int direction) noexcept;
    [[nodiscard]] bool request_screenshot() noexcept;
    [[nodiscard]] std::optional<ScreenshotCapture>
        take_completed_screenshot() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] const Swapchain& swapchain() const noexcept {
        return swapchain_;
    }
    [[nodiscard]] PerformanceSnapshot metrics_snapshot(
        bool include_percentile = true) const noexcept {
        return metrics_.snapshot(include_percentile);
    }
    [[nodiscard]] RunMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint32_t instance_count() const noexcept;
    [[nodiscard]] std::uint64_t triangle_count() const noexcept;
    [[nodiscard]] std::uint32_t gpu_workload_iterations() const noexcept {
        return gpu_workload_iterations_;
    }
    [[nodiscard]] AntiAliasingSettings anti_aliasing() const noexcept {
        return anti_aliasing_;
    }
    [[nodiscard]] AntiAliasingMethod anti_aliasing_requested_method() const noexcept {
        return anti_aliasing_requested_method_;
    }
    [[nodiscard]] bool supports_msaa(AntiAliasingMethod method) const noexcept;
    [[nodiscard]] std::uint32_t anti_aliasing_sample_count_actual() const noexcept {
        return anti_aliasing_sample_count(anti_aliasing_.method);
    }
    [[nodiscard]] std::uint64_t msaa_resident_bytes() const noexcept;
    [[nodiscard]] std::uint64_t smaa_history_resident_bytes() const noexcept {
        const VkExtent2D extent = scene_output_extent();
        return static_cast<std::uint64_t>(extent.width) * extent.height * 8U *
            max_frames_in_flight;
    }
    [[nodiscard]] bool smaa_history_valid() const noexcept {
        return is_temporal_smaa(anti_aliasing_) && smaa_t2x_history_valid_;
    }
    [[nodiscard]] std::uint32_t smaa_temporal_phase() const noexcept {
        return smaa_t2x_phase_;
    }
    [[nodiscard]] VkExtent2D msaa_target_extent() const noexcept;
    [[nodiscard]] std::string_view msaa_depth_format_name() const noexcept;
    [[nodiscard]] VkExtent2D render_extent() const noexcept {
        return current_render_extent_.width > 0U &&
                current_render_extent_.height > 0U
            ? current_render_extent_
            : scene_output_extent();
    }
    [[nodiscard]] bool fsr3_available() const noexcept {
        return fsr3_upscaler_.available();
    }
    [[nodiscard]] std::string_view fsr3_version() const noexcept {
        return fsr3_upscaler_.version();
    }
    [[nodiscard]] std::string_view anti_aliasing_quality_name() const noexcept {
        if (anti_aliasing_.method == AntiAliasingMethod::smaa) {
            return to_string(anti_aliasing_.smaa_quality);
        }
        if (anti_aliasing_.method == AntiAliasingMethod::fsr3) {
            return to_string(anti_aliasing_.fsr3_quality);
        }
        if (anti_aliasing_.method == AntiAliasingMethod::msaa2x) return "2x";
        if (anti_aliasing_.method == AntiAliasingMethod::msaa4x) return "4x";
        return "n/a";
    }
    [[nodiscard]] std::uint32_t max_instance_capacity() const noexcept {
        return max_instance_capacity_;
    }
    [[nodiscard]] static constexpr std::uint32_t
        max_gpu_workload_iterations() noexcept {
        return 4096U;
    }
    [[nodiscard]] bool screenshot_supported() const noexcept {
        return swapchain_.screenshot_supported();
    }
    [[nodiscard]] bool screenshot_pending() const noexcept {
        return screenshot_requested_ || screenshot_in_flight_;
    }
    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
#if VCR_EDITOR_ENABLED
    [[nodiscard]] bool prepare_editor_model(
        const std::filesystem::path& asset_root,
        std::string_view relative_path,
        SceneTransform& recommended_transform,
        std::string& error);
    void enable_async_assets();
    // Pump only at the resource/frame boundary, before building draw packets.
    [[nodiscard]] bool poll_assets();
    [[nodiscard]] bool assets_pending() const noexcept {
        return asset_requests_pending_ > 0;
    }
    [[nodiscard]] std::size_t upload_high_water() const noexcept {
        return asset_upload_.high_water();
    }
    [[nodiscard]] bool tessellation_supported() const noexcept {
        return model_tessellation_enabled_;
    }
    [[nodiscard]] std::uint32_t tessellated_material_count() const noexcept {
        return tessellated_material_count_;
    }
    [[nodiscard]] bool update_editor_scene(
        const SceneRenderView& view,
        const std::filesystem::path& asset_root,
        std::string& error);
    [[nodiscard]] bool set_runtime_transform(SceneNodeId id, TransformRows transform) noexcept;
    void set_visibility_settings(VisibilitySettings settings) noexcept;
    [[nodiscard]] const VisibilitySettings &visibility_settings() const noexcept {
        return visibility_settings_;
    }
    [[nodiscard]] bool gpu_visibility_active() const noexcept {
        return gpu_visibility_active_ && mode_ != RunMode::instancing;
    }
    [[nodiscard]] bool gpu_visibility_available() const noexcept {
        return visibility_pipeline_.get() != VK_NULL_HANDLE;
    }
    [[nodiscard]] const char *visibility_backend() const noexcept {
        if (mode_ == RunMode::instancing) {
            return "direct";
        }
        return gpu_visibility_active_ ? (gpu_visibility_bypassed_ ? "direct" : "gpu") : "cpu";
    }
    [[nodiscard]] const char *cpu_culling_path() const noexcept {
        if (mode_ == RunMode::instancing || gpu_visibility_active_ ||
            scene_metadata_.empty() || !visibility_settings_.frustum) return "off";
        return bounds_hierarchy_active_ ? "hierarchy" : "linear";
    }
    [[nodiscard]] bool coarse_depth_order_active() const noexcept {
        return visibility_settings_.coarse_front_to_back && mode_ != RunMode::instancing &&
            !gpu_visibility_active_ && !scene_metadata_.empty();
    }
    [[nodiscard]] unsigned terrain_lod() const noexcept {
        return terrain_lod_;
    }
    [[nodiscard]] const VisibilityStatistics &visibility_statistics() const noexcept {
        return visibility_statistics_;
    }
    [[nodiscard]] bool apply_editor_material(
        std::string_view material_relative_path,
        PbrMaterial material,
        MaterialTessellation tessellation) noexcept;
    void invalidate_editor_scene() noexcept;
    [[nodiscard]] bool prepare_editor_viewport(VkExtent2D extent);
    [[nodiscard]] VkImageView editor_viewport_image_view() const noexcept {
        return editor_viewport_.color.view();
    }
    [[nodiscard]] VkExtent2D editor_viewport_extent() const noexcept {
        return VkExtent2D{
            editor_viewport_.color.width(),
            editor_viewport_.color.height(),
        };
    }
    [[nodiscard]] std::uint64_t editor_viewport_generation() const noexcept {
        return editor_viewport_generation_;
    }
    [[nodiscard]] std::optional<SceneNodeId> pick_editor_scene(
        glm::vec3 ray_origin,
        glm::vec3 ray_direction) const noexcept;
    [[nodiscard]] std::optional<Bounds> editor_node_bounds(
        SceneNodeId id) const noexcept;
    [[nodiscard]] std::uint64_t swapchain_generation() const noexcept {
        return swapchain_generation_;
    }
#endif

private:
    struct AntiAliasingFrameResources {
        struct MsaaTargets {
            GpuTexture color{};
            GpuTexture depth{};
            VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };
        GpuTexture color{};
        GpuTexture subsurface_base{};
        GpuTexture subsurface_ping{};
        GpuTexture motion_vectors{};
        GpuTexture depth{};
        GpuTexture depth_attachment{};
        GpuTexture smaa_edges{};
        GpuTexture smaa_weights{};
        GpuTexture fsr_output{};
        GpuTexture motion_tile_max{};
        GpuTexture motion_neighbor_max{};
        MsaaTargets msaa{};
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkDescriptorSet subsurface_horizontal_set = VK_NULL_HANDLE;
        VkDescriptorSet subsurface_vertical_set = VK_NULL_HANDLE;
        VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout subsurface_base_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout subsurface_ping_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_attachment_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout edges_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout weights_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout fsr_output_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout motion_tile_max_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout motion_neighbor_max_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
#if VCR_EDITOR_ENABLED
    struct EditorViewportResources {
        GpuTexture color{};
        GpuTexture depth{};
        VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct EditorModelSubmesh {
        std::string material_relative_path{};
        PbrMaterial material{};
        VkDescriptorSet material_descriptor_set = VK_NULL_HANDLE;
        MaterialTessellation tessellation{};
        glm::vec4 tessellation_uv_bounds{0, 0, 1, 1};
        std::uint32_t first_index = 0U;
        std::uint32_t index_count = 0U;
        std::uint32_t gi_first = 0, gi_count = 0;
        glm::vec3 gi_albedo{1};
        std::array<std::uint32_t, 3> lod_first{};
        std::array<std::uint32_t, 3> lod_count{};
        std::array<float, 3> lod_error{};
    };

    struct EditorModelAsset {
        std::string relative_path{};
        GpuBuffer vertex_buffer{};
        GpuBuffer index_buffer{};
        std::vector<EditorModelSubmesh> submeshes{};
        SceneTransform recommended_transform{};
        VkDeviceSize gpu_bytes = 0U;
        Bounds bounds{};
        bool ready = true;
    };

    struct EditorModelDraw {
        VkDescriptorSet material_descriptor_set = VK_NULL_HANDLE;
        MaterialTessellation tessellation{};
        glm::vec4 tessellation_uv_bounds{0, 0, 1, 1};
        std::uint32_t asset_index = 0U;
        std::uint32_t first_index = 0U;
        std::uint32_t index_count = 0U;
        std::uint32_t first_instance = 0U;
        std::uint32_t instance_count = 0U;
        std::uint32_t submesh_index = 0U;
    };

    struct EditorTextureAsset {
        std::string relative_path{};
        GpuTexture texture{};
        CookedTextureSemantic semantic =
            CookedTextureSemantic::unspecified;
        CookedTextureColorSpace color_space =
            CookedTextureColorSpace::unspecified;
        VkDeviceSize gpu_bytes = 0U;
    };
#endif

    void create_lighting_resources();
    void create_lighting_pipelines();
    void destroy_lighting() noexcept;
    void prepare_lighting(FrameContext &frame, const glm::mat4 &view, VkExtent2D extent);
    void record_voxel_geometry(VkCommandBuffer command, FrameContext &frame);
    void record_voxel_light(VkCommandBuffer command, FrameContext &frame);
    void start_voxel_job();
    void snapshot_voxel_scene();
    [[nodiscard]] std::uint32_t append_gi_triangles(std::span<const GiTriangle> triangles);
    [[nodiscard]] std::size_t gi_model_upload_bytes(const PreparedModel &model) const;
    void create_frame_contexts();
    void prepare_scene_packet(const glm::mat4 &view_projection, glm::vec3 camera,
                              VkExtent2D extent);
    void create_gpu_visibility();
    void destroy_gpu_visibility() noexcept;
    void build_gpu_visibility_scene(std::uint32_t plane_count, std::uint32_t plane_first,
                                    std::int32_t plane_vertex, std::uint32_t cube_count);
    void prepare_gpu_scene_packet(const glm::mat4 &vp, glm::vec3 camera, VkExtent2D extent);
    void record_gpu_visibility(VkCommandBuffer command, FrameContext &frame);
    void read_gpu_visibility(FrameContext &frame);
    void upload_gpu_scene_records(VkCommandBuffer command, FrameContext &frame);
    void destroy_frame_contexts() noexcept;
    void create_present_semaphores();
    void destroy_present_semaphores() noexcept;
    void create_pipeline();
    void destroy_pipeline() noexcept;
    void create_pipeline_cache();
    void save_pipeline_cache() const noexcept;
    [[nodiscard]] bool validate_pipeline_cache_data(
        std::span<const std::byte> data) const noexcept;
    void create_scene_resources();
    void destroy_scene_resources() noexcept;
    void create_sky_texture();
    void create_shadow_map();
    void apply_shadow_resolution();
    void create_anti_aliasing_resources();
    void create_anti_aliasing_targets();
    void invalidate_temporal_history() noexcept;
    void detect_msaa_support();
    void prepare_msaa_targets_for_frame(std::size_t frame_index);
    void destroy_anti_aliasing_resources() noexcept;
    void destroy_anti_aliasing_targets() noexcept;
    void create_display_mapping_resources();
    void destroy_display_mapping_resources() noexcept;
    void record_fog(
        VkCommandBuffer command_buffer,
        AntiAliasingFrameResources& resources,
        VkExtent2D extent);
    void record_auto_exposure(
        VkCommandBuffer command_buffer,
        FrameContext& frame,
        VkExtent2D source_extent,
        VkDescriptorSet descriptor_set);
    void read_exposure_state(FrameContext& frame);
    void render_static_shadow_map();
    void record_shadow_map(VkCommandBuffer command_buffer, FrameContext &frame);
    void create_descriptors();
    void create_anti_aliasing_descriptors();
    void destroy_anti_aliasing_descriptors() noexcept;
    void create_subsurface_targets();
    void destroy_subsurface_targets() noexcept;
    void create_subsurface_descriptors();
    void destroy_subsurface_descriptors() noexcept;
    void provision_subsurface_resources();
    void record_subsurface(
        VkCommandBuffer command_buffer,
        FrameContext& frame,
        AntiAliasingFrameResources& resources,
        VkExtent2D extent);
    void read_gpu_timestamps(FrameContext& frame, FrameTimings& timings) const;
    void read_local_light_statistics(FrameContext& frame);
    void complete_screenshot(FrameContext& frame);
    [[nodiscard]] bool prepare_screenshot_buffer();
    void upload_to_device_buffer(GpuBuffer &destination, std::span<const std::byte> bytes,
                                 VkDeviceSize destination_offset = 0);
    void upload_to_device_texture(
        GpuTexture& destination,
        std::span<const std::byte> pixels,
        std::uint32_t bytes_per_pixel,
        std::string_view debug_name);
    void update_frame_uniform(
        FrameContext& frame,
        const glm::mat4& view_matrix);
    void record_commands(
        VkCommandBuffer command_buffer,
        std::uint32_t image_index
#if VCR_EDITOR_ENABLED
        ,
        ImDrawData* editor_draw_data
#endif
    );
    void record_clustered_lights(VkCommandBuffer command_buffer, FrameContext& frame);
    void rebuild_swapchain();
    void recreate_swapchain(VkPresentModeKHR preferred_mode);
    [[nodiscard]] ShaderModule load_shader(const std::filesystem::path& path) const;
#if VCR_EDITOR_ENABLED
    void create_editor_viewport(VkExtent2D extent);
    void destroy_editor_viewport() noexcept;
    void create_editor_material_resources();
    [[nodiscard]] std::optional<std::uint32_t>
    find_or_load_editor_texture(const PreparedTexture &prepared, std::string &error);
    [[nodiscard]] std::optional<VkDescriptorSet>
        create_editor_material_descriptor(
        const GpuTexture& base_color,
        const GpuTexture& normal,
        const GpuTexture& metallic_roughness,
        const GpuTexture& displacement,
        std::string& error);
    [[nodiscard]] std::optional<std::uint32_t>
        find_or_load_editor_model(
            const std::filesystem::path& asset_root,
            std::string_view relative_path,
            std::string& error);
    void record_editor_scene_upload(
        VkCommandBuffer command_buffer,
        FrameContext& frame);
    [[nodiscard]] std::optional<std::uint32_t> publish_editor_model(const PreparedModel &prepared,
                                                                    std::string &error);
#endif
    [[nodiscard]] VkExtent2D scene_output_extent() const noexcept;

    struct alignas(16) GiJob {
        TransformRows transform{};
        glm::vec4 color{1};
        glm::uvec4 range{}; // first triangle, count, batch prefix, reserved
    };
    static_assert(sizeof(GiJob) == 80);
    LightingSettings lighting_settings_{};
    ToneMapper tone_mapper_ = ToneMapper::reinhard;
    ExposureSettings exposure_settings_{};
    ExposureGpuReadback exposure_readback_{};
    FogSettings fog_settings_{};
    ColorGrading color_grading_ = ColorGrading::off;
    ColorGradingLutSize color_grading_lut_size_ = ColorGradingLutSize::size_32;
    SubsurfaceMode subsurface_mode_ = SubsurfaceMode::off;
    MotionBlurSettings motion_blur_settings_{};
    DepthPrepass depth_prepass_ = DepthPrepass::off;
    LodFadePattern lod_fade_pattern_ = LodFadePattern::off;
    float lod_fade_seconds_ = 0.25F;
    bool lighting_paused_ = false;
    bool fog_supported_ = false;
    LightingStatistics lighting_statistics_{};
    LightingUniforms lighting_uniforms_{};
    DeviceObject<VkDescriptorSetLayout> lighting_layout_{};
    DeviceObject<VkDescriptorPool> lighting_pool_{};
    DeviceObject<VkPipelineLayout> lighting_compute_layout_{};
    DeviceObject<VkPipeline> voxel_geometry_pipeline_{}, voxel_light_pipeline_{};
    DeviceObject<VkPipeline> local_light_cluster_pipeline_{};
    DeviceObject<VkImageView> voxel_storage_view_{};
    DeviceObject<VkPipeline> tonemap_pipeline_{};
    DeviceObject<VkPipeline> fog_pipeline_{};
    DeviceObject<VkPipeline> exposure_histogram_pipeline_{};
    DeviceObject<VkPipeline> exposure_adapt_pipeline_{};
    DeviceObject<VkPipeline> subsurface_horizontal_pipeline_{};
    DeviceObject<VkPipeline> subsurface_vertical_pipeline_{};
    GpuBuffer exposure_state_buffer_{};
    GpuTexture exposure_value_{};
    VkImageLayout exposure_value_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    GpuTexture grading_lut_32_{};
    GpuTexture grading_lut_64_{};
    bool exposure_reset_pending_ = true;
    bool subsurface_resources_provisioned_ = false;
    bool subsurface_provision_pending_ = false;
    GpuBuffer voxel_geometry_{}, voxel_triangles_{};
    GpuTexture voxel_radiance_{};
    VkImageLayout voxel_radiance_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    DirtyVoxelBricks voxel_dirty_{};
    std::array<std::uint32_t, gi_brick_count> voxel_job_mask_{};
    std::vector<GiJob> voxel_jobs_{}, voxel_batch_{};
    std::vector<GpuPunctualLight> local_lights_{};
    std::uint64_t local_light_revision_ = 1U;
    LocalLightBackend local_light_backend_ = LocalLightBackend::automatic;
    LocalLightStatistics local_light_statistics_{};
    VoxelSceneTracker voxel_scene_{};
    glm::vec4 gi_last_sun_{}, gi_last_sun_color_{};
    std::uint32_t voxel_job_index_ = 0, voxel_job_triangle_ = 0, voxel_triangle_count_ = 0;
    std::uint32_t voxel_batch_triangles_ = 0;
    std::uint32_t gi_cube_first_ = 0, gi_plane_first_ = 0, gi_terrain_first_ = 0,
                  gi_terrain_count_ = 0;
    std::uint64_t gi_scene_revision_ = 0;
    bool gi_provisioned_ = false, voxel_job_active_ = false, voxel_job_start_ = false;
    bool voxel_ready_ = false, voxel_light_due_ = false, voxel_job_finishes_ = false;
    bool lighting_shadow_initialized_ = false;
    glm::mat4 lighting_shadow_view_{1};
    double lighting_shadow_hour_ = 0;
    float lighting_shadow_aspect_ = 0;
    double gi_light_elapsed_ = 1000.0, gi_geometry_elapsed_ = 1000.0;
    bool gi_geometry_urgent_ = false;
    std::chrono::steady_clock::time_point gi_dirty_since_{};
    VulkanContext* context_ = nullptr;
    SDL_Window* window_ = nullptr;
    Swapchain swapchain_{};
    std::array<FrameContext, max_frames_in_flight> frames_{};
    std::vector<VkSemaphore> present_complete_semaphores_{};
    std::uint32_t frame_index_ = 0;
    bool resize_requested_ = false;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline scene_pipeline_ = VK_NULL_HANDLE;
    VkPipeline terrain_pipeline_ = VK_NULL_HANDLE;
    VkPipeline sky_pipeline_ = VK_NULL_HANDLE;
    VkPipeline scene_shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline terrain_shadow_pipeline_ = VK_NULL_HANDLE;
    DeviceObject<VkPipeline> authored_shadow_pipeline_{};
    DeviceObject<VkPipeline> model_shadow_pipeline_{};
    DeviceObject<VkPipeline> tessellated_model_shadow_pipeline_{};
    VkImageLayout shadow_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    DeviceObject<VkPipeline> aa_pipeline_{};
    DeviceObject<VkPipeline> aa_scene_pipeline_{};
#if VCR_EDITOR_ENABLED
    VkPipeline editor_model_pipeline_ = VK_NULL_HANDLE;
    VkPipeline tessellated_editor_model_pipeline_ = VK_NULL_HANDLE;
    DeviceObject<VkPipeline> aa_editor_model_pipeline_{};
    DeviceObject<VkPipeline> aa_tessellated_editor_model_pipeline_{};
#endif
    DeviceObject<VkPipeline> aa_terrain_pipeline_{};
    DeviceObject<VkPipeline> aa_sky_pipeline_{};
    // Spatial AA needs only HDR color; temporal AA additionally writes motion
    // and depth. Prewarm both paths so switching methods does not compile.
    struct SpatialPipelines {
        DeviceObject<VkPipeline> cubes{};
        DeviceObject<VkPipeline> scene{};
        DeviceObject<VkPipeline> models{};
        DeviceObject<VkPipeline> tessellated_models{};
        DeviceObject<VkPipeline> terrain{};
        DeviceObject<VkPipeline> sky{};
    } smaa_geometry_{};
    // All variants are prewarmed and retire with the other swapchain pipelines.
    // Depth-only variants use the exact corresponding color-pass vertex module.
    SpatialPipelines depth_geometry_{}, depth_temporal_geometry_{};
    DeviceObject<VkPipeline> lod_depth_model_{}, lod_depth_temporal_model_{};
    DeviceObject<VkPipeline> lod_depth_tessellated_model_{},
        lod_depth_temporal_tessellated_model_{};
    SpatialPipelines equal_direct_{}, equal_spatial_{}, equal_temporal_{};
    std::array<SpatialPipelines, 2> msaa_geometry_{};
    std::array<SpatialPipelines, 2> depth_msaa_geometry_{};
    std::array<SpatialPipelines, 2> equal_msaa_{};
    std::array<DeviceObject<VkPipeline>, 2> lod_depth_msaa_model_{};
    std::array<DeviceObject<VkPipeline>, 2>
        lod_depth_msaa_tessellated_model_{};
    SpatialPipelines subsurface_spatial_{}, subsurface_temporal_{};
    SpatialPipelines equal_subsurface_spatial_{}, equal_subsurface_temporal_{};
    VkDescriptorSetLayout anti_aliasing_descriptor_set_layout_ =
        VK_NULL_HANDLE;
    VkPipelineLayout post_process_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout exposure_pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout subsurface_descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout subsurface_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout hud_pipeline_layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> smaa_edge_pipelines_{};
    std::array<VkPipeline, 4> smaa_blurred_edge_pipelines_{};
    std::array<VkPipeline, 4> smaa_blend_pipelines_{};
    VkPipeline smaa_neighborhood_pipeline_ = VK_NULL_HANDLE;
    VkPipeline smaa_blurred_neighborhood_pipeline_ = VK_NULL_HANDLE;
    VkPipeline smaa_neighborhood_history_pipeline_ = VK_NULL_HANDLE;
    VkPipeline smaa_temporal_resolve_pipeline_ = VK_NULL_HANDLE;
    VkPipeline motion_blur_pipeline_ = VK_NULL_HANDLE;
    VkPipeline motion_blur_reconstructed_pipeline_ = VK_NULL_HANDLE;
    VkPipeline motion_blur_hdr_pipeline_ = VK_NULL_HANDLE;
    VkPipeline motion_tile_max_pipeline_ = VK_NULL_HANDLE;
    VkPipeline motion_neighbor_max_pipeline_ = VK_NULL_HANDLE;
    VkPipeline fsr_composite_pipeline_ = VK_NULL_HANDLE;
    VkPipeline hud_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool anti_aliasing_descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool subsurface_descriptor_pool_ = VK_NULL_HANDLE;
    GpuBuffer vertex_buffer_{};
    GpuBuffer index_buffer_{};
    GpuBuffer instance_buffer_{};
    GpuBuffer demo_instance_buffer_{};
    GpuBuffer pbr_sample_vertex_buffer_{};
    GpuBuffer pbr_sample_index_buffer_{};
    RenderFixture fixture_ = RenderFixture::none;
    std::uint32_t pbr_sample_instance_count_ = 10;
    GpuBuffer terrain_vertex_buffer_{};
    GpuBuffer terrain_index_buffer_{};
    std::array<std::uint32_t, 3> terrain_lod_first_{}, terrain_lod_count_{};
    std::array<float, 3> terrain_lod_error_{};
    Bounds terrain_bounds_{};
    unsigned terrain_lod_ = 0;
    GpuTexture sky_texture_{};
    GpuTexture shadow_map_{};
    GpuTexture smaa_area_texture_{};
    GpuTexture smaa_search_texture_{};
    std::array<AntiAliasingFrameResources, max_frames_in_flight>
        anti_aliasing_frames_{};
    Fsr3Upscaler fsr3_upscaler_{};
    VkFormat shadow_map_format_ = VK_FORMAT_UNDEFINED;
    bool shadow_map_linear_ = false;
    std::uint32_t max_shadow_resolution_ = 0;
    glm::mat4 light_view_projection_{1.0F};
    glm::mat4 previous_unjittered_view_projection_{1.0F};
    Fsr3Jitter current_jitter_{};
    VkExtent2D current_render_extent_{};
    GpuBuffer screenshot_readback_{};
    std::uint32_t instancing_count_ = 1;
    std::uint32_t max_instance_capacity_ = 100'000;
    std::uint32_t gpu_workload_iterations_ = 0;
    std::uint32_t frame_serial_ = 0;
    RunMode mode_ = RunMode::scene;
    AntiAliasingSettings anti_aliasing_{};
    AntiAliasingMethod anti_aliasing_requested_method_ = AntiAliasingMethod::off;
    bool smaa_t2x_history_valid_ = false;
    std::uint32_t smaa_t2x_phase_ = 0U;
    VkSampleCountFlags msaa_supported_sample_counts_ = VK_SAMPLE_COUNT_1_BIT;
    PerformanceMetrics metrics_{};
    bool screenshot_requested_ = false;
    bool screenshot_in_flight_ = false;
    bool temporal_history_valid_ = false;
    float fsr_frame_time_ms_ = 16.6667F;
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::optional<ScreenshotCapture> completed_screenshot_{};
#if VCR_EDITOR_ENABLED
    EditorViewportResources editor_viewport_{};
    VkDescriptorSetLayout editor_material_descriptor_set_layout_ =
        VK_NULL_HANDLE;
    VkDescriptorPool editor_material_descriptor_pool_ =
        VK_NULL_HANDLE;
    std::vector<SceneGpuInstance> editor_scene_instances_{};
    struct SceneInstanceMetadata {
        SceneNodeId id{};
        Bounds local_bounds{};
        bool occluder = false;
    };
    RuntimeTransforms runtime_transforms_{};
    std::vector<SceneInstanceMetadata> scene_metadata_{};
    std::vector<RuntimeGpuInstance> scene_packet_{};
    std::vector<EditorModelDraw> visible_model_draws_{};
    std::vector<std::uint32_t> visible_indices_{}, visibility_scratch_{};
    std::vector<std::uint32_t> coarse_depth_keys_{};
    std::vector<std::uint8_t> instance_lods_{};
    std::vector<LodFadeState> instance_lod_fades_{};
    bool lod_fades_active_ = false, reset_lod_fades_ = true;
    glm::vec3 lod_camera_position_{};
    glm::vec3 lod_camera_forward_{};
    bool lod_camera_valid_ = false;
    std::vector<std::uint8_t> instance_visibility_{};
    std::vector<Bounds> world_bounds_{};
    BoundsHierarchy world_hierarchy_{};
    bool hierarchy_rebuild_required_ = true;
    bool bounds_hierarchy_active_ = false;
    VisibilitySettings visibility_settings_{};
    VisibilityStatistics visibility_statistics_{};
    glm::mat4 packet_view_projection_{0.0F};
    VkExtent2D packet_extent_{};
    std::uint64_t packet_runtime_revision_ = 0, packet_document_revision_ = 0;
    std::uint64_t packet_revision_ = 1;
    std::uint64_t source_packet_revision_ = 1;
    std::uint32_t visible_ground_count_ = 0, visible_cube_count_ = 0;
    bool shadow_dirty_ = true;
    std::uint32_t shadow_matrix_dirty_mask_ = 0;
    DeviceObject<VkDescriptorSetLayout> visibility_layout_;
    DeviceObject<VkPipelineLayout> visibility_pipeline_layout_;
    DeviceObject<VkDescriptorPool> visibility_pool_;
    DeviceObject<VkPipeline> visibility_pipeline_;
    GpuBuffer visibility_lod_history_;
    std::vector<VisibilitySource> visibility_sources_;
    std::vector<VisibilityGroup> visibility_groups_;
    std::vector<VkDrawIndexedIndirectCommand> visibility_commands_;
    std::vector<std::uint32_t> node_record_heads_, node_record_next_;
    std::array<std::uint32_t, 64> occluder_candidates_{};
    std::uint32_t occluder_candidate_count_ = 0;
    std::vector<VkBufferCopy> record_copies_;
    std::uint64_t gpu_scene_revision_ = 1, gpu_packet_revision_ = 0;
    std::uint64_t gpu_visibility_revision_ = 1;
    bool gpu_visibility_active_ = false, reset_lod_history_ = true;
    bool gpu_visibility_bypassed_ = false;
    AssetPreparation asset_preparation_;
    UploadBatch asset_upload_;
    struct AssetRequest {
        std::filesystem::path root;
        std::string path, error;
        bool queued = false, done = false;
    };
    std::vector<AssetRequest> asset_requests_;
    std::optional<std::uint32_t> uploading_model_;
    std::optional<std::size_t> uploading_request_;
    std::size_t asset_requests_pending_ = 0;
    bool async_assets_ = false;
    std::vector<EditorModelAsset> editor_model_assets_{};
    std::vector<EditorModelDraw> editor_model_draws_{};
    std::vector<EditorTextureAsset> editor_texture_assets_{};
    std::vector<std::uint32_t> editor_model_sort_scratch_{};
    GpuTexture editor_default_base_color_texture_{};
    GpuTexture editor_default_normal_texture_{};
    GpuTexture editor_default_metallic_roughness_texture_{};
    GpuTexture editor_default_displacement_texture_{};
    VkDeviceSize editor_model_gpu_bytes_ = 0U;
    VkDeviceSize editor_texture_gpu_bytes_ = 0U;
    std::uint32_t editor_material_descriptor_count_ = 0U;
    std::uint32_t tessellated_material_count_ = 0U;
    bool model_tessellation_enabled_ = false;
    std::uint32_t editor_ground_count_ = 0U;
    std::uint32_t editor_cube_count_ = 0U;
    std::uint32_t authored_scene_object_count_ = 0U;
    std::uint64_t authored_scene_triangle_count_ = 0U;
    std::uint64_t editor_scene_revision_ = 1U;
    std::uint64_t editor_viewport_generation_ = 0U;
    std::uint64_t swapchain_generation_ = 1U;
    bool editor_terrain_visible_ = false;
    bool editor_sky_visible_ = false;
    bool editor_sun_visible_ = false;
    bool authored_scene_enabled_ = false;
    bool editor_output_enabled_ = false;
#endif
};

} // namespace vcr
