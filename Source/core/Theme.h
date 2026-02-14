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

        struct Typography
        {
            static juce::Font heading(float scale = 1.0f) { return juce::Font(juce::FontOptions(16.0f * scale, juce::Font::bold)); }
            static juce::Font label(float scale = 1.0f)   { return juce::Font(juce::FontOptions(13.0f * scale, juce::Font::plain)); }
            static juce::Font mono(float scale = 1.0f)    { return juce::Font(juce::FontOptions(12.0f * scale, juce::Font::plain)); }
        };

        struct UiScale
        {
            static float fromWidth(int width)
            {
                return juce::jlimit(0.85f, 1.25f, static_cast<float>(width) / 1600.0f);
            }

            static float fromDesktopScale()
            {
                return juce::Desktop::getInstance().getDisplays().getMainDisplay().scale;
            }
        };

        class ModernLookAndFeel : public juce::LookAndFeel_V4
        {
        public:
            ModernLookAndFeel()
            {
                setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(66, 74, 88));
                setColour(juce::TextButton::buttonOnColourId, Colours::accent().withSaturation(0.9f));
                setColour(juce::TextButton::textColourOffId, Colours::text().withAlpha(0.92f));
                setColour(juce::TextButton::textColourOnId, juce::Colours::black.withAlpha(0.86f));

                setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromRGB(34, 40, 48));
                setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
                setColour(juce::ComboBox::textColourId, Colours::text().withAlpha(0.95f));
                setColour(juce::PopupMenu::backgroundColourId, juce::Colour::fromRGB(28, 34, 42));
                setColour(juce::PopupMenu::highlightedBackgroundColourId, Colours::accent().withAlpha(0.85f));
                setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour::fromRGB(20, 22, 26));

                setColour(juce::Slider::trackColourId, Colours::accent().withAlpha(0.82f));
                setColour(juce::Slider::thumbColourId, juce::Colour::fromRGB(244, 247, 255));
                setColour(juce::Slider::backgroundColourId, juce::Colour::fromRGB(48, 56, 68));
            }

            void drawButtonBackground(juce::Graphics& g,
                                      juce::Button& button,
                                      const juce::Colour& backgroundColour,
                                      bool isMouseOverButton,
                                      bool isButtonDown) override
            {
                auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
                auto base = backgroundColour;
                if (button.getToggleState())
                    base = base.brighter(0.08f);
                if (isButtonDown)
                    base = base.darker(0.16f);
                else if (isMouseOverButton)
                    base = base.brighter(0.11f);

                g.setColour(base);
                g.fillRoundedRectangle(bounds, 8.0f);
                g.setColour(juce::Colours::white.withAlpha(isMouseOverButton ? 0.24f : 0.14f));
                g.drawRoundedRectangle(bounds, 8.0f, 1.0f);
            }

            void drawComboBox(juce::Graphics& g,
                              int width,
                              int height,
                              bool,
                              int,
                              int,
                              int,
                              int,
                              juce::ComboBox& box) override
            {
                auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
                g.setColour(findColour(juce::ComboBox::backgroundColourId));
                g.fillRoundedRectangle(bounds, 7.0f);
                g.setColour(juce::Colours::white.withAlpha(box.hasKeyboardFocus(true) ? 0.28f : 0.12f));
                g.drawRoundedRectangle(bounds, 7.0f, 1.0f);

                juce::Path arrow;
                const auto arrowArea = juce::Rectangle<float>(static_cast<float>(width - 22), 0.0f, 16.0f, static_cast<float>(height));
                const auto centre = arrowArea.getCentre();
                arrow.startNewSubPath(centre.x - 4.0f, centre.y - 2.0f);
                arrow.lineTo(centre.x, centre.y + 2.5f);
                arrow.lineTo(centre.x + 4.0f, centre.y - 2.0f);
                g.setColour(Colours::text().withAlpha(0.75f));
                g.strokePath(arrow, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            void drawLinearSlider(juce::Graphics& g,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  float sliderPos,
                                  float,
                                  float,
                                  const juce::Slider::SliderStyle,
                                  juce::Slider& slider) override
            {
                const auto midY = static_cast<float>(y + height / 2);
                const auto startX = static_cast<float>(x + 2);
                const auto endX = static_cast<float>(x + width - 2);
                const auto posX = juce::jlimit(startX, endX, sliderPos);

                g.setColour(slider.findColour(juce::Slider::backgroundColourId));
                g.fillRoundedRectangle(startX, midY - 2.0f, endX - startX, 4.0f, 2.0f);
                g.setColour(slider.findColour(juce::Slider::trackColourId));
                g.fillRoundedRectangle(startX, midY - 2.0f, posX - startX, 4.0f, 2.0f);

                g.setColour(slider.findColour(juce::Slider::thumbColourId));
                g.fillEllipse(posX - 5.0f, midY - 5.0f, 10.0f, 10.0f);
                g.setColour(juce::Colours::black.withAlpha(0.2f));
                g.drawEllipse(posX - 5.0f, midY - 5.0f, 10.0f, 10.0f, 1.0f);
            }
        };

        class ThemeManager
        {
        public:
            static ThemeManager& instance()
            {
                static ThemeManager manager;
                return manager;
            }

            ModernLookAndFeel& lookAndFeel() noexcept { return lookAndFeelImpl; }

            float uiScaleFor(int width) const noexcept
            {
                return juce::jlimit(0.85f, 1.35f,
                                    UiScale::fromWidth(width)
                                        * juce::jlimit(1.0f, 1.5f, UiScale::fromDesktopScale()));
            }

        private:
            ModernLookAndFeel lookAndFeelImpl;
        };
    }
}
