#pragma once

#include "device_object.hpp"
#include "runtime_scene.hpp"
#include "asset_preparation.hpp"
#include "upload_batch.hpp"
#include "frame_context.hpp"
#include "fsr3_upscaler.hpp"
#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"
#include "cli_options.hpp"
#include "performance_metrics.hpp"
#include "swapchain.hpp"
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

    void initialize(
        VulkanContext& context,
        SDL_Window* window,
        VkPresentModeKHR preferred_present_mode,
        RunMode initial_mode,
        std::uint32_t initial_instances,
        std::uint32_t initial_gpu_workload,
        AntiAliasingSettings initial_anti_aliasing,
        bool enable_screenshot_capture,
        bool enable_editor_output = false,
        bool enable_authored_scene = false);
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
    [[nodiscard]] unsigned terrain_lod() const noexcept {
        return terrain_lod_;
    }
    [[nodiscard]] const VisibilityStatistics &visibility_statistics() const noexcept {
        return visibility_statistics_;
    }
    [[nodiscard]] bool apply_editor_material_factors(
        std::string_view material_relative_path,
        PbrMaterial material) noexcept;
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
    [[nodiscard]] std::uint64_t swapchain_generation() const noexcept {
        return swapchain_generation_;
    }
#endif

private:
    struct AntiAliasingFrameResources {
        GpuTexture color{};
        GpuTexture motion_vectors{};
        GpuTexture depth{};
        GpuTexture depth_attachment{};
        GpuTexture smaa_edges{};
        GpuTexture smaa_weights{};
        GpuTexture fsr_output{};
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkImageLayout color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout depth_attachment_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout edges_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout weights_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout fsr_output_layout = VK_IMAGE_LAYOUT_UNDEFINED;
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
        std::uint32_t first_index = 0U;
        std::uint32_t index_count = 0U;
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
    void create_anti_aliasing_resources();
    void create_anti_aliasing_targets();
    void destroy_anti_aliasing_resources() noexcept;
    void destroy_anti_aliasing_targets() noexcept;
    void render_static_shadow_map();
    void record_shadow_map(VkCommandBuffer command_buffer, FrameContext &frame);
    void create_descriptors();
    void create_anti_aliasing_descriptors();
    void destroy_anti_aliasing_descriptors() noexcept;
    void read_gpu_timestamps(FrameContext& frame, FrameTimings& timings) const;
    void complete_screenshot(FrameContext& frame);
    [[nodiscard]] bool prepare_screenshot_buffer();
    void upload_to_device_buffer(
        GpuBuffer& destination,
        std::span<const std::byte> bytes);
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
    VkImageLayout shadow_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    DeviceObject<VkPipeline> aa_pipeline_{};
    DeviceObject<VkPipeline> aa_scene_pipeline_{};
#if VCR_EDITOR_ENABLED
    VkPipeline editor_model_pipeline_ = VK_NULL_HANDLE;
    DeviceObject<VkPipeline> aa_editor_model_pipeline_{};
#endif
    DeviceObject<VkPipeline> aa_terrain_pipeline_{};
    DeviceObject<VkPipeline> aa_sky_pipeline_{};
    // Spatial AA needs only HDR color; temporal AA additionally writes motion
    // and depth. Prewarm both paths so switching methods does not compile.
    struct SpatialPipelines {
        DeviceObject<VkPipeline> cubes{};
        DeviceObject<VkPipeline> scene{};
        DeviceObject<VkPipeline> models{};
        DeviceObject<VkPipeline> terrain{};
        DeviceObject<VkPipeline> sky{};
    } smaa_geometry_{};
    VkDescriptorSetLayout anti_aliasing_descriptor_set_layout_ =
        VK_NULL_HANDLE;
    VkPipelineLayout post_process_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout hud_pipeline_layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> smaa_edge_pipelines_{};
    std::array<VkPipeline, 4> smaa_blend_pipelines_{};
    VkPipeline smaa_neighborhood_pipeline_ = VK_NULL_HANDLE;
    VkPipeline fsr_composite_pipeline_ = VK_NULL_HANDLE;
    VkPipeline hud_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool anti_aliasing_descriptor_pool_ = VK_NULL_HANDLE;
    GpuBuffer vertex_buffer_{};
    GpuBuffer index_buffer_{};
    GpuBuffer instance_buffer_{};
    GpuBuffer demo_instance_buffer_{};
    GpuBuffer pbr_sample_vertex_buffer_{};
    GpuBuffer pbr_sample_index_buffer_{};
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
    std::vector<std::uint8_t> instance_lods_{};
    std::vector<std::uint8_t> instance_visibility_{};
    std::vector<Bounds> world_bounds_{};
    VisibilitySettings visibility_settings_{};
    VisibilityStatistics visibility_statistics_{};
    glm::mat4 packet_view_projection_{0.0F};
    VkExtent2D packet_extent_{};
    std::uint64_t packet_runtime_revision_ = 0, packet_document_revision_ = 0;
    std::uint64_t packet_revision_ = 1;
    std::uint32_t visible_ground_count_ = 0, visible_cube_count_ = 0;
    bool shadow_dirty_ = true;
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
    VkDeviceSize editor_model_gpu_bytes_ = 0U;
    VkDeviceSize editor_texture_gpu_bytes_ = 0U;
    std::uint32_t editor_material_descriptor_count_ = 0U;
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
