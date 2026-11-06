#include "dlss.hpp"

#include <cstdint>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_vk.h>

namespace Rtx::Dlss
{
    namespace
    {
        NVSDK_NGX_FeatureDiscoveryInfo discovery()
        {
            NVSDK_NGX_FeatureDiscoveryInfo info{};
            info.SDKVersion = NVSDK_NGX_Version_API;
            info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
            info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
            info.Identifier.v.ProjectDesc.ProjectId = sProjectId;
            info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
            info.Identifier.v.ProjectDesc.EngineVersion = "OpenMW RTX";
            info.ApplicationDataPath = L".";
            info.FeatureInfo = nullptr;
            return info;
        }
    }

    std::vector<std::string> requiredDeviceExtensions(const VkInstance instance, const VkPhysicalDevice physicalDevice)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info = discovery();
        std::uint32_t count = 0;
        VkExtensionProperties* properties = nullptr;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                instance, physicalDevice, &info, &count, &properties))
            || properties == nullptr)
            return {};

        std::vector<std::string> names;
        names.reserve(count);
        for (std::uint32_t at = 0; at < count; ++at)
            names.emplace_back(properties[at].extensionName);
        return names;
    }

    bool isSupported(const VkInstance instance, const VkPhysicalDevice physicalDevice)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info = discovery();
        NVSDK_NGX_FeatureRequirement requirement{};
        const NVSDK_NGX_Result result
            = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physicalDevice, &info, &requirement);
        return NVSDK_NGX_SUCCEED(result)
            && requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
    }
}
