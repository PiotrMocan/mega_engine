#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include "gpu_texture.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace {
constexpr std::uint32_t lut_size = 32;
constexpr std::size_t lut_texel_count = lut_size * lut_size * lut_size;
// Hardware linear filtering is allowed finite sub-texel interpolation precision,
// while tetrahedral interpolation is explicit FP32 arithmetic over FP16 texels.
// Keep separate absolute tolerances so the test remains portable without hiding
// a wrong cell, channel, clamp, or tetrahedron selection.
constexpr float trilinear_tolerance = 3.0e-3F;
constexpr float tetrahedral_tolerance = 8.0e-5F;

using Float3 = std::array<float, 3>;
using Double3 = std::array<double, 3>;
using PackedTexel = std::uint64_t;
using Pixel = std::array<float, 4>;

struct SdlLifetime {
    SdlLifetime() { if (!SDL_Init(SDL_INIT_VIDEO)) vcr::fatal(SDL_GetError()); }
    ~SdlLifetime() { SDL_Quit(); }
};
struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept { SDL_DestroyWindow(window); }
};
struct SubmissionRetirement {
    VkDevice device; // Observer; all submitted resources outlive this guard.
    bool pending = false;
    ~SubmissionRetirement() {
        if (pending) {
            const auto result = vkDeviceWaitIdle(device);
            if (result != VK_SUCCESS)
                std::cerr << "Color-grading test retirement failed: VkResult " << result << '\n';
        }
    }
};

struct alignas(16) LutCase {
    std::array<float, 4> color{};
    std::array<std::uint32_t, 4> control{};
};
static_assert(sizeof(LutCase) == 32);
static_assert(offsetof(LutCase, control) == 16);
static_assert(std::is_trivially_copyable_v<LutCase>);
static_assert(std::endian::native == std::endian::little);

struct LutVolume {
    std::vector<PackedTexel> packed;
    std::vector<Double3> decoded;
};

Double3 evaluate_lut(const std::uint32_t kind, const double x, const double y, const double z) {
    switch (kind) {
    case 0:
        return {x, y, z};
    case 1:
        return {
            .05 + .55 * x + .20 * y + .10 * z,
            .03 + .10 * x + .65 * y + .12 * z,
            .02 + .15 * x + .20 * y + .55 * z,
        };
    default:
        return {
            .02 + .50 * x * x + .15 * y * z + .12 * x * y,
            .03 + .40 * y * y + .20 * x * z + .10 * x * y,
            .01 + .45 * z * z + .18 * x * y + .08 * y * z,
        };
    }
}

std::array<LutVolume, 3> make_luts() {
    std::array<LutVolume, 3> luts{};
    for (std::uint32_t kind = 0; kind < luts.size(); ++kind) {
        auto& lut = luts[kind];
        lut.packed.reserve(lut_texel_count);
        lut.decoded.reserve(lut_texel_count);
        for (std::uint32_t z = 0; z < lut_size; ++z) {
            for (std::uint32_t y = 0; y < lut_size; ++y) {
                for (std::uint32_t x = 0; x < lut_size; ++x) {
                    const auto value = evaluate_lut(
                        kind, static_cast<double>(x) / (lut_size - 1),
                        static_cast<double>(y) / (lut_size - 1),
                        static_cast<double>(z) / (lut_size - 1));
                    const glm::vec4 texel{
                        static_cast<float>(value[0]), static_cast<float>(value[1]),
                        static_cast<float>(value[2]), 1.0F};
                    const PackedTexel packed = glm::packHalf4x16(texel);
                    const glm::vec4 decoded = glm::unpackHalf4x16(packed);
                    lut.packed.push_back(packed);
                    lut.decoded.push_back({decoded.x, decoded.y, decoded.z});
                }
            }
        }
    }
    return luts;
}

std::vector<Float3> make_positions() {
    std::vector<Float3> positions{
        {-1.0F, -.25F, -std::numeric_limits<float>::epsilon()},
        {1.25F, 2.0F, std::numeric_limits<float>::max()},
        {-.25F, .5F, 1.25F}, {1.25F, -.5F, .25F},
        {0, 0, 0}, {1, 1, 1}, {.5F, .5F, .5F},
        {7.0F / 31.0F, 13.0F / 31.0F, 23.0F / 31.0F},
        {7.5F / 31.0F, 13.5F / 31.0F, 23.5F / 31.0F},
        {.1234567F, .3765432F, .7512345F},
        {std::nextafter(0.0F, 1.0F), std::nextafter(1.0F, 0.0F), .5F},
    };
    constexpr std::array strict_orders{
        Float3{.75F, .50F, .25F}, Float3{.75F, .25F, .50F},
        Float3{.50F, .75F, .25F}, Float3{.25F, .75F, .50F},
        Float3{.50F, .25F, .75F}, Float3{.25F, .50F, .75F},
    };
    constexpr std::array tie_planes{
        Float3{.50F, .50F, .25F}, Float3{.50F, .25F, .50F},
        Float3{.25F, .50F, .50F}, Float3{.50F, .50F, .75F},
        Float3{.50F, .75F, .50F}, Float3{.75F, .50F, .50F},
        Float3{.50F, .50F, .50F},
    };
    const auto append_cell = [&](const Float3& fractions) {
        positions.push_back({
            (7.0F + fractions[0]) / 31.0F,
            (7.0F + fractions[1]) / 31.0F,
            (7.0F + fractions[2]) / 31.0F,
        });
    };
    for (const auto& fractions : strict_orders) append_cell(fractions);
    for (const auto& fractions : tie_planes) append_cell(fractions);
    return positions;
}

std::vector<LutCase> make_cases(const std::span<const Float3> positions) {
    std::vector<LutCase> cases;
    cases.reserve(positions.size() * 3 * 2);
    for (std::uint32_t lut = 0; lut < 3; ++lut) {
        for (std::uint32_t interpolation = 1; interpolation <= 2; ++interpolation) {
            for (std::size_t sample = 0; sample < positions.size(); ++sample) {
                const auto& position = positions[sample];
                cases.push_back({
                    {position[0], position[1], position[2], 1.0F},
                    {lut, interpolation, static_cast<std::uint32_t>(sample), 0U},
                });
            }
        }
    }
    return cases;
}

std::size_t texel_index(const std::array<std::uint32_t, 3>& coordinate) noexcept {
    return (static_cast<std::size_t>(coordinate[2]) * lut_size + coordinate[1]) *
        lut_size + coordinate[0];
}

Double3 fetch(const LutVolume& lut, const std::array<std::uint32_t, 3>& coordinate) {
    return lut.decoded[texel_index(coordinate)];
}

Double3 add(const Double3& left, const Double3& right) noexcept {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}
Double3 scale(const Double3& value, const double factor) noexcept {
    return {value[0] * factor, value[1] * factor, value[2] * factor};
}
Double3 lerp(const Double3& low, const Double3& high, const double fraction) noexcept {
    return add(scale(low, 1.0 - fraction), scale(high, fraction));
}

struct CellCoordinate {
    std::array<std::uint32_t, 3> low{};
    std::array<std::uint32_t, 3> high{};
    Double3 fraction{};
};

CellCoordinate locate(const std::array<float, 4>& input) noexcept {
    CellCoordinate result{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double coordinate = std::clamp(static_cast<double>(input[axis]), 0.0, 1.0) *
            static_cast<double>(lut_size - 1);
        result.low[axis] = static_cast<std::uint32_t>(std::floor(coordinate));
        result.high[axis] = std::min(result.low[axis] + 1U, lut_size - 1);
        result.fraction[axis] = coordinate - result.low[axis];
    }
    return result;
}

Double3 reference_trilinear(const LutVolume& lut, const std::array<float, 4>& input) {
    const auto cell = locate(input);
    std::array<Double3, 4> along_x{};
    for (std::uint32_t z = 0; z < 2; ++z) {
        for (std::uint32_t y = 0; y < 2; ++y) {
            const std::array<std::uint32_t, 3> low{cell.low[0],
                y == 0 ? cell.low[1] : cell.high[1], z == 0 ? cell.low[2] : cell.high[2]};
            auto high = low;
            high[0] = cell.high[0];
            along_x[z * 2 + y] = lerp(fetch(lut, low), fetch(lut, high), cell.fraction[0]);
        }
    }
    const auto low_z = lerp(along_x[0], along_x[1], cell.fraction[1]);
    const auto high_z = lerp(along_x[2], along_x[3], cell.fraction[1]);
    return lerp(low_z, high_z, cell.fraction[2]);
}

Double3 reference_tetrahedral(const LutVolume& lut, const std::array<float, 4>& input) {
    const auto cell = locate(input);
    std::array<std::uint32_t, 3> axes{0, 1, 2};
    // Stable descending sort is an independent description of the six tetrahedra.
    // Stable x/y/z tie priority matches the shader's >= boundary ownership.
    std::stable_sort(axes.begin(), axes.end(), [&](const auto left, const auto right) {
        return cell.fraction[left] > cell.fraction[right];
    });
    auto corner = cell.low;
    const auto c0 = fetch(lut, corner);
    corner[axes[0]] = cell.high[axes[0]];
    const auto c1 = fetch(lut, corner);
    corner[axes[1]] = cell.high[axes[1]];
    const auto c2 = fetch(lut, corner);
    corner[axes[2]] = cell.high[axes[2]];
    const auto c3 = fetch(lut, corner);
    return add(c0, add(
        scale(add(c1, scale(c0, -1.0)), cell.fraction[axes[0]]),
        add(scale(add(c2, scale(c1, -1.0)), cell.fraction[axes[1]]),
            scale(add(c3, scale(c2, -1.0)), cell.fraction[axes[2]]))));
}

bool verify(const std::span<const LutCase> cases, const std::span<const Pixel> actual,
            const std::array<LutVolume, 3>& luts) {
    std::array<double, 2> maximum_error{};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& input = cases[i];
        const auto interpolation = input.control[1];
        const auto expected = interpolation == 2
            ? reference_tetrahedral(luts[input.control[0]], input.color)
            : reference_trilinear(luts[input.control[0]], input.color);
        const double tolerance = interpolation == 2
            ? tetrahedral_tolerance : trilinear_tolerance;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double value = actual[i][channel];
            const double error = std::abs(value - expected[channel]);
            maximum_error[interpolation - 1] = std::max(maximum_error[interpolation - 1], error);
            if (!std::isfinite(value) || error > tolerance) {
                std::cerr << std::scientific << std::setprecision(12)
                          << "LUT interpolation mismatch case=" << i
                          << " lut=" << input.control[0]
                          << " interpolation=" << interpolation
                          << " sample=" << input.control[2]
                          << " channel=" << channel
                          << " expected=" << expected[channel]
                          << " actual=" << value << " error=" << error
                          << " tolerance=" << tolerance << '\n';
                return false;
            }
        }
        if (actual[i][3] != 1.0F) {
            std::cerr << "LUT reference output alpha changed for case=" << i << '\n';
            return false;
        }
    }
    std::cout << std::scientific << std::setprecision(9)
              << "Color-grading GPU reference: " << cases.size()
              << " cases across identity/affine/nonlinear 32^3 RGBA16F LUTs passed; "
              << "trilinear max error=" << maximum_error[0]
              << " tolerance=" << trilinear_tolerance
              << "; tetrahedral max error=" << maximum_error[1]
              << " tolerance=" << tetrahedral_tolerance << '\n';
    return true;
}

int run() {
    const auto luts = make_luts();
    const auto positions = make_positions();
    const auto cases = make_cases(positions);

    SdlLifetime sdl;
    std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
        "Color grading LUT reference", 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
    if (!window) vcr::fatal(SDL_GetError());
    vcr::VulkanContext context;
    context.initialize(window.get(), true);
    if (!context.validation_enabled()) vcr::fatal("Color-grading GPU test requires validation layer");
    const auto device = context.device();
    const auto& limits = context.properties().limits;
    if (limits.maxImageDimension3D < lut_size || limits.maxComputeWorkGroupInvocations < 64 ||
        limits.maxComputeWorkGroupSize[0] < 64 ||
        limits.maxComputeWorkGroupCount[0] < (cases.size() + 63) / 64 ||
        limits.maxStorageBufferRange < cases.size() * sizeof(LutCase))
        vcr::fatal("Color-grading GPU test exceeds device limits");
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(
        context.physical_device(), VK_FORMAT_R16G16B16A16_SFLOAT, &format_properties);
    constexpr VkFormatFeatureFlags required_format_features =
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((format_properties.optimalTilingFeatures & required_format_features) != required_format_features)
        vcr::fatal("RGBA16F 3D linear sampling is unsupported");
    VkImageFormatProperties image_properties{};
    const auto image_support = vkGetPhysicalDeviceImageFormatProperties(
        context.physical_device(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TYPE_3D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        0, &image_properties);
    if (image_support == VK_ERROR_FORMAT_NOT_SUPPORTED ||
        (image_support == VK_SUCCESS && (image_properties.maxExtent.width < lut_size ||
         image_properties.maxExtent.height < lut_size || image_properties.maxExtent.depth < lut_size)))
        vcr::fatal("RGBA16F 32^3 sampled image is unsupported");
    if (image_support != VK_SUCCESS) VK_CHECK(image_support);
    const auto name = [&](VkObjectType type, auto handle, const char* label) {
        context.set_debug_name(type, vcr::handle_value(handle), label);
    };

    std::array<vcr::GpuTexture, 3> textures{};
    std::array<vcr::GpuBuffer, 3> staging{};
    constexpr std::array texture_names{
        "identity grading reference LUT", "affine grading reference LUT",
        "nonlinear grading reference LUT"};
    for (std::size_t i = 0; i < textures.size(); ++i) {
        textures[i].create_sampled_volume(context, lut_size,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FILTER_LINEAR, texture_names[i]);
        staging[i].create(context, luts[i].packed.size() * sizeof(PackedTexel),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT, texture_names[i]);
        staging[i].write(std::as_bytes(std::span{luts[i].packed}));
    }
    vcr::GpuBuffer input_buffer, output_buffer;
    input_buffer.create(context, cases.size() * sizeof(LutCase), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT, "color-grading reference inputs");
    input_buffer.write(std::as_bytes(std::span{cases}));
    output_buffer.create(context, cases.size() * sizeof(Pixel), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT, "color-grading reference outputs");

    constexpr std::uint32_t binding_count = 5;
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count, .pBindings = bindings.data()};
    vcr::DeviceObject<VkDescriptorSetLayout> layout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
        layout.put(device, vkDestroyDescriptorSetLayout)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, layout.get(), "color-grading reference layout");
    const auto layout_handle = layout.get();
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data()};
    vcr::DeviceObject<VkDescriptorPool> pool;
    VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr,
        pool.put(device, vkDestroyDescriptorPool)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_POOL, pool.get(), "color-grading reference descriptor pool");
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool.get(), .descriptorSetCount = 1, .pSetLayouts = &layout_handle};
    VkDescriptorSet set{}; // Descriptor pool owns this handle.
    VK_CHECK(vkAllocateDescriptorSets(device, &allocate, &set));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET, set, "color-grading reference descriptors");
    std::array<VkDescriptorImageInfo, 3> image_infos{};
    for (std::size_t i = 0; i < image_infos.size(); ++i)
        image_infos[i] = {textures[i].sampler(), textures[i].view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array buffer_infos{
        VkDescriptorBufferInfo{input_buffer.handle(), 0, input_buffer.size()},
        VkDescriptorBufferInfo{output_buffer.handle(), 0, output_buffer.size()},
    };
    std::array<VkWriteDescriptorSet, binding_count> writes{};
    for (std::uint32_t i = 0; i < 3; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[i]};
    for (std::uint32_t i = 0; i < 2; ++i)
        writes[3 + i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = 3 + i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_infos[i]};
    vkUpdateDescriptorSets(device, binding_count, writes.data(), 0, nullptr);

    std::ifstream file(VCR_COLOR_GRADING_TEST_SPV, std::ios::binary | std::ios::ate);
    const std::streamoff length = file.tellg();
    constexpr std::streamoff maximum_shader_bytes = 4 * 1024 * 1024;
    if (!file || length <= 0 || length > maximum_shader_bytes || length % 4 != 0)
        vcr::fatal("Missing or oversized color-grading reference SPIR-V; compile shaders first");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(length)))
        vcr::fatal("Cannot read color-grading reference SPIR-V");
    const VkShaderModuleCreateInfo shader_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(std::uint32_t), .pCode = code.data()};
    vcr::ShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device, &shader_info, nullptr,
        shader.put(device, vkDestroyShaderModule)));
    name(VK_OBJECT_TYPE_SHADER_MODULE, shader.get(), "color-grading reference shader");
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &layout_handle};
    vcr::DeviceObject<VkPipelineLayout> pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
        pipeline_layout.put(device, vkDestroyPipelineLayout)));
    name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipeline_layout.get(), "color-grading reference pipeline layout");
    const VkComputePipelineCreateInfo pipeline_info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader.get(), .pName = "main"},
        .layout = pipeline_layout.get()};
    vcr::DeviceObject<VkPipeline> pipeline;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        pipeline.put(device, vkDestroyPipeline)));
    name(VK_OBJECT_TYPE_PIPELINE, pipeline.get(), "color-grading reference pipeline");

    const VkCommandPoolCreateInfo command_pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = context.queue_families().graphics};
    vcr::DeviceObject<VkCommandPool> command_pool;
    VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr,
        command_pool.put(device, vkDestroyCommandPool)));
    name(VK_OBJECT_TYPE_COMMAND_POOL, command_pool.get(), "color-grading reference command pool");
    const VkCommandBufferAllocateInfo command_allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool.get(), .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    VkCommandBuffer command{}; // Command pool owns this handle.
    VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate, &command));
    name(VK_OBJECT_TYPE_COMMAND_BUFFER, command, "color-grading reference commands");
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command, &begin));
    std::array<VkImageMemoryBarrier2, 3> to_transfer{};
    for (std::size_t i = 0; i < to_transfer.size(); ++i)
        to_transfer[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = textures[i].image(),
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkDependencyInfo upload_dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(to_transfer.size()),
        .pImageMemoryBarriers = to_transfer.data()};
    vkCmdPipelineBarrier2(command, &upload_dependency);
    const VkBufferImageCopy copy{.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0}, .imageExtent = {lut_size, lut_size, lut_size}};
    for (std::size_t i = 0; i < textures.size(); ++i)
        vkCmdCopyBufferToImage(command, staging[i].handle(), textures[i].image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    std::array<VkImageMemoryBarrier2, 3> to_compute{};
    for (std::size_t i = 0; i < to_compute.size(); ++i)
        to_compute[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = textures[i].image(),
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkDependencyInfo sample_dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(to_compute.size()),
        .pImageMemoryBarriers = to_compute.data()};
    vkCmdPipelineBarrier2(command, &sample_dependency);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout.get(),
        0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, static_cast<std::uint32_t>((cases.size() + 63) / 64), 1, 1);
    const VkMemoryBarrier2 readback_barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo readback_dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &readback_barrier};
    vkCmdPipelineBarrier2(command, &readback_dependency);
    VK_CHECK(vkEndCommandBuffer(command));
    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vcr::DeviceObject<VkFence> fence;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, fence.put(device, vkDestroyFence)));
    name(VK_OBJECT_TYPE_FENCE, fence.get(), "color-grading reference completion");
    const VkCommandBufferSubmitInfo command_submit{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = command};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command_submit};
    SubmissionRetirement retirement{device};
    VK_CHECK(vkQueueSubmit2(context.graphics_queue(), 1, &submit, fence.get()));
    retirement.pending = true;
    const auto fence_handle = fence.get();
    VK_CHECK(vkWaitForFences(device, 1, &fence_handle, VK_TRUE, UINT64_MAX));
    retirement.pending = false;
    std::vector<Pixel> actual(cases.size());
    const auto bytes = output_buffer.read_completed_bytes(actual.size() * sizeof(Pixel));
    std::memcpy(actual.data(), bytes.data(), bytes.size());
    return verify(cases, actual, luts) ? 0 : 1;
}
} // namespace

int main() {
    try { return run(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
