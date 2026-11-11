#include "dlssupscaler.hpp"

#include <string_view>

#include <components/rtx/common/error.hpp>

namespace Rtx
{
    struct DlssUpscaler::Context
    {
    };

    struct DlssUpscaler::Targets
    {
    };

    DlssUpscaler::DlssUpscaler(const Device& device, const DlssMode mode)
        : mDevice(device)
        , mMode(mode)
    {
    }

    DlssUpscaler::~DlssUpscaler() = default;

    void DlssUpscaler::declareShared(VkExtent2D, VkExtent2D, std::vector<ArenaImage>&) {}

    std::size_t DlssUpscaler::getSharedCount()
    {
        return 0;
    }

    VkDeviceSize DlssUpscaler::bytesAt(const Device&, DlssMode, VkExtent2D, VkExtent2D)
    {
        return 0;
    }

    void DlssUpscaler::resize(VkExtent2D, VkExtent2D, const ArenaRun&)
    {
        throw Unsupported("this build was made without NVIDIA's DLSS SDK");
    }

    void DlssUpscaler::release() {}

    bool DlssUpscaler::isAt(VkExtent2D, VkExtent2D) const
    {
        return false;
    }

    HandedImage DlssUpscaler::record(VkCommandBuffer, const UpscaleInputs&, GpuTimer*)
    {
        throw Unsupported("this build was made without NVIDIA's DLSS SDK");
    }

    std::string_view DlssUpscaler::describe(const DlssMode mode)
    {
        return mode == DlssMode::RayReconstruction ? "DLSS Ray Reconstruction (not built)" : "DLSS (not built)";
    }
}
