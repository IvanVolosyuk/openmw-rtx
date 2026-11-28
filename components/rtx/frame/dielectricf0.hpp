#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include <components/rtx/common/namedenum.hpp>
#include <components/rtx/shaders/look.h>

namespace Rtx
{
    /// Which non-metal specular reflectance (`F0`) rule the shader resolves: constant base F0,
    /// linear decay across high roughness (`0.50 -> 0.90`), or rough albedo-luminance cavity F0
    /// paired with Lagarde specular occlusion and roughness-attenuated grazing F90 (`[RTX] dielectric f0 mode`).
    enum class DielectricF0Mode : std::uint32_t
    {
        Fixed = Shaders::DIELECTRIC_F0_MODE_FIXED,
        Linear = Shaders::DIELECTRIC_F0_MODE_LINEAR,
        AlbedoLuma = Shaders::DIELECTRIC_F0_MODE_ALBEDO_LUMA,
    };

    inline constexpr NamedEnum sDielectricF0ModeNames{ std::array{
        std::pair{ DielectricF0Mode::Fixed, std::string_view("fixed") },
        std::pair{ DielectricF0Mode::Linear, std::string_view("linear") },
        std::pair{ DielectricF0Mode::AlbedoLuma, std::string_view("albedo-luma") },
    } };

    inline constexpr std::array<DielectricF0Mode, sDielectricF0ModeNames.mNames.size()> sDielectricF0ModeMenu
        = sDielectricF0ModeNames.values();

    inline constexpr DielectricF0Mode sDielectricF0ModeUnread = DielectricF0Mode::AlbedoLuma;
}
