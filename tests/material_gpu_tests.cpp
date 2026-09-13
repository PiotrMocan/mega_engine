#include "device_object.hpp"
#include "gpu_buffer.hpp"
#include "pbr_material.hpp"
#include "runtime_scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace {
struct SdlLifetime {
    SdlLifetime() { if (!SDL_Init(SDL_INIT_VIDEO)) vcr::fatal(SDL_GetError()); }
    ~SdlLifetime() { SDL_Quit(); }
};
struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept { SDL_DestroyWindow(window); }
};
struct SubmissionRetirement {
    VkDevice device; // Observer; declared after all resources used by the submission.
    bool pending = false;
    ~SubmissionRetirement() {
        if (pending) {
            // A failed fence wait must not unwind resources still in GPU use.
            const auto result = vkDeviceWaitIdle(device);
            if (result != VK_SUCCESS)
                std::cerr << "Material test retirement failed: VkResult " << result << '\n';
        }
    }
};

struct alignas(16) MaterialResult {
    glm::vec4 base_color{};
    std::array<float, 4> parameters{};
    std::array<float, 4> numeric{};
    std::array<std::uint32_t, 4> exact{};
};
struct alignas(16) RuntimeResult {
    vcr::TransformRows current{};
    MaterialResult material{};
    vcr::TransformRows previous{};
};
static_assert(sizeof(vcr::PbrMaterial) == 48);
static_assert(offsetof(vcr::PbrMaterial, reserved) == 28);
static_assert(offsetof(vcr::PbrMaterial, subsurface_profile) == 40);
static_assert(offsetof(vcr::PbrMaterial, subsurface_strength) == 44);
static_assert(sizeof(vcr::RuntimeGpuInstance) == 144);
static_assert(offsetof(vcr::SceneGpuInstance, material) == 48);
static_assert(offsetof(vcr::RuntimeGpuInstance, previous) == 96);
static_assert(sizeof(MaterialResult) == 64);
static_assert(offsetof(MaterialResult, parameters) == 16);
static_assert(offsetof(MaterialResult, numeric) == 32);
static_assert(offsetof(MaterialResult, exact) == 48);
static_assert(sizeof(RuntimeResult) == 160);
static_assert(offsetof(RuntimeResult, material) == 48);
static_assert(offsetof(RuntimeResult, previous) == 112);
static_assert(std::is_trivially_copyable_v<vcr::PbrMaterial>);
static_assert(std::is_trivially_copyable_v<vcr::RuntimeGpuInstance>);
static_assert(std::is_trivially_copyable_v<MaterialResult>);
static_assert(std::is_trivially_copyable_v<RuntimeResult>);

constexpr std::array test_iors{
    1.0F, 1.0F + std::numeric_limits<float>::epsilon(), 1.33F, 1.5F,
    2.0F, 2.42F, 16.0F, 65504.0F, 1e20F, std::numeric_limits<float>::max()};
constexpr std::array test_subsurface_strengths{
    0.12345679F, 0.25F, 0.37654321F, 0.5F, 0.7512345F,
    0.875F, 0.9375F, 0.99999F, 1.0F, std::numeric_limits<float>::epsilon()};
static_assert(test_subsurface_strengths.size() == test_iors.size());
constexpr std::size_t material_models_count = 3;
constexpr std::size_t material_count = test_iors.size() * material_models_count;
constexpr std::size_t runtime_count = 2;

// Independent normal-incidence dielectric Fresnel formula. All inputs are
// finite float values, so both subtraction and addition are safe in double.
// Deliberately do not call dielectric_f0 or copy the shader's reciprocal form.
double reference_f0(float ior) noexcept {
    const double value = static_cast<double>(ior);
    const double ratio = (value - 1.0) / (value + 1.0);
    return ratio * ratio;
}

bool same_float(float expected, float actual) noexcept {
    return std::bit_cast<std::uint32_t>(expected) == std::bit_cast<std::uint32_t>(actual);
}
bool same_vector(const glm::vec4& expected, const glm::vec4& actual) noexcept {
    for (glm::length_t channel = 0; channel < 4; ++channel)
        if (!same_float(expected[channel], actual[channel])) return false;
    return true;
}
bool verify_material(const vcr::PbrMaterial& input, const MaterialResult& actual,
                     std::uint32_t index, double& maximum_error) {
    const std::array expected_parameters{
        input.metallic, input.roughness, input.ambient_occlusion, input.reserved};
    const std::array expected_exact{
        static_cast<std::uint32_t>(input.model),
        static_cast<std::uint32_t>(input.subsurface_profile),
        std::bit_cast<std::uint32_t>(input.subsurface_strength),
        index ^ 0x6d61746cU};
    bool preserved = same_vector(input.base_color, actual.base_color) &&
        same_float(input.ior, actual.numeric[1]) &&
        same_float(-7.25F, actual.numeric[2]) && same_float(19.5F, actual.numeric[3]) &&
        expected_exact == actual.exact;
    for (std::size_t channel = 0; channel < expected_parameters.size(); ++channel)
        preserved = preserved && same_float(expected_parameters[channel], actual.parameters[channel]);
    if (!preserved) {
        std::cerr << "Material ABI mismatch record=" << index
                  << " expected model=" << static_cast<std::uint32_t>(input.model)
                  << " actual model=" << actual.exact[0] << '\n';
        return false;
    }
    const double expected = reference_f0(input.ior);
    const double error = std::abs(static_cast<double>(actual.numeric[0]) - expected);
    const double tolerance = 8.0 * std::numeric_limits<float>::epsilon() * std::max(expected, 1e-12);
    if (!std::isfinite(actual.numeric[0]) || actual.numeric[0] < 0 || actual.numeric[0] > 1 ||
        error > tolerance ||
        (input.ior == 1.5F && !same_float(0.04F, actual.numeric[0])) ||
        (input.ior == 1.0F && !same_float(0.0F, actual.numeric[0]))) {
        std::cerr << std::scientific << std::setprecision(12)
                  << "Material F0 mismatch record=" << index << " IOR=" << input.ior
                  << " expected=" << expected << " actual=" << actual.numeric[0]
                  << " error=" << error << " tolerance=" << tolerance << '\n';
        return false;
    }
    maximum_error = std::max(maximum_error, error);
    return true;
}

int run() {
    SdlLifetime sdl;
    std::unique_ptr<SDL_Window, WindowDeleter> window(SDL_CreateWindow(
        "Material numeric and ABI reference", 32, 32, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN));
    if (!window) vcr::fatal(SDL_GetError());
    vcr::VulkanContext context;
    context.initialize(window.get(), true);
    if (!context.validation_enabled()) vcr::fatal("Material GPU test requires validation layer");
    const auto device = context.device();
    const auto name = [&](VkObjectType type, auto handle, const char* label) {
        context.set_debug_name(type, vcr::handle_value(handle), label);
    };

    std::array<vcr::PbrMaterial, material_count> input{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto record = static_cast<std::uint32_t>(i);
        auto& material = input[i];
        // Non-half-representable factors make an accidental FP16 conversion
        // observable even when the record offsets still happen to be correct.
        material.base_color = glm::vec4{static_cast<float>(i + 1) / 32.0F,
                                       .12345679F, .37654321F, .7512345F};
        material.metallic = .1234567F + static_cast<float>(i % 7) / 8.0F;
        material.roughness = .2345678F + static_cast<float>(i % 5) / 8.0F;
        material.ambient_occlusion = .1234567F + static_cast<float>(i) / 64.0F;
        material.reserved = static_cast<float>(65537U + record);
        const auto material_model = i % material_models_count;
        const auto ior_index = i / material_models_count;
        material.ior = test_iors[ior_index];
        material.model = static_cast<vcr::MaterialModel>(material_model);
        if (material.model == vcr::MaterialModel::subsurface) {
            material.subsurface_profile = static_cast<vcr::SubsurfaceProfile>(
                1U + static_cast<std::uint32_t>(ior_index % vcr::maximum_subsurface_profile));
            // Include values that are not exactly representable as FP16 so an
            // accidental narrowing at offset 44 remains observable.
            material.subsurface_strength = test_subsurface_strengths[ior_index];
        } else {
            material.subsurface_profile = vcr::SubsurfaceProfile::none;
            material.subsurface_strength = 0.0F;
        }
    }
    // Both records are the real runtime type, not a separately packed mirror.
    std::array<vcr::RuntimeGpuInstance, runtime_count> runtime_input{};
    for (std::size_t i = 0; i < runtime_input.size(); ++i) {
        auto& instance = runtime_input[i];
        const float marker = static_cast<float>(i * 128);
        instance.current.model_row_0 = {marker + .25F, -.5F, .75F, marker + 1};
        instance.current.model_row_1 = {-2.25F, marker + 3.5F, 4.75F, marker + 5};
        instance.current.model_row_2 = {6.25F, -7.5F, marker + 8.75F, marker + 9};
        instance.previous.x = {-marker - 10.25F, 11.5F, 12.75F, marker + 13};
        instance.previous.y = {14.25F, -marker - 15.5F, 16.75F, marker + 17};
        instance.previous.z = {18.25F, 19.5F, -marker - 20.75F, marker + 21};
        instance.current.material = i == 0 ? input[4] : input.back();
    }

    vcr::GpuBuffer inputs, outputs, runtime_inputs, runtime_outputs;
    const auto create_input = [&](vcr::GpuBuffer& buffer, auto bytes, const char* label) {
        buffer.create(context, bytes.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT, label);
        buffer.write(bytes);
    };
    create_input(inputs, std::as_bytes(std::span(input)), "material reference inputs");
    create_input(runtime_inputs, std::as_bytes(std::span(runtime_input)), "runtime ABI inputs");
    const auto create_output = [&](vcr::GpuBuffer& buffer, VkDeviceSize bytes, const char* label) {
        buffer.create(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, label);
    };
    create_output(outputs, sizeof(MaterialResult) * material_count, "material reference outputs");
    create_output(runtime_outputs, sizeof(RuntimeResult) * runtime_count, "runtime ABI outputs");
    constexpr std::uint32_t binding_count = 4;
    std::array<VkDescriptorSetLayoutBinding, binding_count> bindings{};
    for (std::uint32_t i = 0; i < binding_count; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = binding_count, .pBindings = bindings.data()};
    vcr::DeviceObject<VkDescriptorSetLayout> layout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
        layout.put(device, vkDestroyDescriptorSetLayout)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, layout.get(), "material reference descriptor layout");
    const auto layout_handle = layout.get();
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding_count};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    vcr::DeviceObject<VkDescriptorPool> pool;
    VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, pool.put(device, vkDestroyDescriptorPool)));
    name(VK_OBJECT_TYPE_DESCRIPTOR_POOL, pool.get(), "material reference descriptor pool");
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool.get(), .descriptorSetCount = 1, .pSetLayouts = &layout_handle};
    VkDescriptorSet set{}; // Descriptor pool owns this handle.
    VK_CHECK(vkAllocateDescriptorSets(device, &allocate, &set));
    name(VK_OBJECT_TYPE_DESCRIPTOR_SET, set, "material reference descriptors");
    const std::array buffer_infos{
        VkDescriptorBufferInfo{inputs.handle(), 0, inputs.size()},
        VkDescriptorBufferInfo{outputs.handle(), 0, outputs.size()},
        VkDescriptorBufferInfo{runtime_inputs.handle(), 0, runtime_inputs.size()},
        VkDescriptorBufferInfo{runtime_outputs.handle(), 0, runtime_outputs.size()}};
    std::array<VkWriteDescriptorSet, binding_count> writes{};
    for (std::uint32_t i = 0; i < binding_count; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_infos[i]};
    vkUpdateDescriptorSets(device, binding_count, writes.data(), 0, nullptr);

    std::ifstream file(VCR_MATERIAL_TEST_SPV, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (!file || length <= 0 || length % 4 != 0)
        vcr::fatal("Missing material reference SPIR-V; compile shaders first");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), length)) vcr::fatal("Cannot read material SPIR-V");
    const VkShaderModuleCreateInfo shader_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(std::uint32_t), .pCode = code.data()};
    vcr::ShaderModule shader;
    VK_CHECK(vkCreateShaderModule(device, &shader_info, nullptr, shader.put(device, vkDestroyShaderModule)));
    name(VK_OBJECT_TYPE_SHADER_MODULE, shader.get(), "material reference compute shader");
    const VkPipelineLayoutCreateInfo pipeline_layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &layout_handle};
    vcr::DeviceObject<VkPipelineLayout> pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
        pipeline_layout.put(device, vkDestroyPipelineLayout)));
    name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipeline_layout.get(), "material reference pipeline layout");
    const VkComputePipelineCreateInfo pipeline_info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader.get(), .pName = "main"}, .layout = pipeline_layout.get()};
    vcr::DeviceObject<VkPipeline> pipeline;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        pipeline.put(device, vkDestroyPipeline)));
    name(VK_OBJECT_TYPE_PIPELINE, pipeline.get(), "material reference pipeline");

    const VkCommandPoolCreateInfo command_pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = context.queue_families().graphics};
    vcr::DeviceObject<VkCommandPool> command_pool;
    VK_CHECK(vkCreateCommandPool(device, &command_pool_info, nullptr,
        command_pool.put(device, vkDestroyCommandPool)));
    name(VK_OBJECT_TYPE_COMMAND_POOL, command_pool.get(), "material reference command pool");
    const VkCommandBufferAllocateInfo command_allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool.get(), .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command{}; // Command pool owns this handle.
    VK_CHECK(vkAllocateCommandBuffers(device, &command_allocate, &command));
    name(VK_OBJECT_TYPE_COMMAND_BUFFER, command, "material reference commands");
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command, &begin));
    // GpuBuffer::write flushes host writes; queue submission makes them
    // available to the device before this compute dispatch.
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout.get(), 0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, static_cast<std::uint32_t>(material_count), 1, 1);
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(command, &dependency);
    VK_CHECK(vkEndCommandBuffer(command));
    const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vcr::DeviceObject<VkFence> fence;
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, fence.put(device, vkDestroyFence)));
    name(VK_OBJECT_TYPE_FENCE, fence.get(), "material reference completion");
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
    std::array<MaterialResult, material_count> actual{};
    std::array<RuntimeResult, runtime_count> runtime_actual{};
    const auto material_bytes = outputs.read_completed_bytes(sizeof(actual));
    const auto runtime_bytes = runtime_outputs.read_completed_bytes(sizeof(runtime_actual));
    std::memcpy(actual.data(), material_bytes.data(), material_bytes.size());
    std::memcpy(runtime_actual.data(), runtime_bytes.data(), runtime_bytes.size());
    double maximum_error = 0;
    for (std::size_t i = 0; i < actual.size(); ++i)
        if (!verify_material(input[i], actual[i], static_cast<std::uint32_t>(i), maximum_error)) return 1;
    for (std::size_t i = 0; i < runtime_actual.size(); ++i) {
        const auto& expected = runtime_input[i];
        const auto& result = runtime_actual[i];
        if (!same_vector(expected.current.model_row_0, result.current.x) ||
            !same_vector(expected.current.model_row_1, result.current.y) ||
            !same_vector(expected.current.model_row_2, result.current.z) ||
            !same_vector(expected.previous.x, result.previous.x) ||
            !same_vector(expected.previous.y, result.previous.y) ||
            !same_vector(expected.previous.z, result.previous.z)) {
            std::cerr << "Runtime 144-byte ABI row/stride mismatch record=" << i << '\n';
            return 1;
        }
        if (!verify_material(expected.current.material, result.material,
                             static_cast<std::uint32_t>(i), maximum_error)) return 1;
    }
    std::cout << std::scientific << std::setprecision(12)
              << "Material GPU reference: " << actual.size() << " materials (48-byte stride), "
              << runtime_actual.size() << " runtime records (144-byte stride) passed; max F0 absolute error="
              << maximum_error
              << "; default F0 exact; model/profile/strength bits and color/AO/rows exact\n";
    return 0;
}
} // namespace

int main() {
    try { return run(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
