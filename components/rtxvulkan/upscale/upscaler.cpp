#include "upscaler.hpp"

#include <cassert>
#include <string_view>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/rtx/common/error.hpp>
#include <components/rtxvulkan/device/device.hpp>
#include <components/rtxvulkan/device/dlss.hpp>

namespace Rtx
{
    namespace
    {
        constexpr bool isDlss(const UpscalerKind kind)
        {
            return kind == UpscalerKind::Dlss || kind == UpscalerKind::DlssRr;
        }

        constexpr DlssMode dlssModeOf(const UpscalerKind kind)
        {
            return kind == UpscalerKind::DlssRr ? DlssMode::RayReconstruction : DlssMode::SuperResolution;
        }
    }

    Upscaler::Upscaler(const Device& device, const UpscalerKind requested)
        : mDevice(device)
        , mKind(UpscalerKind::Fsr)
    {
        if (requested == UpscalerKind::Auto)
        {
            if (Dlss::sCompiled && device.supportsDlss())
            {
                try
                {
                    mDlss = std::make_unique<DlssUpscaler>(device, DlssMode::SuperResolution);
                    mKind = UpscalerKind::Dlss;
                }
                catch (const Unsupported& refused)
                {
                    Log(Debug::Warning) << "NVIDIA DLSS could not start, so FSR is used: " << refused.what();
                }
            }

            if (mKind != UpscalerKind::Dlss)
                mFsr = std::make_unique<FsrUpscaler>(device);
        }
        else if (isDlss(requested))
        {
            mDlss = std::make_unique<DlssUpscaler>(device, dlssModeOf(requested));
            mKind = requested;
        }
        else
        {
            mFsr = std::make_unique<FsrUpscaler>(device);
        }
    }

    Upscaler::~Upscaler() = default;

    void Upscaler::ensure(const UpscalerKind kind)
    {
        assert(kind != UpscalerKind::Auto && "an upscaler kind that was never resolved");
        if (kind == mKind && (isDlss(kind) ? mDlss != nullptr : mFsr != nullptr))
            return;

        mFsr.reset();
        mDlss.reset();
        mKind = kind;
        if (isDlss(mKind))
            mDlss = std::make_unique<DlssUpscaler>(mDevice, dlssModeOf(mKind));
        else
            mFsr = std::make_unique<FsrUpscaler>(mDevice);
    }

    void Upscaler::resize(
        const UpscalerKind kind, const VkExtent2D render, const VkExtent2D output, const ArenaRun& shared)
    {
        if (kind != mKind || (isDlss(kind) ? mDlss == nullptr : mFsr == nullptr))
            ensure(kind);

        if (isDlss(mKind))
            mDlss->resize(render, output, shared);
        else
            mFsr->resize(render, output, shared);
    }

    void Upscaler::declareShared(
        const UpscalerKind kind, const VkExtent2D render, const VkExtent2D output, std::vector<ArenaImage>& into)
    {
        switch (kind)
        {
            case UpscalerKind::Dlss:
            case UpscalerKind::DlssRr:
                DlssUpscaler::declareShared(render, output, into);
                break;
            case UpscalerKind::Fsr:
            case UpscalerKind::Auto:
                FsrUpscaler::declareShared(render, output, into);
                break;
        }
    }

    std::size_t Upscaler::getSharedCount(const UpscalerKind kind)
    {
        return isDlss(kind) ? DlssUpscaler::getSharedCount() : FsrUpscaler::getSharedCount();
    }

    VkDeviceSize Upscaler::bytesAt(
        const Device& device, const UpscalerKind kind, const VkExtent2D render, const VkExtent2D output)
    {
        return isDlss(kind) ? DlssUpscaler::bytesAt(device, dlssModeOf(kind), render, output)
                            : FsrUpscaler::bytesAt(device, render, output);
    }

    void Upscaler::release()
    {
        if (mDlss != nullptr)
            mDlss->release();
        if (mFsr != nullptr)
            mFsr->release();
    }

    bool Upscaler::isBuilt(const UpscalerKind kind) const
    {
        if (kind != mKind)
            return false;
        return isDlss(mKind) ? mDlss->isBuilt() : mFsr->isBuilt();
    }

    bool Upscaler::isAt(const UpscalerKind kind, const VkExtent2D render, const VkExtent2D output) const
    {
        if (kind != mKind)
            return false;
        return isDlss(mKind) ? mDlss->isAt(render, output) : mFsr->isAt(render, output);
    }

    void Upscaler::reset()
    {
        if (isDlss(mKind))
            mDlss->reset();
        else
            mFsr->reset();
    }

    bool Upscaler::isFresh() const
    {
        return isDlss(mKind) ? mDlss->isFresh() : mFsr->isFresh();
    }

    HandedImage Upscaler::record(
        const UpscalerKind kind, const VkCommandBuffer commands, const UpscaleInputs& inputs, GpuTimer* timer)
    {
        assert(kind == mKind && "a frame asked one upscaler and the frame's is another");
        return isDlss(mKind) ? mDlss->record(commands, inputs, timer) : mFsr->record(commands, inputs, timer);
    }

    std::string_view Upscaler::describe() const
    {
        return isDlss(mKind) ? DlssUpscaler::describe(dlssModeOf(mKind)) : FsrUpscaler::describe();
    }
}
