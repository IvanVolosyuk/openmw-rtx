#pragma once

#include <cstdint>

#include <components/rtx/frame/dielectricf0.hpp>
#include <components/rtx/frame/upscale.hpp>
#include <components/rtx/shaders/gbuffer.h>
#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

namespace Settings
{
    /// The ray tracing renderer's own settings, read as they are wanted.
    // An exception here ends the process, which is what this function means.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    struct RTXCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<std::int32_t> mLampSourceSize{ mIndex, "RTX", "lamp source size",
            makeClampSanitizerInt(1, 100) };
        SettingValue<std::int32_t> mLampShadowClearance{ mIndex, "RTX", "lamp shadow clearance",
            makeClampSanitizerInt(0, 200) };
        SettingValue<std::int32_t> mActorGlowSourceSize{ mIndex, "RTX", "actor glow source size",
            makeClampSanitizerInt(1, 100) };
        SettingValue<std::int32_t> mActorGlowClearance{ mIndex, "RTX", "actor glow clearance",
            makeClampSanitizerInt(0, 200) };
        SettingValue<std::int32_t> mLampFlicker{ mIndex, "RTX", "lamp flicker strength",
            makeClampSanitizerInt(0, 300) };
        SettingValue<std::int32_t> mLampSpeed{ mIndex, "RTX", "lamp animation speed",
            makeClampSanitizerInt(10, 400) };
        SettingValue<std::int32_t> mLampMovement{ mIndex, "RTX", "lamp movement", makeClampSanitizerInt(0, 2000) };
        SettingValue<std::int32_t> mLampBreathing{ mIndex, "RTX", "lamp size variation", makeClampSanitizerInt(0, 90) };
        SettingValue<bool> mInteriorWindowSunlight{ mIndex, "RTX", "interior window sunlight" };
        SettingValue<std::int32_t> mInteriorSunlightStrength{ mIndex, "RTX", "interior sunlight strength",
            makeClampSanitizerInt(0, 3000) };
        SettingValue<std::int32_t> mInteriorWindowSkylight{ mIndex, "RTX", "interior window skylight",
            makeClampSanitizerInt(0, 3000) };
        SettingValue<std::int32_t> mInteriorWindowGodrays{ mIndex, "RTX", "interior window godrays",
            makeClampSanitizerInt(0, 300) };

        /// The most cells `distant land cells` takes: a bound on how much world a frame is asked to
        /// stand, and so on what the ring's ask costs. Nought stays nought, which hands the reach
        /// back to `viewing distance`.
        static constexpr float sMaxDistantLandCells = 30.0f;

        /// Slider bounds and step size for per-weather fog visibility distances (in cells; `0` follows
        /// distant land).
        static constexpr float sMinWeatherFogDistanceCells = 0.25f;
        static constexpr float sMaxWeatherFogDistanceCells = sMaxDistantLandCells;
        static constexpr float sWeatherFogDistanceStepCells = 0.25f;

        /// The fewest the two menus offer. A file may name fewer, and the launcher leaves such a
        /// value as it found it.
        static constexpr float sMinDistantLandCellsInMenu = 4.0f;

        /// The most bounce rays the eye's surface may trace, which the trace keeps a landing's list for.
        static constexpr std::int32_t sMaxBounceRays = static_cast<std::int32_t>(Rtx::Shaders::MAX_BOUNCE_RAYS);

        /// How far out from the eye the world is built, in cells: handed to the world mirror where the
        /// renderer is made and again when the menu moves it, so the rings, the air and the map follow one
        /// number. It is a property of the structure rays are cast against, not of the camera, which is
        /// what `viewing distance` is about (`Rtx::CellGrid::reachOf`).
        SettingValue<float> mDistantLandCells{ mIndex, "RTX", "distant land cells",
            makeClampSanitizerFloat(0.0f, sMaxDistantLandCells) };

        SettingValue<float> mWeatherFogClear{ mIndex, "RTX", "weather fog distance clear",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogCloudy{ mIndex, "RTX", "weather fog distance cloudy",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogFoggy{ mIndex, "RTX", "weather fog distance foggy",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogOvercast{ mIndex, "RTX", "weather fog distance overcast",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogRain{ mIndex, "RTX", "weather fog distance rain",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogThunderstorm{ mIndex, "RTX", "weather fog distance thunderstorm",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogAshstorm{ mIndex, "RTX", "weather fog distance ashstorm",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogBlight{ mIndex, "RTX", "weather fog distance blight",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogSnow{ mIndex, "RTX", "weather fog distance snow",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };
        SettingValue<float> mWeatherFogBlizzard{ mIndex, "RTX", "weather fog distance blizzard",
            makeClampSanitizerFloat(0.0f, sMaxWeatherFogDistanceCells) };

        /// How hard the upscaler works, as `Rtx::sUpscaleModeNames` spells the modes and read by it at
        /// load. Changing it rebuilds every target; under no upscaler it waits for one. `Rtx::sUpscaleMenu`
        /// is what the launcher and the settings window offer of it.
        SettingValue<Rtx::Upscale> mUpscale{ mIndex, "RTX", "upscale" };

        /// Which upscaler does the work, or none, as `Rtx::sUpscalerNames` spells them. Read once where
        /// the renderer is made, because each brings its own histories and pipelines.
        SettingValue<Rtx::UpscalerChoice> mUpscaler{ mIndex, "RTX", "upscaler" };

        /// Whether the eye's surface traces bounces, or the record's ambient stands on it:
        /// `Rtx::SamplingSwitches::mIndirect`, handed to the renderer where it is made and again when
        /// the menu moves it, as the count below is.
        SettingValue<bool> mIndirectLighting{ mIndex, "RTX", "indirect lighting" };

        /// How many bounce rays the eye's surface traces a frame: `Rtx::SamplingSwitches::mBounceRays`.
        SettingValue<std::int32_t> mBounceRays{ mIndex, "RTX", "bounce rays",
            makeClampSanitizerInt(1, sMaxBounceRays) };

        static constexpr float sMinDielectricF0Base = 0.0f;
        static constexpr float sMaxDielectricF0Base = 0.1f;

        /// Non-metal dielectric F0 calculation mode (`Rtx::sDielectricF0ModeNames`) and base F0 (`0.0` to `0.1`).
        SettingValue<Rtx::DielectricF0Mode> mDielectricF0Mode{ mIndex, "RTX", "dielectric f0 mode" };
        SettingValue<float> mDielectricF0Base{ mIndex, "RTX", "dielectric f0 base",
            makeClampSanitizerFloat(sMinDielectricF0Base, sMaxDielectricF0Base) };
    };
}
