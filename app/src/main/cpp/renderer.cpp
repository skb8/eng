#include "renderer.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

Swapchain::~Swapchain() {
    const auto device = context_.device;
    if (swapchain_) vkDeviceWaitIdle(device);
    if (fence_) vkDestroyFence(device, fence_, nullptr);
    if (imageAvailable_) vkDestroySemaphore(device, imageAvailable_, nullptr);
    if (commandPool_) vkDestroyCommandPool(device, commandPool_, nullptr);
    for (const auto& frame : frames_) {
        if (frame.ready) vkDestroySemaphore(device, frame.ready, nullptr);
        if (frame.framebuffer) vkDestroyFramebuffer(device, frame.framebuffer, nullptr);
        if (frame.view) vkDestroyImageView(device, frame.view, nullptr);
    }
    if (cubePipeline_) vkDestroyPipeline(device, cubePipeline_, nullptr);
    if (hudPipeline_) vkDestroyPipeline(device, hudPipeline_, nullptr);
    if (renderPass_) vkDestroyRenderPass(device, renderPass_, nullptr);
    if (depthView_) vkDestroyImageView(device, depthView_, nullptr);
    if (depthImage_) vmaDestroyImage(context_.allocator, depthImage_, depthAllocation_);
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
}

bool Swapchain::create(VkSurfaceKHR surface, VkExtent2D windowSize) {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice, surface, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    extent_ = caps.currentExtent;
    if (extent_.width == std::numeric_limits<uint32_t>::max()) {
        extent_.width = std::clamp(windowSize.width, caps.minImageExtent.width,
                                   caps.maxImageExtent.width);
        extent_.height = std::clamp(windowSize.height, caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) return false;

    const auto formats = enumerate<VkSurfaceFormatKHR>(
        [this, surface](uint32_t* count, VkSurfaceFormatKHR* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice, surface, count, values);
        });
    if (formats.empty()) throw std::runtime_error("Vulkan surface has no supported formats");
    const auto isSrgb = [](const VkSurfaceFormatKHR& format) {
        return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
               (format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_B8G8R8A8_SRGB);
    };
    const auto preferred = std::find_if(formats.begin(), formats.end(), isSrgb);
    const VkSurfaceFormatKHR format = preferred == formats.end() ? formats.front() : *preferred;
    format_ = format.format == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : format.format;

    const VkCompositeAlphaFlagBitsKHR alphaModes[]{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    const auto alpha = std::find_if(std::begin(alphaModes), std::end(alphaModes),
        [&caps](VkCompositeAlphaFlagBitsKHR mode) { return caps.supportedCompositeAlpha & mode; });
    if (alpha == std::end(alphaModes)) throw std::runtime_error("No Vulkan surface alpha mode");

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = format_;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = *alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(context_.device, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    createDepth();
    createRenderPass();
    cubePipeline_ = createPipeline(true);
    hudPipeline_ = createPipeline(false);
    createFrames();
    createCommands();
    return true;
}

void Swapchain::createDepth() {
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = context_.depthFormat;
    image.extent = {extent_.width, extent_.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(context_.allocator, &image, &allocation, &depthImage_, &depthAllocation_,
                         nullptr), "vmaCreateImage");

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = depthImage_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = context_.depthFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(context_.device, &view, nullptr, &depthView_), "vkCreateImageView(depth)");
}

void Swapchain::createRenderPass() {
    VkAttachmentDescription attachments[2]{};
    auto& color = attachments[0];
    color.format = format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    auto& depth = attachments[1];
    depth.format = context_.depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    check(vkCreateRenderPass(context_.device, &info, nullptr, &renderPass_), "vkCreateRenderPass");
}

VkPipeline Swapchain::createPipeline(bool depthTest) const {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = context_.vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = context_.fragmentShader;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 2;
    vertex.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const VkViewport viewport{0, 0, static_cast<float>(extent_.width),
                              static_cast<float>(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo samples{};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = depthTest;
    depth.depthWriteEnable = depthTest;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = !depthTest;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo color{};
    color.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color.attachmentCount = 1;
    color.pAttachments = &blend;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &color;
    info.layout = context_.layout;
    info.renderPass = renderPass_;

    VkPipeline pipeline{};
    check(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
          "vkCreateGraphicsPipelines");
    return pipeline;
}

void Swapchain::createFrames() {
    const auto device = context_.device;
    const auto images = enumerate<VkImage>([this, device](uint32_t* count, VkImage* values) {
        return vkGetSwapchainImagesKHR(device, swapchain_, count, values);
    });
    frames_.resize(images.size());

    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t index = 0; index < images.size(); ++index) {
        auto& frame = frames_[index];
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[index];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &frame.view), "vkCreateImageView");

        const VkImageView attachments[]{frame.view, depthView_};
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = renderPass_;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = attachments;
        framebuffer.width = extent_.width;
        framebuffer.height = extent_.height;
        framebuffer.layers = 1;
        check(vkCreateFramebuffer(device, &framebuffer, nullptr, &frame.framebuffer),
              "vkCreateFramebuffer");
        check(vkCreateSemaphore(device, &semaphore, nullptr, &frame.ready), "vkCreateSemaphore");
    }
    check(vkCreateSemaphore(device, &semaphore, nullptr, &imageAvailable_), "vkCreateSemaphore");
}

void Swapchain::createCommands() {
    const auto device = context_.device;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = context_.queueFamily;
    check(vkCreateCommandPool(device, &pool, nullptr, &commandPool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffer.commandPool = commandPool_;
    buffer.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &buffer, &command_), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device, &fence, nullptr, &fence_), "vkCreateFence");
}

Swapchain::FrameResult Swapchain::draw(const Scene& scene) {
    const auto device = context_.device;
    check(vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    uint32_t index = 0;
    constexpr uint64_t acquireTimeout = 16'000'000;  // Nanoseconds.
    const VkResult acquired = vkAcquireNextImageKHR(
        device, swapchain_, acquireTimeout, imageAvailable_, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return FrameResult::Outdated;
    if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) return FrameResult::Retry;
    if (acquired != VK_SUBOPTIMAL_KHR) check(acquired, "vkAcquireNextImageKHR");

    const auto& hud = scene.hud(extent_.width, extent_.height);
    if (hud.size() > maxHudVertices) throw std::runtime_error("Vulkan HUD buffer is full");
    context_.uploadHud(hud.data(), hud.size() * sizeof(Vertex));

    check(vkResetCommandPool(device, commandPool_, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");

    VkClearValue clear[2]{};
    clear[0].color = {{0.014f, 0.026f, 0.055f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0};
    const auto& frame = frames_[index];
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = renderPass_;
    pass.framebuffer = frame.framebuffer;
    pass.renderArea.extent = extent_;
    pass.clearValueCount = 2;
    pass.pClearValues = clear;

    vkCmdBeginRenderPass(command_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkDeviceSize offset = 0;
    const auto mvp = scene.viewProjection(static_cast<float>(extent_.width) / extent_.height);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, cubePipeline_);
    vkCmdBindVertexBuffers(command_, 0, 1, &context_.cube.handle, &offset);
    vkCmdPushConstants(command_, context_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), &mvp);
    vkCmdDraw(command_, cubeVertexCount, 1, 0, 0);

    const glm::mat4 identity{1.0f};
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline_);
    vkCmdBindVertexBuffers(command_, 0, 1, &context_.hud.handle, &offset);
    vkCmdPushConstants(command_, context_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(identity),
                       &identity);
    vkCmdDraw(command_, static_cast<uint32_t>(hud.size()), 1, 0, 0);
    vkCmdEndRenderPass(command_);
    check(vkEndCommandBuffer(command_), "vkEndCommandBuffer");

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.ready;
    check(vkResetFences(device, 1, &fence_), "vkResetFences");
    check(vkQueueSubmit(context_.queue, 1, &submit, fence_), "vkQueueSubmit");

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.ready;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    const VkResult presented = vkQueuePresentKHR(context_.queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) return FrameResult::Outdated;
    if (presented != VK_SUBOPTIMAL_KHR) check(presented, "vkQueuePresentKHR");
    return FrameResult::Presented;
}
