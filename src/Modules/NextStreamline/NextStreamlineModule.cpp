#include "Engine/Common/CoreMinimal.hpp"
#include "Modules/NextStreamline/NextStreamlineModule.hpp"

#include "Engine/Options.hpp"
#include "Engine/Rendering/Upscaler/UpscalerRegistry.hpp"
#include "Engine/Vulkan/DeviceCreationAugmenter.hpp"
#include "Engine/Vulkan/VulkanInterposer.hpp"
#include "Modules/NextStreamline/NgxIntegration.hpp"
#include "Modules/NextStreamline/StreamlineIntegration.hpp"

namespace Modules::NextStreamline
{
    void Install(const Runtime::Config::Options& options)
    {
#if WITH_STREAMLINE && WIN32
        if (options.DisableStreamline)
        {
            SPDLOG_INFO("Streamline DLSS plugins disabled for this application");
            return;
        }
        if (!StreamlineWrapper::ShouldInitialize())
        {
            SPDLOG_INFO("Streamline DLSS plugins disabled because no NVIDIA adapter is present");
            return;
        }

        // Starts slInit on a worker and returns immediately; the first seam that needs
        // Streamline joins it. Registering before the result is known is safe because each
        // seam falls through to the native Vulkan path while IsInitialized() is false.
        StreamlineWrapper::Initialize();

        Vulkan::SetInterposer(&StreamlineWrapper::InterposerInstance());
        Vulkan::RegisterDeviceCreationAugmenter(&StreamlineWrapper::DeviceAugmenterInstance());
        Rendering::Upscaler::RegisterUpscalerFactory(
            [] { return Rendering::Upscaler::CreateStreamlineUpscaler(); });
#elif WITH_DLSS_NGX
        // Streamline has no Linux build; DLSS-SR and DLSS-RR go through the NGX SDK directly.
        if (options.DisableStreamline)
        {
            SPDLOG_INFO("DLSS (NGX) disabled for this application");
            return;
        }
        if (!NgxWrapper::ShouldInitialize())
        {
            SPDLOG_INFO("DLSS (NGX) disabled because no NVIDIA driver is loaded");
            return;
        }

        Vulkan::SetInterposer(&NgxWrapper::InterposerInstance());
        Vulkan::RegisterDeviceCreationAugmenter(&NgxWrapper::DeviceAugmenterInstance());
        Rendering::Upscaler::RegisterUpscalerFactory(
            [] { return NgxWrapper::CreateUpscaler(); });
        SPDLOG_INFO("DLSS provider installed through NGX (Super Resolution, Ray Reconstruction)");
#else
        (void)options;
#endif
    }
}
