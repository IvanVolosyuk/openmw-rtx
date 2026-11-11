#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx::Dlss
{
    /// Whether this build was made with NVIDIA's NGX SDK. The one reading of the build flag, so
    /// the rest of the backend can ask a `constexpr bool` and not the preprocessor.
    inline constexpr bool sCompiled = OPENMW_RTX_DLSS;

    /// The project identifier NGX files DLSS's model and over-the-air updates under. It is a
    /// GUID-like custom-engine id, not an NVIDIA application id; any stable id of the fork's own
    /// serves, and changing it starts NGX's choices for this application over.
    inline constexpr const char* sProjectId = "4f3c1d2e-7a6b-4c58-9e0f-1a2b3c4d5e6f";

    /// The instance extensions NGX needs for DLSS Super Resolution and Ray Reconstruction on this
    /// driver, which the instance must enable before it is made. Asked before there is an instance,
    /// which is why it takes none.
    std::vector<std::string> requiredInstanceExtensions();

    /// The device extensions NGX needs for DLSS Super Resolution and Ray Reconstruction on this
    /// driver, which a device must enable before it is made. Empty where NGX cannot answer, which
    /// leaves feature creation to report why.
    std::vector<std::string> requiredDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice);

    /// Whether the driver and card accept DLSS Super Resolution's feature, as NGX answers before a
    /// device exists.
    bool isSupported(VkInstance instance, VkPhysicalDevice physicalDevice);

    /// Whether the driver and card accept DLSS Ray Reconstruction's feature, as NGX answers before a
    /// device exists.
    bool isRayReconstructionSupported(VkInstance instance, VkPhysicalDevice physicalDevice);
}
