#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <stdexcept>
#include <vector>

class VulkanError : public std::runtime_error {
public:
    VulkanError(VkResult result, const char* operation);
    const VkResult result;
};

void check(VkResult result, const char* operation);

template <typename T, typename Enumerate>
std::vector<T> enumerate(Enumerate query) {
    for (;;) {
        uint32_t count = 0;
        check(query(&count, nullptr), "Vulkan enumeration count");
        std::vector<T> items(count);
        if (count == 0) return items;
        const auto result = query(&count, items.data());
        if (result == VK_INCOMPLETE) continue;
        check(result, "Vulkan enumeration");
        items.resize(count);
        return items;
    }
}

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void createInstance();
    void createDevice(VkSurfaceKHR surface);
    void requirePresentSupport(VkSurfaceKHR surface) const;
    void uploadHud(const void* bytes, size_t size) const;

    VkInstance instance{};
    VkPhysicalDevice physicalDevice{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queueFamily{};
    VmaAllocator allocator{};
    VkFormat depthFormat{};
    VkPipelineLayout layout{};
    VkShaderModule vertexShader{};
    VkShaderModule fragmentShader{};

    struct Buffer {
        VkBuffer handle{};
        VmaAllocation allocation{};
        void* mapped{};
    };
    Buffer cube;
    Buffer hud;

private:
    bool selectQueue(VkPhysicalDevice candidate, VkSurfaceKHR surface);
    void createBuffer(Buffer& buffer, VkDeviceSize size);
    VkShaderModule createShader(const uint32_t* words, size_t size) const;
};
