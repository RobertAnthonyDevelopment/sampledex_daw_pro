#pragma once
#include <JuceHeader.h>
#include "Theme.h"

namespace sampledex
{
    class TimelineView final : public juce::Component
    {
    public:
        TimelineView()
        {
            setBufferedToImage(true);
        }

        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.fillAll(theme::Colours::background());
            g.setColour(juce::Colours::white.withAlpha(0.04f));
            for (int x = 0; x < r.getWidth(); x += 40)
                g.drawVerticalLine(x, 0.0f, static_cast<float>(r.getHeight()));
        }
    };
}
