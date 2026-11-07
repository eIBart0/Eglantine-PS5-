// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/frontend/emu_window.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "display_refresh.h"
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace Vulkan {
vk::SurfaceKHR CreateSurface(const vk::Instance& instance,
                            const Core::Frontend::EmuWindow::WindowSystemInfo& window_info) {
    if (window_info.type != Core::Frontend::WindowSystemType::PS5)
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    std::fprintf(stderr, "[EglantinePS5] Vulkan instance ready; creating native surface\n");
    const auto& dispatch = instance.Dispatch();
    const auto proc = [&](const char* name) {
        auto function = dispatch.vkGetInstanceProcAddr(*instance, name);
        if (!function) throw vk::Exception(VK_ERROR_EXTENSION_NOT_PRESENT);
        return function;
    };
    const auto check = [](VkResult result, int line = __builtin_LINE()) {
        std::fprintf(stderr, "[EglantinePS5] Vulkan surface line=%d result=%d\n", line, static_cast<int>(result));
        if (result != VK_SUCCESS) throw vk::Exception(result);
    };
    // ponytail: the native driver exposes one GPU/display/plane; enumerate all
    // and select explicitly if a future driver supports multiple outputs.
    uint32_t count = 1;
    VkPhysicalDevice physical{};
    check(reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(proc("vkEnumeratePhysicalDevices"))(
        *instance, &count, &physical));
    if (count != 1) {
        std::fprintf(stderr, "[EglantinePS5] Vulkan surface enumeration count=%u\n", count);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    VkDisplayPropertiesKHR display{};
    count = 1;
    check(reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
        proc("vkGetPhysicalDeviceDisplayPropertiesKHR"))(physical, &count, &display));
    if (count != 1) {
        std::fprintf(stderr, "[EglantinePS5] Vulkan surface enumeration count=%u\n", count);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const auto get_modes = reinterpret_cast<PFN_vkGetDisplayModePropertiesKHR>(
        proc("vkGetDisplayModePropertiesKHR"));
    std::vector<VkDisplayModePropertiesKHR> modes;
    VkResult result;
    do {
        count = 0;
        check(get_modes(physical, display.display, &count, nullptr));
        if (!count) throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
        modes.resize(count);
        result = get_modes(physical, display.display, &count, modes.data());
    } while (result == VK_INCOMPLETE);
    check(result);
    if (!count) throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    modes.resize(count);
    // The driver lists 120 Hz first when the output took it (display_refresh.h). A session that
    // asked for it takes that mode; any other keeps the 60 Hz one.
    // The size: the output resolution's (Settings > Video), so the frame is copied 1:1 and VideoOut
    // scales it to the screen. The driver lists 3840x2160 first, and its five framebuffers are
    // 160 MiB of the pool where five at 1920x1080 are 40 (each size a session uses is registered
    // and kept until the session's device goes). EDEN_PS5_SCANOUT_4K=1: 3840x2160 as before.
    const bool fast = Eden::Display::requested_hz.load() >= 120;
    const bool scanout_4k = [] {
        const char* value = std::getenv("EDEN_PS5_SCANOUT_4K");
        return value != nullptr && value[0] == '1';
    }();
    const auto width = static_cast<uint32_t>(Eden::Display::output_width.load());
    const auto height = static_cast<uint32_t>(Eden::Display::output_height.load());
    auto mode = modes.front();
    bool found = false;
    for (const auto& candidate : modes) {
        if ((candidate.parameters.refreshRate >= 119000) == fast && !scanout_4k &&
            candidate.parameters.visibleRegion.width == width &&
            candidate.parameters.visibleRegion.height == height) {
            mode = candidate;
            found = true;
            break;
        }
    }
    for (const auto& candidate : modes) {
        if (found) break;
        if ((candidate.parameters.refreshRate >= 119000) == fast) {
            mode = candidate;
            break;
        }
    }
    Eden::Display::output_millihertz.store(static_cast<int>(mode.parameters.refreshRate));
    if (mode.parameters.refreshRate >= 119000) Eden::Display::settle.store(true);
    std::fprintf(stderr, "[EglantinePS5] Vulkan display: %u modes, asked for %d Hz at %ux%u, using %.2f Hz at %ux%u\n",
                 count, Eden::Display::requested_hz.load(), width, height, mode.parameters.refreshRate / 1000.0,
                 mode.parameters.visibleRegion.width, mode.parameters.visibleRegion.height);
    const VkDisplaySurfaceCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode,
        .planeIndex = 0,
        .planeStackIndex = 0,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = mode.parameters.visibleRegion,
    };
    VkSurfaceKHR surface{};
    check(reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(proc("vkCreateDisplayPlaneSurfaceKHR"))(
        *instance, &info, nullptr, &surface));
    return vk::SurfaceKHR(surface, *instance, dispatch);
}
} // namespace Vulkan
