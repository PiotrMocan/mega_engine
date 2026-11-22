#include "renderer.hpp"
#if VCR_EDITOR_ENABLED
#include "foliage_proxy_bake.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

namespace vcr {
namespace {
VkRect2D projected_rectangle(const glm::mat4& vp, glm::vec3 bounds_low, glm::vec3 bounds_high,
                             VkExtent2D extent, float& nearest) noexcept {
    glm::vec2 low(1), high(-1);
    nearest = std::numeric_limits<float>::max();
    for (unsigned i = 0; i < 8; ++i) {
        const glm::vec3 corner{(i & 1) ? bounds_high.x : bounds_low.x, (i & 2) ? bounds_high.y : bounds_low.y,
                               (i & 4) ? bounds_high.z : bounds_low.z};
        const auto clip = vp * glm::vec4(corner, 1);
        nearest = std::min(nearest, clip.w);
        if (clip.w <= 0.001F)
            continue;
        const auto uv = glm::vec2(clip) / clip.w;
        low = glm::min(low, uv);
        high = glm::max(high, uv);
    }
    if (nearest <= .001F)
        return {{0, 0}, extent};
    const auto size = glm::vec2(extent.width, extent.height);
    const glm::ivec2 p =
        glm::ivec2(glm::clamp(glm::floor((low * .5F + .5F) * size) - 1.F, glm::vec2(0), size));
    const glm::ivec2 q =
        glm::ivec2(glm::clamp(glm::ceil((high * .5F + .5F) * size) + 1.F, glm::vec2(0), size));
    return {{p.x, p.y},
            {static_cast<unsigned>(std::max(0, q.x - p.x)), static_cast<unsigned>(std::max(0, q.y - p.y))}};
}
} // namespace

void Renderer::configure_foliage_proxy(const std::filesystem::path& override_manifest, const SceneRenderView& view,
                                       const std::filesystem::path& asset_root) {
    // Startup only. Every failure before publication leaves the scene on its polygon path.
    if (foliage_proxy_.pipeline.get())
        return;
    try {
        const auto manifest = override_manifest.empty()
            ? asset_root / "config" / "foliage_proxy.json" : override_manifest;
        if (override_manifest.empty() && !std::filesystem::exists(manifest))
            return;
        if (std::filesystem::file_size(manifest) > 1024 * 1024)
            throw std::length_error("Foliage manifest exceeds 1 MiB");
        std::ifstream file(manifest);
        const auto settings = nlohmann::json::parse(file);
        const auto tile = settings.at("tile").get<std::array<int, 2>>();
        const auto models = settings.at("models").get<std::set<std::string, std::less<>>>();
        std::vector<SceneModelInstance> selected;
        std::vector<std::uint32_t> member(max_scene_nodes);
        for (const auto& m : view.models) {
            if (!models.contains(m.asset_path) || std::floor(double(m.model_row_0.w) / 32.0) != tile[0] ||
                std::floor(double(m.model_row_2.w) / 32.0) != tile[1])
                continue;
            selected.push_back(m);
            if (m.id && m.id.index < member.size())
                member[m.id.index] = m.id.generation;
        }
        if (selected.empty())
            throw std::runtime_error("No matching instances in the 32 m tile");
        unsigned resolution = settings.value("resolution", 64U);
        float threshold = settings.value("max_cell_pixels", 1.25F);
        if (!foliage::valid_resolution(resolution) || !std::isfinite(threshold) || threshold < .1F ||
            threshold > 16.F)
            throw std::invalid_argument("Invalid foliage resolution or pixel threshold");
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(context_->allocator(), budgets.data());
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(context_->physical_device(), &memory);
        VkDeviceSize available = 0;
        for (unsigned i = 0; i < memory.memoryHeapCount; ++i)
            if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                available =
                    std::max(available, budgets[i].budget - std::min(budgets[i].budget, budgets[i].usage));
        while (resolution > 8) {
            const auto cells = std::uint64_t(resolution) * resolution * resolution;
            if (cells * (foliage::accumulator_bytes + 32) <= available / 8 &&
                cells * foliage::accumulator_bytes <= context_->properties().limits.maxStorageBufferRange) break;
            resolution /= 2;
        }
        const auto start = std::chrono::steady_clock::now();
        const auto input = foliage::prepare_tile(asset_root, selected, resolution);
        std::unique_ptr<char, decltype(&SDL_free)> pref(SDL_GetPrefPath("VCR", "FoliageProxy"), SDL_free);
        std::filesystem::path cache;
        if (pref)
            cache = std::filesystem::path(reinterpret_cast<const char8_t*>(pref.get()));
        foliage::Tile volume;
        const bool hit = !cache.empty() && foliage::load_tile(cache, input.key, volume);
        if (!hit) {
            const auto base = SDL_GetBasePath();
            if (!base)
                throw std::runtime_error("Cannot locate foliage compute shaders");
            const std::filesystem::path install(reinterpret_cast<const char8_t*>(base));
            auto shaders = install / "shaders";
            if (!std::filesystem::exists(shaders / "foliage_proxy_bake.comp.spv"))
                shaders = install / "engine" / "shaders";
            volume = foliage::bake_tile(*context_, input, shaders);
            if (!cache.empty() && !foliage::store_tile(cache, input.key, volume, 512ULL << 20))
                std::clog << "Foliage cache unavailable for publication\n";
        }
        FoliageProxyRender next;
        next.origin = {volume.origin[0], volume.origin[1], volume.origin[2]};
        const auto bounds = foliage::occupied_bounds(volume);
        if (!bounds.high[0])
            throw std::runtime_error("No occupied foliage cells");
        // Pad the scissor by one pixel in projected_rectangle for cell-boundary sampling.
        for (int a = 0; a < 3; ++a) {
            next.bounds_low[a] = next.origin[a] + volume.cell_size * bounds.low[a];
            next.bounds_high[a] = next.origin[a] + volume.cell_size * bounds.high[a];
        }
        next.cell_size = volume.cell_size;
        next.resolution = volume.resolution;
        next.max_cell_pixels = threshold;
        next.scene_revision = view.revision;
        next.depth_format = swapchain_.depth_format();
        next.lighting_layout = lighting_layout_.get();
        const auto bytes = std::as_bytes(std::span(volume.words));
        if (bytes.size() > context_->properties().limits.maxStorageBufferRange)
            throw std::length_error("Proxy descriptor range");
        next.volume.create(*context_, bytes.size(),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, "static foliage proxy bricks");
        struct Params {
            glm::vec4 origin_cell;
            glm::uvec4 grid;
        };
        const Params params{{next.origin, next.cell_size}, {resolution, 0, 0, 0}};
        next.parameters.create(
            *context_, sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            "foliage proxy grid");
        next.parameters.write(std::as_bytes(std::span{&params, 1}));
        const auto device = context_->device();
        const std::array bindings{VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                  VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = bindings.data()};
        VK_CHECK(vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                             next.set_layout.put(device, vkDestroyDescriptorSetLayout)));
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                               VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = 1,
                                              .poolSizeCount = 2,
                                              .pPoolSizes = sizes.data()};
        VK_CHECK(
            vkCreateDescriptorPool(device, &pool, nullptr, next.pool.put(device, vkDestroyDescriptorPool)));
        const auto tile_layout = next.set_layout.get();
        const VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = next.pool.get(),
                                                .descriptorSetCount = 1,
                                                .pSetLayouts = &tile_layout};
        VK_CHECK(vkAllocateDescriptorSets(device, &alloc, &next.descriptors));
        const std::array infos{VkDescriptorBufferInfo{next.volume.handle(), 0, bytes.size()},
                               VkDescriptorBufferInfo{next.parameters.handle(), 0, sizeof(params)}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (unsigned i = 0; i < 2; ++i)
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                         .dstSet = next.descriptors,
                         .dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = bindings[i].descriptorType,
                         .pBufferInfo = &infos[i]};
        vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
        const std::array layouts{descriptor_set_layout_, tile_layout, next.lighting_layout};
        const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 3,
                                                .pSetLayouts = layouts.data(),
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &range};
        VK_CHECK(vkCreatePipelineLayout(device, &layout, nullptr,
                                        next.layout.put(device, vkDestroyPipelineLayout)));
        const auto vs = load_shader("sky.vert.spv"), fs = load_shader("foliage_proxy.frag.spv");
        const std::array stages{
            VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                            .module = vs.get(),
                                            .pName = "main"},
            VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                            .module = fs.get(),
                                            .pName = "main"}};
        const VkPipelineVertexInputStateCreateInfo vertex{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1};
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1};
        const VkPipelineMultisampleStateCreateInfo samples{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        const VkPipelineDepthStencilStateCreateInfo depth{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL};
        const VkPipelineColorBlendAttachmentState blend{.blendEnable = VK_TRUE,
                                                        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                        .dstColorBlendFactor =
                                                            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                        .colorBlendOp = VK_BLEND_OP_ADD,
                                                        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                        .alphaBlendOp = VK_BLEND_OP_ADD,
                                                        .colorWriteMask = 0xF};
        const VkPipelineColorBlendStateCreateInfo color{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blend};
        const std::array states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dyn{.sType =
                                                       VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = 2,
                                                   .pDynamicStates = states.data()};
        const VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT;
        const VkPipelineRenderingCreateInfo rendering{.sType =
                                                          VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                      .colorAttachmentCount = 1,
                                                      .pColorAttachmentFormats = &hdr,
                                                      .depthAttachmentFormat = next.depth_format};
        const VkGraphicsPipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                    .pNext = &rendering,
                                                    .stageCount = 2,
                                                    .pStages = stages.data(),
                                                    .pVertexInputState = &vertex,
                                                    .pInputAssemblyState = &assembly,
                                                    .pViewportState = &viewport,
                                                    .pRasterizationState = &raster,
                                                    .pMultisampleState = &samples,
                                                    .pDepthStencilState = &depth,
                                                    .pColorBlendState = &color,
                                                    .pDynamicState = &dyn,
                                                    .layout = next.layout.get()};
        VK_CHECK(vkCreateGraphicsPipelines(device, pipeline_cache_, 1, &pipeline, nullptr,
                                           next.pipeline.put(device, vkDestroyPipeline)));
        context_->set_debug_name(VK_OBJECT_TYPE_PIPELINE, handle_value(next.pipeline.get()),
                                 "foliage proxy ray traversal");
        UploadBatch upload;
        upload.begin(*context_, bytes.size());
        const VkBufferCopy copy{upload.stage(bytes), 0, bytes.size()};
        vkCmdCopyBuffer(upload.command_buffer(), upload.buffer(), next.volume.handle(), 1, &copy);
        const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                       .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        const VkDependencyInfo dep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(upload.command_buffer(), &dep);
        upload.submit();
        upload.wait();
        for (std::size_t i = 0; i < scene_metadata_.size(); ++i) {
            auto& m = scene_metadata_[i];
            m.foliage_proxy_member =
                m.id && m.id.index < member.size() && member[m.id.index] == m.id.generation;
            next.source_records += static_cast<unsigned>(m.foliage_proxy_member);
            if (i < visibility_sources_.size())
                visibility_sources_[i].extent.w = m.foliage_proxy_member ? 1.F : 0.F;
        }
        next.member_generations = std::move(member);
        ++gpu_scene_revision_;
        foliage_proxy_ = std::move(next);
        std::clog
            << "Foliage proxy " << (hit ? "cache hit" : "baked") << ", resolution " << resolution
            << ", instances " << selected.size() << ", bytes " << bytes.size() << ", startup ms "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
            << '\n';
    } catch (const std::exception& e) {
        std::clog << "Foliage proxy polygon fallback: " << e.what() << '\n';
    }
}

void Renderer::update_foliage_proxy(const glm::mat4& vp, VkExtent2D extent) {
    auto& tile = foliage_proxy_;
    const auto previous = tile.phase;
    const bool supported =
        tile.scene_revision && tile.scene_revision == editor_scene_revision_ && visibility_settings_.lod &&
        (anti_aliasing_.method == AntiAliasingMethod::off ||
         (anti_aliasing_.method == AntiAliasingMethod::smaa && !is_temporal_smaa(anti_aliasing_))) &&
        motion_blur_mode_actual() == MotionBlurMode::off && subsurface_mode_actual() == SubsurfaceMode::off &&
        ray_tracing_mode_actual() == RayTracingMode::off && tile.lighting_layout == lighting_layout_.get() &&
        tile.depth_format == swapchain_.depth_format();
    if (supported) {
        float nearest{};
        tile.scissor = projected_rectangle(vp, tile.bounds_low, tile.bounds_high, extent, nearest);
        const float pixels = .5F * float(extent.height) / std::tan(glm::radians(vertical_fov_degrees_) * .5F);
        const float projected = tile.cell_size * pixels / std::max(nearest, .001F);
        tile.far = nearest > 0 && projected < tile.max_cell_pixels * (tile.far ? 1.15F : .85F);
        const unsigned step =
            static_cast<unsigned>(std::clamp(fsr_frame_time_ms_ * 4.095F / lod_fade_seconds_, 1.F, 4095.F));
        tile.phase = tile.far ? std::min(4095U, previous + step) : previous - std::min(previous, step);
    } else {
        tile.phase = 0;
        tile.far = false;
    }
    if (previous != tile.phase) {
        packet_view_projection_ = glm::mat4(0);
        hiz_valid_ = false;
    }
}

void Renderer::draw_foliage_proxy(VkCommandBuffer cmd, FrameContext& frame, VkExtent2D extent) {
    const auto& tile = foliage_proxy_;
    if (!tile.phase || !tile.scissor.extent.width || !tile.scissor.extent.height)
        return;
    const std::array sets{frame.scene_descriptor_set, tile.descriptors, frame.lighting_descriptor_set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tile.pipeline.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tile.layout.get(), 0, 3, sets.data(), 0,
                            nullptr);
    const std::array push{0U, 0U, 0U, tile.phase};
    vkCmdPushConstants(cmd, tile.layout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, push.data());
    vkCmdSetScissor(cmd, 0, 1, &tile.scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    const VkRect2D full{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &full);
}
} // namespace vcr
#endif
