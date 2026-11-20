#include "vulkan_context.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace {
constexpr uint32_t vertexSpirv[] = {
#include "scene.vert.inc"
};
constexpr uint32_t fragmentSpirv[] = {
#include "scene.frag.inc"
};
}  // namespace

VulkanError::VulkanError(VkResult code, const char* operation)
    : std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(code)),
      result(code) {}

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw VulkanError(result, operation);
}

VulkanContext::~VulkanContext() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (vertexShader) vkDestroyShaderModule(device, vertexShader, nullptr);
        if (fragmentShader) vkDestroyShaderModule(device, fragmentShader, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (cube.handle) vmaDestroyBuffer(allocator, cube.handle, cube.allocation);
        if (hud.handle) vmaDestroyBuffer(allocator, hud.handle, hud.allocation);
        if (allocator) vmaDestroyAllocator(allocator);
        vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
}

void VulkanContext::createInstance() {
    const char* extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "eng";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
}

bool VulkanContext::selectQueue(VkPhysicalDevice candidate, VkSurfaceKHR surface) {
    const auto extensions = enumerate<VkExtensionProperties>(
        [candidate](uint32_t* count, VkExtensionProperties* values) {
            return vkEnumerateDeviceExtensionProperties(candidate, nullptr, count, values);
        });
    const auto isSwapchain = [](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    };
    if (std::none_of(extensions.begin(), extensions.end(), isSwapchain)) return false;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
    for (uint32_t index = 0; index < count; ++index) {
        if (!(queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present),
              "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (present) {
            physicalDevice = candidate;
            queueFamily = index;
            return true;
        }
    }
    return false;
}

void VulkanContext::createDevice(VkSurfaceKHR surface) {
    const auto devices = enumerate<VkPhysicalDevice>(
        [this](uint32_t* count, VkPhysicalDevice* values) {
            return vkEnumeratePhysicalDevices(instance, count, values);
        });
    const auto suitable = [this, surface](VkPhysicalDevice candidate) {
        return selectQueue(candidate, surface);
    };
    if (std::none_of(devices.begin(), devices.end(), suitable)) {
        throw std::runtime_error("No Vulkan queue supports graphics and presentation");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &extension;
    check(vkCreateDevice(physicalDevice, &info, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    const VkFormat formats[]{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    for (const auto format : formats) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depthFormat = format;
            break;
        }
    }
    if (depthFormat == VK_FORMAT_UNDEFINED) throw std::runtime_error("No Vulkan depth format");

    VmaAllocatorCreateInfo memory{};
    memory.instance = instance;
    memory.physicalDevice = physicalDevice;
    memory.device = device;
    memory.vulkanApiVersion = VK_API_VERSION_1_0;
    check(vmaCreateAllocator(&memory, &allocator), "vmaCreateAllocator");

    const auto vertices = cubeVertices();
    createBuffer(cube, sizeof(vertices));
    std::memcpy(cube.mapped, vertices.data(), sizeof(vertices));
    check(vmaFlushAllocation(allocator, cube.allocation, 0, sizeof(vertices)), "vmaFlushAllocation");
    createBuffer(hud, sizeof(Vertex) * maxHudVertices);

    const VkPushConstantRange transform{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &transform;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    vertexShader = createShader(vertexSpirv, sizeof(vertexSpirv));
    fragmentShader = createShader(fragmentSpirv, sizeof(fragmentSpirv));
}

void VulkanContext::requirePresentSupport(VkSurfaceKHR surface) const {
    VkBool32 present = VK_FALSE;
    check(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamily, surface, &present),
          "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!present) throw std::runtime_error("Vulkan queue cannot present to the new surface");
}

void VulkanContext::createBuffer(Buffer& buffer, VkDeviceSize size) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    check(vmaCreateBuffer(allocator, &info, &allocation, &buffer.handle, &buffer.allocation,
                          &mapped), "vmaCreateBuffer");
    buffer.mapped = mapped.pMappedData;
}

void VulkanContext::uploadHud(const void* bytes, size_t size) const {
    std::memcpy(hud.mapped, bytes, size);
    check(vmaFlushAllocation(allocator, hud.allocation, 0, size), "vmaFlushAllocation");
}

VkShaderModule VulkanContext::createShader(const uint32_t* words, size_t size) const {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode = words;
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}
