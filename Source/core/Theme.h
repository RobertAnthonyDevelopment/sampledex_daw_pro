#pragma once
#include <JuceHeader.h>

namespace sampledex
{
    namespace theme
    {
        struct Colours
        {
            // Backgrounds
            static juce::Colour background()      { return juce::Colour::fromRGB(24, 27, 32); }
            static juce::Colour panel()           { return juce::Colour::fromRGB(38, 44, 52); }
            static juce::Colour header()          { return juce::Colour::fromRGB(49, 57, 68); }
            static juce::Colour darker()          { return juce::Colour::fromRGB(18, 21, 25); }
            
            // Accents
            static juce::Colour accent()          { return juce::Colour::fromRGB(255, 166, 41); }
            static juce::Colour selection()       { return juce::Colour::fromRGB(255, 166, 41).withAlpha(0.35f); }
            static juce::Colour playhead()        { return juce::Colour::fromRGB(255, 77, 77); }
            
            // Elements
            static juce::Colour text()            { return juce::Colour::fromRGB(235, 240, 245); }
            static juce::Colour gridLine()        { return juce::Colour::fromRGB(210, 220, 230).withAlpha(0.14f); }
            static juce::Colour clipAudio()       { return juce::Colour::fromRGB(79, 133, 210); }
            static juce::Colour clipMidi()        { return juce::Colour::fromRGB(69, 179, 121); }
        };

        struct Dimensions
        {
            static constexpr int trackHeaderWidth = 200;
            static constexpr int trackHeight = 80;
            static constexpr int transportHeight = 50;
        };
    }
}
