#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "renderer.hpp"
#include "scene.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {

using Window = std::shared_ptr<ANativeWindow>;
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<float>;
using namespace std::chrono_literals;

struct SurfaceRequest {
    Window window;
    VkExtent2D size{};
    jlong owner = 0;
};

struct Touch {
    int action;
    float x;
    float y;
};

class AndroidSurface {
public:
    AndroidSurface(VkInstance instance, ANativeWindow* window) : instance_(instance) {
        VkAndroidSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        info.window = window;
        check(vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &handle),
              "vkCreateAndroidSurfaceKHR");
    }

    ~AndroidSurface() { vkDestroySurfaceKHR(instance_, handle, nullptr); }
    AndroidSurface(const AndroidSurface&) = delete;
    AndroidSurface& operator=(const AndroidSurface&) = delete;

    VkSurfaceKHR handle{};

private:
    VkInstance instance_;
};

void logFailure(const std::exception& error) {
    __android_log_print(ANDROID_LOG_ERROR, "eng", "%s", error.what());
}

class Game {
public:
    Game() : thread_(&Game::run, this) {}

    ~Game() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    jlong setSurface(Window window, jlong owner, VkExtent2D size) {
        std::unique_lock lock(mutex_);
        if (owner != 0 && owner != request_.owner) {
            // A newer owner may be queued while the worker still uses the old window.
            if (!window) waitUntilApplied(lock, revision_);
            return 0;
        }
        if (!window && owner == 0) return 0;

        const bool detaching = !window;
        if (!detaching && owner == 0) owner = ++nextOwner_;
        request_ = {std::move(window), size, detaching ? 0 : owner};
        touches_.clear();
        const auto revision = ++revision_;
        changed_.notify_all();
        // Android can destroy the Surface as soon as surfaceDestroyed returns.
        if (detaching) waitUntilApplied(lock, revision);
        return detaching ? 0 : owner;
    }

    void touch(jlong owner, Touch touch) {
        std::lock_guard lock(mutex_);
        if (owner == 0 || owner != request_.owner) return;

        constexpr int move = 2;
        if (touch.action == move && !touches_.empty() && touches_.back().action == move) {
            touches_.back() = touch;
            return;
        }
        if (touches_.size() == 64) touches_.pop_front();
        touches_.push_back(touch);
    }

private:
    void waitUntilApplied(std::unique_lock<std::mutex>& lock, uint64_t revision) {
        changed_.wait(lock, [this, revision] { return applied_ >= revision || stopping_; });
    }

    void run() {
        Scene scene;
        SurfaceRequest active;
        std::unique_ptr<VulkanContext> context;
        std::unique_ptr<AndroidSurface> surface;
        std::unique_ptr<Swapchain> swapchain;
        bool deviceReady = false;
        bool blocked = false;
        uint64_t current = 0;
        auto lastUpdate = Clock::now();
        auto lastPresent = lastUpdate;
        std::deque<Touch> input;

        for (;;) {
            SurfaceRequest next;
            bool newSurface = false;
            {
                std::unique_lock lock(mutex_);
                if (!active.window || active.size.width == 0 || active.size.height == 0 || blocked) {
                    changed_.wait(lock, [this, current] { return stopping_ || revision_ != current; });
                }
                if (stopping_) return;
                if (revision_ != current) {
                    current = revision_;
                    next = request_;
                    newSurface = true;
                }
                input.clear();
                input.swap(touches_);
            }

            if (newSurface) {
                swapchain.reset();
                if (next.owner != active.owner || next.window.get() != active.window.get()) {
                    surface.reset();
                }
                active = std::move(next);
                blocked = false;
                scene.beginSurface();
                lastUpdate = lastPresent = Clock::now();
                {
                    std::lock_guard lock(mutex_);
                    applied_ = current;
                }
                changed_.notify_all();
            }
            if (!active.window || active.size.width == 0 || active.size.height == 0) continue;

            for (const auto& touch : input) scene.touch(touch.action, touch.x, touch.y);

            try {
                if (!context) {
                    context = std::make_unique<VulkanContext>();
                    context->createInstance();
                }
                if (!surface) {
                    surface = std::make_unique<AndroidSurface>(context->instance, active.window.get());
                    if (deviceReady) context->requirePresentSupport(surface->handle);
                }
                if (!deviceReady) {
                    context->createDevice(surface->handle);
                    deviceReady = true;
                }
                if (!swapchain) {
                    auto nextSwapchain = std::make_unique<Swapchain>(*context);
                    if (!nextSwapchain->create(surface->handle, active.size)) {
                        waitForSurfaceChange(current);
                        continue;
                    }
                    swapchain = std::move(nextSwapchain);
                    lastPresent = Clock::now();
                }

                const auto now = Clock::now();
                scene.update(Seconds(now - lastUpdate).count());
                lastUpdate = now;
                switch (swapchain->draw(scene)) {
                    case Swapchain::FrameResult::Presented: {
                        const auto presented = Clock::now();
                        scene.presented(Seconds(presented - lastPresent).count());
                        lastPresent = presented;
                        break;
                    }
                    case Swapchain::FrameResult::Retry:
                        waitForSurfaceChange(current);
                        break;
                    case Swapchain::FrameResult::Outdated:
                        swapchain.reset();
                        waitForSurfaceChange(current);
                        break;
                }
            } catch (const VulkanError& error) {
                if (error.result == VK_ERROR_OUT_OF_DATE_KHR) {
                    swapchain.reset();
                    waitForSurfaceChange(current);
                    continue;
                }
                logFailure(error);
                swapchain.reset();
                surface.reset();
                if (!deviceReady || error.result == VK_ERROR_DEVICE_LOST) {
                    context.reset();
                    deviceReady = false;
                }
                blocked = true;
            } catch (const std::exception& error) {
                logFailure(error);
                swapchain.reset();
                surface.reset();
                if (!deviceReady) context.reset();
                blocked = true;
            }
        }
    }

    void waitForSurfaceChange(uint64_t current) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, 16ms, [this, current] { return stopping_ || revision_ != current; });
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    SurfaceRequest request_;
    std::deque<Touch> touches_;
    jlong nextOwner_ = 0;
    uint64_t revision_ = 0;
    uint64_t applied_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

Game& game() {
    static Game instance;
    return instance;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_io_github_skb8_eng_MainActivity_nativeSurface(JNIEnv* env, jclass, jobject surface,
                                                 jlong owner, jint width, jint height) {
    try {
        Window window;
        if (surface) {
            auto* nativeWindow = ANativeWindow_fromSurface(env, surface);
            if (!nativeWindow) return owner;
            window = Window(nativeWindow, ANativeWindow_release);
        }
        const VkExtent2D size{width > 0 ? static_cast<uint32_t>(width) : 0u,
                              height > 0 ? static_cast<uint32_t>(height) : 0u};
        return game().setSurface(std::move(window), owner, size);
    } catch (const std::exception& error) {
        logFailure(error);
        return owner;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_skb8_eng_MainActivity_nativeTouch(JNIEnv*, jclass, jlong owner, jint action,
                                               jfloat x, jfloat y) {
    try {
        if (owner != 0) game().touch(owner, {action, x, y});
    } catch (const std::exception& error) {
        logFailure(error);
    }
}
