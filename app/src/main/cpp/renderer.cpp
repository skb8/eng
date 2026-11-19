#include <android/native_window_jni.h>
#include <jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": VkResult " +
                                 std::to_string(result));
    }
}

template <typename T, typename Enumerate>
std::vector<T> enumerate(Enumerate query) {
    for (;;) {
        uint32_t count = 0;
        check(query(&count, nullptr), "Vulkan enumeration count");
        std::vector<T> items(count);
        if (count == 0) {
            return items;
        }
        const VkResult result = query(&count, items.data());
        if (result != VK_INCOMPLETE) {
            check(result, "Vulkan enumeration");
            items.resize(count);
            return items;
        }
    }
}

class Renderer {
public:
    explicit Renderer(ANativeWindow* window) : window_(window) {}

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    ~Renderer() {
        if (device_) {
            vkDeviceWaitIdle(device_);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (imageAvailable_) vkDestroySemaphore(device_, imageAvailable_, nullptr);
            if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
            for (const auto& frame : frames_) {
                if (frame.ready) vkDestroySemaphore(device_, frame.ready, nullptr);
                if (frame.framebuffer) vkDestroyFramebuffer(device_, frame.framebuffer, nullptr);
                if (frame.view) vkDestroyImageView(device_, frame.view, nullptr);
            }
            if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
            if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        ANativeWindow_release(window_);
    }

    void initialize() {
        createSurface();
        createDevice();
        createSwapchain();
        createRenderPass();
        createFrames();
        createCommands();
    }

    bool draw() {
        uint32_t index = 0;
        constexpr uint64_t acquireTimeout = 250'000'000;  // Nanoseconds.
        const VkResult acquired = vkAcquireNextImageKHR(
            device_, swapchain_, acquireTimeout, imageAvailable_, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_TIMEOUT) {
            return false;
        }
        if (acquired != VK_SUBOPTIMAL_KHR) {
            check(acquired, "vkAcquireNextImageKHR");
        }

        const Frame& frame = frames_[index];
        check(vkResetCommandPool(device_, commandPool_, 0), "vkResetCommandPool");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");

        const VkClearValue color{{{0.02f, 0.16f, 0.45f, 1.0f}}};
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = renderPass_;
        pass.framebuffer = frame.framebuffer;
        pass.renderArea.extent = extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &color;

        vkCmdBeginRenderPass(command_, &pass, VK_SUBPASS_CONTENTS_INLINE);
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
        check(vkResetFences(device_, 1, &fence_), "vkResetFences");
        check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &frame.ready;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
            return false;
        }
        if (presented != VK_SUBOPTIMAL_KHR) {
            check(presented, "vkQueuePresentKHR");
        }
        return true;
    }

private:
    struct Frame {
        VkImageView view{};
        VkFramebuffer framebuffer{};
        // Reacquiring this image ensures present has consumed its semaphore.
        VkSemaphore ready{};
    };

    void createSurface() {
        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "eng";
        app.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo instance{};
        instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance.pApplicationInfo = &app;
        instance.enabledExtensionCount = 2;
        instance.ppEnabledExtensionNames = extensions;
        check(vkCreateInstance(&instance, nullptr, &instance_), "vkCreateInstance");

        VkAndroidSurfaceCreateInfoKHR surface{};
        surface.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surface.window = window_;
        check(vkCreateAndroidSurfaceKHR(instance_, &surface, nullptr, &surface_),
              "vkCreateAndroidSurfaceKHR");
    }

    bool selectQueue(VkPhysicalDevice candidate) {
        const auto extensions = enumerate<VkExtensionProperties>(
            [candidate](uint32_t* count, VkExtensionProperties* values) {
                return vkEnumerateDeviceExtensionProperties(candidate, nullptr, count, values);
            });
        const auto isSwapchain = [](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        };
        if (std::none_of(extensions.begin(), extensions.end(), isSwapchain)) {
            return false;
        }

        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
        for (uint32_t index = 0; index < count; ++index) {
            if (!(queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            VkBool32 canPresent = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_, &canPresent),
                  "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (canPresent) {
                physicalDevice_ = candidate;
                queueFamily_ = index;
                return true;
            }
        }
        return false;
    }

    void createDevice() {
        const auto devices = enumerate<VkPhysicalDevice>(
            [this](uint32_t* count, VkPhysicalDevice* values) {
                return vkEnumeratePhysicalDevices(instance_, count, values);
            });
        const auto suitable = [this](VkPhysicalDevice candidate) { return selectQueue(candidate); };
        if (std::none_of(devices.begin(), devices.end(), suitable)) {
            throw std::runtime_error("No Vulkan queue supports graphics and presentation");
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = queueFamily_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;

        const char* extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device{};
        device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        device.enabledExtensionCount = 1;
        device.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(physicalDevice_, &device, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        extent_ = caps.currentExtent;
        if (extent_.width == std::numeric_limits<uint32_t>::max()) {
            const auto width = static_cast<uint32_t>(std::max(0, ANativeWindow_getWidth(window_)));
            const auto height = static_cast<uint32_t>(std::max(0, ANativeWindow_getHeight(window_)));
            extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent_.width == 0 || extent_.height == 0) {
            throw std::runtime_error("Vulkan surface has zero extent");
        }

        const auto formats = enumerate<VkSurfaceFormatKHR>(
            [this](uint32_t* count, VkSurfaceFormatKHR* values) {
                return vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, count, values);
            });
        if (formats.empty()) {
            throw std::runtime_error("Vulkan surface has no supported formats");
        }
        const auto isSrgb = [](const VkSurfaceFormatKHR& format) {
            return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                   (format.format == VK_FORMAT_R8G8B8A8_SRGB ||
                    format.format == VK_FORMAT_B8G8R8A8_SRGB);
        };
        const auto preferred = std::find_if(formats.begin(), formats.end(), isSrgb);
        VkSurfaceFormatKHR format = preferred == formats.end() ? formats.front() : *preferred;
        if (format.format == VK_FORMAT_UNDEFINED) {
            format.format = VK_FORMAT_R8G8B8A8_UNORM;
        }
        format_ = format.format;

        const VkCompositeAlphaFlagBitsKHR alphaModes[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        };
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (const auto mode : alphaModes) {
            if (caps.supportedCompositeAlpha & mode) {
                alpha = mode;
                break;
            }
        }

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount != 0) {
            imageCount = std::min(imageCount, caps.maxImageCount);
        }

        VkSwapchainCreateInfoKHR swapchain{};
        swapchain.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchain.surface = surface_;
        swapchain.minImageCount = imageCount;
        swapchain.imageFormat = format_;
        swapchain.imageColorSpace = format.colorSpace;
        swapchain.imageExtent = extent_;
        swapchain.imageArrayLayers = 1;
        swapchain.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapchain.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain.preTransform = caps.currentTransform;
        swapchain.compositeAlpha = alpha;
        swapchain.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchain.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device_, &swapchain, nullptr, &swapchain_),
              "vkCreateSwapchainKHR");
    }

    void createRenderPass() {
        VkAttachmentDescription attachment{};
        attachment.format = format_;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        const VkAttachmentReference output{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &output;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass.attachmentCount = 1;
        pass.pAttachments = &attachment;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &dependency;
        check(vkCreateRenderPass(device_, &pass, nullptr, &renderPass_), "vkCreateRenderPass");
    }

    void createFrames() {
        const auto images = enumerate<VkImage>([this](uint32_t* count, VkImage* values) {
            return vkGetSwapchainImagesKHR(device_, swapchain_, count, values);
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
            check(vkCreateImageView(device_, &view, nullptr, &frame.view), "vkCreateImageView");

            VkFramebufferCreateInfo framebuffer{};
            framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer.renderPass = renderPass_;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &frame.view;
            framebuffer.width = extent_.width;
            framebuffer.height = extent_.height;
            framebuffer.layers = 1;
            check(vkCreateFramebuffer(device_, &framebuffer, nullptr, &frame.framebuffer),
                  "vkCreateFramebuffer");
            check(vkCreateSemaphore(device_, &semaphore, nullptr, &frame.ready),
                  "vkCreateSemaphore");
        }
        check(vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_),
              "vkCreateSemaphore");
    }

    void createCommands() {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = queueFamily_;
        check(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buffer.commandPool = commandPool_;
        buffer.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffer.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &buffer, &command_), "vkAllocateCommandBuffers");

        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
    }

    ANativeWindow* window_;
    VkInstance instance_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physicalDevice_{};
    uint32_t queueFamily_{};
    VkDevice device_{};
    VkQueue queue_{};
    VkSwapchainKHR swapchain_{};
    VkExtent2D extent_{};
    VkFormat format_{};
    VkRenderPass renderPass_{};
    std::vector<Frame> frames_;
    VkCommandPool commandPool_{};
    VkCommandBuffer command_{};
    VkSemaphore imageAvailable_{};
    VkFence fence_{};
};

void throwJavaError(JNIEnv* env, const char* message) {
    if (!env->ExceptionCheck()) {
        jclass exception = env->FindClass("java/lang/RuntimeException");
        if (exception) {
            env->ThrowNew(exception, message);
        }
    }
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_io_github_skb8_eng_MainActivity_nativeCreate(JNIEnv* env, jclass, jobject surface) {
    try {
        std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)> window(
            ANativeWindow_fromSurface(env, surface), ANativeWindow_release);
        if (!window) {
            throw std::runtime_error("Cannot acquire an Android native window");
        }

        auto renderer = std::make_unique<Renderer>(window.get());
        window.release();  // Renderer owns the window reference.
        renderer->initialize();
        return reinterpret_cast<jlong>(renderer.release());
    } catch (const std::exception& error) {
        throwJavaError(env, error.what());
        return 0;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_skb8_eng_MainActivity_nativeDraw(JNIEnv* env, jclass, jlong handle) {
    try {
        return reinterpret_cast<Renderer*>(handle)->draw() ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& error) {
        throwJavaError(env, error.what());
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_skb8_eng_MainActivity_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<Renderer*>(handle);
}
