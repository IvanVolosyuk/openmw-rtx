#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_SHARED_DLSS_H
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_SHARED_DLSS_H

#include <components/rtx/shaders/camera.h>
#include <components/rtx/shaders/hosttypes.h>
#include <components/rtx/shaders/portable.h>
#include <components/rtx/shaders/storageformat.h>

// What the DLSS input resolve reads and writes, for the shader and the host that binds it. DLSS
// itself is handed two full-resolution images: the reversed device depth the renderer already uses
// for FSR, and screen-space motion in render pixels. The trace stores both in checkerboard fields,
// so one dispatch rebuilds them at every pixel before NGX sees them.

#define DLSS_DEPTH_FORMAT STORAGE_R32F
#define DLSS_MOTION_FORMAT STORAGE_RG16F
#define DLSS_OUTPUT_FORMAT STORAGE_RGBA16F
#define DLSSD_ALBEDO_FORMAT STORAGE_RGBA16F
#define DLSSD_NORMAL_ROUGHNESS_FORMAT STORAGE_RGBA16F

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// The camera the trace sampled through and the near plane its reversed depth is written
    /// against, as `FsrInputConstants` carries them: the two eyes a pixel's surface can name, the
    /// frame whose field mapping wrote it, and the near plane.
    struct DlssInputConstants
    {
        Eyes mEyes;
        float mNear;
        uint mFrame;
    };

    /// What the DLSS Ray Reconstruction input resolve reads of the frame: the two eyes, the far
    /// plane written where a ray meets the sky, the frame whose field mapping wrote the channels,
    /// and whether the scene carries specular lobes (`CHANNEL_SPECULAR` / `CHANNEL_SPECULAR_ALBEDO`).
    struct DlssdInputConstants
    {
        Eyes mEyes;
        float mFar;
        uint mFrame;
        uint mLobed;
    };

    /// The Super Resolution input resolve's bindings, in the order the host writes them.
    const uint DLSS_INPUTS_BIND_SURFACE = 0;
    const uint DLSS_INPUTS_BIND_MOTION = 1;
    const uint DLSS_INPUTS_BIND_DEPTH = 2;
    const uint DLSS_INPUTS_BIND_MOTION_OUT = 3;
    const uint DLSS_INPUTS_BIND_CONSTANTS = 4;

    /// The Ray Reconstruction input resolve's bindings, in the order the host writes them.
    const uint DLSSD_INPUTS_BIND_SURFACE = 0;
    const uint DLSSD_INPUTS_BIND_MOTION = 1;
    const uint DLSSD_INPUTS_BIND_ALBEDO = 2;
    const uint DLSSD_INPUTS_BIND_SPECULAR = 3;
    const uint DLSSD_INPUTS_BIND_SPECULAR_ALBEDO = 4;
    const uint DLSSD_INPUTS_BIND_DEPTH = 5;
    const uint DLSSD_INPUTS_BIND_MOTION_OUT = 6;
    const uint DLSSD_INPUTS_BIND_DIFFUSE_ALBEDO_OUT = 7;
    const uint DLSSD_INPUTS_BIND_SPECULAR_ALBEDO_OUT = 8;
    const uint DLSSD_INPUTS_BIND_NORMAL_ROUGHNESS_OUT = 9;
    const uint DLSSD_INPUTS_BIND_CONSTANTS = 10;

    /// The resolve's square workgroup: eight pixels on a side, as the trace's own stages use.
    const uint DLSS_INPUT_WORKGROUP = 8;

#ifdef RTX_HOST
    static_assert(sizeof(DlssdInputConstants) == sizeof(Eyes) + 12, "DlssdInputConstants must be scalar-packed");
}
#endif

#endif
