#pragma once

namespace Runtime::Config
{
    class Options;
}

namespace Modules::NextStreamline
{
    // Installs the NVIDIA DLSS provider when the hardware supports it and hooks the
    // engine's Vulkan interposer, device augmenter and upscaler factory. Windows goes
    // through Streamline (DLSS-SR / DLSS-RR / Frame Generation / Reflex); Linux has no
    // Streamline, so it drives the NGX SDK directly (DLSS-SR / DLSS-RR only). Call from
    // the application entry before the engine is created; a no-op elsewhere or when
    // disabled.
    void Install(const Runtime::Config::Options& options);
}
