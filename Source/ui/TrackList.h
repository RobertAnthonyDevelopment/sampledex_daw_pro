#pragma once
#include <JuceHeader.h>
#include "Theme.h"

namespace sampledex
{
    class TrackList final : public juce::Component
    {
    public:
        void setTrackNames(juce::StringArray namesIn)
        {
            if (trackNames != namesIn)
            {
                trackNames = std::move(namesIn);
                repaint();
            }
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(theme::Colours::darker().withMultipliedBrightness(0.95f));
            auto r = getLocalBounds().reduced(8);
            g.setColour(theme::Colours::text().withAlpha(0.9f));
            g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
            g.drawText("Tracks", r.removeFromTop(20), juce::Justification::centredLeft);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::plain)));

            for (int i = 0; i < trackNames.size() && r.getHeight() > 18; ++i)
            {
                auto row = r.removeFromTop(20);
                g.setColour((i % 2 == 0) ? juce::Colours::white.withAlpha(0.06f)
                                         : juce::Colours::transparentBlack);
                g.fillRoundedRectangle(row.toFloat(), 4.0f);
                g.setColour(theme::Colours::text().withAlpha(0.74f));
                g.drawText(trackNames[i], row.reduced(6, 1), juce::Justification::centredLeft, true);
            }
        }

    private:
        juce::StringArray trackNames;
    };
}
