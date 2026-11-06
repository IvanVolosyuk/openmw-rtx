#include "dlssupscaler.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_params.h>

#include <components/debug/debuglog.hpp>
#include <components/rtx/common/error.hpp>
#include <components/rtx/renderer/framezone.hpp>
#include <components/rtxvulkan/device/commands.hpp>
#include <components/rtxvulkan/device/device.hpp>
#include <components/rtxvulkan/device/dlss.hpp>
#include <components/rtxvulkan/device/gputimer.hpp>
#include <components/rtxvulkan/device/memory/barriers.hpp>
#include <components/rtxvulkan/device/memory/buffer.hpp>
#include <components/rtxvulkan/device/memory/formats.hpp>
#include <components/rtxvulkan/device/memory/imageuse.hpp>
#include <components/rtxvulkan/pipeline/computepipeline.hpp>
#include <components/rtxvulkan/pipeline/dispatch.hpp>
#include <components/rtxvulkan/pipeline/pipeline.hpp>
#include <components/rtxvulkan/shaders/shared/dlss.h>
#include <components/rtxvulkan/trace/framestep.hpp>

#include "fsrframe.hpp"

namespace Rtx
{
    namespace
    {
        /// What the input resolve binds, in the shader's own numbering. The surface and motion
        /// channels are storage images: the trace left them in `GENERAL`, and a resolve only ever
        /// loads texels.
        constexpr std::array sInputBindings{
            computeBinding(Shaders::DLSS_INPUTS_BIND_SURFACE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_MOTION, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_DEPTH, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_MOTION_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        };

        constexpr ImageUse sInputWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };

        /// The layout NGX is documented to read its inputs in, and the one it leaves its output in.
        /// The evaluation may change either while it runs and promises to put them back.
        constexpr ImageUse sDlssRead{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
        constexpr ImageUse sDlssWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };

        /// The quality value NGX picks its default preset and memory budget from. The renderer's
        /// modes are ratios; this is the nearest of NGX's own names, and the feature is still told
        /// the actual traced extent, so a 1.7 balanced differs from NGX's 1.724 by a few pixels a
        /// side and not by a model.
        NVSDK_NGX_PerfQuality_Value qualityOf(const VkExtent2D render, const VkExtent2D output)
        {
            const float ratio = static_cast<float>(output.width) / static_cast<float>(render.width);
            if (ratio <= 1.01f)
                return NVSDK_NGX_PerfQuality_Value_DLAA;
            if (ratio >= 2.9f)
                return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            if (ratio >= 1.9f)
                return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            if (ratio >= 1.6f)
                return NVSDK_NGX_PerfQuality_Value_Balanced;

            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }

        std::string resultText(const NVSDK_NGX_Result result)
        {
            return std::format("NGX result {:#x}", static_cast<std::uint32_t>(result));
        }

        ImageDescription depthDescription(const VkExtent2D extent)
        {
            return ImageDescription{ .mWidth = extent.width,
                .mHeight = extent.height,
                .mFormat = toVulkanFormat(DLSS_DEPTH_FORMAT),
                .mUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
        }

        ImageDescription motionDescription(const VkExtent2D extent)
        {
            return ImageDescription{ .mWidth = extent.width,
                .mHeight = extent.height,
                .mFormat = toVulkanFormat(DLSS_MOTION_FORMAT),
                .mUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
        }

        ImageDescription outputDescription(const VkExtent2D extent)
        {
            return ImageDescription{ .mWidth = extent.width,
                .mHeight = extent.height,
                .mFormat = toVulkanFormat(DLSS_OUTPUT_FORMAT),
                .mUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
        }

        NVSDK_NGX_Resource_VK viewOf(const Image& image)
        {
            return NVSDK_NGX_Create_ImageView_Resource_VK(image.getView(), image.getHandle(),
                VkImageSubresourceRange{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 },
                image.getFormat(), image.getWidth(), image.getHeight(),
                (image.getUsage() & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
        }
    }

    /// NGX's process-wide state for one device, made where DLSS first builds its targets. A
    /// configuration that only runs FSR never loads the runtime.
    struct DlssUpscaler::Context
    {
        explicit Context(const Device& device)
            : mDevice(device)
            , mInputs(device, sInputBindings, {}, "dlssinputs.comp.spv", "dlss inputs")
            , mBlocks([&](const FrameSlot) {
                return Buffer::hostWritten(device, sizeof(Shaders::DlssInputConstants),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "dlss constants");
            })
        {
            const std::wstring path = L".";
            const NVSDK_NGX_Result init = NVSDK_NGX_VULKAN_Init_with_ProjectID(Dlss::sProjectId,
                NVSDK_NGX_ENGINE_TYPE_CUSTOM, "OpenMW RTX", path.c_str(), device.getInstanceHandle(),
                device.getPhysicalDevice().getHandle(), device.getHandle());
            if (NVSDK_NGX_FAILED(init))
                throw Unsupported("NVIDIA DLSS could not initialize NGX: " + resultText(init)
                    + "; the DLSS SDK's runtime library must sit beside the game");

            const NVSDK_NGX_Result allocated = NVSDK_NGX_VULKAN_GetCapabilityParameters(&mParameters);
            if (NVSDK_NGX_FAILED(allocated) || mParameters == nullptr)
            {
                NVSDK_NGX_VULKAN_Shutdown1(device.getHandle());
                throw Unsupported("NVIDIA DLSS could not read NGX's capabilities: " + resultText(allocated));
            }

            int available = 0;
            const NVSDK_NGX_Result queried
                = mParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            if (NVSDK_NGX_FAILED(queried) || available == 0)
            {
                NVSDK_NGX_VULKAN_DestroyParameters(mParameters);
                mParameters = nullptr;
                NVSDK_NGX_VULKAN_Shutdown1(device.getHandle());
                throw Unsupported("NVIDIA DLSS is not available on this card or driver");
            }

            // The memory a released feature holds is cached by default for the next creation.
            // A resize is frequent enough that the cache is not worth the memory a card's whole
            // frame budget would rather keep for the world.
            NVSDK_NGX_Parameter_SetI(mParameters, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        }

        ~Context()
        {
            if (mParameters != nullptr)
                NVSDK_NGX_VULKAN_DestroyParameters(mParameters);
            NVSDK_NGX_VULKAN_Shutdown1(mDevice.getHandle());
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        const Device& mDevice;
        ComputePipeline<NoConstants> mInputs;
        PerSlot<Buffer> mBlocks;
        NVSDK_NGX_Parameter* mParameters = nullptr;
    };

    struct DlssUpscaler::Targets
    {
        Targets(const Device& device, const VkExtent2D render, const VkExtent2D output, const ArenaRun& shared)
            : mRender(render)
            , mOutput(output)
            , mShared(shared)
            , mDepth(MemoryUse::Frame, device, depthDescription(render), "dlss-depth")
            , mMotion(MemoryUse::Frame, device, motionDescription(render), "dlss-motion")
        {
        }

        ~Targets()
        {
            if (mFeature != nullptr)
                NVSDK_NGX_VULKAN_ReleaseFeature(mFeature);
        }

        Targets(const Targets&) = delete;
        Targets& operator=(const Targets&) = delete;

        VkExtent2D mRender;
        VkExtent2D mOutput;
        ArenaRun mShared;
        Image mDepth;
        Image mMotion;
        NVSDK_NGX_Handle* mFeature = nullptr;
    };

    DlssUpscaler::DlssUpscaler(const Device& device)
        : mDevice(device)
        , mContext(std::make_unique<Context>(device))
    {
    }

    DlssUpscaler::~DlssUpscaler() = default;

    void DlssUpscaler::declareShared(
        const VkExtent2D, const VkExtent2D output, std::vector<ArenaImage>& into)
    {
        into.push_back(ArenaImage{ .mDescription = outputDescription(output),
            .mSpan = spanOf(FrameStep::Upscale, FrameStep::Display),
            .mName = "dlss-output" });
    }

    std::size_t DlssUpscaler::getSharedCount()
    {
        return 1;
    }

    VkDeviceSize DlssUpscaler::bytesAt(
        const Device& device, const VkExtent2D render, const VkExtent2D)
    {
        return Image::bytesFor(device, depthDescription(render))
            + Image::bytesFor(device, motionDescription(render));
    }

    void DlssUpscaler::resize(const VkExtent2D render, const VkExtent2D output, const ArenaRun& shared)
    {
        assert(render.width > 0 && render.height > 0 && output.width >= render.width
            && output.height >= render.height);
        assert(shared.size() == 1 && "the DLSS upscaler's shared image was not placed as it declared it");

        if (isAt(render, output))
        {
            mTargets->mShared = shared;
            return;
        }

        // **The feature is made after the device and with a command buffer**, as NGX's Vulkan
        // integration asks: creation records its own work, and a resize is already a drained point.
        mTargets = std::make_unique<Targets>(mDevice, render, output, shared);

        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = render.width;
        create.Feature.InHeight = render.height;
        create.Feature.InTargetWidth = output.width;
        create.Feature.InTargetHeight = output.height;
        create.Feature.InPerfQualityValue = qualityOf(render, output);

        // **HDR, low-resolution motion, jittered motion and reversed depth**: the renderer's frame
        // is linear radiance, its motion is resolved at the render extent and already includes the
        // trace's jitter, and its depth is `near / view depth` with the near plane at one. Exposure
        // is DLSS's own meter, because the renderer's meter runs after the upscaler.
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
            | NVSDK_NGX_DLSS_Feature_Flags_MVJittered | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
            | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        create.InEnableOutputSubrects = false;

        NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
        mDevice.getPool().submitAndWait([&](VkCommandBuffer commands) {
            result = NGX_VULKAN_CREATE_DLSS_EXT(
                commands, 1, 1, &mTargets->mFeature, mContext->mParameters, &create);
        });

        if (NVSDK_NGX_FAILED(result) || mTargets->mFeature == nullptr)
            throw Unsupported("NVIDIA DLSS could not create its feature at " + std::to_string(render.width) + "x"
                + std::to_string(render.height) + " to " + std::to_string(output.width) + "x"
                + std::to_string(output.height) + ": " + resultText(result));

        mFresh = true;
    }

    void DlssUpscaler::release()
    {
        mTargets.reset();
    }

    bool DlssUpscaler::isAt(const VkExtent2D render, const VkExtent2D output) const
    {
        const auto same = [](const VkExtent2D a, const VkExtent2D b) {
            return a.width == b.width && a.height == b.height;
        };
        return mTargets != nullptr && same(mTargets->mRender, render) && same(mTargets->mOutput, output);
    }

    HandedImage DlssUpscaler::record(
        const VkCommandBuffer commands, const UpscaleInputs& inputs, GpuTimer* timer)
    {
        const GpuZone timed(timer, commands, FrameZone::Upscale);

        assert(mTargets != nullptr && "a DLSS upscale before a resize");
        assert(mContext != nullptr && "a DLSS upscale without NGX");
        Targets& targets = *mTargets;

        const bool reset = std::exchange(mFresh, false);
        const Shaders::DlssInputConstants constants{
            .mEyes = inputs.mEyes,
            .mNear = FsrFrame::sNear,
            .mFrame = inputs.mFrame,
        };
        const Buffer& block = mContext->mBlocks.at(inputs.mSlot);
        block.writeAt(0, std::span(&constants, 1));
        block.nameForNext();

        // **The resolve writes every pixel of both images**, so their previous contents are
        // discarded: the mode's own edge is the first frame after a resize, and the feature is new
        // then too.
        {
            Barriers barriers(commands);
            targets.mDepth.addTransition(barriers, Use::sUndefined, sInputWrite);
            targets.mMotion.addTransition(barriers, Use::sUndefined, sInputWrite);
            targets.mShared[0].addTransition(barriers, targets.mShared.firstUseOf(0), sDlssWrite);
            barriers.flush();
        }

        DescriptorWrites writes(mContext->mInputs);
        writes.image(Shaders::DLSS_INPUTS_BIND_SURFACE, inputs.mSurface.describeStorage());
        writes.image(Shaders::DLSS_INPUTS_BIND_MOTION, inputs.mMotion.describeStorage());
        writes.image(Shaders::DLSS_INPUTS_BIND_DEPTH, targets.mDepth.describeStorage());
        writes.image(Shaders::DLSS_INPUTS_BIND_MOTION_OUT, targets.mMotion.describeStorage());
        writes.buffer(Shaders::DLSS_INPUTS_BIND_CONSTANTS, block.describe(0, sizeof(constants)));
        dispatch(commands, mContext->mInputs, writes, NoConstants{},
            Groups::covering(targets.mRender.width, targets.mRender.height, Shaders::DLSS_INPUT_WORKGROUP));

        // The resolve's writes and the trace's channels handed to NGX, in one dependency. What NGX
        // does inside its evaluation is its own; the layout it is promised is the one the wrapper
        // names, and the evaluation leaves the output in `GENERAL`.
        {
            Barriers barriers(commands);
            inputs.mColour.addTransition(barriers, Use::sAnyGeneralRead, sDlssRead);
            targets.mDepth.addTransition(barriers, sInputWrite, sDlssRead);
            targets.mMotion.addTransition(barriers, sInputWrite, sDlssRead);
            barriers.flush();
        }

        NVSDK_NGX_Resource_VK colour = viewOf(inputs.mColour);
        NVSDK_NGX_Resource_VK depth = viewOf(targets.mDepth);
        NVSDK_NGX_Resource_VK motion = viewOf(targets.mMotion);
        NVSDK_NGX_Resource_VK output = viewOf(targets.mShared[0]);

        NVSDK_NGX_VK_DLSS_Eval_Params evaluation{};
        evaluation.Feature.pInColor = &colour;
        evaluation.Feature.pInOutput = &output;
        evaluation.pInDepth = &depth;
        evaluation.pInMotionVectors = &motion;
        evaluation.InJitterOffsetX = inputs.mEyes.mWorld.mJitter.x();
        evaluation.InJitterOffsetY = inputs.mEyes.mWorld.mJitter.y();
        evaluation.InRenderSubrectDimensions
            = NVSDK_NGX_Dimensions{ .Width = targets.mRender.width, .Height = targets.mRender.height };
        evaluation.InReset = reset ? 1 : 0;
        // The trace writes motion in render pixels, which is the unit DLSS asks for, and its
        // fourth component is the water split flag, which the resolve drops.
        evaluation.InMVScaleX = 1.0f;
        evaluation.InMVScaleY = 1.0f;
        evaluation.InPreExposure = 1.0f;
        evaluation.InExposureScale = 1.0f;
        evaluation.InFrameTimeDeltaInMsec = inputs.mSeconds * 1000.0f;

        const NVSDK_NGX_Result result
            = NGX_VULKAN_EVALUATE_DLSS_EXT(commands, targets.mFeature, mContext->mParameters, &evaluation);
        if (NVSDK_NGX_FAILED(result))
            throw std::runtime_error("NVIDIA DLSS failed to evaluate a frame: " + resultText(result));

        return HandedImage{ .mImage = targets.mShared[0], .mLeftAs = Use::sComputeReadWrite };
    }

    std::string_view DlssUpscaler::describe()
    {
        return "DLSS 4.5 (NGX 310.9.16)";
    }
}
