#include "rtxsettings.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <components/rtx/common/error.hpp>
#include <components/rtx/frame/upscale.hpp>
#include <components/rtx/mirror/cells/cellgrid.hpp>
#include <components/rtx/scene/specularlayout.hpp>
#include <components/rtx/shaders/gbuffer.h>
#include <components/settings/values.hpp>

namespace MWRender
{
    static_assert(Settings::RTXCategory::sMaxDistantLandCells == Rtx::LandReach::sMostCells,
        "the setting's bound and the reach's are one number");

    RtxSettingValues RtxSettingValues::fromRegistry()
    {
        return RtxSettingValues{
            .mUpscale = Settings::rtx().mUpscale.get(),
            .mUpscaler = Settings::rtx().mUpscaler.get(),
            .mIndirectLighting = Settings::rtx().mIndirectLighting,
            .mAntiFirefly = Settings::rtx().mAntiFirefly,
            .mIndirectLimit = Settings::rtx().mIndirectLimit,
            .mIndirectBrightness = Settings::rtx().mIndirectBrightness.get() / 100.0f,
            .mEmissiveBounce = Settings::rtx().mEmissiveBounce.get() / 100.0f,
            .mBounceRays = Settings::rtx().mBounceRays,
            .mDirectLampRays = Settings::rtx().mDirectLampRays,
            .mShadowFloor = Settings::rtx().mShadowDrawFloor.get() / 100.0f,
            .mAmbientScale = Settings::rtx().mAmbientBrightness.get() / 100.0f,
            .mLampFalloff = Settings::rtx().mLampFalloff.get() / 100.0f,
            .mLamps = {
                .mSourceScale = Settings::rtx().mLampSourceSize.get() / 25.0f,
                .mReachScale = Settings::rtx().mLampReach.get() / 100.0f,
                .mFlicker = Settings::rtx().mLampFlicker.get() / 100.0f,
                .mSpeed = Settings::rtx().mLampSpeed.get() / 100.0f,
                .mMovement = Settings::rtx().mLampMovement.get() / 100.0f,
                .mSizeVariation = Settings::rtx().mLampBreathing.get() / 100.0f,
                .mEmptyBrightness = Settings::rtx().mEmptyLampBrightness.get() / 100.0f,
                .mEmptySourceScale = Settings::rtx().mEmptyLampSourceSize.get() / 100.0f,
            },
            .mDistantLandCells = Settings::rtx().mDistantLandCells,
            .mViewingDistance = Settings::camera().mViewingDistance,
            .mObjectPaging = Settings::terrain().mObjectPaging,
            .mObjectPagingMinSize = Settings::terrain().mObjectPagingMinSize,
            .mGroundcover = Settings::groundcover().mEnabled,
            .mGroundcoverDistance = Settings::groundcover().mRenderingDistance,
            .mGroundcoverDensity = Settings::groundcover().mDensity,
            .mGroundcoverPointLighting = Settings::groundcover().mPointLighting,
            .mSpecularMapLayout = Settings::rtx().mSpecularMapLayout.get(),
            .mAnisotropy = Settings::general().mAnisotropy,
            .mGamma = Settings::video().mGamma,
            .mLitEnvironmentMaps = Settings::shaders().mApplyLightingToEnvironmentMaps,
        };
    }

    RtxSettings RtxSettings::derive(const RtxSettingValues& values)
    {
        // The registry holds the setting to a finite number over nought; a harness's line holds
        // nothing, and past either end the picture is all black, all white or no number at all.
        if (!(values.mGamma > 0.0f && std::isfinite(values.mGamma)))
            throw Rtx::InputError(
                std::format("a gamma of {} is not a finite number greater than nought", values.mGamma));

        if (values.mBounceRays < 1 || values.mBounceRays > static_cast<int>(Rtx::Shaders::MAX_BOUNCE_RAYS))
            throw Rtx::InputError(
                std::format("{} bounce rays is outside one to {}", values.mBounceRays, Rtx::Shaders::MAX_BOUNCE_RAYS));

        if (values.mDirectLampRays < 1
            || values.mDirectLampRays > static_cast<int>(Rtx::Shaders::MAX_DIRECT_LAMP_RAYS))
            throw Rtx::InputError(std::format(
                "{} direct lamp rays is outside one to {}", values.mDirectLampRays, Rtx::Shaders::MAX_DIRECT_LAMP_RAYS));

        if (!(values.mShadowFloor >= 0.0f && values.mShadowFloor <= 0.25f))
            throw Rtx::InputError("shadow draw floor must be finite and between zero and 0.25");

        if (!(values.mIndirectLimit >= 0.0f && values.mIndirectLimit <= 64.0f))
            throw Rtx::InputError("indirect brightness limit must be finite and between zero and 64");

        if (!(values.mIndirectBrightness >= 0.0f && values.mIndirectBrightness <= 2.0f))
            throw Rtx::InputError("indirect brightness must be finite and between zero and 2.0");

        if (!(values.mEmissiveBounce >= 0.0f && values.mEmissiveBounce <= 2.0f))
            throw Rtx::InputError("emissive bounce must be finite and between zero and 2.0");

        if (!(values.mAmbientScale >= 0.0f && values.mAmbientScale <= 2.0f))
            throw Rtx::InputError("ambient brightness must be finite and between zero and 2.0");

        if (!(values.mLampFalloff >= 0.5f && values.mLampFalloff <= 3.0f))
            throw Rtx::InputError("lamp falloff must be finite and between 0.5 and 3.0");

        if (!(values.mLamps.mReachScale >= 0.25f && values.mLamps.mReachScale <= 2.0f))
            throw Rtx::InputError("lamp reach scale must be finite and between 0.25 and 2.0");

        return RtxSettings{
            .mUpscale = values.mUpscale,
            .mUpscaler = values.mUpscaler,
            .mIndirectLighting = values.mIndirectLighting,
            .mAntiFirefly = values.mAntiFirefly,
            .mIndirectLimit = values.mIndirectLimit,
            .mIndirectBrightness = values.mIndirectBrightness,
            .mEmissiveBounce = values.mEmissiveBounce,
            .mBounceRays = static_cast<std::uint32_t>(values.mBounceRays),
            .mDirectLampRays = static_cast<std::uint32_t>(values.mDirectLampRays),
            .mShadowFloor = values.mShadowFloor,
            .mAmbientScale = values.mAmbientScale,
            .mLampFalloff = values.mLampFalloff,
            .mMirror = {
                .mReach = { .mCells = values.mDistantLandCells, .mViewingDistance = values.mViewingDistance },
                .mLamps = values.mLamps,
                .mDistantStatics = values.mObjectPaging,
                .mMinSize = values.mObjectPagingMinSize,
                .mGroundcoverReach = values.mGroundcover ? values.mGroundcoverDistance : 0.0f,
                .mGroundcoverDensity = values.mGroundcoverDensity,
                .mGroundcoverLampLit = values.mGroundcoverPointLighting,
                .mSpecularLayout = values.mSpecularMapLayout,
            },
            .mAnisotropy = static_cast<std::uint32_t>(std::max(values.mAnisotropy, 1)),
            .mGamma = values.mGamma,
            .mLitEnvironmentMaps = values.mLitEnvironmentMaps,
        };
    }
}
