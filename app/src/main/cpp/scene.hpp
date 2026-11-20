#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

struct Vertex {
    glm::vec3 position;
    glm::vec4 color;
};

constexpr uint32_t cubeVertexCount = 36;
constexpr uint32_t maxHudVertices = 8192;

std::array<Vertex, cubeVertexCount> cubeVertices();

class Scene {
public:
    void touch(int action, float x, float y);
    void update(float seconds);
    void presented(float seconds);
    glm::mat4 viewProjection(float aspect) const;
    const std::vector<Vertex>& hud(uint32_t width, uint32_t height) const;
    void beginSurface();

private:
    float yaw_ = 0.55f;
    float pitch_ = 0.35f;
    bool dragging_ = false;
    glm::vec2 previousTouch_{};
    float sampleSeconds_ = 0.0f;
    unsigned sampleFrames_ = 0;
    unsigned fps_ = 0;
    float frameMilliseconds_ = 0.0f;
    mutable std::vector<Vertex> hudVertices_;
};
