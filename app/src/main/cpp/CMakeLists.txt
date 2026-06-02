cmake_minimum_required(VERSION 3.22.1)

Project(Pluvia)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wno-unused-function -Wimplicit-function-declaration")

# glslangValidator (for the vulkan_renderer window shaders). Not in the NDK; it
# ships with the Android emulator and the Vulkan SDK. Discover it via common
# locations so the Gradle externalNativeBuild works without a manual PATH; fall
# back to bare "glslangValidator" on PATH if not found.
find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS
        "${ANDROID_NDK}/../../emulator/lib64/vulkan"
        "$ENV{ANDROID_HOME}/emulator/lib64/vulkan"
        "$ENV{ANDROID_SDK_ROOT}/emulator/lib64/vulkan"
        "$ENV{VULKAN_SDK}/bin")
if(NOT GLSLANG_VALIDATOR)
    set(GLSLANG_VALIDATOR glslangValidator)
endif()

#add_subdirectory(proot)
add_subdirectory(virglrenderer)
add_subdirectory(patchelf)
add_subdirectory(extras/adrenotools)

add_library(winlator SHARED
            winlator/drawable.c
            winlator/gpu_image.c
            winlator/sysvshared_memory.c
            winlator/xconnector_epoll.c
            winlator/alsa_client.c
            winlator/patchelf_wrapper.cpp)

# Direct Android Compositing AHardwareBuffer pool (Android-side allocator +
# release-fence helpers). The DAC present-receiver itself is now compiled INTO
# vulkan_renderer (see below) so it can call VulkanRendererContext directly,
# instead of the old dlsym workaround. This lib holds only the pool natives.
add_library(dac SHARED
            winlator/ahb_bridge.c)
target_link_libraries(dac
                      log
                      android
                      dl)

target_link_libraries(winlator
                      log
                      android
                      jnigraphics
                      aaudio
                      EGL
                      GLESv2
                      GLESv3)


add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/window_vert.h
    COMMAND ${GLSLANG_VALIDATOR} -V --vn window_vert_code -x
            -o ${CMAKE_CURRENT_BINARY_DIR}/window_vert.h
            ${CMAKE_CURRENT_SOURCE_DIR}/winlator/window.vert
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/winlator/window.vert
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/window_frag.h
    COMMAND ${GLSLANG_VALIDATOR} -V --vn window_frag_code -x
            -o ${CMAKE_CURRENT_BINARY_DIR}/window_frag.h
            ${CMAKE_CURRENT_SOURCE_DIR}/winlator/window.frag
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/winlator/window.frag
)

add_library(vulkan_renderer SHARED
            winlator/vulkan_jni.cpp
            winlator/VulkanRendererContext.cpp
            winlator/VulkanRendererScanout.cpp
            winlator/dac_present_receiver.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/window_vert.h
            ${CMAKE_CURRENT_BINARY_DIR}/window_frag.h)

target_include_directories(vulkan_renderer PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_options(vulkan_renderer PRIVATE
                       -Wall -Wextra -fvisibility=hidden -std=c++17)

target_link_libraries(vulkan_renderer
                      log
                      android
                      vulkan
                      adrenotools
                      dl
                      atomic)
