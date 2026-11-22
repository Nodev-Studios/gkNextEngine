#include "Engine/Common/CoreMinimal.hpp"
#include "Modules/NextStreamline/NgxIntegration.hpp"

#if WITH_DLSS_NGX

#include "Engine/Assets/GPU/UniformBuffer.hpp"
#include "Engine/Rendering/Upscaler/IUpscaler.hpp"
#include "Engine/Rendering/Upscaler/UpscalerTypes.hpp"
#include "Engine/Vulkan/DebugUtilities.hpp"
#include "Engine/Vulkan/DeviceCreationAugmenter.hpp"
#include "Engine/Vulkan/VulkanInterposer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <string>
#include <system_error>

#include <glm/gtc/type_ptr.hpp>

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

namespace
{
    // Identifies this engine to NGX. NVIDIA issues application ids per shipped title; an
    // engine without one identifies itself with a project GUID and the CUSTOM engine type.
    constexpr const char* ngxProjectId = "a3ff1176-ab9c-4e50-b133-c48e241a5640";
    constexpr const char* ngxEngineVersion = "1.0";
    constexpr uint32_t nvidiaVendorId = 0x10DE;
    // A feature handle may still be referenced by command buffers that were recorded but have
    // not finished executing. Handles retired mid-run wait this many frames before release.
    constexpr uint32_t ngxDeferredReleaseFrames = 16;

    struct FNgxState
    {
        std::wstring applicationDataPath;
        std::wstring featureSearchPath;
        const wchar_t* featureSearchPaths[1] = {nullptr};
        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        bool commonInfoReady = false;

        // Extension names handed to Vulkan must outlive instance/device creation, and the
        // arrays NGX returns are owned by NGX. std::list keeps element addresses stable.
        std::list<std::string> extensionNames;

        bool deviceExtensionsEnabled = false;

        bool initialized = false;
        VkDevice device = VK_NULL_HANDLE;
        NVSDK_NGX_Parameter* parameters = nullptr;
        Rendering::Upscaler::FFeatureCaps caps{};
    };

    FNgxState& NgxState()
    {
        static FNgxState state;
        return state;
    }

    const char* NgxResultString(NVSDK_NGX_Result result)
    {
        // GetNGXResultAsString returns a wide string; narrow it for spdlog.
        static thread_local std::string text;
        const wchar_t* wide = GetNGXResultAsString(result);
        text.clear();
        for (; wide != nullptr && *wide != L'\0'; ++wide)
        {
            text.push_back(*wide < 0x80 ? static_cast<char>(*wide) : '?');
        }
        return text.c_str();
    }

    void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        if (message == nullptr)
        {
            return;
        }
        std::string_view text(message);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        {
            text.remove_suffix(1);
        }
        if (text.empty())
        {
            return;
        }
        // NGX reports its optional configuration file and OTA model cache as errors whenever
        // they are absent, which is the normal state; neither affects the bundled models.
        const bool benign = text.find("NGXReadConfigFile") != std::string_view::npos ||
            text.find("NGXCreateModelsFolder") != std::string_view::npos;
        if (!benign && text.find("rror") != std::string_view::npos)
        {
            SPDLOG_WARN("NGX: {}", text);
        }
        else
        {
            SPDLOG_DEBUG("NGX: {}", text);
        }
    }

    std::filesystem::path NgxExecutableDirectory()
    {
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error || executable.empty())
        {
            return std::filesystem::current_path(error);
        }
        return executable.parent_path();
    }

    // NGX wants a writable directory for its own bookkeeping; keep it out of the install tree.
    std::filesystem::path NgxApplicationDataDirectory()
    {
        std::filesystem::path base;
        if (const char* cacheHome = std::getenv("XDG_CACHE_HOME"); cacheHome != nullptr && *cacheHome != '\0')
        {
            base = cacheHome;
        }
        else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        {
            base = std::filesystem::path(home) / ".cache";
        }
        else
        {
            std::error_code error;
            base = std::filesystem::temp_directory_path(error);
        }

        const auto directory = base / "gkNextEngine" / "ngx";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory;
    }

    const NVSDK_NGX_FeatureCommonInfo& NgxCommonInfo()
    {
        auto& state = NgxState();
        if (!state.commonInfoReady)
        {
            state.applicationDataPath = NgxApplicationDataDirectory().wstring();
            // The DLSS model libraries (libnvidia-ngx-dlss*.so) ship next to the executable.
            state.featureSearchPath = NgxExecutableDirectory().wstring();
            state.featureSearchPaths[0] = state.featureSearchPath.c_str();
            state.commonInfo.PathListInfo.Path = state.featureSearchPaths;
            state.commonInfo.PathListInfo.Length = 1;
            state.commonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
            state.commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
            state.commonInfo.LoggingInfo.DisableOtherLoggingSinks = true;
            state.commonInfoReady = true;
        }
        return state.commonInfo;
    }

    const char* NgxFeatureName(NVSDK_NGX_Feature feature)
    {
        return feature == NVSDK_NGX_Feature_RayReconstruction ? "DLSS Ray Reconstruction" : "DLSS";
    }

    bool NgxContainsExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
    {
        return std::any_of(extensions.begin(), extensions.end(),
            [name](const VkExtensionProperties& extension)
            {
                return std::strcmp(extension.extensionName, name) == 0;
            });
    }

    void NgxAppendExtension(std::vector<const char*>& extensions, const char* name)
    {
        const auto found = std::find_if(extensions.begin(), extensions.end(),
            [name](const char* current)
            {
                return current != nullptr && std::strcmp(current, name) == 0;
            });
        if (found == extensions.end())
        {
            extensions.push_back(NgxState().extensionNames.emplace_back(name).c_str());
        }
    }

    // What NGX needs from Vulkan, for every feature at once.
    //
    // This deliberately uses the SDK's original query rather than the per-feature discovery
    // API (NVSDK_NGX_VULKAN_GetFeatureRequirements and the two GetFeature*ExtensionRequirements
    // calls). Those load the feature libraries before NVSDK_NGX_VULKAN_Init, and with more
    // than one DLSS library present (Super Resolution and Ray Reconstruction, plus the copy the
    // driver ships) the NGX core then binds the Super Resolution slot to the Ray
    // Reconstruction library: creating a DLSS feature succeeds and evaluating it crashes
    // inside libnvidia-ngx-dlssd. Whether a feature is really available is asked after Init,
    // through the capability parameters, which is the documented source of truth anyway.
    struct FNgxRequiredExtensions
    {
        bool valid = false;
        std::vector<std::string> instance;
        std::vector<std::string> device;
    };

    const FNgxRequiredExtensions& NgxRequiredExtensions()
    {
        static FNgxRequiredExtensions required = []
        {
            FNgxRequiredExtensions result;
            unsigned int instanceCount = 0;
            const char** instanceNames = nullptr;
            unsigned int deviceCount = 0;
            const char** deviceNames = nullptr;
            const NVSDK_NGX_Result ngxResult = NVSDK_NGX_VULKAN_RequiredExtensions(
                &instanceCount, &instanceNames, &deviceCount, &deviceNames);
            if (NVSDK_NGX_FAILED(ngxResult))
            {
                SPDLOG_WARN("DLSS disabled: NGX could not report its Vulkan extensions ({})",
                            NgxResultString(ngxResult));
                return result;
            }
            for (unsigned int i = 0; i < instanceCount; ++i)
            {
                result.instance.emplace_back(instanceNames[i]);
            }
            for (unsigned int i = 0; i < deviceCount; ++i)
            {
                result.device.emplace_back(deviceNames[i]);
            }
            result.valid = true;
            return result;
        }();
        return required;
    }

    // All-or-nothing: one missing extension contributes none, so a half-enabled set is never
    // left behind for a device NGX cannot run on.
    bool NgxAppendAvailableExtensions(
        const char* scope,
        const std::vector<std::string>& required,
        const std::vector<VkExtensionProperties>& available,
        std::vector<const char*>& extensions)
    {
        for (const auto& name : required)
        {
            if (!NgxContainsExtension(available, name.c_str()))
            {
                SPDLOG_WARN("DLSS disabled because {} extension {} is unavailable", scope, name);
                return false;
            }
        }
        for (const auto& name : required)
        {
            NgxAppendExtension(extensions, name.c_str());
        }
        return true;
    }

    class FNgxInterposer final : public Vulkan::IVulkanInterposer
    {
    public:
        void AppendRequiredInstanceExtensions(std::vector<const char*>& extensions) override
        {
            const auto& required = NgxRequiredExtensions();
            if (!required.valid)
            {
                return;
            }
            const auto available = Vulkan::GetEnumerateVector(
                static_cast<const char*>(nullptr), vkEnumerateInstanceExtensionProperties);
            NgxAppendAvailableExtensions("instance", required.instance, available, extensions);
        }
    };

    class FNgxDeviceAugmenter final : public Vulkan::IDeviceCreationAugmenter
    {
    public:
        void* OnPhysicalDeviceSelected(
            VkInstance /*instance*/,
            VkPhysicalDevice physicalDevice,
            std::vector<const char*>& requiredExtensions,
            void* featureChain) override
        {
            auto& state = NgxState();
            state.deviceExtensionsEnabled = false;

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physicalDevice, &properties);
            if (properties.vendorID != nvidiaVendorId)
            {
                SPDLOG_INFO("DLSS disabled: the selected adapter ({}) is not an NVIDIA GPU",
                            properties.deviceName);
                return featureChain;
            }

            const auto& required = NgxRequiredExtensions();
            if (!required.valid)
            {
                return featureChain;
            }
            const auto available = Vulkan::GetEnumerateVector(
                physicalDevice, static_cast<const char*>(nullptr), vkEnumerateDeviceExtensionProperties);
            state.deviceExtensionsEnabled =
                NgxAppendAvailableExtensions("device", required.device, available, requiredExtensions);
            return featureChain;
        }
    };

    NVSDK_NGX_PerfQuality_Value ToNgxPerfQuality(uint32_t rawMode)
    {
        using Rendering::Upscaler::EUpscaleMode;
        switch (Rendering::Upscaler::GetUpscaleModeInfo(rawMode).mode)
        {
        case EUpscaleMode::Quality:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case EUpscaleMode::Balanced:
            return NVSDK_NGX_PerfQuality_Value_Balanced;
        case EUpscaleMode::Performance:
            return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case EUpscaleMode::UltraPerformance:
            return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case EUpscaleMode::Native:
            return NVSDK_NGX_PerfQuality_Value_DLAA;
        case EUpscaleMode::Auto:
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }

    VkImageAspectFlags NgxAspectMask(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    void NgxTransition(
        VkCommandBuffer commandBuffer,
        const Rendering::Upscaler::FImageResource& image,
        VkImageLayout from,
        VkImageLayout to)
    {
        if (!image.IsValid() || from == to)
        {
            return;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange = {NgxAspectMask(image.format), 0, 1, 0, 1};
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier);
    }

    NVSDK_NGX_Resource_VK ToNgxResource(const Rendering::Upscaler::FImageResource& image, bool readWrite)
    {
        // Depth is sampled through its depth aspect only, even when the image carries stencil.
        const VkImageAspectFlags aspect =
            (NgxAspectMask(image.format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0
                ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
                : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT);
        return NVSDK_NGX_Create_ImageView_Resource_VK(
            image.view,
            image.image,
            {aspect, 0, 1, 0, 1},
            image.format,
            image.extent.width,
            image.extent.height,
            readWrite);
    }

    // Everything a feature instance is created against. A change in any of it means the
    // instance has to be recreated; nothing here can be changed on a live handle.
    struct FNgxFeatureKey
    {
        bool rayReconstruction = false;
        NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        VkExtent2D renderExtent{};
        VkExtent2D outputExtent{};
        bool hdr = false;
        bool outputSubrect = false;

        bool operator==(const FNgxFeatureKey& other) const
        {
            return rayReconstruction == other.rayReconstruction &&
                perfQuality == other.perfQuality &&
                renderExtent.width == other.renderExtent.width &&
                renderExtent.height == other.renderExtent.height &&
                outputExtent.width == other.outputExtent.width &&
                outputExtent.height == other.outputExtent.height &&
                hdr == other.hdr &&
                outputSubrect == other.outputSubrect;
        }
    };

    class FNgxUpscaler final : public Rendering::Upscaler::IUpscaler
    {
    public:
        void OnDeviceCreated(
            const Rendering::Upscaler::FDeviceInfo& deviceInfo,
            Rendering::Upscaler::FFeatureCaps& caps) override
        {
            auto& state = NgxState();
            ShutdownNgx();
            caps = {};

            if (!state.deviceExtensionsEnabled)
            {
                return;
            }

            const auto& commonInfo = NgxCommonInfo();
            const NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(
                ngxProjectId,
                NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                ngxEngineVersion,
                state.applicationDataPath.c_str(),
                deviceInfo.instance,
                deviceInfo.physicalDevice,
                deviceInfo.device,
                vkGetInstanceProcAddr,
                vkGetDeviceProcAddr,
                &commonInfo,
                NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(initResult))
            {
                SPDLOG_WARN("DLSS disabled: NGX initialization failed ({})", NgxResultString(initResult));
                return;
            }
            state.initialized = true;
            state.device = deviceInfo.device;

            const NVSDK_NGX_Result parametersResult =
                NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.parameters);
            if (NVSDK_NGX_FAILED(parametersResult) || state.parameters == nullptr)
            {
                SPDLOG_WARN("DLSS disabled: NGX capability parameters unavailable ({})",
                            NgxResultString(parametersResult));
                ShutdownNgx();
                return;
            }

            const bool superResolution = QueryFeatureAvailable(
                NVSDK_NGX_Feature_SuperSampling,
                NVSDK_NGX_Parameter_SuperSampling_Available,
                NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult);
            const bool rayReconstruction = QueryFeatureAvailable(
                NVSDK_NGX_Feature_RayReconstruction,
                NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult);

            Rendering::Upscaler::SetUpscalerTypeSupport(
                state.caps.supportedTypes, Rendering::Upscaler::EUpscalerType::DLSS, superResolution);
            Rendering::Upscaler::SetUpscalerTypeSupport(
                state.caps.supportedTypes,
                Rendering::Upscaler::EUpscalerType::DLSSRayReconstruction,
                rayReconstruction);
            state.caps.requestedDeviceExtensionsAvailable = true;
            caps = state.caps;

            SPDLOG_INFO("NGX Vulkan device ready. DLSS={}, RR={}", superResolution, rayReconstruction);
            if (!superResolution && !rayReconstruction)
            {
                ShutdownNgx();
            }
        }

        void SetActiveType(Rendering::Upscaler::EUpscalerType type) override
        {
            if (!feature_)
            {
                return;
            }
            const bool wanted = featureKey_.rayReconstruction
                ? type == Rendering::Upscaler::EUpscalerType::DLSSRayReconstruction
                : type == Rendering::Upscaler::EUpscalerType::DLSS;
            if (!wanted)
            {
                RetireFeature();
            }
        }

        void OnSwapChainDestroyed() override
        {
            // The renderer idles the device before tearing a swapchain down, so nothing can
            // still be executing against these handles.
            ReleaseAllFeatures();
        }

        void Shutdown() override
        {
            ReleaseAllFeatures();
            ShutdownNgx();
        }

        Rendering::Upscaler::FOptimalRenderSettings GetOptimalRenderSettings(
            uint32_t superResolutionMode,
            VkExtent2D outputExtent,
            bool upscalerEnabled,
            bool /*hdrOutput*/,
            Rendering::Upscaler::EUpscalerType type) override
        {
            Rendering::Upscaler::FOptimalRenderSettings result{};
            const auto& modeInfo = Rendering::Upscaler::GetUpscaleModeInfo(superResolutionMode);
            result.renderExtent = Rendering::Upscaler::ScaleExtent(outputExtent, modeInfo.fallbackScale);
            result.minRenderExtent = result.renderExtent;
            result.maxRenderExtent = outputExtent;

            const auto& state = NgxState();
            if (!state.initialized || !upscalerEnabled ||
                !Rendering::Upscaler::SupportsUpscalerType(state.caps.supportedTypes, type) ||
                outputExtent.width == 0 || outputExtent.height == 0)
            {
                return result;
            }

            const bool useRR = Rendering::Upscaler::GetUpscalerTypeInfo(
                static_cast<uint32_t>(type)).requiresRayReconstruction;
            const NVSDK_NGX_PerfQuality_Value perfQuality = ToNgxPerfQuality(superResolutionMode);
            unsigned int optimalWidth = 0;
            unsigned int optimalHeight = 0;
            unsigned int maxWidth = 0;
            unsigned int maxHeight = 0;
            unsigned int minWidth = 0;
            unsigned int minHeight = 0;
            float sharpness = 0.0f;
            const NVSDK_NGX_Result ngxResult = useRR
                ? NGX_DLSSD_GET_OPTIMAL_SETTINGS(
                    state.parameters, outputExtent.width, outputExtent.height, perfQuality,
                    &optimalWidth, &optimalHeight, &maxWidth, &maxHeight, &minWidth, &minHeight,
                    &sharpness)
                : NGX_DLSS_GET_OPTIMAL_SETTINGS(
                    state.parameters, outputExtent.width, outputExtent.height, perfQuality,
                    &optimalWidth, &optimalHeight, &maxWidth, &maxHeight, &minWidth, &minHeight,
                    &sharpness);
            if (NVSDK_NGX_FAILED(ngxResult) || optimalWidth == 0 || optimalHeight == 0)
            {
                SPDLOG_WARN("NGX optimal settings query failed ({}); using fallback scale {}",
                            NgxResultString(ngxResult), modeInfo.fallbackScale);
                return result;
            }

            result.renderExtent = {optimalWidth, optimalHeight};
            result.minRenderExtent = {minWidth, minHeight};
            result.maxRenderExtent = {maxWidth, maxHeight};
            result.fromStreamline = true;
            return result;
        }

        Rendering::Upscaler::FFrameToken BeginFrame(uint32_t frameIndex, bool, uint32_t) override
        {
            TickRetiredFeatures();
            return {NgxState().initialized ? this : nullptr, frameIndex};
        }

        void MarkFrame(Rendering::Upscaler::EFrameMarker, const Rendering::Upscaler::FFrameToken&) override {}
        void SetReflexOptions(bool, uint32_t) override {}
        void ReflexSleep(const Rendering::Upscaler::FFrameToken&) override {}

        bool Evaluate(const Rendering::Upscaler::FFrameInputs& inputs) override
        {
            const auto& state = NgxState();
            if (!state.initialized ||
                !Rendering::Upscaler::SupportsUpscalerType(state.caps.supportedTypes, inputs.upscalerType) ||
                inputs.commandBuffer == VK_NULL_HANDLE || inputs.ubo == nullptr)
            {
                return false;
            }

            const bool useRR = Rendering::Upscaler::GetUpscalerTypeInfo(
                static_cast<uint32_t>(inputs.upscalerType)).requiresRayReconstruction;
            if (!inputs.scalingInputColor.IsValid() || !inputs.scalingOutputColor.IsValid() ||
                !inputs.depth.IsValid() || !inputs.motionVectors.IsValid())
            {
                return false;
            }
            if (useRR && (!inputs.albedo.IsValid() || !inputs.specularAlbedo.IsValid() ||
                          !inputs.normalRoughness.IsValid()))
            {
                return false;
            }

            // The output image is normally exactly the output rectangle. Only address a
            // sub-rectangle when the target really is larger than what DLSS writes.
            const auto outputX = static_cast<uint32_t>(std::max(inputs.outputOffset.x, 0));
            const auto outputY = static_cast<uint32_t>(std::max(inputs.outputOffset.y, 0));
            const bool outputSubrect = (outputX != 0 || outputY != 0) &&
                outputX + inputs.outputExtent.width <= inputs.scalingOutputColor.extent.width &&
                outputY + inputs.outputExtent.height <= inputs.scalingOutputColor.extent.height;

            FNgxFeatureKey key{};
            key.rayReconstruction = useRR;
            key.perfQuality = ToNgxPerfQuality(inputs.superResolutionMode);
            key.renderExtent = inputs.renderExtent;
            key.outputExtent = inputs.outputExtent;
            key.hdr = inputs.inputColorIsLinear || inputs.hdrOutput;
            key.outputSubrect = outputSubrect;
            if (!EnsureFeature(inputs.commandBuffer, key))
            {
                return false;
            }

            // NGX samples its inputs and writes its output through storage; the engine keeps
            // these images in other layouts between passes, so bracket the dispatch.
            const Rendering::Upscaler::FImageResource* sampled[] = {
                &inputs.scalingInputColor,
                &inputs.depth,
                &inputs.motionVectors,
                useRR ? &inputs.albedo : nullptr,
                useRR ? &inputs.specularAlbedo : nullptr,
                useRR ? &inputs.normalRoughness : nullptr,
                useRR ? &inputs.diffuseHitDistance : nullptr,
                useRR ? &inputs.specularHitDistance : nullptr,
            };
            for (const auto* image : sampled)
            {
                if (image != nullptr)
                {
                    NgxTransition(inputs.commandBuffer, *image, image->layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            NgxTransition(inputs.commandBuffer, inputs.scalingOutputColor,
                          inputs.scalingOutputColor.layout, VK_IMAGE_LAYOUT_GENERAL);

            const NVSDK_NGX_Result result = useRR
                ? EvaluateRayReconstruction(inputs, outputSubrect, outputX, outputY)
                : EvaluateSuperResolution(inputs, outputSubrect, outputX, outputY);
            NgxTransition(inputs.commandBuffer, inputs.scalingOutputColor,
                          VK_IMAGE_LAYOUT_GENERAL, inputs.scalingOutputColor.layout);
            for (const auto* image : sampled)
            {
                if (image != nullptr)
                {
                    NgxTransition(inputs.commandBuffer, *image,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image->layout);
                }
            }

            if (NVSDK_NGX_FAILED(result))
            {
                SPDLOG_ERROR("NGX {} evaluation failed: {}",
                             NgxFeatureName(useRR ? NVSDK_NGX_Feature_RayReconstruction
                                                  : NVSDK_NGX_Feature_SuperSampling),
                             NgxResultString(result));
                return false;
            }
            return true;
        }

        void TagFrameGeneration(const Rendering::Upscaler::FFrameInputs&) override {}
        void UpdateFrameGenerationState() override {}

        Rendering::Upscaler::FFrameGenerationState FrameGenerationState() const override
        {
            return {};
        }

    private:
        struct FRetiredFeature
        {
            NVSDK_NGX_Handle* handle = nullptr;
            uint32_t framesLeft = 0;
        };

        static bool QueryFeatureAvailable(
            NVSDK_NGX_Feature feature,
            const char* availableName,
            const char* needsDriverName,
            const char* initResultName)
        {
            const auto& state = NgxState();
            int available = 0;
            const NVSDK_NGX_Result result =
                NVSDK_NGX_Parameter_GetI(state.parameters, availableName, &available);
            if (NVSDK_NGX_SUCCEED(result) && available != 0)
            {
                return true;
            }

            int needsUpdatedDriver = 0;
            NVSDK_NGX_Parameter_GetI(state.parameters, needsDriverName, &needsUpdatedDriver);
            int initResult = static_cast<int>(NVSDK_NGX_Result_Fail);
            NVSDK_NGX_Parameter_GetI(state.parameters, initResultName, &initResult);
            SPDLOG_INFO("{} unavailable through NGX: {}{}",
                        NgxFeatureName(feature),
                        NgxResultString(static_cast<NVSDK_NGX_Result>(initResult)),
                        needsUpdatedDriver != 0 ? " (the NVIDIA driver needs an update)" : "");
            return false;
        }

        static void ShutdownNgx()
        {
            auto& state = NgxState();
            if (state.parameters != nullptr)
            {
                NVSDK_NGX_VULKAN_DestroyParameters(state.parameters);
                state.parameters = nullptr;
            }
            if (state.initialized)
            {
                NVSDK_NGX_VULKAN_Shutdown1(state.device);
            }
            state.initialized = false;
            state.device = VK_NULL_HANDLE;
            state.caps = {};
        }

        void RetireFeature()
        {
            if (feature_ != nullptr)
            {
                retired_.push_back({feature_, ngxDeferredReleaseFrames});
                feature_ = nullptr;
                SPDLOG_INFO("NGX {} GPU resources released",
                            NgxFeatureName(featureKey_.rayReconstruction
                                ? NVSDK_NGX_Feature_RayReconstruction
                                : NVSDK_NGX_Feature_SuperSampling));
            }
        }

        void TickRetiredFeatures()
        {
            for (auto it = retired_.begin(); it != retired_.end();)
            {
                if (it->framesLeft > 0)
                {
                    --it->framesLeft;
                    ++it;
                    continue;
                }
                NVSDK_NGX_VULKAN_ReleaseFeature(it->handle);
                it = retired_.erase(it);
            }
        }

        void ReleaseAllFeatures()
        {
            RetireFeature();
            for (const auto& retired : retired_)
            {
                NVSDK_NGX_VULKAN_ReleaseFeature(retired.handle);
            }
            retired_.clear();
        }

        bool EnsureFeature(VkCommandBuffer commandBuffer, const FNgxFeatureKey& key)
        {
            if (feature_ != nullptr && featureKey_ == key)
            {
                return true;
            }
            if (failedKeyValid_ && failedKey_ == key)
            {
                // Creation already failed for exactly this configuration; do not retry and
                // log every frame. Any change to the key clears this.
                return false;
            }
            RetireFeature();

            auto& state = NgxState();
            NVSDK_NGX_Parameter* parameters = state.parameters;
            NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

            int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
            if (key.hdr)
            {
                flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
            }

            NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
            if (key.rayReconstruction)
            {
                // Preset F is the current Ray Reconstruction transformer, as on the Streamline path.
                const auto preset = static_cast<unsigned int>(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, preset);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, preset);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, preset);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, preset);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, preset);

                NVSDK_NGX_DLSSD_Create_Params createParams{};
                createParams.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
                createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
                createParams.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
                createParams.InWidth = key.renderExtent.width;
                createParams.InHeight = key.renderExtent.height;
                createParams.InTargetWidth = key.outputExtent.width;
                createParams.InTargetHeight = key.outputExtent.height;
                createParams.InPerfQualityValue = key.perfQuality;
                createParams.InFeatureCreateFlags = flags;
                createParams.InEnableOutputSubrects = key.outputSubrect;
                result = NGX_VULKAN_CREATE_DLSSD_EXT1(
                    state.device, commandBuffer, 1, 1, &feature_, parameters, &createParams);
            }
            else
            {
                // NVIDIA's per-mode recommendation since DLSS 4.5, as on the Streamline path: K
                // where quality headroom exists, the second-generation transformer presets
                // where the input resolution is low.
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
                NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_DLSS_Hint_Render_Preset_L);

                // Both features share one parameter block, and creating a Ray Reconstruction
                // feature leaves DLSS.Denoise.Mode = DLUnified in it. The Super Resolution
                // library reads that parameter at creation, so state the mode it must run in.
                NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_Off);

                // The engine supplies no exposure texture, so DLSS derives exposure itself.
                // Streamline makes the same choice whenever no exposure buffer is tagged.
                flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

                NVSDK_NGX_DLSS_Create_Params createParams{};
                createParams.Feature.InWidth = key.renderExtent.width;
                createParams.Feature.InHeight = key.renderExtent.height;
                createParams.Feature.InTargetWidth = key.outputExtent.width;
                createParams.Feature.InTargetHeight = key.outputExtent.height;
                createParams.Feature.InPerfQualityValue = key.perfQuality;
                createParams.InFeatureCreateFlags = flags;
                createParams.InEnableOutputSubrects = key.outputSubrect;
                result = NGX_VULKAN_CREATE_DLSS_EXT1(
                    state.device, commandBuffer, 1, 1, &feature_, parameters, &createParams);
            }

            const char* featureName = NgxFeatureName(key.rayReconstruction
                ? NVSDK_NGX_Feature_RayReconstruction
                : NVSDK_NGX_Feature_SuperSampling);
            if (NVSDK_NGX_FAILED(result) || feature_ == nullptr)
            {
                SPDLOG_ERROR("NGX {} feature creation failed: {}", featureName, NgxResultString(result));
                feature_ = nullptr;
                failedKey_ = key;
                failedKeyValid_ = true;
                return false;
            }

            failedKeyValid_ = false;
            featureKey_ = key;
            SPDLOG_INFO("NGX {} feature created: {}x{} -> {}x{}{}",
                        featureName,
                        key.renderExtent.width, key.renderExtent.height,
                        key.outputExtent.width, key.outputExtent.height,
                        key.hdr ? " (HDR input)" : "");
            return true;
        }

        NVSDK_NGX_Result EvaluateSuperResolution(
            const Rendering::Upscaler::FFrameInputs& inputs,
            bool outputSubrect,
            uint32_t outputX,
            uint32_t outputY)
        {
            NVSDK_NGX_Resource_VK color = ToNgxResource(inputs.scalingInputColor, false);
            NVSDK_NGX_Resource_VK output = ToNgxResource(inputs.scalingOutputColor, true);
            NVSDK_NGX_Resource_VK depth = ToNgxResource(inputs.depth, false);
            NVSDK_NGX_Resource_VK motion = ToNgxResource(inputs.motionVectors, false);

            NVSDK_NGX_VK_DLSS_Eval_Params params{};
            params.Feature.pInColor = &color;
            params.Feature.pInOutput = &output;
            params.pInDepth = &depth;
            params.pInMotionVectors = &motion;
            params.InJitterOffsetX = inputs.ubo->Jitter.x;
            params.InJitterOffsetY = inputs.ubo->Jitter.y;
            params.InRenderSubrectDimensions = {inputs.renderExtent.width, inputs.renderExtent.height};
            params.InReset = inputs.reset ? 1 : 0;
            // Engine motion vectors are already in render-resolution pixels.
            params.InMVScaleX = 1.0f;
            params.InMVScaleY = 1.0f;
            if (outputSubrect)
            {
                params.InOutputSubrectBase = {outputX, outputY};
            }
            return NGX_VULKAN_EVALUATE_DLSS_EXT(
                inputs.commandBuffer, feature_, NgxState().parameters, &params);
        }

        NVSDK_NGX_Result EvaluateRayReconstruction(
            const Rendering::Upscaler::FFrameInputs& inputs,
            bool outputSubrect,
            uint32_t outputX,
            uint32_t outputY)
        {
            NVSDK_NGX_Resource_VK color = ToNgxResource(inputs.scalingInputColor, false);
            NVSDK_NGX_Resource_VK output = ToNgxResource(inputs.scalingOutputColor, true);
            NVSDK_NGX_Resource_VK depth = ToNgxResource(inputs.depth, false);
            NVSDK_NGX_Resource_VK motion = ToNgxResource(inputs.motionVectors, false);
            NVSDK_NGX_Resource_VK albedo = ToNgxResource(inputs.albedo, false);
            NVSDK_NGX_Resource_VK specularAlbedo = ToNgxResource(inputs.specularAlbedo, false);
            NVSDK_NGX_Resource_VK normalRoughness = ToNgxResource(inputs.normalRoughness, false);
            NVSDK_NGX_Resource_VK diffuseHitDistance = ToNgxResource(inputs.diffuseHitDistance, false);
            NVSDK_NGX_Resource_VK specularHitDistance = ToNgxResource(inputs.specularHitDistance, false);

            // glm stores column-major, which is the same memory layout as the row-major,
            // translation-in-the-last-row matrices NGX documents.
            glm::mat4 worldToView = inputs.ubo->ModelView;
            glm::mat4 viewToClip = inputs.ubo->ProjectionUnJit;

            NVSDK_NGX_VK_DLSSD_Eval_Params params{};
            params.pInColor = &color;
            params.pInOutput = &output;
            params.pInDepth = &depth;
            params.pInMotionVectors = &motion;
            params.pInDiffuseAlbedo = &albedo;
            params.pInSpecularAlbedo = &specularAlbedo;
            // Roughness is packed into normals.w (NVSDK_NGX_DLSS_Roughness_Mode_Packed).
            params.pInNormals = &normalRoughness;
            params.pInRoughness = &normalRoughness;
            // The engine has one motion vector target; reflections reuse it, as on Streamline.
            params.pInMotionVectorsReflections = &motion;
            if (inputs.diffuseHitDistance.IsValid())
            {
                params.pInDiffuseHitDistance = &diffuseHitDistance;
            }
            if (inputs.specularHitDistance.IsValid())
            {
                params.pInSpecularHitDistance = &specularHitDistance;
            }
            params.pInWorldToViewMatrix = glm::value_ptr(worldToView);
            params.pInViewToClipMatrix = glm::value_ptr(viewToClip);
            params.InJitterOffsetX = inputs.ubo->Jitter.x;
            params.InJitterOffsetY = inputs.ubo->Jitter.y;
            params.InRenderSubrectDimensions = {inputs.renderExtent.width, inputs.renderExtent.height};
            params.InReset = inputs.reset ? 1 : 0;
            params.InMVScaleX = 1.0f;
            params.InMVScaleY = 1.0f;
            if (outputSubrect)
            {
                params.InOutputSubrectBase = {outputX, outputY};
            }
            return NGX_VULKAN_EVALUATE_DLSSD_EXT(
                inputs.commandBuffer, feature_, NgxState().parameters, &params);
        }

        NVSDK_NGX_Handle* feature_ = nullptr;
        FNgxFeatureKey featureKey_{};
        FNgxFeatureKey failedKey_{};
        bool failedKeyValid_ = false;
        std::vector<FRetiredFeature> retired_;
    };
}

namespace NgxWrapper
{
    bool ShouldInitialize()
    {
        // NGX is implemented by the proprietary driver; without its kernel module there is no
        // libnvidia-ngx to load and every query below would only produce noise in the log.
        std::error_code error;
        return std::filesystem::exists("/proc/driver/nvidia/version", error);
    }

    Vulkan::IVulkanInterposer& InterposerInstance()
    {
        static FNgxInterposer interposer;
        return interposer;
    }

    Vulkan::IDeviceCreationAugmenter& DeviceAugmenterInstance()
    {
        static FNgxDeviceAugmenter augmenter;
        return augmenter;
    }

    std::unique_ptr<Rendering::Upscaler::IUpscaler> CreateUpscaler()
    {
        return std::make_unique<FNgxUpscaler>();
    }
}

#endif // WITH_DLSS_NGX
