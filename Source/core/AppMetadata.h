#pragma once
#include <JuceHeader.h>

namespace sampledex::meta
{
    // Centralized branding / IDs. Helpful when you later add AUv3, iOS targets, licensing, etc.
    static constexpr const char* companyName  = "Sampledex";
    static constexpr const char* productName  = "Sampledex ChordLab";

    // "Manufacturer code" (FourCC) analogous to plugin manufacturer codes.
    // Not strictly required for a standalone app, but kept here for continuity with your plugin workflow.
    static constexpr juce::uint32 manufacturerCode = juce::fourCharCode ('S','d','x','1');

    // Product/app code (FourCC) – also mainly relevant for plugin ecosystems.
    static constexpr juce::uint32 productCode      = juce::fourCharCode ('C','L','A','B');
} // namespace sampledex::meta
