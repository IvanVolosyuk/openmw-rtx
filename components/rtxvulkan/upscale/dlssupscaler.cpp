#include "dlssupscaler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// clang-format off
// `nvsdk_ngx_helpers_dlssd_vk.h` uses `NVSDK_NGX_ENSURE_VK_IMAGEVIEW`, defined in
// `nvsdk_ngx_helpers_vk.h`, without including it first.
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_params_dlssd.h>
// clang-format on

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
        /// What the Super Resolution input resolve binds, in the shader's own numbering. The surface
        /// and motion channels are storage images: the trace left them in `GENERAL`, and a resolve
        /// only ever loads texels.
        constexpr std::array sInputBindings{
            computeBinding(Shaders::DLSS_INPUTS_BIND_SURFACE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_MOTION, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_DEPTH, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_MOTION_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSS_INPUTS_BIND_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        };

        /// What the Ray Reconstruction input resolve binds, in the shader's own numbering.
        constexpr std::array sRrInputBindings{
            computeBinding(Shaders::DLSSD_INPUTS_BIND_SURFACE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_MOTION, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_ALBEDO, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_SPECULAR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_SPECULAR_ALBEDO, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_DEPTH, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_MOTION_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_DIFFUSE_ALBEDO_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_SPECULAR_ALBEDO_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_NORMAL_ROUGHNESS_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::DLSSD_INPUTS_BIND_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        };

        constexpr ImageUse sInputWrite{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };

        /// The layout NGX is documented to read its inputs in, and the one it leaves its output in.
        /// The evaluation may change either while it runs and promises to put them back.
        constexpr ImageUse sDlssRead{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT };
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

        std::string_view modeName(const DlssMode mode)
        {
            return mode == DlssMode::RayReconstruction ? "NVIDIA DLSS Ray Reconstruction" : "NVIDIA DLSS";
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

        ImageDescription albedoDescription(const VkExtent2D extent)
        {
            return ImageDescription{ .mWidth = extent.width,
                .mHeight = extent.height,
                .mFormat = toVulkanFormat(DLSSD_ALBEDO_FORMAT),
                .mUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
        }

        ImageDescription normalRoughnessDescription(const VkExtent2D extent)
        {
            return ImageDescription{ .mWidth = extent.width,
                .mHeight = extent.height,
                .mFormat = toVulkanFormat(DLSSD_NORMAL_ROUGHNESS_FORMAT),
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

        /// Builds `WorldToViewMatrix` and `ViewToClipMatrix` in column-major order (which equals
        /// row-major left-multiply, as NGX's `DLSSD` evaluation asks) from the camera basis and
        /// origin `sampleFrame` handed the trace.
        void fillDlssdMatrices(
            const UpscaleInputs& inputs, std::array<float, 16>& worldToView, std::array<float, 16>& viewToClip)
        {
            const Shaders::Camera& eye = inputs.mEyes.mWorld;
            const float halfW = std::max(eye.mBasis.mRight.length(), 1.0e-6f);
            const float halfH = std::max(eye.mBasis.mUp.length(), 1.0e-6f);
            const Shaders::vec3 rx = eye.mBasis.mRight * (1.0f / halfW);
            const Shaders::vec3 uy = eye.mBasis.mUp * (1.0f / halfH);
            const Shaders::vec3 bz = -eye.mBasis.mForward;
            const Shaders::vec3& origin = inputs.mOrigin;

            worldToView = {
                rx.x(),
                uy.x(),
                bz.x(),
                0.0f,
                rx.y(),
                uy.y(),
                bz.y(),
                0.0f,
                rx.z(),
                uy.z(),
                bz.z(),
                0.0f,
                -(rx * origin),
                -(uy * origin),
                -(bz * origin),
                1.0f,
            };

            const float near = FsrFrame::sNear;
            const float far = std::max(inputs.mFar, near + 1.0f);
            if (eye.mOrthographic != 0u)
            {
                viewToClip = {
                    1.0f / halfW,
                    0.0f,
                    0.0f,
                    0.0f,
                    0.0f,
                    -1.0f / halfH,
                    0.0f,
                    0.0f,
                    0.0f,
                    0.0f,
                    1.0f / (near - far),
                    0.0f,
                    -eye.mBasis.mCentre.x(),
                    -eye.mBasis.mCentre.y(),
                    near / (near - far),
                    1.0f,
                };
                return;
            }

            viewToClip = {
                1.0f / halfW,
                0.0f,
                0.0f,
                0.0f,
                0.0f,
                -1.0f / halfH,
                0.0f,
                0.0f,
                -eye.mBasis.mCentre.x(),
                -eye.mBasis.mCentre.y(),
                far / (near - far),
                -1.0f,
                0.0f,
                0.0f,
                -(far * near) / (far - near),
                0.0f,
            };
        }
    }

    /// NGX's process-wide state for one device, made where DLSS first builds its targets. A
    /// configuration that only runs FSR never loads the runtime.
    struct DlssUpscaler::Context
    {
        Context(const Device& device, const DlssMode mode)
            : mDevice(device)
            , mInputs(mode == DlssMode::RayReconstruction
                      ? ComputePipeline<NoConstants>(
                          device, sRrInputBindings, {}, "dlssdinputs.comp.spv", "dlssd inputs")
                      : ComputePipeline<NoConstants>(device, sInputBindings, {}, "dlssinputs.comp.spv", "dlss inputs"))
            , mBlocks([&](const FrameSlot) {
                const VkDeviceSize blockBytes = mode == DlssMode::RayReconstruction
                    ? sizeof(Shaders::DlssdInputConstants)
                    : sizeof(Shaders::DlssInputConstants);
                return Buffer::hostWritten(device, blockBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "dlss constants");
            })
        {
            const std::wstring path = std::filesystem::temp_directory_path().wstring();
            const NVSDK_NGX_Result init = NVSDK_NGX_VULKAN_Init_with_ProjectID(Dlss::sProjectId,
                NVSDK_NGX_ENGINE_TYPE_CUSTOM, "OpenMW RTX", path.c_str(), device.getInstanceHandle(),
                device.getPhysicalDevice().getHandle(), device.getHandle());
            if (NVSDK_NGX_FAILED(init))
                throw Unsupported(std::string(modeName(mode)) + " could not initialize NGX: " + resultText(init)
                    + "; the DLSS SDK's runtime libraries must sit beside the game");

            const NVSDK_NGX_Result allocated = NVSDK_NGX_VULKAN_GetCapabilityParameters(&mParameters);
            if (NVSDK_NGX_FAILED(allocated) || mParameters == nullptr)
            {
                NVSDK_NGX_VULKAN_Shutdown1(device.getHandle());
                throw Unsupported(
                    std::string(modeName(mode)) + " could not read NGX's capabilities: " + resultText(allocated));
            }

            int available = 0;
            const char* const capability = mode == DlssMode::RayReconstruction
                ? NVSDK_NGX_Parameter_SuperSamplingDenoising_Available
                : NVSDK_NGX_Parameter_SuperSampling_Available;
            const NVSDK_NGX_Result queried = mParameters->Get(capability, &available);
            if (NVSDK_NGX_FAILED(queried) || available == 0)
            {
                NVSDK_NGX_VULKAN_DestroyParameters(mParameters);
                mParameters = nullptr;
                NVSDK_NGX_VULKAN_Shutdown1(device.getHandle());
                throw Unsupported(std::string(modeName(mode)) + " is not available on this card or driver");
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
        Targets(const Device& device, const DlssMode mode, const VkExtent2D render, const VkExtent2D output,
            const ArenaRun& shared)
            : mRender(render)
            , mOutput(output)
            , mShared(shared)
            , mDepth(MemoryUse::Frame, device, depthDescription(render), "dlss-depth")
            , mMotion(MemoryUse::Frame, device, motionDescription(render), "dlss-motion")
        {
            if (mode == DlssMode::RayReconstruction)
            {
                mDiffuseAlbedo.emplace(MemoryUse::Frame, device, albedoDescription(render), "dlssd-diffuse-albedo");
                mSpecularAlbedo.emplace(MemoryUse::Frame, device, albedoDescription(render), "dlssd-specular-albedo");
                mNormalRoughness.emplace(
                    MemoryUse::Frame, device, normalRoughnessDescription(render), "dlssd-normal-roughness");
            }
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
        std::optional<Image> mDiffuseAlbedo;
        std::optional<Image> mSpecularAlbedo;
        std::optional<Image> mNormalRoughness;
        NVSDK_NGX_Handle* mFeature = nullptr;
    };

    DlssUpscaler::DlssUpscaler(const Device& device, const DlssMode mode)
        : mDevice(device)
        , mMode(mode)
        , mContext(std::make_unique<Context>(device, mode))
    {
    }

    DlssUpscaler::~DlssUpscaler() = default;

    void DlssUpscaler::declareShared(const VkExtent2D, const VkExtent2D output, std::vector<ArenaImage>& into)
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
        const Device& device, const DlssMode mode, const VkExtent2D render, const VkExtent2D)
    {
        VkDeviceSize bytes
            = Image::bytesFor(device, depthDescription(render)) + Image::bytesFor(device, motionDescription(render));
        if (mode == DlssMode::RayReconstruction)
            bytes += 2 * Image::bytesFor(device, albedoDescription(render))
                + Image::bytesFor(device, normalRoughnessDescription(render));
        return bytes;
    }

    void DlssUpscaler::resize(const VkExtent2D render, const VkExtent2D output, const ArenaRun& shared)
    {
        assert(render.width > 0 && render.height > 0 && output.width >= render.width && output.height >= render.height);
        assert(shared.size() == 1 && "the DLSS upscaler's shared image was not placed as it declared it");

        if (isAt(render, output))
        {
            mTargets->mShared = shared;
            return;
        }

        // **The feature is made after the device and with a command buffer**, as NGX's Vulkan
        // integration asks: creation records its own work, and a resize is already a drained point.
        mTargets = std::make_unique<Targets>(mDevice, mMode, render, output, shared);

        NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
        if (mMode == DlssMode::RayReconstruction)
        {
            NVSDK_NGX_DLSSD_Create_Params create{};
            create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
            create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
            create.InWidth = render.width;
            create.InHeight = render.height;
            create.InTargetWidth = output.width;
            create.InTargetHeight = output.height;
            create.InPerfQualityValue = qualityOf(render, output);
            // Linear view-space depth is positive away from the camera, so `DepthInverted` is off;
            // HDR, render-resolution unjittered motion vectors, and NGX's own exposure meter hold.
            create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            create.InEnableOutputSubrects = false;

            mDevice.getPool().submitAndWait([&](VkCommandBuffer commands) {
                result = NGX_VULKAN_CREATE_DLSSD_EXT1(
                    mDevice.getHandle(), commands, 1, 1, &mTargets->mFeature, mContext->mParameters, &create);
            });
        }
        else
        {
            NVSDK_NGX_DLSS_Create_Params create{};
            create.Feature.InWidth = render.width;
            create.Feature.InHeight = render.height;
            create.Feature.InTargetWidth = output.width;
            create.Feature.InTargetHeight = output.height;
            create.Feature.InPerfQualityValue = qualityOf(render, output);

            // Motion projects the surface at both ends without jitter: a stationary surface has zero
            // motion even as rayAt samples a different subpixel. Marking it jittered makes NGX subtract
            // a jitter delta that is not there. Depth is reversed and colour is linear HDR; exposure is
            // DLSS's own meter, because the renderer's meter runs after the upscaler.
            create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            create.InEnableOutputSubrects = false;

            mDevice.getPool().submitAndWait([&](VkCommandBuffer commands) {
                result
                    = NGX_VULKAN_CREATE_DLSS_EXT(commands, 1, 1, &mTargets->mFeature, mContext->mParameters, &create);
            });
        }

        if (NVSDK_NGX_FAILED(result) || mTargets->mFeature == nullptr)
            throw Unsupported(std::string(modeName(mMode)) + " could not create its feature at "
                + std::to_string(render.width) + "x" + std::to_string(render.height) + " to "
                + std::to_string(output.width) + "x" + std::to_string(output.height) + ": " + resultText(result));

        mFresh = true;
    }

    void DlssUpscaler::release()
    {
        mTargets.reset();
    }

    bool DlssUpscaler::isAt(const VkExtent2D render, const VkExtent2D output) const
    {
        const auto same
            = [](const VkExtent2D a, const VkExtent2D b) { return a.width == b.width && a.height == b.height; };
        return mTargets != nullptr && same(mTargets->mRender, render) && same(mTargets->mOutput, output);
    }

    HandedImage DlssUpscaler::record(const VkCommandBuffer commands, const UpscaleInputs& inputs, GpuTimer* timer)
    {
        const GpuZone timed(timer, commands, FrameZone::Upscale);

        assert(mTargets != nullptr && "a DLSS upscale before a resize");
        assert(mContext != nullptr && "a DLSS upscale without NGX");
        Targets& targets = *mTargets;

        const bool reset = std::exchange(mFresh, false);
        const Buffer& block = mContext->mBlocks.at(inputs.mSlot);

        if (mMode == DlssMode::RayReconstruction)
        {
            assert(targets.mDiffuseAlbedo.has_value() && targets.mSpecularAlbedo.has_value()
                && targets.mNormalRoughness.has_value());
            Image& diffuseAlbedo = *targets.mDiffuseAlbedo;
            Image& specularAlbedo = *targets.mSpecularAlbedo;
            Image& normalRoughness = *targets.mNormalRoughness;

            const Shaders::DlssdInputConstants constants{
                .mEyes = inputs.mEyes,
                .mFar = inputs.mFar,
                .mFrame = inputs.mFrame,
                .mLobed = inputs.mLobed ? 1u : 0u,
            };
            block.writeAt(0, std::span(&constants, 1));
            block.nameForNext();

            {
                Barriers barriers(commands);
                targets.mDepth.addTransition(barriers, Use::sUndefined, sInputWrite);
                targets.mMotion.addTransition(barriers, Use::sUndefined, sInputWrite);
                diffuseAlbedo.addTransition(barriers, Use::sUndefined, sInputWrite);
                specularAlbedo.addTransition(barriers, Use::sUndefined, sInputWrite);
                normalRoughness.addTransition(barriers, Use::sUndefined, sInputWrite);
                targets.mShared[0].addTransition(barriers, targets.mShared.firstUseOf(0), sDlssWrite);
                barriers.flush();
            }

            DescriptorWrites writes(mContext->mInputs);
            writes.image(Shaders::DLSSD_INPUTS_BIND_SURFACE, inputs.mSurface.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_MOTION, inputs.mMotion.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_ALBEDO, inputs.mAlbedo.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_SPECULAR, inputs.mSpecular.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_SPECULAR_ALBEDO, inputs.mSpecularAlbedo.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_DEPTH, targets.mDepth.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_MOTION_OUT, targets.mMotion.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_DIFFUSE_ALBEDO_OUT, diffuseAlbedo.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_SPECULAR_ALBEDO_OUT, specularAlbedo.describeStorage());
            writes.image(Shaders::DLSSD_INPUTS_BIND_NORMAL_ROUGHNESS_OUT, normalRoughness.describeStorage());
            writes.buffer(Shaders::DLSSD_INPUTS_BIND_CONSTANTS, block.describe(0, sizeof(constants)));
            dispatch(commands, mContext->mInputs, writes, NoConstants{},
                Groups::covering(targets.mRender.width, targets.mRender.height, Shaders::DLSS_INPUT_WORKGROUP));

            {
                Barriers barriers(commands);
                inputs.mColour.addTransition(barriers, Use::sAnyGeneralRead, sDlssRead);
                targets.mDepth.addTransition(barriers, sInputWrite, sDlssRead);
                targets.mMotion.addTransition(barriers, sInputWrite, sDlssRead);
                diffuseAlbedo.addTransition(barriers, sInputWrite, sDlssRead);
                specularAlbedo.addTransition(barriers, sInputWrite, sDlssRead);
                normalRoughness.addTransition(barriers, sInputWrite, sDlssRead);
                barriers.flush();
            }

            NVSDK_NGX_Resource_VK colour = viewOf(inputs.mColour);
            NVSDK_NGX_Resource_VK depth = viewOf(targets.mDepth);
            NVSDK_NGX_Resource_VK motion = viewOf(targets.mMotion);
            NVSDK_NGX_Resource_VK diffuse = viewOf(diffuseAlbedo);
            NVSDK_NGX_Resource_VK specular = viewOf(specularAlbedo);
            NVSDK_NGX_Resource_VK normals = viewOf(normalRoughness);
            NVSDK_NGX_Resource_VK output = viewOf(targets.mShared[0]);

            std::array<float, 16> worldToView{};
            std::array<float, 16> viewToClip{};
            fillDlssdMatrices(inputs, worldToView, viewToClip);

            NVSDK_NGX_VK_DLSSD_Eval_Params evaluation{};
            evaluation.pInColor = &colour;
            evaluation.pInOutput = &output;
            evaluation.pInDepth = &depth;
            evaluation.pInMotionVectors = &motion;
            evaluation.pInDiffuseAlbedo = &diffuse;
            evaluation.pInSpecularAlbedo = &specular;
            evaluation.pInNormals = &normals;
            evaluation.pInRoughness = &normals;
            evaluation.InJitterOffsetX = -inputs.mEyes.mWorld.mJitter.x();
            evaluation.InJitterOffsetY = -inputs.mEyes.mWorld.mJitter.y();
            evaluation.InRenderSubrectDimensions
                = NVSDK_NGX_Dimensions{ .Width = targets.mRender.width, .Height = targets.mRender.height };
            evaluation.InReset = reset ? 1 : 0;
            evaluation.InMVScaleX = 1.0f;
            evaluation.InMVScaleY = 1.0f;
            evaluation.InPreExposure = 1.0f;
            evaluation.InExposureScale = 1.0f;
            evaluation.pInWorldToViewMatrix = worldToView.data();
            evaluation.pInViewToClipMatrix = viewToClip.data();
            evaluation.InFrameTimeDeltaInMsec = inputs.mSeconds * 1000.0f;

            const NVSDK_NGX_Result result
                = NGX_VULKAN_EVALUATE_DLSSD_EXT(commands, targets.mFeature, mContext->mParameters, &evaluation);
            if (NVSDK_NGX_FAILED(result))
                throw std::runtime_error(
                    "NVIDIA DLSS Ray Reconstruction failed to evaluate a frame: " + resultText(result));

            return HandedImage{ .mImage = targets.mShared[0], .mLeftAs = Use::sComputeReadWrite };
        }

        const Shaders::DlssInputConstants constants{
            .mEyes = inputs.mEyes,
            .mNear = FsrFrame::sNear,
            .mFrame = inputs.mFrame,
        };
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
        // rayAt offsets the sample, whereas NGX names the resulting image displacement, which is
        // opposite. FsrFrame uses the same conversion for the same ray samples.
        evaluation.InJitterOffsetX = -inputs.mEyes.mWorld.mJitter.x();
        evaluation.InJitterOffsetY = -inputs.mEyes.mWorld.mJitter.y();
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

    std::string_view DlssUpscaler::describe(const DlssMode mode)
    {
        return mode == DlssMode::RayReconstruction ? "DLSS Ray Reconstruction 4.5 (NGX 310.9.16)"
                                                   : "DLSS 4.5 (NGX 310.9.16)";
    }
}
