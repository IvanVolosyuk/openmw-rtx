#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include <apps/openmw/mwrender/rtx/rtxsettings.hpp>
#include <components/rtx/common/error.hpp>
#include <components/rtx/frame/upscale.hpp>
#include <components/rtx/mirror/cells/cellgrid.hpp>
#include <components/rtx/scene/specularlayout.hpp>
#include <components/rtx/shaders/gbuffer.h>

namespace MWRender
{
    namespace
    {
        /// A set every field of which differs from its default, so a field the derivation drops
        /// or swaps with another shows.
        RtxSettingValues valid()
        {
            return RtxSettingValues{
                .mUpscale = Rtx::Upscale::Balanced,
                .mUpscaler = Rtx::UpscalerKind::Dlss,
                .mIndirectLighting = false,
                .mAntiFirefly = true,
                .mIndirectLimit = 4.0f,
                .mIndirectBrightness = 0.65f,
                .mEmissiveBounce = 0.1f,
                .mBounceRays = 16,
                .mDirectLampRays = 6,
                .mShadowFloor = 0.05f,
                .mAmbientScale = 0.25f,
                .mLampFalloff = 1.65f,
                .mLamps = {
                    .mSourceScale = 0.5f,
                    .mReachScale = 0.6f,
                    .mEmptyBrightness = 0.75f,
                    .mEmptySourceScale = 2.0f,
                },
                .mDistantLandCells = 6.0f,
                .mViewingDistance = 7168.0f,
                .mObjectPaging = false,
                .mObjectPagingMinSize = 0.025f,
                .mGroundcover = true,
                .mGroundcoverDistance = 3072.0f,
                .mGroundcoverDensity = 0.5f,
                .mGroundcoverPointLighting = false,
                .mSpecularMapLayout = Rtx::SpecularLayout::MetalRoughness,
                .mAnisotropy = 8,
                .mGamma = 1.5f,
                .mLitEnvironmentMaps = true,
            };
        }

        /// **Each value reaches its setting, and the cells mean the reach**, for the game and the
        /// harness alike, since both derive through this.
        TEST(RtxSettingsTest, eachValueReachesItsSettingAndTheCellsTheReach)
        {
            const RtxSettings derived = RtxSettings::derive(valid());
            EXPECT_EQ(derived.mUpscale, Rtx::Upscale::Balanced);
            EXPECT_EQ(derived.mUpscaler, Rtx::UpscalerKind::Dlss);

            RtxSettingValues rayReconstructed = valid();
            rayReconstructed.mUpscaler = Rtx::UpscalerKind::DlssRr;
            EXPECT_EQ(RtxSettings::derive(rayReconstructed).mUpscaler, Rtx::UpscalerKind::DlssRr);
            EXPECT_FALSE(derived.mIndirectLighting);
            EXPECT_EQ(derived.mBounceRays, 16u);
            EXPECT_EQ(derived.mDirectLampRays, 6u);
            EXPECT_EQ(derived.mShadowFloor, 0.05f);
            EXPECT_EQ(derived.mAmbientScale, 0.25f);
            EXPECT_FLOAT_EQ(derived.mLampFalloff, 1.65f);
            EXPECT_FLOAT_EQ(derived.mIndirectBrightness, 0.65f);
            EXPECT_FLOAT_EQ(derived.mEmissiveBounce, 0.1f);
            EXPECT_EQ(derived.mMirror.mLamps.mSourceScale, 0.5f);
            EXPECT_EQ(derived.mMirror.mLamps.mReachScale, 0.6f);
            EXPECT_EQ(derived.mMirror.mLamps.mEmptyBrightness, 0.75f);
            EXPECT_EQ(derived.mMirror.mLamps.mEmptySourceScale, 2.0f);
            EXPECT_TRUE(derived.mAntiFirefly);
            EXPECT_EQ(derived.mIndirectLimit, 4.0f);
            // Cells and the viewing distance, which a worldspace's grid turns into units: six of
            // Morrowind's 8192 are 49152.
            EXPECT_EQ(derived.mMirror.mReach, (Rtx::LandReach{ .mCells = 6.0f, .mViewingDistance = 7168.0f }));
            EXPECT_EQ(Rtx::CellGrid().reachOf(derived.mMirror.mReach), 49152.0f);
            EXPECT_FALSE(derived.mMirror.mDistantStatics);
            EXPECT_EQ(derived.mMirror.mMinSize, 0.025f);
            EXPECT_EQ(derived.mMirror.mGroundcoverReach, 3072.0f);
            EXPECT_EQ(derived.mMirror.mGroundcoverDensity, 0.5f);
            EXPECT_FALSE(derived.mMirror.mGroundcoverLampLit);
            EXPECT_EQ(derived.mMirror.mSpecularLayout, Rtx::SpecularLayout::MetalRoughness);
            EXPECT_EQ(derived.mAnisotropy, 8u);
            EXPECT_EQ(derived.mGamma, 1.5f);
            EXPECT_TRUE(derived.mLitEnvironmentMaps);

            RtxSettingValues handedBack = valid();
            handedBack.mDistantLandCells = 0.0f;
            EXPECT_EQ(Rtx::CellGrid().reachOf(RtxSettings::derive(handedBack).mMirror.mReach), 7168.0f)
                << "nought cells hands the reach to the viewing distance";

            RtxSettingValues bare = valid();
            bare.mGroundcover = false;
            EXPECT_EQ(RtxSettings::derive(bare).mMirror.mGroundcoverReach, 0.0f)
                << "groundcover switched off stands none, whatever its distance";

            RtxSettingValues unfiltered = valid();
            unfiltered.mAnisotropy = 0;
            EXPECT_EQ(RtxSettings::derive(unfiltered).mAnisotropy, 1u) << "nought filters as one does";
        }

        /// A gamma that is not a finite number over nought is refused: nought and less raise the
        /// picture to no power or a negative one, and an infinite one to the power of nought, which
        /// is white. The registry holds none of them; a harness's line holds anything.
        TEST(RtxSettingsTest, anInvalidIndirectLimitIsRefused)
        {
            for (const float limit :
                { -1.0f, 65.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
            {
                auto values = valid();
                values.mIndirectLimit = limit;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError);
            }
        }

        TEST(RtxSettingsTest, aGammaThatIsNotOverNoughtIsRefused)
        {
            for (const float gamma :
                { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
            {
                RtxSettingValues values = valid();
                values.mGamma = gamma;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << gamma;
            }

            RtxSettingValues least = valid();
            least.mGamma = std::numeric_limits<float>::min();
            EXPECT_EQ(RtxSettings::derive(least).mGamma, std::numeric_limits<float>::min())
                << "anything over nought stands";
        }

        /// A count of bounce rays outside one to `MAX_BOUNCE_RAYS` is refused: past the top there is no
        /// landing's list to run over, and no bounce at all is `[RTX] indirect lighting`'s to say. The
        /// registry clamps to the range; a harness's line holds anything.
        TEST(RtxSettingsTest, aCountOfBounceRaysOutsideItsRangeIsRefused)
        {
            constexpr int most = static_cast<int>(Rtx::Shaders::MAX_BOUNCE_RAYS);
            for (const int rays : { 0, -1, most + 1 })
            {
                RtxSettingValues values = valid();
                values.mBounceRays = rays;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << rays;
            }

            for (const int rays : { 1, 4, 8, 16 })
            {
                RtxSettingValues values = valid();
                values.mBounceRays = rays;
                EXPECT_EQ(RtxSettings::derive(values).mBounceRays, static_cast<std::uint32_t>(rays)) << rays;
            }
        }

        TEST(RtxSettingsTest, aCountOfDirectLampRaysOrShadowFloorOutsideItsRangeIsRefused)
        {
            constexpr int most = static_cast<int>(Rtx::Shaders::MAX_DIRECT_LAMP_RAYS);
            for (const int rays : { 0, -1, most + 1 })
            {
                RtxSettingValues values = valid();
                values.mDirectLampRays = rays;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << rays;
            }
            for (const int rays : { 1, 4, most })
            {
                RtxSettingValues values = valid();
                values.mDirectLampRays = rays;
                EXPECT_EQ(RtxSettings::derive(values).mDirectLampRays, static_cast<std::uint32_t>(rays)) << rays;
            }

            for (const float floor :
                { -0.01f, 0.26f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
            {
                RtxSettingValues values = valid();
                values.mShadowFloor = floor;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << floor;
            }
            for (const float floor : { 0.0f, 0.05f, 0.25f })
            {
                RtxSettingValues values = valid();
                values.mShadowFloor = floor;
                EXPECT_FLOAT_EQ(RtxSettings::derive(values).mShadowFloor, floor);
            }

            for (const float ambient :
                { -0.01f, 2.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
            {
                RtxSettingValues values = valid();
                values.mAmbientScale = ambient;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << ambient;
            }
            for (const float ambient : { 0.0f, 1.0f, 2.0f })
            {
                RtxSettingValues values = valid();
                values.mAmbientScale = ambient;
                EXPECT_FLOAT_EQ(RtxSettings::derive(values).mAmbientScale, ambient);
            }

            for (const float reach :
                { 0.24f, 2.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
            {
                RtxSettingValues values = valid();
                values.mLamps.mReachScale = reach;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << reach;
            }
            for (const float reach : { 0.25f, 1.0f, 2.0f })
            {
                RtxSettingValues values = valid();
                values.mLamps.mReachScale = reach;
                EXPECT_FLOAT_EQ(RtxSettings::derive(values).mMirror.mLamps.mReachScale, reach);
            }

            for (const float falloff :
                { 0.49f, 3.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
            {
                RtxSettingValues values = valid();
                values.mLampFalloff = falloff;
                EXPECT_THROW(RtxSettings::derive(values), Rtx::InputError) << falloff;
            }
            for (const float falloff : { 0.5f, 1.0f, 1.65f, 3.0f })
            {
                RtxSettingValues values = valid();
                values.mLampFalloff = falloff;
                EXPECT_FLOAT_EQ(RtxSettings::derive(values).mLampFalloff, falloff);
            }

            for (const float gain :
                { -0.01f, 2.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
            {
                RtxSettingValues indirectValues = valid();
                indirectValues.mIndirectBrightness = gain;
                EXPECT_THROW(RtxSettings::derive(indirectValues), Rtx::InputError) << gain;

                RtxSettingValues emissiveValues = valid();
                emissiveValues.mEmissiveBounce = gain;
                EXPECT_THROW(RtxSettings::derive(emissiveValues), Rtx::InputError) << gain;
            }
            for (const float gain : { 0.0f, 0.1f, 0.65f, 1.0f, 2.0f })
            {
                RtxSettingValues values = valid();
                values.mIndirectBrightness = gain;
                values.mEmissiveBounce = gain;
                const RtxSettings derivedGain = RtxSettings::derive(values);
                EXPECT_FLOAT_EQ(derivedGain.mIndirectBrightness, gain);
                EXPECT_FLOAT_EQ(derivedGain.mEmissiveBounce, gain);
            }
        }
    }
}
