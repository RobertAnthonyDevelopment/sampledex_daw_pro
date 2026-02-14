#pragma once
#include <JuceHeader.h>
#include "Theme.h"

namespace sampledex
{
    class MixerView final : public juce::Component
    {
    public:
        MixerView() { setBufferedToImage(true); }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(theme::Colours::panel().darker(0.25f));
            auto r = getLocalBounds().reduced(8);
            g.setColour(theme::Colours::text().withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            g.drawText("Mixer Dock", r.removeFromTop(20), juce::Justification::centredLeft);
        }
    };
}
