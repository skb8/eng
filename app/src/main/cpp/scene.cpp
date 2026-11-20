#include "scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

std::array<Vertex, cubeVertexCount> cubeVertices() {
    constexpr glm::vec3 corners[] = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
    };
    struct Face {
        int corners[4];
        glm::vec4 color;
    };
    constexpr Face faces[] = {
        {{0, 3, 2, 1}, {0.09f, 0.34f, 0.95f, 1}}, {{4, 5, 6, 7}, {0.82f, 0.20f, 0.13f, 1}},
        {{0, 4, 7, 3}, {0.53f, 0.17f, 0.74f, 1}}, {{1, 2, 6, 5}, {0.06f, 0.58f, 0.38f, 1}},
        {{3, 7, 6, 2}, {0.95f, 0.57f, 0.08f, 1}}, {{0, 1, 5, 4}, {0.04f, 0.53f, 0.71f, 1}},
    };
    std::array<Vertex, cubeVertexCount> vertices{};
    size_t next = 0;
    for (const auto& face : faces) {
        for (int index : {0, 1, 2, 0, 2, 3}) {
            vertices[next++] = {corners[face.corners[index]], face.color};
        }
    }
    return vertices;
}

void Scene::touch(int action, float x, float y) {
    const glm::vec2 point{x, y};
    switch (action) {
        case 0:  // Android ACTION_DOWN.
            dragging_ = true;
            previousTouch_ = point;
            break;
        case 2:  // Android ACTION_MOVE.
            if (dragging_) {
                const auto delta = point - previousTouch_;
                yaw_ -= delta.x * 5.5f;
                pitch_ = std::clamp(pitch_ + delta.y * 3.5f, -1.4f, 1.4f);
                previousTouch_ = point;
            }
            break;
        default:  // ACTION_UP and ACTION_CANCEL.
            dragging_ = false;
            break;
    }
}

void Scene::update(float seconds) {
    if (!dragging_) {
        yaw_ += std::min(seconds, 0.1f) * 0.55f;
    }
    yaw_ = std::remainder(yaw_, 6.2831853f);
}

void Scene::presented(float seconds) {
    if (seconds > 0.0f) {
        sampleSeconds_ += seconds;
        ++sampleFrames_;
        if (sampleSeconds_ >= 0.35f) {
            fps_ = static_cast<unsigned>(std::lround(sampleFrames_ / sampleSeconds_));
            frameMilliseconds_ = 1000.0f * sampleSeconds_ / sampleFrames_;
            sampleSeconds_ = 0.0f;
            sampleFrames_ = 0;
        }
    }
}

void Scene::beginSurface() {
    dragging_ = false;
    sampleSeconds_ = 0.0f;
    sampleFrames_ = 0;
    fps_ = 0;
    frameMilliseconds_ = 0.0f;
}

glm::mat4 Scene::viewProjection(float aspect) const {
    constexpr float distance = 5.8f;
    const glm::vec3 eye = distance * glm::vec3{
        std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_),
        std::cos(yaw_) * std::cos(pitch_)};
    const float verticalFov = 2.0f * std::atan(0.43f / std::min(aspect, 1.0f));
    auto projection = glm::perspective(verticalFov, aspect, 0.1f, 50.0f);
    projection[1][1] *= -1.0f;
    return projection * glm::lookAt(eye, glm::vec3{0}, glm::vec3{0, 1, 0});
}

namespace {
using Glyph = std::array<uint8_t, 7>;

// Each number encodes a row of five pixels, from the most significant bit.
constexpr Glyph digits[] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
};

constexpr Glyph glyph(char ch) {
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    switch (ch) {
        case 'F': return {31, 16, 16, 30, 16, 16, 16};
        case 'M': return {17, 27, 21, 21, 17, 17, 17};
        case 'P': return {30, 17, 17, 30, 16, 16, 16};
        case 'S': return {15, 16, 16, 14, 1, 1, 30};
        case 'X': return {17, 17, 10, 4, 10, 17, 17};
        case '.': return {0, 0, 0, 0, 0, 0, 4};
        default: return {};
    }
}
}  // namespace

const std::vector<Vertex>& Scene::hud(uint32_t width, uint32_t height) const {
    hudVertices_.clear();
    char performance[32];
    char resolution[32];
    std::snprintf(performance, sizeof(performance), "%u FPS  %.1f MS", fps_, frameMilliseconds_);
    std::snprintf(resolution, sizeof(resolution), "%u X %u", width, height);
    const float scale = std::min(width, height) / 320.0f;
    const auto rect = [this, width, height](float x, float y, float w, float h, glm::vec4 color) {
        const float left = x * 2.0f / width - 1.0f;
        const float right = (x + w) * 2.0f / width - 1.0f;
        const float top = y * 2.0f / height - 1.0f;
        const float bottom = (y + h) * 2.0f / height - 1.0f;
        const Vertex corners[] = {
            {{left, top, 0}, color}, {{right, top, 0}, color},
            {{right, bottom, 0}, color}, {{left, bottom, 0}, color},
        };
        for (int index : {0, 1, 2, 0, 2, 3}) hudVertices_.push_back(corners[index]);
    };
    rect(8 * scale, 9 * scale, 130 * scale, 27 * scale, {0.009f, 0.016f, 0.028f, 0.85f});
    const auto line = [scale, &rect](const char* text, float y) {
        float x = 13.0f;
        for (; *text; ++text, x += 6.0f) {
            const auto rows = glyph(*text);
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if (rows[row] & (16 >> column)) {
                        rect((x + column) * scale, (y + row) * scale, scale, scale,
                             {0.75f, 0.89f, 0.96f, 1});
                    }
                }
            }
        }
    };
    line(performance, 14.0f);
    line(resolution, 24.0f);
    return hudVertices_;
}
