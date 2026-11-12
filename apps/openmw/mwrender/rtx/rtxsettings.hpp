#pragma once

#include <cstdint>

#include <components/rtx/frame/upscale.hpp>
#include <components/rtx/scene/specularlayout.hpp>

#include "mirrorknobs.hpp"

namespace MWRender
{
    /// The ray tracer's settings as values, before anything reads their meaning. Two sources fill
    /// one: the player's registry for a played session, and a harness's own for a run — its
    /// command line, the player's registry for a watched window, the shipped defaults for a
    /// measured one. The sources stay apart and the meaning is `RtxSettings::derive`'s alone.
    ///
    /// The upscaler and the map layout arrive read: each source parses its own text where it
    /// enters, the registry at load.
    struct RtxSettingValues
    {
        Rtx::Upscale mUpscale = Rtx::Upscale::Off;
        Rtx::UpscalerKind mUpscaler = Rtx::UpscalerKind::Auto;
        bool mIndirectLighting = true;
        bool mAntiFirefly = false;
        float mIndirectLimit = 0.0f;
        float mIndirectBrightness = 1.0f;
        float mEmissiveBounce = 1.0f;
        int mBounceRays = 1;
        int mDirectLampRays = 1;
        float mShadowFloor = 0.0f;
        float mAmbientScale = 1.0f;
        float mLampFalloff = 1.0f;
        Rtx::LampSettings mLamps;
        float mDistantLandCells = 0.0f;
        float mViewingDistance = 0.0f;
        bool mObjectPaging = true;
        float mObjectPagingMinSize = 0.0f;
        bool mGroundcover = false;
        float mGroundcoverDistance = 0.0f;
        float mGroundcoverDensity = 0.0f;
        bool mGroundcoverPointLighting = true;
        Rtx::SpecularLayout mSpecularMapLayout = Rtx::SpecularLayout::Classic;
        int mAnisotropy = 0;
        float mGamma = 1.0f;
        bool mLitEnvironmentMaps = false;

        /// `[RTX]`, `[Camera] viewing distance`, `[Terrain]`'s paging, `[Groundcover]`, `[General] anisotropy`,
        /// `[Video] gamma` and `[Shaders] apply lighting to environment maps`: the one place the
        /// game reads these settings.
        static RtxSettingValues fromRegistry();
    };

    /// What those values mean for a ray tracer: the one derivation, for both hosts.
    struct RtxSettings
    {
        Rtx::Upscale mUpscale = Rtx::Upscale::Off;

        /// `RenderProfile::mUpscaler`: the setting, its `Auto` resolved where the renderer is made.
        Rtx::UpscalerKind mUpscaler = Rtx::UpscalerKind::Auto;

        /// `SamplingSwitches::mIndirect`: the setting.
        bool mIndirectLighting = true;
        bool mAntiFirefly = false;
        float mIndirectLimit = 0.0f;
        float mIndirectBrightness = 1.0f;
        float mEmissiveBounce = 1.0f;

        /// `SamplingSwitches::mBounceRays`: the setting, from one to `Shaders::MAX_BOUNCE_RAYS`.
        std::uint32_t mBounceRays = 1;

        /// `SamplingSwitches::mDirectLampRays`: the setting, from one to `Shaders::MAX_DIRECT_LAMP_RAYS`.
        std::uint32_t mDirectLampRays = 1;

        /// `SamplingSwitches::mShadowFloor`: the setting, from nought to a quarter.
        float mShadowFloor = 0.0f;

        /// Scale applied to cell and weather record ambient fill (`[RTX] ambient brightness`).
        float mAmbientScale = 1.0f;

        /// `SamplingSwitches::mLampFalloff`: lamp distance-attenuation steepness (`[RTX] lamp falloff`).
        float mLampFalloff = 1.0f;

        MirrorKnobs mMirror;

        /// `RenderProfile::mAnisotropy`: the setting, where nought means what one does.
        std::uint32_t mAnisotropy = 1;

        /// `RenderProfile::mGamma`: the setting, a finite number greater than nought.
        float mGamma = 1.0f;

        /// `RenderProfile::mLitEnvironmentMaps`: the setting.
        bool mLitEnvironmentMaps = false;

        /// Throws `Rtx::InputError` for a gamma that is not a finite number greater than nought, and for
        /// a count of bounce rays outside one to `Shaders::MAX_BOUNCE_RAYS`: a setting refused rather than
        /// defaulted, so a typo is said at once and not traced under for a session.
        static RtxSettings derive(const RtxSettingValues& values);
    };
}
