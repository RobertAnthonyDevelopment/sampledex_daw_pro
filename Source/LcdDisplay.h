#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include "TransportEngine.h"
#include "Theme.h"

namespace sampledex
{
    class LcdDisplay : public juce::Component,
                       private juce::Timer
    {
    public:
        enum class PositionMode
        {
            Musical = 1,
            Timecode = 2,
            Samples = 3
        };

        struct DashboardStatus
        {
            double gridStepBeats = 0.25;
            int swingPercent = 50;
            bool tripletGrid = false;
            bool punchEnabled = false;
            bool recordArmed = false;
            bool monitorSafeEnabled = true;
            bool monitorInputActive = false;
            bool diskSpaceLow = false;
            bool tempoMapped = false;
            bool hasPreviousTempoEvent = false;
            bool hasNextTempoEvent = false;
            double nextTempoEventBeat = -1.0;
            double nextTempoEventBpm = 0.0;
            double cpuPercent = 0.0;
            int guardDropCount = 0;
            juce::String syncSource = "INT";
        };

        std::function<void(double)> onRequestSetTempoBpm;
        std::function<void(int, int)> onRequestSetTimeSignature;
        std::function<void(double)> onRequestJumpToBeat;
        std::function<void(int64_t)> onRequestJumpToSample;
        std::function<void()> onRequestJumpToPreviousTempoEvent;
        std::function<void()> onRequestJumpToNextTempoEvent;

        LcdDisplay(TransportEngine& transportEngine, juce::AudioDeviceManager& audioDeviceManager)
            : transport(transportEngine), deviceManager(audioDeviceManager)
        {
            modeSelector.addItem("Bars|Beats", static_cast<int>(PositionMode::Musical));
            modeSelector.addItem("Timecode", static_cast<int>(PositionMode::Timecode));
            modeSelector.addItem("Samples", static_cast<int>(PositionMode::Samples));
            modeSelector.setSelectedId(static_cast<int>(PositionMode::Musical), juce::dontSendNotification);
            modeSelector.onChange = [this]
            {
                updateCachedDisplayData();
                repaint();
            };
            modeSelector.setTooltip("Main LCD mode");
            addAndMakeVisible(modeSelector);

            setWantsKeyboardFocus(true);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            startTimerHz(20);
        }

        void setStatusProvider(std::function<DashboardStatus()> provider)
        {
            statusProvider = std::move(provider);
            updateCachedDisplayData();
            repaint();
        }

        void setPositionMode(PositionMode mode)
        {
            modeSelector.setSelectedId(static_cast<int>(mode), juce::dontSendNotification);
            updateCachedDisplayData();
            repaint();
        }

        PositionMode getPositionMode() const
        {
            const int selectedId = modeSelector.getSelectedId();
            switch (selectedId)
            {
                case static_cast<int>(PositionMode::Timecode): return PositionMode::Timecode;
                case static_cast<int>(PositionMode::Samples): return PositionMode::Samples;
                default: return PositionMode::Musical;
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(4);

            auto top = area.removeFromTop(27);
            transportStateBounds = top.removeFromLeft(92).reduced(2);
            auto rhs = top.removeFromRight(112);
            modeSelector.setBounds(rhs.reduced(2));
            meterBounds = top.removeFromRight(72).reduced(2);
            auto tempoArea = top.removeFromRight(166).reduced(2);
            tempoNextMarkerBounds = tempoArea.removeFromRight(18);
            tempoArea.removeFromRight(2);
            tempoPrevMarkerBounds = tempoArea.removeFromRight(18);
            tempoArea.removeFromRight(2);
            tempoBounds = tempoArea;
            primaryBounds = top.reduced(2);

            secondaryBounds = area.removeFromTop(18).reduced(2);
            area.removeFromTop(1);

            auto bottom = area.removeFromTop(16);
            const int sectionW = juce::jmax(72, bottom.getWidth() / 4);
            gridBounds = bottom.removeFromLeft(sectionW).reduced(2, 0);
            engineBounds = bottom.removeFromLeft(sectionW + 36).reduced(2, 0);
            syncBounds = bottom.removeFromLeft(68).reduced(2, 0);
            safetyBounds = bottom.reduced(2, 0);
        }

        void paint(juce::Graphics& g) override
        {
            const auto bg = theme::Colours::darker().brighter(0.03f);
            g.setGradientFill(juce::ColourGradient(bg,
                                                   0.0f,
                                                   0.0f,
                                                   bg.brighter(0.05f),
                                                   0.0f,
                                                   static_cast<float>(getHeight()),
                                                   false));
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
            g.setColour(theme::Colours::accent().withAlpha(0.35f));
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);

            paintSegment(g, transportStateBounds, transportStateText, transportStateColour, juce::Colours::black, 12.5f, juce::Font::bold);
            paintSegment(g, primaryBounds, primaryReadoutText, juce::Colour::fromRGB(27, 40, 30), juce::Colour::fromRGB(178, 248, 192), 16.0f, juce::Font::bold);
            paintSegment(g, secondaryBounds, secondaryReadoutText, juce::Colour::fromRGB(24, 30, 36), juce::Colours::white.withAlpha(0.80f), 11.5f, juce::Font::plain);
            paintSegment(g, tempoBounds, tempoText, juce::Colour::fromRGB(28, 36, 42), juce::Colour::fromRGB(172, 228, 255), 11.2f, juce::Font::bold);
            paintTempoJumpButton(g, tempoPrevMarkerBounds, "<", cachedStatus.hasPreviousTempoEvent);
            paintTempoJumpButton(g, tempoNextMarkerBounds, ">", cachedStatus.hasNextTempoEvent);
            paintSegment(g, meterBounds, meterText, juce::Colour::fromRGB(28, 36, 42), juce::Colour::fromRGB(255, 210, 134), 11.2f, juce::Font::bold);
            paintSegment(g, gridBounds, gridText, juce::Colour::fromRGB(23, 27, 34), juce::Colours::white.withAlpha(0.76f), 10.0f, juce::Font::plain);
            paintSegment(g, engineBounds, engineText, juce::Colour::fromRGB(23, 27, 34), juce::Colours::white.withAlpha(0.76f), 10.0f, juce::Font::plain);
            paintSegment(g, syncBounds, syncText, juce::Colour::fromRGB(23, 27, 34), juce::Colours::white.withAlpha(0.82f), 10.0f, juce::Font::bold);

            const auto warningColour = warningActive
                ? juce::Colour::fromRGB(214, 71, 71)
                : juce::Colour::fromRGB(52, 72, 58);
            const auto warningTextColour = warningActive
                ? juce::Colours::white
                : juce::Colours::white.withAlpha(0.70f);
            paintSegment(g, safetyBounds, warningText, warningColour, warningTextColour, 10.0f, juce::Font::bold);

            if (xrunFlashCounter > 0)
            {
                auto flashBounds = getLocalBounds().toFloat().reduced(1.5f);
                g.setColour(juce::Colour::fromRGB(238, 86, 78).withAlpha(0.55f));
                g.drawRoundedRectangle(flashBounds, 4.0f, 2.0f);
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            grabKeyboardFocus();

            if (e.mods.isPopupMenu())
                return;

            if (!primaryBounds.contains(e.getPosition()))
                return;

            pendingPrimaryClick = true;
            primaryDragActive = false;
            primaryDragStartPos = e.getPosition();
            primaryDragStartBeat = cachedBeat;
            primaryDragStartSample = cachedSample;
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (!pendingPrimaryClick && !primaryDragActive)
                return;

            if (!primaryDragActive)
            {
                const auto dragDistance = e.getDistanceFromDragStart();
                if (dragDistance < 3)
                    return;
                primaryDragActive = true;
                pendingPrimaryClick = false;
            }

            const int deltaPixels = primaryDragStartPos.y - e.getPosition().y;
            if (deltaPixels == 0)
                return;

            applyDragScrubDelta(deltaPixels, e.mods);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                showContextMenu(e);
                pendingPrimaryClick = false;
                primaryDragActive = false;
                return;
            }

            const bool wasDragging = primaryDragActive;
            primaryDragActive = false;
            bool handledClick = false;

            if (pendingPrimaryClick && !wasDragging)
            {
                if (primaryBounds.contains(e.getPosition()))
                {
                    showPrimaryEditPrompt();
                    handledClick = true;
                }
                else if (tempoPrevMarkerBounds.contains(e.getPosition()))
                {
                    if (cachedStatus.hasPreviousTempoEvent && onRequestJumpToPreviousTempoEvent)
                        onRequestJumpToPreviousTempoEvent();
                    handledClick = true;
                }
                else if (tempoNextMarkerBounds.contains(e.getPosition()))
                {
                    if (cachedStatus.hasNextTempoEvent && onRequestJumpToNextTempoEvent)
                        onRequestJumpToNextTempoEvent();
                    handledClick = true;
                }
                else if (tempoBounds.contains(e.getPosition()))
                {
                    showTempoEditPrompt();
                    handledClick = true;
                }
                else if (meterBounds.contains(e.getPosition()))
                {
                    showMeterEditPrompt();
                    handledClick = true;
                }
            }

            if (!wasDragging && !handledClick)
            {
                if (tempoPrevMarkerBounds.contains(e.getPosition()))
                {
                    if (cachedStatus.hasPreviousTempoEvent && onRequestJumpToPreviousTempoEvent)
                        onRequestJumpToPreviousTempoEvent();
                }
                else if (tempoNextMarkerBounds.contains(e.getPosition()))
                {
                    if (cachedStatus.hasNextTempoEvent && onRequestJumpToNextTempoEvent)
                        onRequestJumpToNextTempoEvent();
                }
                else if (tempoBounds.contains(e.getPosition()))
                    showTempoEditPrompt();
                else if (meterBounds.contains(e.getPosition()))
                    showMeterEditPrompt();
            }

            pendingPrimaryClick = false;
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const bool clickable = primaryBounds.contains(e.getPosition())
                                || tempoPrevMarkerBounds.contains(e.getPosition())
                                || tempoNextMarkerBounds.contains(e.getPosition())
                                || tempoBounds.contains(e.getPosition())
                                || meterBounds.contains(e.getPosition());
            setMouseCursor(clickable ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            setMouseCursor(juce::MouseCursor::NormalCursor);
            pendingPrimaryClick = false;
            primaryDragActive = false;
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress('1', juce::ModifierKeys::commandModifier, 0))
            {
                setPositionMode(PositionMode::Musical);
                return true;
            }
            if (key == juce::KeyPress('2', juce::ModifierKeys::commandModifier, 0))
            {
                setPositionMode(PositionMode::Timecode);
                return true;
            }
            if (key == juce::KeyPress('3', juce::ModifierKeys::commandModifier, 0))
            {
                setPositionMode(PositionMode::Samples);
                return true;
            }
            return false;
        }

    private:
        static constexpr int musicalTicksPerBeat = 960;
        static constexpr double epsilon = 1.0e-6;

        void timerCallback() override
        {
            if (xrunFlashCounter > 0)
                --xrunFlashCounter;

            updateCachedDisplayData();
            repaint();
        }

        static juce::String formatWithCommas(int64_t value)
        {
            juce::String raw = juce::String(value);
            const bool negative = raw.startsWithChar('-');
            if (negative)
                raw = raw.substring(1);

            juce::String grouped;
            for (int i = raw.length() - 1, digit = 0; i >= 0; --i, ++digit)
            {
                if (digit > 0 && (digit % 3) == 0)
                    grouped = "," + grouped;
                grouped = raw.substring(i, i + 1) + grouped;
            }
            return negative ? ("-" + grouped) : grouped;
        }

        static juce::String gridStepToString(double beats)
        {
            if (std::abs(beats - 4.0) < 0.001) return "1/1";
            if (std::abs(beats - 2.0) < 0.001) return "1/2";
            if (std::abs(beats - 1.0) < 0.001) return "1/4";
            if (std::abs(beats - 0.5) < 0.001) return "1/8";
            if (std::abs(beats - 0.25) < 0.001) return "1/16";
            if (std::abs(beats - 0.125) < 0.001) return "1/32";
            if (std::abs(beats - (1.0 / 3.0)) < 0.001) return "1/8T";
            if (std::abs(beats - (1.0 / 6.0)) < 0.001) return "1/16T";
            return juce::String::formatted("%.3f", beats) + "b";
        }

        static juce::String formatMusicalBeat(double beat, int numerator, int denominator)
        {
            const int num = juce::jmax(1, numerator);
            const int den = juce::jmax(1, denominator);
            const double beatUnitLength = 4.0 / static_cast<double>(den);
            const double beatsPerBar = static_cast<double>(num) * beatUnitLength;

            const double clampedBeat = juce::jmax(0.0, beat);
            int bar = static_cast<int>(std::floor(clampedBeat / beatsPerBar)) + 1;
            double beatInsideBar = clampedBeat - (static_cast<double>(bar - 1) * beatsPerBar);
            beatInsideBar = juce::jmax(0.0, beatInsideBar);

            int beatNumber = static_cast<int>(std::floor(beatInsideBar / beatUnitLength)) + 1;
            beatNumber = juce::jlimit(1, num, beatNumber);

            double beatFraction = beatInsideBar - (static_cast<double>(beatNumber - 1) * beatUnitLength);
            int tick = juce::roundToInt((beatFraction / beatUnitLength) * static_cast<double>(musicalTicksPerBeat));

            if (tick >= musicalTicksPerBeat)
            {
                tick -= musicalTicksPerBeat;
                ++beatNumber;
                if (beatNumber > num)
                {
                    beatNumber = 1;
                    ++bar;
                }
            }

            return juce::String(bar) + " | " + juce::String(beatNumber) + " | " + juce::String(tick);
        }

        static juce::String formatTimecodeFromSamples(int64_t samplePosition, double sampleRate)
        {
            const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
            const double totalSeconds = static_cast<double>(juce::jmax<int64_t>(0, samplePosition)) / sr;
            const int hours = static_cast<int>(std::floor(totalSeconds / 3600.0));
            const int minutes = static_cast<int>(std::floor(std::fmod(totalSeconds, 3600.0) / 60.0));
            const int seconds = static_cast<int>(std::floor(std::fmod(totalSeconds, 60.0)));
            int millis = juce::roundToInt((totalSeconds - std::floor(totalSeconds)) * 1000.0);
            if (millis >= 1000)
                millis = 999;
            return juce::String::formatted("%02d:%02d:%02d.%03d", hours, minutes, seconds, millis);
        }

        static juce::String formatPrimaryReadout(PositionMode mode,
                                                 double beat,
                                                 int64_t samplePosition,
                                                 double sampleRate,
                                                 int numerator,
                                                 int denominator)
        {
            switch (mode)
            {
                case PositionMode::Timecode:
                    return formatTimecodeFromSamples(samplePosition, sampleRate);
                case PositionMode::Samples:
                    return formatWithCommas(samplePosition) + " smp";
                case PositionMode::Musical:
                default:
                    return formatMusicalBeat(beat, numerator, denominator);
            }
        }

        static bool parseTimeSignatureText(const juce::String& text, int& outNum, int& outDen)
        {
            auto cleaned = text.trim().removeCharacters(" ");
            juce::StringArray parts;
            parts.addTokens(cleaned, "/", "");
            parts.removeEmptyStrings();
            if (parts.size() != 2)
                return false;

            const int n = parts[0].getIntValue();
            const int d = parts[1].getIntValue();
            if (n <= 0 || d <= 0)
                return false;

            outNum = juce::jlimit(1, 32, n);
            outDen = juce::jlimit(1, 32, d);
            return true;
        }

        static bool parseMusicalPositionText(const juce::String& text,
                                             int numerator,
                                             int denominator,
                                             double& outBeat)
        {
            const int num = juce::jmax(1, numerator);
            const int den = juce::jmax(1, denominator);
            const double beatUnitLength = 4.0 / static_cast<double>(den);
            const double beatsPerBar = static_cast<double>(num) * beatUnitLength;

            juce::StringArray parts;
            parts.addTokens(text.removeCharacters(" "), "|:", "");
            parts.removeEmptyStrings();
            if (parts.size() < 2)
                return false;

            const int bar = juce::jmax(1, parts[0].getIntValue());
            const int beatInBar = juce::jlimit(1, num, parts[1].getIntValue());
            int tick = 0;
            if (parts.size() >= 3)
                tick = juce::jlimit(0, musicalTicksPerBeat - 1, parts[2].getIntValue());

            outBeat = static_cast<double>(bar - 1) * beatsPerBar
                    + static_cast<double>(beatInBar - 1) * beatUnitLength
                    + (static_cast<double>(tick) / static_cast<double>(musicalTicksPerBeat)) * beatUnitLength;
            return true;
        }

        static bool parseTimecodeText(const juce::String& text, double& outSeconds)
        {
            juce::String clean = text.trim().removeCharacters(" ");
            clean = clean.replaceCharacter('.', ':');

            juce::StringArray parts;
            parts.addTokens(clean, ":", "");
            parts.removeEmptyStrings();
            if (parts.isEmpty() || parts.size() > 4)
                return false;

            int h = 0;
            int m = 0;
            int s = 0;
            int ms = 0;

            if (parts.size() == 4)
            {
                h = juce::jmax(0, parts[0].getIntValue());
                m = juce::jmax(0, parts[1].getIntValue());
                s = juce::jmax(0, parts[2].getIntValue());
                ms = juce::jmax(0, parts[3].getIntValue());
            }
            else if (parts.size() == 3)
            {
                m = juce::jmax(0, parts[0].getIntValue());
                s = juce::jmax(0, parts[1].getIntValue());
                ms = juce::jmax(0, parts[2].getIntValue());
            }
            else if (parts.size() == 2)
            {
                s = juce::jmax(0, parts[0].getIntValue());
                ms = juce::jmax(0, parts[1].getIntValue());
            }
            else
            {
                s = juce::jmax(0, parts[0].getIntValue());
            }

            ms = juce::jlimit(0, 999, ms);
            outSeconds = static_cast<double>(h) * 3600.0
                       + static_cast<double>(m) * 60.0
                       + static_cast<double>(s)
                       + (static_cast<double>(ms) / 1000.0);
            return true;
        }

        static bool parseSamplesText(const juce::String& text, int64_t& outSamples)
        {
            juce::String clean = text.removeCharacters(", _");
            clean = clean.trim();
            if (clean.isEmpty())
                return false;
            const int64 parsed = clean.getLargeIntValue();
            if (parsed < 0)
                return false;
            outSamples = parsed;
            return true;
        }

        void updateCachedDisplayData()
        {
            cachedStatus = statusProvider ? statusProvider() : DashboardStatus{};
            cachedBeat = transport.getCurrentBeat();
            cachedSample = transport.getCurrentSample();
            cachedTempoBpm = transport.getTempo();

            const auto position = transport.getCurrentPositionInfo();
            cachedNumerator = juce::jmax(1, position.timeSigNumerator);
            cachedDenominator = juce::jmax(1, position.timeSigDenominator);

            const auto setup = deviceManager.getAudioDeviceSetup();
            cachedSampleRate = setup.sampleRate > 0.0 ? setup.sampleRate : 44100.0;
            cachedBufferSize = juce::jmax(0, setup.bufferSize);

            int inputLatency = 0;
            int outputLatency = 0;
            if (auto* device = deviceManager.getCurrentAudioDevice())
            {
                inputLatency = juce::jmax(0, device->getInputLatencyInSamples());
                outputLatency = juce::jmax(0, device->getOutputLatencyInSamples());
                if (cachedBufferSize <= 0)
                    cachedBufferSize = juce::jmax(0, device->getCurrentBufferSizeSamples());
                if (cachedSampleRate <= 0.0)
                    cachedSampleRate = juce::jmax(1.0, device->getCurrentSampleRate());
            }

            const int roundTripSamples = inputLatency + outputLatency + cachedBufferSize;
            const double roundTripMs = cachedSampleRate > 0.0
                ? (1000.0 * static_cast<double>(roundTripSamples) / cachedSampleRate)
                : 0.0;

            const PositionMode primaryMode = getPositionMode();
            const PositionMode secondaryMode = (primaryMode == PositionMode::Musical)
                ? PositionMode::Timecode
                : PositionMode::Musical;

            primaryReadoutText = formatPrimaryReadout(primaryMode,
                                                      cachedBeat,
                                                      cachedSample,
                                                      cachedSampleRate,
                                                      cachedNumerator,
                                                      cachedDenominator);
            secondaryReadoutText = formatPrimaryReadout(secondaryMode,
                                                        cachedBeat,
                                                        cachedSample,
                                                        cachedSampleRate,
                                                        cachedNumerator,
                                                        cachedDenominator);

            const bool isRecording = transport.recording();
            const bool isPlaying = transport.playing();
            const bool isLooping = transport.isLooping();
            transportStateText = isRecording ? "REC" : (isPlaying ? "PLAY" : "STOP");
            if (isLooping)
                transportStateText << " LOOP";
            if (cachedStatus.punchEnabled)
                transportStateText << " PUNCH";
            if (cachedStatus.recordArmed && !isRecording)
                transportStateText << " ARM";

            transportStateColour = isRecording ? juce::Colour::fromRGB(216, 62, 62)
                                : (isPlaying ? juce::Colour::fromRGB(62, 188, 101)
                                             : juce::Colour::fromRGB(89, 106, 114));

            tempoText = juce::String(cachedTempoBpm, 1) + " BPM";
            if (cachedStatus.tempoMapped)
            {
                if (cachedStatus.nextTempoEventBeat >= 0.0 && cachedStatus.nextTempoEventBpm > 0.0)
                {
                    tempoText = juce::String(cachedTempoBpm, 1)
                             + "->"
                             + juce::String(cachedStatus.nextTempoEventBpm, 1)
                             + " @" + juce::String(cachedStatus.nextTempoEventBeat, 1);
                }
                else if (cachedStatus.nextTempoEventBeat >= 0.0)
                {
                    tempoText << " @" << juce::String(cachedStatus.nextTempoEventBeat, 1);
                }
                else
                    tempoText << " mapped";
            }

            meterText = juce::String(cachedNumerator) + "/" + juce::String(cachedDenominator);
            gridText = "Grid " + gridStepToString(cachedStatus.gridStepBeats)
                     + " Sw " + juce::String(cachedStatus.swingPercent) + "%"
                     + (cachedStatus.tripletGrid ? " T" : "");

            engineText = "SR " + juce::String(cachedSampleRate, 0)
                       + " Buf " + juce::String(cachedBufferSize)
                       + " RTL " + juce::String(roundTripMs, 1) + "ms"
                       + " CPU " + juce::String(cachedStatus.cpuPercent, 1) + "%";

            if (cachedStatus.guardDropCount > lastGuardDropCount)
                xrunFlashCounter = 24;
            lastGuardDropCount = cachedStatus.guardDropCount;

            if (xrunFlashCounter > 0)
                engineText << " XRUN";

            syncText = cachedStatus.syncSource.isNotEmpty() ? cachedStatus.syncSource : juce::String("INT");
            warningText = "SAFE";
            warningActive = false;

            if (cachedStatus.diskSpaceLow)
            {
                warningText = "DISK LOW";
                warningActive = true;
            }
            else if (cachedStatus.recordArmed && !cachedStatus.monitorInputActive)
            {
                warningText = "MON OFF";
                warningActive = true;
            }
            else if (!cachedStatus.monitorSafeEnabled)
            {
                warningText = "SAFE OFF";
                warningActive = true;
            }
            else
            {
                warningText = "MON SAFE";
            }
        }

        void paintSegment(juce::Graphics& g,
                          juce::Rectangle<int> area,
                          const juce::String& text,
                          juce::Colour background,
                          juce::Colour textColour,
                          float fontSize,
                          int fontStyleFlags) const
        {
            if (area.isEmpty())
                return;

            const auto r = area.toFloat();
            g.setColour(background);
            g.fillRoundedRectangle(r, 3.0f);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawRoundedRectangle(r, 3.0f, 0.9f);
            g.setColour(textColour);
            g.setFont(juce::Font(juce::FontOptions(fontSize, static_cast<juce::Font::FontStyleFlags>(fontStyleFlags))));
            g.drawFittedText(text, area.reduced(4, 1), juce::Justification::centredLeft, 1);
        }

        static void paintTempoJumpButton(juce::Graphics& g,
                                         juce::Rectangle<int> area,
                                         const juce::String& glyph,
                                         bool enabled)
        {
            if (area.isEmpty())
                return;

            const auto background = enabled ? juce::Colour::fromRGB(36, 45, 52)
                                            : juce::Colour::fromRGB(28, 34, 40);
            const auto textColour = enabled ? juce::Colour::fromRGB(198, 234, 255)
                                            : juce::Colours::white.withAlpha(0.30f);

            g.setColour(background);
            g.fillRoundedRectangle(area.toFloat(), 3.0f);
            g.setColour(juce::Colours::white.withAlpha(enabled ? 0.22f : 0.10f));
            g.drawRoundedRectangle(area.toFloat(), 3.0f, 0.9f);
            g.setColour(textColour);
            g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
            g.drawFittedText(glyph, area, juce::Justification::centred, 1);
        }

        void applyDragScrubDelta(int deltaPixels, juce::ModifierKeys mods)
        {
            const auto mode = getPositionMode();
            if (mode == PositionMode::Musical)
            {
                const double step = mods.isShiftDown() ? (1.0 / 32.0)
                                   : (mods.isCommandDown() || mods.isCtrlDown()) ? 1.0
                                                                                  : 0.25;
                const double targetBeat = juce::jmax(0.0, primaryDragStartBeat + (static_cast<double>(deltaPixels) * step));
                if (onRequestJumpToBeat)
                    onRequestJumpToBeat(targetBeat);
                return;
            }

            if (mode == PositionMode::Timecode)
            {
                const double stepSeconds = mods.isShiftDown() ? 0.01
                                        : (mods.isCommandDown() || mods.isCtrlDown()) ? 1.0
                                                                                       : 0.10;
                const double targetSeconds = juce::jmax(0.0, (static_cast<double>(primaryDragStartSample) / cachedSampleRate)
                                                              + (static_cast<double>(deltaPixels) * stepSeconds));
                const int64_t targetSample = static_cast<int64_t>(std::llround(targetSeconds * cachedSampleRate));
                if (onRequestJumpToSample)
                    onRequestJumpToSample(targetSample);
                else if (onRequestJumpToBeat)
                    onRequestJumpToBeat(targetSeconds * (cachedTempoBpm / 60.0));
                return;
            }

            const int64_t stepSamples = mods.isShiftDown() ? 16
                                       : (mods.isCommandDown() || mods.isCtrlDown()) ? 2048
                                                                                      : 256;
            const int64_t targetSample = juce::jmax<int64_t>(0,
                                                             primaryDragStartSample
                                                                 + (static_cast<int64_t>(deltaPixels) * stepSamples));
            if (onRequestJumpToSample)
                onRequestJumpToSample(targetSample);
        }

        void showPrimaryEditPrompt()
        {
            const auto mode = getPositionMode();
            const juce::String title = "Set Position";
            const juce::String prompt = (mode == PositionMode::Musical)
                ? "Enter Bar|Beat|Tick (example: 12|3|240)"
                : (mode == PositionMode::Timecode)
                    ? "Enter Timecode (HH:MM:SS.mmm)"
                    : "Enter Sample Position";

            auto* window = new juce::AlertWindow(title, prompt, juce::AlertWindow::NoIcon);
            window->addTextEditor("value", primaryReadoutText, "Position");
            window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
            window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            juce::Component::SafePointer<LcdDisplay> safeThis(this);
            window->enterModalState(true,
                                    juce::ModalCallbackFunction::create(
                                        [safeThis, window, mode](int result)
                                        {
                                            std::unique_ptr<juce::AlertWindow> owner(window);
                                            if (safeThis == nullptr || result != 1)
                                                return;

                                            const auto text = window->getTextEditorContents("value");
                                            if (mode == PositionMode::Musical)
                                            {
                                                double beat = 0.0;
                                                if (parseMusicalPositionText(text,
                                                                             safeThis->cachedNumerator,
                                                                             safeThis->cachedDenominator,
                                                                             beat))
                                                {
                                                    if (safeThis->onRequestJumpToBeat)
                                                        safeThis->onRequestJumpToBeat(beat);
                                                }
                                            }
                                            else if (mode == PositionMode::Timecode)
                                            {
                                                double seconds = 0.0;
                                                if (parseTimecodeText(text, seconds))
                                                {
                                                    const int64_t sample = static_cast<int64_t>(std::llround(seconds * safeThis->cachedSampleRate));
                                                    if (safeThis->onRequestJumpToSample)
                                                        safeThis->onRequestJumpToSample(sample);
                                                    else if (safeThis->onRequestJumpToBeat)
                                                        safeThis->onRequestJumpToBeat(seconds * (safeThis->cachedTempoBpm / 60.0));
                                                }
                                            }
                                            else
                                            {
                                                int64_t sample = 0;
                                                if (parseSamplesText(text, sample) && safeThis->onRequestJumpToSample)
                                                    safeThis->onRequestJumpToSample(sample);
                                            }
                                        }));
        }

        void showTempoEditPrompt()
        {
            auto* window = new juce::AlertWindow("Set Tempo", "Enter BPM value", juce::AlertWindow::NoIcon);
            window->addTextEditor("value", juce::String(cachedTempoBpm, 2), "BPM");
            window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
            window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            juce::Component::SafePointer<LcdDisplay> safeThis(this);
            window->enterModalState(true,
                                    juce::ModalCallbackFunction::create(
                                        [safeThis, window](int result)
                                        {
                                            std::unique_ptr<juce::AlertWindow> owner(window);
                                            if (safeThis == nullptr || result != 1)
                                                return;

                                            const double newBpm = juce::jmax(1.0, window->getTextEditorContents("value").getDoubleValue());
                                            if (safeThis->onRequestSetTempoBpm)
                                                safeThis->onRequestSetTempoBpm(newBpm);
                                        }));
        }

        void showMeterEditPrompt()
        {
            auto* window = new juce::AlertWindow("Set Time Signature",
                                                 "Enter numerator/denominator",
                                                 juce::AlertWindow::NoIcon);
            window->addTextEditor("value",
                                  juce::String(cachedNumerator) + "/" + juce::String(cachedDenominator),
                                  "Meter");
            window->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
            window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            juce::Component::SafePointer<LcdDisplay> safeThis(this);
            window->enterModalState(true,
                                    juce::ModalCallbackFunction::create(
                                        [safeThis, window](int result)
                                        {
                                            std::unique_ptr<juce::AlertWindow> owner(window);
                                            if (safeThis == nullptr || result != 1)
                                                return;

                                            int num = 4;
                                            int den = 4;
                                            if (parseTimeSignatureText(window->getTextEditorContents("value"), num, den))
                                            {
                                                if (safeThis->onRequestSetTimeSignature)
                                                    safeThis->onRequestSetTimeSignature(num, den);
                                            }
                                        }));
        }

        void showContextMenu(const juce::MouseEvent& e)
        {
            juce::PopupMenu menu;
            menu.addSectionHeader("LCD Display");
            menu.addItem(1, "Bars | Beats | Ticks", true, getPositionMode() == PositionMode::Musical);
            menu.addItem(2, "Timecode", true, getPositionMode() == PositionMode::Timecode);
            menu.addItem(3, "Samples", true, getPositionMode() == PositionMode::Samples);
            menu.addSeparator();
            menu.addItem(10, "Edit Position...");
            menu.addItem(11, "Edit Tempo...");
            menu.addItem(12, "Edit Meter...");
            menu.addSeparator();
            menu.addItem(20, "Jump to Previous Tempo Event", cachedStatus.hasPreviousTempoEvent);
            menu.addItem(21, "Jump to Next Tempo Event", cachedStatus.hasNextTempoEvent);

            menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(juce::Rectangle<int>(e.getScreenPosition(), { 1, 1 })),
                               [safe = juce::Component::SafePointer<LcdDisplay>(this)](int selectedId)
                               {
                                   if (safe == nullptr || selectedId == 0)
                                       return;

                                   if (selectedId == 1)
                                   {
                                       safe->setPositionMode(PositionMode::Musical);
                                       return;
                                   }
                                   if (selectedId == 2)
                                   {
                                       safe->setPositionMode(PositionMode::Timecode);
                                       return;
                                   }
                                   if (selectedId == 3)
                                   {
                                       safe->setPositionMode(PositionMode::Samples);
                                       return;
                                   }
                                   if (selectedId == 10)
                                   {
                                       safe->showPrimaryEditPrompt();
                                       return;
                                   }
                                   if (selectedId == 11)
                                   {
                                       safe->showTempoEditPrompt();
                                       return;
                                   }
                                   if (selectedId == 12)
                                   {
                                       safe->showMeterEditPrompt();
                                       return;
                                   }
                                   if (selectedId == 20)
                                   {
                                       if (safe->onRequestJumpToPreviousTempoEvent)
                                           safe->onRequestJumpToPreviousTempoEvent();
                                       return;
                                   }
                                   if (selectedId == 21)
                                   {
                                       if (safe->onRequestJumpToNextTempoEvent)
                                           safe->onRequestJumpToNextTempoEvent();
                                       return;
                                   }
                               });
        }

        TransportEngine& transport;
        juce::AudioDeviceManager& deviceManager;
        juce::ComboBox modeSelector;
        std::function<DashboardStatus()> statusProvider;

        juce::Rectangle<int> transportStateBounds;
        juce::Rectangle<int> primaryBounds;
        juce::Rectangle<int> secondaryBounds;
        juce::Rectangle<int> tempoBounds;
        juce::Rectangle<int> tempoPrevMarkerBounds;
        juce::Rectangle<int> tempoNextMarkerBounds;
        juce::Rectangle<int> meterBounds;
        juce::Rectangle<int> gridBounds;
        juce::Rectangle<int> engineBounds;
        juce::Rectangle<int> syncBounds;
        juce::Rectangle<int> safetyBounds;

        DashboardStatus cachedStatus;
        double cachedBeat = 0.0;
        int64_t cachedSample = 0;
        double cachedTempoBpm = 120.0;
        double cachedSampleRate = 44100.0;
        int cachedBufferSize = 0;
        int cachedNumerator = 4;
        int cachedDenominator = 4;

        juce::String transportStateText { "STOP" };
        juce::Colour transportStateColour { juce::Colour::fromRGB(89, 106, 114) };
        juce::String primaryReadoutText;
        juce::String secondaryReadoutText;
        juce::String tempoText;
        juce::String meterText { "4/4" };
        juce::String gridText;
        juce::String engineText;
        juce::String syncText { "INT" };
        juce::String warningText { "SAFE" };
        bool warningActive = false;

        bool pendingPrimaryClick = false;
        bool primaryDragActive = false;
        juce::Point<int> primaryDragStartPos;
        double primaryDragStartBeat = 0.0;
        int64_t primaryDragStartSample = 0;
        int lastGuardDropCount = 0;
        int xrunFlashCounter = 0;
    };
}
