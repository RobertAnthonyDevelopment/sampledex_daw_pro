#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include "TimelineModel.h"
#include "Theme.h"

namespace sampledex
{
    class StepSequencerComponent : public juce::Component
    {
    public:
        std::function<void(int, const juce::String&, std::function<void(Clip&)>)> onRequestClipEdit;

        StepSequencerComponent()
        {
            rootSelector.addItemList({"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 1);
            rootSelector.setSelectedItemIndex(rootNote, juce::dontSendNotification);
            rootSelector.onChange = [this]
            {
                rootNote = juce::jlimit(0, 11, rootSelector.getSelectedItemIndex());
                syncClipFromPattern();
                repaint();
            };

            scaleSelector.addItem("Major", 1);
            scaleSelector.addItem("Minor", 2);
            scaleSelector.addItem("Dorian", 3);
            scaleSelector.addItem("Mixolydian", 4);
            scaleSelector.addItem("Pentatonic", 5);
            scaleSelector.setSelectedId(scaleMode + 1, juce::dontSendNotification);
            scaleSelector.onChange = [this]
            {
                scaleMode = juce::jlimit(0, 4, scaleSelector.getSelectedId() - 1);
                syncClipFromPattern();
                repaint();
            };

            stepCountSelector.addItem("8 Steps", 1);
            stepCountSelector.addItem("16 Steps", 2);
            stepCountSelector.addItem("32 Steps", 3);
            stepCountSelector.setSelectedId(2, juce::dontSendNotification);
            stepCountSelector.onChange = [this]
            {
                switch (stepCountSelector.getSelectedId())
                {
                    case 1: numSteps = 8; break;
                    case 2: numSteps = 16; break;
                    case 3: numSteps = 32; break;
                    default: numSteps = 16; break;
                }
                loadPatternFromClip();
                repaint();
            };

            gateSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            gateSlider.setRange(0.1, 1.0, 0.01);
            gateSlider.setValue(gateAmount, juce::dontSendNotification);
            gateSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            gateSlider.onValueChange = [this]
            {
                gateAmount = static_cast<float>(gateSlider.getValue());
                syncClipFromPattern();
                repaint();
            };

            octaveSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            octaveSlider.setRange(1, 7, 1);
            octaveSlider.setValue(baseOctave, juce::dontSendNotification);
            octaveSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            octaveSlider.onValueChange = [this]
            {
                baseOctave = static_cast<int>(octaveSlider.getValue());
                syncClipFromPattern();
                repaint();
            };

            addAndMakeVisible(rootSelector);
            addAndMakeVisible(scaleSelector);
            addAndMakeVisible(stepCountSelector);
            addAndMakeVisible(gateSlider);
            addAndMakeVisible(octaveSlider);
        }

        void setClip(Clip* newClip, int newClipIndex = -1)
        {
            clip = (newClip != nullptr && newClip->type == ClipType::MIDI) ? newClip : nullptr;
            clipIndex = clip != nullptr ? newClipIndex : -1;
            loadPatternFromClip();
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(theme::Colours::darker());

            auto controls = getControlBounds();
            g.setColour(theme::Colours::panel());
            g.fillRoundedRectangle(controls.toFloat(), 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.drawRoundedRectangle(controls.toFloat(), 4.0f, 1.0f);

            g.setColour(juce::Colours::white.withAlpha(0.65f));
            g.setFont(11.0f);
            g.drawText("Gate", controls.getRight() - 232, controls.getY(), 32, controls.getHeight(), juce::Justification::centredLeft);
            g.drawText("Oct", controls.getRight() - 102, controls.getY(), 26, controls.getHeight(), juce::Justification::centredLeft);

            if (clip == nullptr)
            {
                g.setColour(juce::Colours::grey);
                g.setFont(20.0f);
                g.drawText("Select a MIDI Clip for Step Sequencing", getGridBounds(), juce::Justification::centred);
                return;
            }

            auto grid = getGridBounds();
            auto noteLane = grid.removeFromLeft(noteLabelWidth);
            if (grid.isEmpty())
                return;

            const float rowHeight = static_cast<float>(grid.getHeight()) / static_cast<float>(numRows);
            const float stepWidth = static_cast<float>(grid.getWidth()) / static_cast<float>(numSteps);

            for (int row = 0; row < numRows; ++row)
            {
                const int note = getNoteForRow(row);
                const bool isRoot = ((note % 12) + 12) % 12 == rootNote;
                const float y = grid.getY() + (row * rowHeight);

                juce::Colour rowColour = juce::Colour::fromRGB(35, 38, 42);
                if (isRoot)
                    rowColour = rowColour.interpolatedWith(theme::Colours::accent(), 0.2f);

                g.setColour(rowColour);
                g.fillRect(static_cast<float>(noteLane.getX()), y, static_cast<float>(noteLane.getWidth()), rowHeight);
                g.fillRect(static_cast<float>(grid.getX()), y, static_cast<float>(grid.getWidth()), rowHeight);

                g.setColour(juce::Colours::black.withAlpha(0.3f));
                g.drawLine(static_cast<float>(grid.getX()), y, static_cast<float>(grid.getRight()), y);

                g.setColour(juce::Colours::white.withAlpha(0.72f));
                g.setFont(11.0f);
                g.drawText(getNoteName(note), noteLane.getX() + 6, static_cast<int>(y), noteLane.getWidth() - 8, static_cast<int>(rowHeight), juce::Justification::centredLeft);

                for (int step = 0; step < numSteps; ++step)
                {
                    juce::Rectangle<float> cell(
                        grid.getX() + (step * stepWidth) + 1.0f,
                        y + 1.0f,
                        stepWidth - 2.0f,
                        rowHeight - 2.0f
                    );

                    const bool active = pattern[static_cast<size_t>(row)][step];
                    if (active)
                    {
                        g.setColour(theme::Colours::accent().withAlpha(0.85f));
                        g.fillRoundedRectangle(cell, 2.0f);
                    }
                    else
                    {
                        g.setColour(juce::Colours::black.withAlpha(0.2f));
                        g.fillRoundedRectangle(cell, 2.0f);
                    }

                    if ((step % 4) == 0)
                    {
                        g.setColour(juce::Colours::white.withAlpha(0.22f));
                        g.drawLine(grid.getX() + (step * stepWidth), static_cast<float>(grid.getY()), grid.getX() + (step * stepWidth), static_cast<float>(grid.getBottom()));
                    }
                }
            }

            g.setColour(juce::Colours::white.withAlpha(0.3f));
            g.drawRect(noteLane);
            g.drawRect(grid);
        }

        void resized() override
        {
            auto controls = getControlBounds().reduced(4, 2);
            rootSelector.setBounds(controls.removeFromLeft(72));
            scaleSelector.setBounds(controls.removeFromLeft(130));
            stepCountSelector.setBounds(controls.removeFromLeft(96));
            controls.removeFromLeft(8);
            gateSlider.setBounds(controls.removeFromLeft(90));
            controls.removeFromLeft(12);
            octaveSlider.setBounds(controls.removeFromLeft(72));
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (!toggleStepAtPosition(e, true))
                dragActive = false;
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (!dragActive || clip == nullptr)
                return;

            toggleStepAtPosition(e, false);
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            dragActive = false;
            lastTouchedRow = -1;
            lastTouchedStep = -1;
        }

    private:
        static constexpr int numRows = 8;
        static constexpr int controlHeight = 30;
        static constexpr int noteLabelWidth = 52;

        bool toggleStepAtPosition(const juce::MouseEvent& e, bool startDrag)
        {
            if (clip == nullptr)
                return false;

            auto grid = getGridBounds();
            auto lane = grid.removeFromLeft(noteLabelWidth);
            juce::ignoreUnused(lane);

            if (!grid.contains(e.getPosition()))
                return false;

            const float rowHeight = static_cast<float>(grid.getHeight()) / static_cast<float>(numRows);
            const float stepWidth = static_cast<float>(grid.getWidth()) / static_cast<float>(numSteps);

            const int row = juce::jlimit(0, numRows - 1, static_cast<int>((e.position.y - grid.getY()) / rowHeight));
            const int step = juce::jlimit(0, numSteps - 1, static_cast<int>((e.position.x - grid.getX()) / stepWidth));

            if (startDrag)
            {
                if (e.mods.isRightButtonDown())
                    dragSetState = false;
                else
                    dragSetState = !pattern[static_cast<size_t>(row)][step];

                dragActive = true;
            }

            if (!dragActive || (row == lastTouchedRow && step == lastTouchedStep))
                return true;

            pattern[static_cast<size_t>(row)].setBit(step, dragSetState);
            lastTouchedRow = row;
            lastTouchedStep = step;
            syncClipFromPattern();
            repaint();
            return true;
        }

        void clearPattern()
        {
            for (auto& row : pattern)
                row.clear();
        }

        void loadPatternFromClip()
        {
            clearPattern();

            if (clip == nullptr || clip->lengthBeats <= 0.0)
                return;

            const double stepBeatLength = clip->lengthBeats / static_cast<double>(numSteps);
            if (stepBeatLength <= 0.0)
                return;

            for (const auto& ev : clip->events)
            {
                const int step = juce::jlimit(0, numSteps - 1, static_cast<int>(std::floor((ev.startBeat / stepBeatLength) + 0.5)));
                const int row = findClosestRowForNote(ev.noteNumber);
                if (row >= 0)
                    pattern[static_cast<size_t>(row)].setBit(step, true);
            }
        }

        void syncClipFromPattern()
        {
            if (clip == nullptr || clip->lengthBeats <= 0.0)
                return;

            const auto patternSnapshot = pattern;
            const int localNumSteps = numSteps;
            const int localRoot = rootNote;
            const int localScaleMode = scaleMode;
            const int localBaseOctave = baseOctave;
            const float localGateAmount = gateAmount;

            const auto getScaleForMode = [](int mode) -> const std::vector<int>&
            {
                static const std::array<std::vector<int>, 5> scales = {
                    std::vector<int>{ 0, 2, 4, 5, 7, 9, 11 }, // Major
                    std::vector<int>{ 0, 2, 3, 5, 7, 8, 10 }, // Minor
                    std::vector<int>{ 0, 2, 3, 5, 7, 9, 10 }, // Dorian
                    std::vector<int>{ 0, 2, 4, 5, 7, 9, 10 }, // Mixolydian
                    std::vector<int>{ 0, 3, 5, 7, 10 }        // Pentatonic
                };
                return scales[static_cast<size_t>(juce::jlimit(0, 4, mode))];
            };

            const auto scaleIntervals = getScaleForMode(localScaleMode);
            const auto noteForRow = [&](int row)
            {
                const int fromBottom = (numRows - 1) - row;
                const int degree = fromBottom % static_cast<int>(scaleIntervals.size());
                const int octaveOffset = fromBottom / static_cast<int>(scaleIntervals.size());
                const int note = (localBaseOctave * 12)
                               + localRoot
                               + scaleIntervals[static_cast<size_t>(degree)]
                               + (12 * octaveOffset);
                return juce::jlimit(0, 127, note);
            };

            performClipEdit(dragActive ? "Paint Step Pattern" : "Update Step Pattern",
                            [patternSnapshot, localNumSteps, localGateAmount, noteForRow](Clip& target)
                            {
                                if (target.lengthBeats <= 0.0)
                                    return;

                                const double stepBeatLength = target.lengthBeats / static_cast<double>(juce::jmax(1, localNumSteps));
                                const double noteDuration = juce::jmax(0.0625, stepBeatLength * localGateAmount);
                                target.events.clear();

                                for (int row = 0; row < numRows; ++row)
                                {
                                    const int note = noteForRow(row);
                                    for (int step = 0; step < localNumSteps; ++step)
                                    {
                                        if (!patternSnapshot[static_cast<size_t>(row)][step])
                                            continue;

                                        TimelineEvent ev;
                                        ev.startBeat = step * stepBeatLength;
                                        ev.durationBeats = noteDuration;
                                        ev.noteNumber = note;
                                        ev.velocity = 100;
                                        target.events.push_back(ev);
                                    }
                                }

                                std::sort(target.events.begin(), target.events.end(),
                                          [](const TimelineEvent& a, const TimelineEvent& b)
                                          {
                                              if (std::abs(a.startBeat - b.startBeat) > 0.0001)
                                                  return a.startBeat < b.startBeat;
                                              return a.noteNumber < b.noteNumber;
                                          });
                            });
        }

        void performClipEdit(const juce::String& actionName, std::function<void(Clip&)> editFn)
        {
            if (clip == nullptr)
                return;

            if (onRequestClipEdit && clipIndex >= 0)
                onRequestClipEdit(clipIndex, actionName, std::move(editFn));
            else
                editFn(*clip);

            loadPatternFromClip();
        }

        int getNoteForRow(int row) const
        {
            const auto& scale = getScaleIntervals();
            const int fromBottom = (numRows - 1) - row;
            const int degree = fromBottom % static_cast<int>(scale.size());
            const int octaveOffset = fromBottom / static_cast<int>(scale.size());
            const int note = (baseOctave * 12) + rootNote + scale[static_cast<size_t>(degree)] + (12 * octaveOffset);
            return juce::jlimit(0, 127, note);
        }

        int findClosestRowForNote(int note) const
        {
            int bestRow = -1;
            int bestDistance = std::numeric_limits<int>::max();
            for (int row = 0; row < numRows; ++row)
            {
                const int distance = std::abs(getNoteForRow(row) - note);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestRow = row;
                }
            }
            return bestRow;
        }

        juce::String getNoteName(int note) const
        {
            static const std::array<const char*, 12> names = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int pitchClass = ((note % 12) + 12) % 12;
            const int octave = (note / 12) - 1;
            return juce::String(names[static_cast<size_t>(pitchClass)]) + juce::String(octave);
        }

        const std::vector<int>& getScaleIntervals() const
        {
            static const std::array<std::vector<int>, 5> scales = {
                std::vector<int>{ 0, 2, 4, 5, 7, 9, 11 }, // Major
                std::vector<int>{ 0, 2, 3, 5, 7, 8, 10 }, // Minor
                std::vector<int>{ 0, 2, 3, 5, 7, 9, 10 }, // Dorian
                std::vector<int>{ 0, 2, 4, 5, 7, 9, 10 }, // Mixolydian
                std::vector<int>{ 0, 3, 5, 7, 10 }        // Pentatonic
            };

            return scales[static_cast<size_t>(juce::jlimit(0, 4, scaleMode))];
        }

        juce::Rectangle<int> getControlBounds() const
        {
            auto area = getLocalBounds().reduced(6);
            return area.removeFromTop(controlHeight);
        }

        juce::Rectangle<int> getGridBounds() const
        {
            auto area = getLocalBounds().reduced(6);
            area.removeFromTop(controlHeight + 6);
            return area;
        }

        Clip* clip = nullptr;
        int clipIndex = -1;
        std::array<juce::BigInteger, numRows> pattern;
        juce::ComboBox rootSelector;
        juce::ComboBox scaleSelector;
        juce::ComboBox stepCountSelector;
        juce::Slider gateSlider;
        juce::Slider octaveSlider;

        int rootNote = 0;
        int scaleMode = 0;
        int numSteps = 16;
        int baseOctave = 4;
        float gateAmount = 0.85f;

        bool dragActive = false;
        bool dragSetState = false;
        int lastTouchedRow = -1;
        int lastTouchedStep = -1;
    };
}
