# External downloads are prepared by `gnb setup`. CMake only validates paths.

# --- NVIDIA Streamline ---
# Pinned rather than globbed: an older SDK left under external/ must not be picked up silently.
# Keep in sync with [external.streamline].version in gnb.toml.
set(GK_STREAMLINE_VERSION "2.14.16")
if(WITH_STREAMLINE)
    set(STREAMLINE_ROOT "${CMAKE_SOURCE_DIR}/external/streamline-${GK_STREAMLINE_VERSION}")
    if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl.h")
        message(FATAL_ERROR "Streamline SDK v${GK_STREAMLINE_VERSION} missing under external/. Run `gnb deps fetch streamline` (or `gnb setup`) first.")
    endif()
    set(STREAMLINE_INCLUDE_DIR "${STREAMLINE_ROOT}/include")
    set(STREAMLINE_LIB_DIR "${STREAMLINE_ROOT}/lib/x64")
    set(STREAMLINE_BIN_DIR "${STREAMLINE_ROOT}/bin/x64")
    message(STATUS "Streamline Root: ${STREAMLINE_ROOT}")
endif()

# --- NVIDIA DLSS SDK (NGX, Linux) ---
# Pinned like Streamline. Keep in sync with [external.dlss].version in gnb.toml.
set(GK_DLSS_NGX_VERSION "310.9.16")
set(WITH_DLSS_NGX OFF)
if(ENABLE_DLSS_NGX AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(DLSS_NGX_ROOT "${CMAKE_SOURCE_DIR}/external/dlss-${GK_DLSS_NGX_VERSION}")
    set(DLSS_NGX_INCLUDE_DIR "${DLSS_NGX_ROOT}/include")
    set(DLSS_NGX_STATIC_LIB "${DLSS_NGX_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a")
    set(DLSS_NGX_RUNTIME_LIBS
        "${DLSS_NGX_ROOT}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.${GK_DLSS_NGX_VERSION}"
        "${DLSS_NGX_ROOT}/lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.${GK_DLSS_NGX_VERSION}"
    )
    set(dlssNgxComplete ON)
    foreach(dlssNgxFile "${DLSS_NGX_INCLUDE_DIR}/nvsdk_ngx_vk.h" "${DLSS_NGX_STATIC_LIB}" ${DLSS_NGX_RUNTIME_LIBS})
        if(NOT EXISTS "${dlssNgxFile}")
            set(dlssNgxComplete OFF)
        endif()
    endforeach()
    if(dlssNgxComplete)
        set(WITH_DLSS_NGX ON)
        message(STATUS "DLSS SDK Root: ${DLSS_NGX_ROOT}")
    else()
        message(STATUS "DLSS SDK v${GK_DLSS_NGX_VERSION} not found under external/: building without DLSS. "
                       "Run `gnb deps fetch dlss` (or `gnb setup`) and reconfigure to enable it.")
    endif()
endif()

# --- AMD FidelityFX SDK (FSR 3.1 Vulkan) ---
if(WITH_FIDELITYFX)
    set(FIDELITYFX_SDK_CONTAINER "${CMAKE_SOURCE_DIR}/external/fidelityfx-sdk-1.1.4")
    set(FIDELITYFX_SDK_ROOT "${FIDELITYFX_SDK_CONTAINER}/FidelityFX-SDK-1.1.4")
    if(NOT EXISTS "${FIDELITYFX_SDK_ROOT}/ffx-api/include/ffx_api/ffx_api.h" OR
       NOT EXISTS "${FIDELITYFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib")
        message(FATAL_ERROR "FidelityFX SDK v1.1.4 missing under external/. Run `gnb setup` first.")
    endif()
    set(FIDELITYFX_INCLUDE_DIR "${FIDELITYFX_SDK_ROOT}/ffx-api/include")
    set(FIDELITYFX_LIB "${FIDELITYFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib")
    set(FIDELITYFX_DLL "${FIDELITYFX_SDK_ROOT}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll")
    message(STATUS "FidelityFX SDK Root: ${FIDELITYFX_SDK_ROOT}")
endif()

