# NVIDIA DLSS, downloaded or pointed at.
#
# The SDK is not redistributable as source and is not kept in this tree. A configure either finds
# `OPENMW_DLSS_SDK` (an extracted NVIDIA/DLSS source tree) or downloads the small set of headers and
# the one library the backend links from NVIDIA's pinned tag. The runtime DLL/so is installed beside
# the executable, which is the form NVIDIA's licence permits a built application to ship.
#
# A failed download turns the option off with a warning: an offline build still makes the FSR-only
# renderer rather than stopping on a dependency the player may not want.

option(OPENMW_RTX_DLSS "Build the ray tracer with NVIDIA DLSS support" ON)
set(OPENMW_DLSS_SDK "" CACHE PATH
    "An extracted NVIDIA/DLSS SDK tree (the folder holding include/ and lib/), or empty to download")
set(OPENMW_DLSS_VERSION "310.9.16" CACHE STRING "NVIDIA DLSS SDK release to download when OPENMW_DLSS_SDK is empty")

set(OPENMW_DLSS_INCLUDE_DIR "")
set(OPENMW_DLSS_LIBRARY "")
set(OPENMW_DLSS_RUNTIME_FILE "")
set(OPENMW_DLSSD_RUNTIME_FILE "")
set(OPENMW_DLSS_LICENSE "")

if (OPENMW_RTX_DLSS)
    if (OPENMW_DLSS_SDK)
        set(_dlss_root "${OPENMW_DLSS_SDK}")
    else ()
        set(_dlss_root "${CMAKE_BINARY_DIR}/dlss-sdk")
        file(MAKE_DIRECTORY "${_dlss_root}/include")

        function(openmw_dlss_download relative expected destination)
            if (EXISTS "${destination}")
                return()
            endif ()

            file(DOWNLOAD
                "https://raw.githubusercontent.com/NVIDIA/DLSS/v${OPENMW_DLSS_VERSION}/${relative}"
                "${destination}.part"
                EXPECTED_HASH SHA256=${expected}
                STATUS _status
                TLS_VERIFY ON
                SHOW_PROGRESS)
            list(GET _status 0 _code)
            if (NOT _code EQUAL 0)
                list(GET _status 1 _message)
                message(WARNING "DLSS: ${relative} could not be downloaded: ${_message}")
                file(REMOVE "${destination}.part")
                return()
            endif ()
            file(RENAME "${destination}.part" "${destination}")
        endfunction()

        set(_headers
            nvsdk_ngx.h|dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
            nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
            nvsdk_ngx_defs_dlssd.h|2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716
            nvsdk_ngx_defs_vk.h|0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031
            nvsdk_ngx_helpers_dlssd_vk.h|11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b
            nvsdk_ngx_helpers_vk.h|c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623
            nvsdk_ngx_loader.h|abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75
            nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
            nvsdk_ngx_params_dlssd.h|d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e
            nvsdk_ngx_vk.h|2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d
        )
        foreach (_header ${_headers})
            string(REPLACE "|" ";" _parts "${_header}")
            list(GET _parts 0 _name)
            list(GET _parts 1 _hash)
            openmw_dlss_download("include/${_name}" "${_hash}" "${_dlss_root}/include/${_name}")
        endforeach ()

        if (WIN32)
            set(_lib "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
            set(_lib_hash "bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c")
            set(_runtime "lib/Windows_x86_64/rel/nvngx_dlss.dll")
            set(_runtime_hash "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e")
            set(_dlssd_runtime "lib/Windows_x86_64/rel/nvngx_dlssd.dll")
            set(_dlssd_runtime_hash "b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee")
        elseif (UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            set(_lib "lib/Linux_x86_64/libnvsdk_ngx.a")
            set(_lib_hash "d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df")
            set(_runtime "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${OPENMW_DLSS_VERSION}")
            set(_runtime_hash "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6")
            set(_dlssd_runtime "lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.${OPENMW_DLSS_VERSION}")
            set(_dlssd_runtime_hash "a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96")
        else ()
            message(WARNING "DLSS: no SDK runtime is available for this target")
            set(OPENMW_RTX_DLSS OFF)
        endif ()

        if (OPENMW_RTX_DLSS)
            openmw_dlss_download("${_lib}" "${_lib_hash}" "${_dlss_root}/${_lib}")
            openmw_dlss_download("${_runtime}" "${_runtime_hash}" "${_dlss_root}/${_runtime}")
            openmw_dlss_download("${_dlssd_runtime}" "${_dlssd_runtime_hash}" "${_dlss_root}/${_dlssd_runtime}")
            openmw_dlss_download("LICENSE.txt"
                "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
                "${_dlss_root}/LICENSE.txt")
        endif ()
    endif ()

    if (OPENMW_RTX_DLSS)
        set(OPENMW_DLSS_INCLUDE_DIR "${_dlss_root}/include")
        if (EXISTS "${_dlss_root}/LICENSE.txt")
            set(OPENMW_DLSS_LICENSE "${_dlss_root}/LICENSE.txt")
        endif ()
        if (WIN32)
            set(OPENMW_DLSS_LIBRARY "${_dlss_root}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
            set(OPENMW_DLSS_RUNTIME_FILE "${_dlss_root}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
            set(OPENMW_DLSSD_RUNTIME_FILE "${_dlss_root}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
        elseif (UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            set(OPENMW_DLSS_LIBRARY "${_dlss_root}/lib/Linux_x86_64/libnvsdk_ngx.a")
            set(OPENMW_DLSS_RUNTIME_FILE
                "${_dlss_root}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${OPENMW_DLSS_VERSION}")
            set(OPENMW_DLSSD_RUNTIME_FILE
                "${_dlss_root}/lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.${OPENMW_DLSS_VERSION}")
        endif ()

        if (NOT EXISTS "${OPENMW_DLSS_INCLUDE_DIR}/nvsdk_ngx.h"
            OR NOT EXISTS "${OPENMW_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssd_vk.h"
            OR NOT EXISTS "${OPENMW_DLSS_LIBRARY}"
            OR NOT EXISTS "${OPENMW_DLSS_RUNTIME_FILE}"
            OR NOT EXISTS "${OPENMW_DLSSD_RUNTIME_FILE}")
            message(WARNING "DLSS: the SDK at '${_dlss_root}' is incomplete; building without it")
            set(OPENMW_RTX_DLSS OFF)
        endif ()
    endif ()
endif ()

message(STATUS "DLSS: ${OPENMW_RTX_DLSS}")
