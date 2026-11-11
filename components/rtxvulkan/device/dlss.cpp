#include "dlss.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_vk.h>

namespace Rtx::Dlss
{
    namespace
    {
        constexpr std::array sFeatures{
            NVSDK_NGX_Feature_SuperSampling,
            NVSDK_NGX_Feature_RayReconstruction,
        };

        NVSDK_NGX_FeatureDiscoveryInfo discovery(const NVSDK_NGX_Feature feature)
        {
            NVSDK_NGX_FeatureDiscoveryInfo info{};
            info.SDKVersion = NVSDK_NGX_Version_API;
            info.FeatureID = feature;
            info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
            info.Identifier.v.ProjectDesc.ProjectId = sProjectId;
            info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
            info.Identifier.v.ProjectDesc.EngineVersion = "OpenMW RTX";
            info.ApplicationDataPath = L".";
            info.FeatureInfo = nullptr;
            return info;
        }

        void appendUnique(
            std::vector<std::string>& into, const VkExtensionProperties* const properties, const std::uint32_t count)
        {
            for (std::uint32_t at = 0; at < count; ++at)
            {
                const std::string_view name(properties[at].extensionName);
                if (std::ranges::find(into, name) == into.end())
                    into.emplace_back(name);
            }
        }

        bool featureSupported(
            const VkInstance instance, const VkPhysicalDevice physicalDevice, const NVSDK_NGX_Feature feature)
        {
            NVSDK_NGX_FeatureDiscoveryInfo info = discovery(feature);
            NVSDK_NGX_FeatureRequirement requirement{};
            const NVSDK_NGX_Result result
                = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physicalDevice, &info, &requirement);
            return NVSDK_NGX_SUCCEED(result)
                && requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
        }
    }

    std::vector<std::string> requiredInstanceExtensions()
    {
        std::vector<std::string> names;
        for (const NVSDK_NGX_Feature feature : sFeatures)
        {
            NVSDK_NGX_FeatureDiscoveryInfo info = discovery(feature);
            std::uint32_t count = 0;
            VkExtensionProperties* properties = nullptr;
            if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties))
                && properties != nullptr)
                appendUnique(names, properties, count);
        }
        return names;
    }

    std::vector<std::string> requiredDeviceExtensions(const VkInstance instance, const VkPhysicalDevice physicalDevice)
    {
        std::vector<std::string> names;
        for (const NVSDK_NGX_Feature feature : sFeatures)
        {
            NVSDK_NGX_FeatureDiscoveryInfo info = discovery(feature);
            std::uint32_t count = 0;
            VkExtensionProperties* properties = nullptr;
            if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                    instance, physicalDevice, &info, &count, &properties))
                && properties != nullptr)
                appendUnique(names, properties, count);
        }
        return names;
    }

    bool isSupported(const VkInstance instance, const VkPhysicalDevice physicalDevice)
    {
        return featureSupported(instance, physicalDevice, NVSDK_NGX_Feature_SuperSampling);
    }

    bool isRayReconstructionSupported(const VkInstance instance, const VkPhysicalDevice physicalDevice)
    {
        return featureSupported(instance, physicalDevice, NVSDK_NGX_Feature_RayReconstruction);
    }
}
