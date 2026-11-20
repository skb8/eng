cmake_minimum_required(VERSION 3.22.1)
project(eng LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(glm
    URL https://codeload.github.com/g-truc/glm/tar.gz/refs/tags/1.0.1
    URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c)
FetchContent_Declare(vma
    URL https://codeload.github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tar.gz/refs/tags/v3.2.1
    URL_HASH SHA256=5e7749504cb802427ffb7bec38a0b6a15db46ae253f00560acb3e624d9fe695c)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "Use header-only GLM")
FetchContent_MakeAvailable(glm vma)

set(glslc_hints "${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64"
    "${CMAKE_ANDROID_NDK}/shader-tools/darwin-x86_64"
    "${CMAKE_ANDROID_NDK}/shader-tools/windows-x86_64")
find_program(GLSLC glslc HINTS ${glslc_hints} NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(shader_headers)
foreach(shader IN ITEMS scene.vert scene.frag)
    set(input "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${GLSLC}" --target-env=vulkan1.0 -mfmt=num "${input}" -o "${output}"
        DEPENDS "${input}"
        VERBATIM)
    list(APPEND shader_headers "${output}")
endforeach()

add_library(eng SHARED game.cpp renderer.cpp scene.cpp vulkan_context.cpp vma.cpp ${shader_headers})
target_compile_features(eng PRIVATE cxx_std_17)
target_compile_definitions(eng PRIVATE VK_USE_PLATFORM_ANDROID_KHR GLM_FORCE_DEPTH_ZERO_TO_ONE
    VMA_VULKAN_VERSION=1000000 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
target_compile_options(eng PRIVATE -Wall -Wextra)
target_include_directories(eng PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(eng SYSTEM PRIVATE "${vma_SOURCE_DIR}/include")
target_link_libraries(eng PRIVATE android log vulkan glm::glm)
