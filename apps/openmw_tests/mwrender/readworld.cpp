#include <cmath>
#include <cstddef>
#include <limits>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Group>
#include <osg/Math>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <apps/openmw/mwrender/rtx/skyreader.hpp>
#include <apps/openmw/mwrender/sceneframe.hpp>
#include <apps/openmw/mwrender/sky.hpp>
#include <apps/openmw/mwrender/skystate.hpp>
#include <apps/openmw/mwworld/weather.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/image/colour.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/world/fogbuilder.hpp>
#include <components/rtx/world/frameworld.hpp>
#include <components/rtx/world/moon.hpp>
#include <components/rtx/world/skycontent.hpp>
#include <components/settings/values.hpp>
#include <components/sky/skyclock.hpp>
#include <components/sky/sundisc.hpp>
#include <components/sky/sunglarefader.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

namespace MWRender
{
    namespace
    {
        constexpr float sReach = 4.0f * static_cast<float>(Constants::CellSizeInUnits);

        /// The two records a reading is made of.
        struct Standing
        {
            SkyState mSky;
            WorldState mWorld;
        };

        /// Noon under clear weather, wherever the caller says the player is standing: the sun
        /// straight overhead, the weather run wherever there is a sky over the player.
        Standing standingIn(const Location where)
        {
            // Value-initialised: the reader reads every scalar of both records, and not every one
            // of them is given a default.
            Standing standing{};
            SkyState& sky = standing.mSky;
            WorldState& world = standing.mWorld;

            // Morrowind's shipped day: sunrise at six for two hours, sunset at eighteen for two.
            sky.mTimes.mNightEnd = 6.f;
            sky.mTimes.mSunriseDuration = 2.f;
            sky.mTimes.mDayStart = 8.f;
            sky.mTimes.mDayEnd = 18.f;
            sky.mTimes.mNightStart = 20.f;

            world.mLocation = where;
            world.mSkyShown = where != Location::Interior;
            sky.mWeatherRan = where != Location::Interior;
            world.mGameHour = 12.0f;
            sky.mWeather.mFogDepth = 0.69f;
            sky.mWeather.mBaseWindSpeed = 0.3f;
            sky.mWeather.mGlareView = 1.0f;
            world.mAir = { .mColour = osg::Vec4f(0.62f, 0.77f, 1.0f, 1.0f) };
            sky.mWeather.mSkyColor = osg::Vec4f(0.11f, 0.24f, 0.6f, 1.0f);

            // An orbit direction whose disc, bent by `Sky::sunDiscPosition`, stands straight up;
            // and the same point on the light, for a room where the weather has not run.
            sky.mSunDirection = osg::Vec3f(0.0f, 0.0f, -100.0f);
            world.mSunLightPosition = osg::Vec4f(0.0f, 0.0f, 1.0f, 0.0f);
            world.mSunColour = osg::Vec4f(1.0f, 0.97f, 0.85f, 1.0f);
            sky.mWeather.mSunDiscColor = osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f);

            return standing;
        }

        /// What the weather drops, over an empty archive: nothing, until a test hands it a
        /// weather that rains.
        struct Falling
        {
            VFS::Manager mVfs;
            Resource::ImageManager mImages{ &mVfs, 0 };
            Resource::NifFileManager mNifs{ &mVfs, nullptr };
            Resource::BgsmFileManager mMaterials{ &mVfs, 0 };
            Resource::SceneManager mScenes{ &mVfs, &mImages, &mNifs, &mMaterials, 0 };
            osg::ref_ptr<osg::Group> mRoot = new osg::Group;
            osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
            Precipitation mPrecipitation{ mRoot, mCamera, &mScenes };

            Falling()
            {
                // The box's shaders are the rasterizer's, and an empty archive holds none of them.
                mScenes.setShadersEnabled(false);
            }
        };

        /// A reader at the reach the assertions below count on, with the sky and the moons it
        /// holds before a world arrives: none, and nothing falling.
        Rtx::WorldReading readFrom(const Standing& standing)
        {
            const SkyReader reader;
            const Falling falling;

            return reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach);
        }

        /// A quasi-exterior stands under the exterior's sun and in the exterior's air.
        ///
        /// **Because that is the cell the game hands it.** `World::updateWeather` runs the weather
        /// system for `isCellExterior() || isCellQuasiExterior()`, so what reaches a reading of
        /// Mournhold or of a Vivec canton is a weather's own `Land_Fog_Depth`, fog colour and sun —
        /// and the Construction Set greys the whole `AMBI` record out for such a cell, so there is
        /// nothing of a room's to read instead.
        ///
        /// **Read as a room's air it lost the sky.** `roomFog` builds an even medium with no layer
        /// for a ray to climb out of, so a ray to the sun crossed every unit of it: Mournhold at
        /// noon came back as flat fog colour with no sun anywhere in it.
        TEST(RtxReadWorldTest, aQuasiExteriorTakesTheWeathersSunAndAir)
        {
            const Rtx::WorldReading open = readFrom(standingIn(Location::Exterior));
            const Rtx::WorldReading quasi = readFrom(standingIn(Location::QuasiExterior));

            EXPECT_TRUE(open.mOutdoors);
            EXPECT_TRUE(quasi.mOutdoors) << "a cell with weather over it is outdoors";

            // The sun the symptom was about: there is one at noon, and it is the exterior's.
            EXPECT_NE(quasi.mDaylight.mLight.mSun.mIrradiance, osg::Vec3f());
            EXPECT_EQ(quasi.mDaylight.mLight.mSun.mIrradiance, open.mDaylight.mLight.mSun.mIrradiance);
            EXPECT_EQ(quasi.mDaylight.mLight.mSun.mPosition, open.mDaylight.mLight.mSun.mPosition);
            EXPECT_EQ(quasi.mDaylight.mLight.mAmbient, open.mDaylight.mLight.mAmbient);

            // And the sky it hangs in, which an air read as a room's had closed over.
            EXPECT_EQ(quasi.mDaylight.mSkyZenith, open.mDaylight.mSkyZenith);
            EXPECT_EQ(quasi.mDaylight.mSkyHorizon, open.mDaylight.mSkyHorizon);

            const Rtx::Fog& air = quasi.mDaylight.mFog;
            EXPECT_EQ(air.mColour, open.mDaylight.mFog.mColour);
            EXPECT_EQ(air.mExtinction, open.mDaylight.mFog.mExtinction);
            EXPECT_EQ(air.mUniform, Rtx::Shaders::FOG_EXTERIOR_EVEN) << "banked, as every other weather's air is";
            EXPECT_EQ(air.mLift, open.mDaylight.mFog.mLift) << "and standing as high";
            EXPECT_EQ(air.mWind, open.mDaylight.mFog.mWind);

            // **The one element that parts them**, because it is about this renderer and not about
            // the weather: the ring where the ground stops is what it hides, and every wall of a
            // quasi-exterior is built.
            EXPECT_EQ(open.mDaylight.mFog.mEdge, sReach);
            EXPECT_EQ(air.mEdge, 0.0f);
        }

        /// **Night-Eye leaves the ambient the world stated**, to the bit: it lifts the picture in the
        /// tone pass and not the light the trace gathers. The ambient a weather states is `k / 255`,
        /// and `16 / 255` lifted by 0.7 and the lift taken off again came back an ulp under it, which
        /// decodes off the curve and not off the byte's table.
        TEST(RtxReadWorldTest, nightEyeLeavesTheAmbientTheWorldStated)
        {
            const osg::Vec4f before(16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f, 1.0f);
            const osg::Vec4f lift(0.7f, 0.7f, 0.7f, 0.0f);

            Standing plain = standingIn(Location::Exterior);
            plain.mWorld.mAmbientBeforeNightEye = before;
            plain.mWorld.mAmbientColour = before;

            Standing lifted = plain;
            lifted.mWorld.mAmbientColour = before + lift;
            lifted.mWorld.mNightEye = lifted.mWorld.mAmbientColour - before;
            ASSERT_NE(lifted.mWorld.mAmbientColour - lifted.mWorld.mNightEye, before)
                << "the subtraction gives the ambient back here, so this shows nothing";

            const Rtx::WorldReading without = readFrom(plain);
            const Rtx::WorldReading with = readFrom(lifted);
            EXPECT_EQ(with.mDaylight.mLight.mAmbient, without.mDaylight.mLight.mAmbient);
            EXPECT_EQ(with.mNightEye, osg::Vec3f(0.7f, 0.7f, 0.7f));
            EXPECT_EQ(without.mNightEye, osg::Vec3f());
        }

        /// The disc stands where the orbit's direction puts it, bent toward the horizon by the
        /// rule both renderers share, and not where the light happens to point: `match sunlight
        /// to sun` may have left the light on the orbit, and the shadows have to fall from the disc.
        TEST(RtxReadWorldTest, theDiscIsWhereTheOrbitPutsItAndNotWhereTheLightPoints)
        {
            Standing dusk = standingIn(Location::Exterior);
            dusk.mSky.mSunDirection = osg::Vec3f(-300.0f, 75.0f, -100.0f);
            dusk.mWorld.mSunLightPosition = osg::Vec4f(300.0f, -75.0f, 100.0f, 0.0f);

            // `(300, -75, 400 - 300)`, unit length: the disc a hundred up where the light is
            // a hundred up too, but the disc's is the bent height and the light's the orbit's.
            osg::Vec3f disc(300.0f, -75.0f, 100.0f);
            disc.normalize();
            EXPECT_EQ(readFrom(dusk).mDaylight.mLight.mSun.mPosition, disc);

            dusk.mSky.mSunDirection = osg::Vec3f(-100.0f, 75.0f, -100.0f);
            osg::Vec3f higher(100.0f, -75.0f, 300.0f);
            higher.normalize();
            EXPECT_EQ(readFrom(dusk).mDaylight.mLight.mSun.mPosition, higher) << "the light did not move, the disc did";
        }

        /// The deck scrolls and the fog churns by the reader's own clock, which the frame steps
        /// and the frame does not carry: one frame at the shipped scale under the fastest deck
        /// moves the scroll and the seconds by that frame. The water's clock is another.
        TEST(RtxReadWorldTest, theDeckAndTheFogReadTheSkysClock)
        {
            constexpr float step = 1.0f / 60.0f;
            const Standing standing = standingIn(Location::Exterior);
            const Falling falling;

            SkyReader reader;
            const Rtx::WorldReading still
                = reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach);
            EXPECT_EQ(still.mClouds.mScroll, 0.0f);
            EXPECT_EQ(still.mSkySeconds, 0.0);

            reader.step(step, Sky::sVanillaTimeScale, 400.0f);
            const Rtx::WorldReading stepped
                = reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach);
            EXPECT_FLOAT_EQ(stepped.mClouds.mScroll, step);
            EXPECT_DOUBLE_EQ(stepped.mSkySeconds, static_cast<double>(step));

            // **And the water reads the simulation's own seconds, never the sky's**: the sky's
            // clock stands where it stood on a frame with no sky, which is every interior, and the
            // water in one still moves.
            const Rtx::WorldReading indoors
                = reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 12.5, sReach);
            EXPECT_EQ(indoors.mSeconds, 12.5);
            EXPECT_DOUBLE_EQ(indoors.mSkySeconds, static_cast<double>(step));
        }

        /// A script's view distance closes the air nearer than the world reaches and never further:
        /// the world stands to the setting's reach, and the eye sees `min(reach, d)` of it.
        TEST(RtxReadWorldTest, aScriptsViewDistanceClosesTheAirAndNeverOpensIt)
        {
            const Standing standing = standingIn(Location::Exterior);
            const Falling falling;
            const SkyReader reader;

            const auto edgeFor = [&](const EyeState& eye) {
                return reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0, eye.closesAirAt(sReach))
                    .mDaylight.mFog.mEdge;
            };

            EXPECT_EQ(edgeFor(EyeState{}), sReach) << "no script, the world's reach";
            EXPECT_EQ(edgeFor(EyeState{ .mScriptViewDistance = 2000.0f }), 2000.0f) << "a sandstorm's";
            EXPECT_EQ(edgeFor(EyeState{ .mScriptViewDistance = 4.0f * sReach }), sReach)
                << "past the world there is no world to see";
        }

        /// A room is lit by its own record, and the record is the only thing that decides it.
        ///
        /// **The alternative the two above are not.** A cell that is a room carries an `AMBI`, has
        /// no sun at any hour, and holds the still even air `sInteriorFogReach` is measured over.
        TEST(RtxReadWorldTest, aRoomIsLitByItsOwnRecordAndHasNoSun)
        {
            Standing cellar = standingIn(Location::Interior);
            cellar.mWorld.mRoom = ESM::Cell::AMBIstruct{ .mAmbient = 0x00201818u,
                .mSunlight = 0x00403028u,
                .mFog = 0x00151510u,
                .mFogDensity = cellar.mSky.mWeather.mFogDepth };

            const Rtx::WorldReading room = readFrom(cellar);

            EXPECT_FALSE(room.mOutdoors);
            EXPECT_EQ(room.mDaylight.mLight.mSun.mIrradiance, osg::Vec3f()) << "noon reached a cellar";
            EXPECT_EQ(room.mDaylight.mFog.mUniform, 1.0f);
            EXPECT_EQ(room.mDaylight.mFog.mEdge, 0.0f);
            EXPECT_NEAR(room.mDaylight.mFog.mExtinction,
                Rtx::fogExtinction(cellar.mSky.mWeather.mFogDepth, Rtx::sInteriorFogReach), 1e-10f);

            // Its sky is its own air and not the one the player last stood under, which the weather
            // system stopped writing the moment they stepped inside.
            EXPECT_EQ(room.mDaylight.mSkyZenith, room.mDaylight.mSkyHorizon);
            EXPECT_NE(room.mDaylight.mSkyZenith, readFrom(standingIn(Location::Exterior)).mDaylight.mSkyZenith);
        }

        /// `tsky` hides the sky and leaves the light: the rasterizer masks the sky node out and
        /// clears to the fog colour, and the sun, the moons and the dome go on lighting the ground.
        /// So the reading is outdoors as before, with the same sky, sun and disc, and says only that
        /// its sky is not drawn; and it has no glare, whose node hangs under the hidden sun.
        TEST(RtxReadWorldTest, theSkyToggleHidesTheSkyAndKeepsItsLight)
        {
            Standing standing = standingIn(Location::Exterior);
            standing.mWorld.mSkyShown = false;

            const Rtx::WorldReading hidden = readFrom(standing);
            const Rtx::WorldReading shown = readFrom(standingIn(Location::Exterior));

            EXPECT_TRUE(hidden.mOutdoors) << "a hidden sky stopped lighting";
            EXPECT_FALSE(hidden.mSkyDrawn);
            EXPECT_TRUE(shown.mSkyDrawn);
            EXPECT_EQ(hidden.mDaylight.mSkyZenith, shown.mDaylight.mSkyZenith);
            EXPECT_EQ(hidden.mDaylight.mSkyHorizon, shown.mDaylight.mSkyHorizon);
            EXPECT_EQ(hidden.mDaylight.mLight.mSun.mIrradiance, shown.mDaylight.mLight.mSun.mIrradiance);
            EXPECT_EQ(hidden.mDaylight.mLight.mSun.mDiscColour, shown.mDaylight.mLight.mSun.mDiscColour);
            EXPECT_EQ(hidden.mSunGlare.mFade, 0.0f) << "a glare on a hidden sun";
        }

        /// **The water scatters back the colour the content settles its murk at**:
        /// `Water_UnderwaterColor` at `Water_UnderwaterColorWeight`, read off the fallbacks, which the
        /// seed states as Morrowind's own `012,030,037` at 0.85 — and not the air's share the
        /// rasterizer's underwater fog mixes in beside it, which a grey storm's air does not move.
        TEST(RtxReadWorldTest, theWaterScattersTheColourTheContentSettlesItsMurkAt)
        {
            Standing standing = standingIn(Location::Exterior);
            standing.mWorld.mWaterFog.mColour = osg::Vec4f(0.5f, 0.5f, 0.5f, 1.0f);

            const osg::Vec3f scatter = readFrom(standing).mWaterScatter;
            const osg::Vec3f shipped = Rtx::Shaders::WATER_SCATTER_SHIPPED;
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_FLOAT_EQ(scatter[channel], shipped[channel]) << channel;
        }

        /// **And the extinction keeps the pairing `look.h` states**: the shipped colour peaks in
        /// blue, where the water takes least away, so the two describe one water.
        TEST(RtxReadWorldTest, theShippedWaterColourPeaksWhereTheExtinctionIsLeast)
        {
            const osg::Vec3f scatter = Rtx::Shaders::WATER_SCATTER_SHIPPED;
            const osg::Vec3f extinction = Rtx::Shaders::WATER_EXTINCTION;
            EXPECT_GT(scatter.z(), scatter.y());
            EXPECT_GT(scatter.y(), scatter.x());
            EXPECT_LT(extinction.z(), extinction.y());
            EXPECT_LT(extinction.y(), extinction.x());
            EXPECT_FLOAT_EQ(scatter.z(), 37.0f / 255.0f * 0.85f) << "Morrowind's own blue at its weight";
        }

        /// **The glare fades with the disc through sunrise and sunset**, as `SkyManager::setWeather`
        /// hands the sun `Glare_View` times the disc's alpha: at 06:15 of the shipped day the disc
        /// stands at a quarter, and so does the glare against a whole disc's.
        ///
        /// **And the fader is the content's, as `SunGlareCallback` reads it**: the seed's colour
        /// `222,095,039` doubled and clamped, 2 * 222 / 255 past one, 190 / 255 and 78 / 255; its most
        /// of 0.5; and thirty degrees in radians.
        TEST(RtxReadWorldTest, theGlareFadesWithTheDisc)
        {
            const Rtx::WorldReading whole = readFrom(standingIn(Location::Exterior));
            Standing dawn = standingIn(Location::Exterior);
            dawn.mSky.mWeather.mSunDiscColor.a() = 0.25f;
            const Rtx::WorldReading faded = readFrom(dawn);

            ASSERT_GT(whole.mSunGlare.mFade, 0.0f) << "no glare at noon to compare against";
            EXPECT_FLOAT_EQ(faded.mSunGlare.mFade, 0.25f * whole.mSunGlare.mFade);

            const Sky::SunGlareFader& fader = whole.mSunGlare.mFader;
            EXPECT_EQ(fader.mColour.x(), 1.0f);
            EXPECT_FLOAT_EQ(fader.mColour.y(), 190.0f / 255.0f);
            EXPECT_FLOAT_EQ(fader.mColour.z(), 78.0f / 255.0f);
            EXPECT_EQ(fader.mMax, 0.5f);
            EXPECT_FLOAT_EQ(fader.mAngleMax, osg::DegreesToRadians(30.0f));
        }

        /// A script paints Secunda `Moons_Script_Color`, and Secunda alone, as
        /// `SkyManager::setMoonColour` paints it — read off the fallback map the mirror read once,
        /// which is what stands in for the game's own record here.
        TEST(RtxReadWorldTest, theScriptColourPaintsSecundaAlone)
        {
            const osg::Vec3f white(1.0f, 1.0f, 1.0f);
            const osg::Vec3f paint = Rtx::decodeColour(Fallback::Map::getColour("Moons_Script_Color"));
            ASSERT_NE(paint, white);

            Standing standing = standingIn(Location::Exterior);
            standing.mWorld.mMoonRed = true;

            const Rtx::WorldReading red = readFrom(standing);
            EXPECT_EQ(red.mMoons[static_cast<std::size_t>(Rtx::Moon::Masser)].mPaint, white);
            EXPECT_EQ(red.mMoons[static_cast<std::size_t>(Rtx::Moon::Secunda)].mPaint, paint);

            const Rtx::WorldReading plain = readFrom(standingIn(Location::Exterior));
            EXPECT_EQ(plain.mMoons[static_cast<std::size_t>(Rtx::Moon::Secunda)].mPaint, white);
        }

        /// **The cloud blend is cleaned where it enters.** `Weather::transitionDelta` divides by
        /// `Clouds_Maximum_Percent`, which the shipped fallbacks leave at nought for ash and blight,
        /// so a crossing into either hands over an infinity or a NaN: nothing recorded is a deck
        /// crossed at once. A share is clamped to one, and a real one passes through.
        TEST(RtxReadWorldTest, aCloudBlendNothingRecordedIsACrossingDone)
        {
            const auto blendOf = [](float factor) {
                Standing standing = standingIn(Location::Exterior);
                standing.mSky.mWeather.mCloudBlendFactor = factor;
                return readFrom(standing).mClouds.mBlend;
            };

            EXPECT_EQ(blendOf(std::numeric_limits<float>::quiet_NaN()), 1.0f);
            EXPECT_EQ(blendOf(std::numeric_limits<float>::infinity()), 1.0f);
            EXPECT_EQ(blendOf(-1.0f), 0.0f);
            EXPECT_EQ(blendOf(7.0f), 1.0f);
            EXPECT_EQ(blendOf(0.25f), 0.25f);
        }

        /// What falls is kept off by a roof up to the top of the game's own occluder box — the
        /// precipitation's range and a cell over it — and by nothing where nothing falls or
        /// `weather particle occlusion` is off. Asked of
        /// the precipitation itself: a weather that rains sizes the box by its own numbers, the
        /// height being the mean of the drops' two spawn heights.
        TEST(RtxReadWorldTest, theShelterIsTheOccludersBox)
        {
            const Standing standing = standingIn(Location::Exterior);
            const SkyReader reader;

            MWWorld::Weather record;
            record.blank();
            record.mRainEffect = VFS::Path::Normalized("meshes/raindrop.nif");
            record.mRainDiameter = 600.0f;
            record.mRainMinHeight = 200.0f;
            record.mRainMaxHeight = 700.0f;
            record.mRainSpeed = 200.0f;
            record.mRainEntranceSpeed = 1.0f;
            record.mRainMaxRaindrops = 650;

            SkyState rain = standing.mSky;
            rain.mWeather.mWeather = &record;
            rain.mWeather.mPrecipitationAlpha = 1.0f;

            // `weather particle occlusion` decides for both renderers, and defaults to off: the
            // ray tracer had sheltered the rain the rasterizer lets fall under every awning.
            const bool setting = Settings::shaders().mWeatherParticleOcclusion;
            for (const bool shelters : { false, true })
            {
                Settings::shaders().mWeatherParticleOcclusion.set(shelters);
                Falling falling;

                // (200 + 700) / 2 = 450, and a cell over it.
                falling.mPrecipitation.setWeather(rain);
                EXPECT_EQ(reader.read(rain, standing.mWorld, falling.mPrecipitation, 0.0f, sReach).mShelterHeight,
                    shelters ? 450.0f + static_cast<float>(Constants::CellSizeInUnits) : 0.0f)
                    << "setting " << shelters;

                falling.mPrecipitation.setWeather(standing.mSky);
                EXPECT_EQ(
                    reader.read(standing.mSky, standing.mWorld, falling.mPrecipitation, 0.0f, sReach).mShelterHeight,
                    0.0f)
                    << "nothing falling, setting " << shelters;
            }
            Settings::shaders().mWeatherParticleOcclusion.set(setting);
        }

        TEST(RtxReadWorldTest, interiorWindowLightingTransitionsSmoothlyThroughDawnAndDusk)
        {
            const auto rgb = [](float r, float g, float b) {
                return osg::Vec4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            };
            const auto luminance = [](const osg::Vec3f& c) {
                return 0.2126f * c.x() + 0.7152f * c.y() + 0.0722f * c.z();
            };

            Sky::TimeOfDaySettings times;
            times.mNightStart = 20.0f;
            times.mNightEnd = 6.0f;
            times.mDayStart = 8.0f;
            times.mDayEnd = 18.0f;
            times.mSunriseDuration = 2.0f;
            times.mStarsPostSunsetStart = 1.0f;
            times.mStarsPreSunriseFinish = 2.0f;
            times.mStarsFadingDuration = 2.0f;
            times.mSunriseTransitions["Sky"] = { 0.5f, 1.0f, 1.5f, 0.5f };
            times.mSunriseTransitions["Ambient"] = { 0.5f, 2.0f, 1.0f, 1.25f };
            times.mSunriseTransitions["Fog"] = { 0.5f, 1.0f, 2.0f, 1.0f };
            times.mSunriseTransitions["Sun"] = { 0.0f, 0.0f, 1.0f, 1.25f };

            const Sky::TimeOfDayInterpolator<osg::Vec4f> skyColor(
                rgb(117, 141, 164), rgb(95, 135, 203), rgb(56, 89, 129), rgb(9, 10, 11));
            const Sky::TimeOfDayInterpolator<osg::Vec4f> fogColor(
                rgb(255, 189, 157), rgb(206, 227, 255), rgb(255, 189, 157), rgb(9, 10, 11));
            const Sky::TimeOfDayInterpolator<osg::Vec4f> ambientColor(
                rgb(47, 66, 96), rgb(137, 140, 160), rgb(68, 75, 96), rgb(32, 35, 42));
            const Sky::TimeOfDayInterpolator<osg::Vec4f> sunColor(
                rgb(242, 159, 119), rgb(255, 252, 238), rgb(255, 114, 79), rgb(59, 97, 176));

            SkyReader reader;
            reader.setInteriorWindowSunlight(true);
            reader.setInteriorWindowSkylight(30.0f);
            reader.setInteriorSunlightStrength(10.0f);
            const Falling falling;

            const auto evalAt = [&](float hour) {
                Standing s = standingIn(Location::Interior);
                s.mSky.mTimes = times;
                s.mSky.mExteriorValid = true;
                s.mSky.mSunUp = Sky::sunUp(hour, times);
                s.mWorld.mGameHour = hour;
                s.mWorld.mRoom = ESM::Cell::AMBIstruct{
                    .mAmbient = 0x00201818u,
                    .mSunlight = 0x00403028u,
                    .mFog = 0x00151510u,
                    .mFogDensity = 0.5f,
                };

                float adjustedHour = hour < times.mNightEnd ? hour + 24.0f : hour;
                const float dayDuration = times.mNightStart - times.mNightEnd;
                const float nightDuration = 24.0f - dayDuration;
                float orbit = 0.0f;
                if (adjustedHour < times.mNightStart)
                {
                    const float t = (adjustedHour - times.mNightEnd) / dayDuration;
                    orbit = 1.0f - 2.0f * t;
                }
                else
                {
                    const float t = (adjustedHour - times.mNightStart) / nightDuration;
                    orbit = 2.0f * t - 1.0f;
                }
                s.mSky.mSunDirection = Sky::sunDirection(orbit);

                s.mSky.mWeather.mSkyColor = skyColor.getValue(hour, times, "Sky");
                s.mSky.mWeather.mFogColor = fogColor.getValue(hour, times, "Fog");
                s.mSky.mWeather.mAmbientColor = ambientColor.getValue(hour, times, "Ambient");
                s.mSky.mWeather.mSunColor = sunColor.getValue(hour, times, "Sun");
                s.mSky.mWeather.mGlareView = 1.0f;
                s.mSky.mWeather.mSunDiscColor = osg::Vec4f(1.0f, 1.0f, 1.0f, Sky::sunDiscAlpha(hour, times));

                const Rtx::WorldReading reading = reader.read(s.mSky, s.mWorld, falling.mPrecipitation, 0.0, sReach, true);
                const WindowPortalLighting portal = reader.readWindowPortalLighting(s.mWorld, s.mSky, true);
                return std::pair{ reading, portal };
            };

            // Full night reaches exact zero even with skylight at 3000%.
            for (float nightHour : { 0.0f, 5.0f, 5.5f, 21.0f })
            {
                const auto [reading, portal] = evalAt(nightHour);
                EXPECT_TRUE(portal.mActive);
                EXPECT_EQ(portal.mSkyScatter, osg::Vec3f()) << "hour " << nightHour;
                EXPECT_EQ(portal.mSunScatter, osg::Vec3f()) << "hour " << nightHour;
                ASSERT_TRUE(reading.mInteriorWindowSky.has_value());
                EXPECT_EQ(reading.mInteriorWindowSky->mSun.mIrradiance, osg::Vec3f()) << "hour " << nightHour;
            }

            // Pre-dawn (05:45) and civil dusk (20:01, 20:30) deliver soft sky twilight with zero direct sun.
            for (float twilightHour : { 5.75f, 20.0f + 1.0f / 60.0f, 20.5f })
            {
                const auto [reading, portal] = evalAt(twilightHour);
                EXPECT_GT(luminance(portal.mSkyScatter), 0.0f) << "hour " << twilightHour;
                EXPECT_EQ(portal.mSunScatter, osg::Vec3f()) << "hour " << twilightHour;
                ASSERT_TRUE(reading.mInteriorWindowSky.has_value());
                EXPECT_EQ(reading.mInteriorWindowSky->mSun.mIrradiance, osg::Vec3f()) << "hour " << twilightHour;
            }

            // Continuity across sunrise (05:59:59 -> 06:00:01) and sunset (19:59:59 -> 20:00:01): no switch cliff,
            // and 2-minute steps across horizon remain a tiny fraction of daytime luminance.
            const float noonLum = luminance(evalAt(12.0f).second.mSkyScatter);
            const auto [dawnEdgeBefore, dawnEdgePortalBefore] = evalAt(6.0f - 1.0f / 3600.0f);
            const auto [dawnEdgeAfter, dawnEdgePortalAfter] = evalAt(6.0f + 1.0f / 3600.0f);
            const float dawnEdgeJump
                = std::abs(luminance(dawnEdgePortalAfter.mSkyScatter) - luminance(dawnEdgePortalBefore.mSkyScatter))
                / std::max(luminance(dawnEdgePortalBefore.mSkyScatter), 1e-4f);
            EXPECT_LT(dawnEdgeJump, 0.01f);

            const auto [duskEdgeBefore, duskEdgePortalBefore] = evalAt(20.0f - 1.0f / 3600.0f);
            const auto [duskEdgeAfter, duskEdgePortalAfter] = evalAt(20.0f + 1.0f / 3600.0f);
            const float duskEdgeJump
                = std::abs(luminance(duskEdgePortalBefore.mSkyScatter) - luminance(duskEdgePortalAfter.mSkyScatter))
                / std::max(luminance(duskEdgePortalBefore.mSkyScatter), 1e-4f);
            EXPECT_LT(duskEdgeJump, 0.01f);

            const auto [dawnBefore, dawnPortalBefore] = evalAt(5.0f + 59.0f / 60.0f);
            const auto [dawnAfter, dawnPortalAfter] = evalAt(6.0f + 1.0f / 60.0f);
            const float dawnStepVsNoon
                = std::abs(luminance(dawnPortalAfter.mSkyScatter) - luminance(dawnPortalBefore.mSkyScatter)) / noonLum;
            EXPECT_LT(dawnStepVsNoon, 0.05f);
            EXPECT_LT(luminance(dawnAfter.mInteriorWindowSky->mSun.mIrradiance),
                0.01f * luminance(evalAt(12.0f).first.mInteriorWindowSky->mSun.mIrradiance));

            const auto [duskBefore, duskPortalBefore] = evalAt(19.0f + 59.0f / 60.0f);
            const auto [duskAfter, duskPortalAfter] = evalAt(20.0f + 1.0f / 60.0f);
            const float duskDrop
                = std::abs(luminance(duskPortalBefore.mSkyScatter) - luminance(duskPortalAfter.mSkyScatter))
                / std::max(luminance(duskPortalBefore.mSkyScatter), 1e-4f);
            EXPECT_LT(duskDrop, 0.15f);
            EXPECT_LT(luminance(duskBefore.mInteriorWindowSky->mSun.mIrradiance),
                0.01f * luminance(evalAt(12.0f).first.mInteriorWindowSky->mSun.mIrradiance));

            // Monotonic increase across dawn and decrease across dusk.
            EXPECT_LT(luminance(evalAt(5.5f).second.mSkyScatter), luminance(evalAt(5.75f).second.mSkyScatter));
            EXPECT_LT(luminance(evalAt(5.75f).second.mSkyScatter), luminance(evalAt(6.0f).second.mSkyScatter));
            EXPECT_LT(luminance(evalAt(6.0f).second.mSkyScatter), luminance(evalAt(6.5f).second.mSkyScatter));
            EXPECT_LT(luminance(evalAt(6.5f).second.mSkyScatter), luminance(evalAt(7.0f).second.mSkyScatter));

            EXPECT_GT(luminance(evalAt(18.5f).second.mSkyScatter), luminance(evalAt(19.5f).second.mSkyScatter));
            EXPECT_GT(luminance(evalAt(19.5f).second.mSkyScatter), luminance(duskPortalAfter.mSkyScatter));
            EXPECT_GT(luminance(duskPortalAfter.mSkyScatter), luminance(evalAt(20.5f).second.mSkyScatter));
            EXPECT_GT(luminance(evalAt(20.5f).second.mSkyScatter), luminance(evalAt(21.0f).second.mSkyScatter));

            // Late-sunset sunlight (19:45) remains warm amber/orange rather than turning blue.
            const auto [lateSunset, latePortal] = evalAt(19.75f);
            ASSERT_TRUE(lateSunset.mInteriorWindowSky.has_value());
            const osg::Vec3f& sunsetSun = lateSunset.mInteriorWindowSky->mSun.mIrradiance;
            EXPECT_GT(sunsetSun.x(), sunsetSun.y());
            EXPECT_GT(sunsetSun.y(), sunsetSun.z());
            EXPECT_GT(latePortal.mSunScatter.x(), latePortal.mSunScatter.z());
        }
    }
}
