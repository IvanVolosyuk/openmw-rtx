#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/device/memory/image.hpp>
#include <components/rtxvulkan/device/memory/imagearena.hpp>

#include "upscaleinputs.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;

    enum class DlssMode
    {
        SuperResolution,
        RayReconstruction,
    };

    /// NVIDIA DLSS Super Resolution and Ray Reconstruction, through the NGX runtime the driver
    /// installs. The SDK's loader is linked in; the model itself is the driver's, so a card and
    /// driver NGX accepts at start is what this class needs. Feature creation and evaluation are
    /// recorded into the frame's own command buffer, as FSR's dispatches are.
    ///
    /// **The SDK is the latest at build time and the preset is left at its default**, so NVIDIA's
    /// over-the-air model updates reach a build that predates them. `describe()` names the SDK the
    /// binary was compiled against, which is what a report needs to compare.
    class DlssUpscaler
    {
    public:
        explicit DlssUpscaler(const Device& device, DlssMode mode = DlssMode::SuperResolution);
        ~DlssUpscaler();

        DlssUpscaler(const DlssUpscaler&) = delete;
        DlssUpscaler& operator=(const DlssUpscaler&) = delete;

        DlssMode getMode() const { return mMode; }

        /// Builds what one pair of extents keeps, releasing the last, and starts a history of its
        /// own. Takes `shared`, the image `declareShared` declared, as the frame's arena placed it.
        void resize(VkExtent2D render, VkExtent2D output, const ArenaRun& shared);

        /// The one image a reconstruction at these extents writes and no later frame reads, which
        /// the frame's arena places: the upscaled output, which the display reads.
        static void declareShared(VkExtent2D render, VkExtent2D output, std::vector<ArenaImage>& into);

        /// How many images `declareShared` declares.
        static std::size_t getSharedCount();

        /// What `resize` to these extents builds in the device's memory, apart from the shared
        /// image the arena holds.
        static VkDeviceSize bytesAt(const Device& device, DlssMode mode, VkExtent2D render, VkExtent2D output);

        /// Lets what `resize` built go, the feature included.
        void release();

        bool isBuilt() const { return mTargets != nullptr; }
        bool isAt(VkExtent2D render, VkExtent2D output) const;

        void reset() { mFresh = true; }
        bool isFresh() const { return mFresh; }

        /// Records the input resolve and the NGX evaluation, and hands the output image to the
        /// display. After `resize`.
        HandedImage record(VkCommandBuffer commands, const UpscaleInputs& inputs, GpuTimer* timer);

        /// One line for `info`: which DLSS this binary was built against.
        static std::string_view describe(DlssMode mode = DlssMode::SuperResolution);

    private:
        struct Context;
        struct Targets;

        const Device& mDevice;
        DlssMode mMode = DlssMode::SuperResolution;

        /// NGX's process-wide state for this device, made on the first resize: a configuration
        /// that only ever runs FSR never loads the runtime.
        std::unique_ptr<Context> mContext;

        /// Empty until `resize`, and after `release`.
        std::unique_ptr<Targets> mTargets;

        bool mFresh = true;
    };
}
