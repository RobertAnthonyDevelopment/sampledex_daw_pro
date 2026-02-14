#pragma once
#include <JuceHeader.h>
#include "Theme.h"

namespace sampledex
{
    class TransportBar final : public juce::Component
    {
    public:
        TransportBar()
        {
            setBufferedToImage(true);
            setInterceptsMouseClicks(false, true);
        }

        void paint(juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            juce::ColourGradient grad(theme::Colours::header().brighter(0.10f),
                                      bounds.getTopLeft(),
                                      theme::Colours::panel().darker(0.15f),
                                      bounds.getBottomLeft(),
                                      false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(bounds.reduced(2.0f), 10.0f);

            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawRoundedRectangle(bounds.reduced(2.0f), 10.0f, 1.0f);
            g.setColour(theme::Colours::gridLine().withAlpha(0.35f));
            g.drawLine(4.0f, bounds.getBottom() - 1.0f, bounds.getRight() - 4.0f, bounds.getBottom() - 1.0f, 1.0f);
        }
    };
}
