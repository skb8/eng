#pragma once

#include "vulkan_context.hpp"

class Scene;

class Swapchain {
public:
    explicit Swapchain(const VulkanContext& context) : context_(context) {}
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    enum class FrameResult { Presented, Retry, Outdated };

    bool create(VkSurfaceKHR surface, VkExtent2D windowSize);
    FrameResult draw(const Scene& scene);

private:
    struct Frame {
        VkImageView view{};
        VkFramebuffer framebuffer{};
        // Reacquiring this image makes its present semaphore available for reuse.
        VkSemaphore ready{};
    };

    void createDepth();
    void createRenderPass();
    void createFrames();
    void createCommands();
    VkPipeline createPipeline(bool depthTest) const;

    const VulkanContext& context_;
    VkSwapchainKHR swapchain_{};
    VkExtent2D extent_{};
    VkFormat format_{};
    VkImage depthImage_{};
    VmaAllocation depthAllocation_{};
    VkImageView depthView_{};
    VkRenderPass renderPass_{};
    VkPipeline cubePipeline_{};
    VkPipeline hudPipeline_{};
    std::vector<Frame> frames_;
    VkCommandPool commandPool_{};
    VkCommandBuffer command_{};
    VkSemaphore imageAvailable_{};
    VkFence fence_{};
};
