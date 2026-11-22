#pragma once

#include "Engine/Common/CoreMinimal.hpp"

namespace Rendering::Upscaler
{
    class IUpscaler;
}

namespace Vulkan
{
    class IDeviceCreationAugmenter;
    class IVulkanInterposer;
}

// NVIDIA DLSS through the NGX SDK, talking to the driver directly. Streamline has no Linux
// build, so this is the Linux counterpart of StreamlineIntegration: the same two upscaler
// types (DLSS Super Resolution and DLSS Ray Reconstruction) behind the same IUpscaler seam.
// NGX has no presentation layer, so frame generation, Reflex and PCL stay unsupported here.
// NGX SDK symbols stay in the cpp so this header never needs the SDK.
namespace NgxWrapper
{
#if WITH_DLSS_NGX
    // True when an NVIDIA kernel driver is loaded; NGX has nothing to talk to otherwise.
    bool ShouldInitialize();

    // Only contributes instance extensions; every other entry point stays vanilla Vulkan.
    Vulkan::IVulkanInterposer& InterposerInstance();
    Vulkan::IDeviceCreationAugmenter& DeviceAugmenterInstance();

    std::unique_ptr<Rendering::Upscaler::IUpscaler> CreateUpscaler();
#endif
}
