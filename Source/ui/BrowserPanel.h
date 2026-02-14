#pragma once
#include <JuceHeader.h>
#include "Theme.h"

namespace sampledex
{
    class BrowserPanel final : public juce::Component
    {
    public:
        BrowserPanel()
        {
            setBufferedToImage(true);
        }

        void paint(juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.fillAll(theme::Colours::panel().darker(0.35f));
            g.setColour(theme::Colours::text().withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            g.drawText("Browser", r.removeFromTop(24).reduced(8, 0), juce::Justification::centredLeft);

            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(r.reduced(8).removeFromTop(34).toFloat(), 6.0f);
            g.setColour(theme::Colours::text().withAlpha(0.6f));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText("Search samples, clips, presets...", r.reduced(16).removeFromTop(30), juce::Justification::centredLeft);
        }
    };
}
