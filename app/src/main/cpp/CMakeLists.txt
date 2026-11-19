cmake_minimum_required(VERSION 3.22.1)
project(eng LANGUAGES CXX)

add_library(eng SHARED renderer.cpp)
target_compile_features(eng PRIVATE cxx_std_17)
target_compile_definitions(eng PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(eng PRIVATE -Wall -Wextra)
target_link_libraries(eng PRIVATE android vulkan)
