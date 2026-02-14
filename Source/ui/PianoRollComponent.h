#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>
#include "TimelineModel.h"
#include "Theme.h"

namespace sampledex
{
    class PianoRollComponent : public juce::Component, private juce::ScrollBar::Listener
    {
        enum class ResizeEdge
        {
            None,
            Left,
            Right
        };

    public:
        std::function<void(int, const juce::String&, std::function<void(Clip&)>)> onRequestClipEdit;
        std::function<void(int)> onSwingChanged;
        std::function<void(int, int, int)> onPreviewStepNote;

        PianoRollComponent()
        {
            setWantsKeyboardFocus(true);

            rootSelector.addItemList({"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 1);
            rootSelector.setSelectedItemIndex(rootNote, juce::dontSendNotification);
            rootSelector.onChange = [this]
            {
                rootNote = juce::jlimit(0, 11, rootSelector.getSelectedItemIndex());
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
                repaint();
            };

            snapSelector.addItem("Snap 1/4", 1);
            snapSelector.addItem("Snap 1/8", 2);
            snapSelector.addItem("Snap 1/16", 3);
            snapSelector.addItem("Snap 1/32", 4);
            snapSelector.addItem("Snap 1/8T", 5);
            snapSelector.addItem("Snap 1/16T", 6);
            snapSelector.setSelectedId(3, juce::dontSendNotification);
            snapSelector.onChange = [this]
            {
                switch (snapSelector.getSelectedId())
                {
                    case 1: snapBeat = 1.0; break;
                    case 2: snapBeat = 0.5; break;
                    case 3: snapBeat = 0.25; break;
                    case 4: snapBeat = 0.125; break;
                    case 5: snapBeat = 1.0 / 3.0; break;
                    case 6: snapBeat = 1.0 / 6.0; break;
                    default: snapBeat = 0.25; break;
                }
                repaint();
            };

            swingSlider.setRange(50.0, 75.0, 1.0);
            swingSlider.setValue(static_cast<double>(swingPercent), juce::dontSendNotification);
            swingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            swingSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            swingSlider.setChangeNotificationOnlyOnRelease(false);
            swingSlider.onValueChange = [this]
            {
                setSwingPercent(static_cast<int>(std::round(swingSlider.getValue())));
            };

            lengthSelector.addItem("Len 1/4", 1);
            lengthSelector.addItem("Len 1/2", 2);
            lengthSelector.addItem("Len 1", 3);
            lengthSelector.addItem("Len 2", 4);
            lengthSelector.addItem("Len 4", 5);
            lengthSelector.setSelectedId(3, juce::dontSendNotification);
            lengthSelector.onChange = [this]
            {
                switch (lengthSelector.getSelectedId())
                {
                    case 1: noteLengthBeats = 0.25; break;
                    case 2: noteLengthBeats = 0.5; break;
                    case 3: noteLengthBeats = 1.0; break;
                    case 4: noteLengthBeats = 2.0; break;
                    case 5: noteLengthBeats = 4.0; break;
                    default: noteLengthBeats = 1.0; break;
                }
            };

            zoomSelector.addItem("Rows 16", 1);
            zoomSelector.addItem("Rows 24", 2);
            zoomSelector.addItem("Rows 36", 3);
            zoomSelector.addItem("Rows 48", 4);
            zoomSelector.setSelectedId(2, juce::dontSendNotification);
            zoomSelector.onChange = [this]
            {
                switch (zoomSelector.getSelectedId())
                {
                    case 1: visibleNoteCount = 16; break;
                    case 2: visibleNoteCount = 24; break;
                    case 3: visibleNoteCount = 36; break;
                    case 4: visibleNoteCount = 48; break;
                    default: visibleNoteCount = 24; break;
                }
                lowestVisibleNote = juce::jlimit(0, 127 - visibleNoteCount, lowestVisibleNote);
                repaint();
            };

            velocitySlider.setRange(1.0, 127.0, 1.0);
            velocitySlider.setValue(defaultVelocity, juce::dontSendNotification);
            velocitySlider.setSliderStyle(juce::Slider::LinearHorizontal);
            velocitySlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            velocitySlider.setChangeNotificationOnlyOnRelease(true);
            velocitySlider.onValueChange = [this]
            {
                const int newVelocity = juce::jlimit(1, 127, static_cast<int>(std::round(velocitySlider.getValue())));
                defaultVelocity = newVelocity;

                if (updatingVelocitySlider || clip == nullptr || selectedNoteIndex < 0)
                    return;

                const int noteIndex = selectedNoteIndex;
                performClipEdit("Edit Note Velocity",
                                [noteIndex, newVelocity](Clip& target)
                                {
                                    if (noteIndex >= 0 && noteIndex < static_cast<int>(target.events.size()))
                                        target.events[static_cast<size_t>(noteIndex)].velocity = static_cast<uint8_t>(newVelocity);
                                });
            };

            ccSelector.addItem("CC1 Mod Wheel", 1);
            ccSelector.addItem("CC7 Volume", 2);
            ccSelector.addItem("CC10 Pan", 3);
            ccSelector.addItem("CC11 Expression", 4);
            ccSelector.addItem("CC64 Sustain", 5);
            ccSelector.addItem("CC74 Brightness", 6);
            ccSelector.addItem("Pitch Bend", 7);
            ccSelector.addItem("Channel Pressure", 8);
            ccSelector.addItem("Poly Aftertouch", 9);
            ccSelector.addItem("Program Change", 10);
            ccSelector.setSelectedId(1, juce::dontSendNotification);
            ccSelector.onChange = [this]
            {
                ccLaneIndex = juce::jlimit(0, getTotalLaneCount() - 1, ccSelector.getSelectedId() - 1);
                repaint();
            };

            quantizeButton.setButtonText("Quantize");
            quantizeButton.onClick = [this]
            {
                if (clip == nullptr)
                    return;

                performClipEdit("Quantize MIDI",
                                [this](Clip& target)
                                {
                                    const double clipMaxBeat = juce::jmax(0.0, target.lengthBeats - 0.0625);
                                    for (auto& ev : target.events)
                                    {
                                        ev.noteNumber = snapNoteToScale(ev.noteNumber);
                                        const double quantStart = juce::jlimit(0.0, clipMaxBeat, quantizeBeatToGrid(ev.startBeat));
                                        const double noteEnd = ev.startBeat + ev.durationBeats;
                                        double quantEnd = quantizeBeatToGrid(noteEnd);
                                        quantEnd = juce::jmax(quantStart + 0.0625, quantEnd);
                                        quantEnd = juce::jmin(target.lengthBeats, quantEnd);

                                        ev.startBeat = quantStart;
                                        ev.durationBeats = juce::jmax(0.0625, quantEnd - quantStart);
                                    }

                                    for (auto& cc : target.ccEvents)
                                        cc.beat = juce::jlimit(0.0, target.lengthBeats, quantizeBeatToGrid(cc.beat));
                                    for (auto& bend : target.pitchBendEvents)
                                        bend.beat = juce::jlimit(0.0, target.lengthBeats, quantizeBeatToGrid(bend.beat));
                                    for (auto& pressure : target.channelPressureEvents)
                                        pressure.beat = juce::jlimit(0.0, target.lengthBeats, quantizeBeatToGrid(pressure.beat));
                                    for (auto& poly : target.polyAftertouchEvents)
                                        poly.beat = juce::jlimit(0.0, target.lengthBeats, quantizeBeatToGrid(poly.beat));
                                    for (auto& program : target.programChangeEvents)
                                        program.beat = juce::jlimit(0.0, target.lengthBeats, quantizeBeatToGrid(program.beat));
                                    for (auto& raw : target.rawEvents)
                                        raw.beat = juce::jlimit(0.0, target.lengthBeats, quantizeBeatToGrid(raw.beat));

                                    std::sort(target.events.begin(), target.events.end(),
                                              [](const TimelineEvent& a, const TimelineEvent& b) { return a.startBeat < b.startBeat; });
                                    std::sort(target.ccEvents.begin(), target.ccEvents.end(),
                                              [](const MidiCCEvent& a, const MidiCCEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.pitchBendEvents.begin(), target.pitchBendEvents.end(),
                                              [](const MidiPitchBendEvent& a, const MidiPitchBendEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.channelPressureEvents.begin(), target.channelPressureEvents.end(),
                                              [](const MidiChannelPressureEvent& a, const MidiChannelPressureEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.polyAftertouchEvents.begin(), target.polyAftertouchEvents.end(),
                                              [](const MidiPolyAftertouchEvent& a, const MidiPolyAftertouchEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.programChangeEvents.begin(), target.programChangeEvents.end(),
                                              [](const MidiProgramChangeEvent& a, const MidiProgramChangeEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.rawEvents.begin(), target.rawEvents.end(),
                                              [](const MidiRawEvent& a, const MidiRawEvent& b) { return a.beat < b.beat; });
                                });
            };

            humanizeButton.setButtonText("Humanize");
            humanizeButton.onClick = [this]
            {
                if (clip == nullptr)
                    return;

                performClipEdit("Humanize MIDI",
                                [this](Clip& target)
                                {
                                    juce::Random random;
                                    const double beatJitter = snapBeat * 0.15;
                                    const double clipMaxBeat = juce::jmax(0.0, target.lengthBeats - 0.0625);

                                    for (auto& ev : target.events)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        ev.startBeat = juce::jlimit(0.0, clipMaxBeat, ev.startBeat + offset);

                                        const int velDelta = random.nextInt(21) - 10;
                                        const int vel = juce::jlimit(1, 127, static_cast<int>(ev.velocity) + velDelta);
                                        ev.velocity = static_cast<uint8_t>(vel);
                                    }

                                    for (auto& cc : target.ccEvents)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        cc.beat = juce::jlimit(0.0, target.lengthBeats, cc.beat + offset);

                                        const int valDelta = random.nextInt(17) - 8;
                                        const int value = juce::jlimit(0, 127, static_cast<int>(cc.value) + valDelta);
                                        cc.value = static_cast<uint8_t>(value);
                                    }

                                    for (auto& bend : target.pitchBendEvents)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        bend.beat = juce::jlimit(0.0, target.lengthBeats, bend.beat + offset);
                                    }
                                    for (auto& pressure : target.channelPressureEvents)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        pressure.beat = juce::jlimit(0.0, target.lengthBeats, pressure.beat + offset);
                                    }
                                    for (auto& poly : target.polyAftertouchEvents)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        poly.beat = juce::jlimit(0.0, target.lengthBeats, poly.beat + offset);
                                    }
                                    for (auto& program : target.programChangeEvents)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        program.beat = juce::jlimit(0.0, target.lengthBeats, program.beat + offset);
                                    }
                                    for (auto& raw : target.rawEvents)
                                    {
                                        const double offset = random.nextDouble() * (2.0 * beatJitter) - beatJitter;
                                        raw.beat = juce::jlimit(0.0, target.lengthBeats, raw.beat + offset);
                                    }

                                    std::sort(target.events.begin(), target.events.end(),
                                              [](const TimelineEvent& a, const TimelineEvent& b) { return a.startBeat < b.startBeat; });
                                    std::sort(target.ccEvents.begin(), target.ccEvents.end(),
                                              [](const MidiCCEvent& a, const MidiCCEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.pitchBendEvents.begin(), target.pitchBendEvents.end(),
                                              [](const MidiPitchBendEvent& a, const MidiPitchBendEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.channelPressureEvents.begin(), target.channelPressureEvents.end(),
                                              [](const MidiChannelPressureEvent& a, const MidiChannelPressureEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.polyAftertouchEvents.begin(), target.polyAftertouchEvents.end(),
                                              [](const MidiPolyAftertouchEvent& a, const MidiPolyAftertouchEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.programChangeEvents.begin(), target.programChangeEvents.end(),
                                              [](const MidiProgramChangeEvent& a, const MidiProgramChangeEvent& b) { return a.beat < b.beat; });
                                    std::sort(target.rawEvents.begin(), target.rawEvents.end(),
                                              [](const MidiRawEvent& a, const MidiRawEvent& b) { return a.beat < b.beat; });
                                });
            };

            aiAssistButton.setButtonText("AI Assist");
            aiAssistButton.onClick = [this]
            {
                if (clip == nullptr)
                    return;

                performClipEdit("AI Assist MIDI Cleanup",
                                [this](Clip& target)
                                {
                                    const double clipMaxBeat = juce::jmax(0.0, target.lengthBeats - 0.0625);
                                    for (auto& ev : target.events)
                                    {
                                        ev.noteNumber = scaleSnapEnabled ? snapNoteToScale(ev.noteNumber)
                                                                         : juce::jlimit(0, 127, ev.noteNumber);
                                        const double start = juce::jlimit(0.0, clipMaxBeat, quantizeBeatToGrid(ev.startBeat));
                                        const double end = juce::jlimit(start + 0.0625, target.lengthBeats,
                                                                        quantizeBeatToGrid(ev.startBeat + ev.durationBeats));
                                        ev.startBeat = start;
                                        ev.durationBeats = juce::jmax(0.0625, end - start);
                                        ev.velocity = static_cast<uint8_t>(juce::jlimit(1, 127, static_cast<int>(ev.velocity)));
                                    }

                                    std::sort(target.events.begin(), target.events.end(),
                                              [](const TimelineEvent& a, const TimelineEvent& b)
                                              {
                                                  if (std::abs(a.startBeat - b.startBeat) > 0.0001)
                                                      return a.startBeat < b.startBeat;
                                                  return a.noteNumber < b.noteNumber;
                                              });
                                });
            };

            progressionSelector.addItem("Progression 001", 1);
            for (int i = 2; i <= 100; ++i)
                progressionSelector.addItem("Progression " + juce::String(i).paddedLeft('0', 3), i);
            progressionSelector.setSelectedId(1, juce::dontSendNotification);

            generateProgressionButton.setButtonText("Generate");
            generateProgressionButton.onClick = [this] { generateChordProgression(false); };

            regenerateProgressionButton.setButtonText("Regen");
            regenerateProgressionButton.onClick = [this] { generateChordProgression(true); };

            selectToolButton.setButtonText("Select");
            selectToolButton.setClickingTogglesState(true);
            selectToolButton.setRadioGroupId(701);
            selectToolButton.setToggleState(true, juce::dontSendNotification);
            selectToolButton.onClick = [this] { activeTool = EditTool::Select; };

            drawToolButton.setButtonText("Draw");
            drawToolButton.setClickingTogglesState(true);
            drawToolButton.setRadioGroupId(701);
            drawToolButton.onClick = [this] { activeTool = EditTool::Draw; };

            eraseToolButton.setButtonText("Erase");
            eraseToolButton.setClickingTogglesState(true);
            eraseToolButton.setRadioGroupId(701);
            eraseToolButton.onClick = [this] { activeTool = EditTool::Erase; };

            scaleSnapButton.setButtonText("Scale Snap");
            scaleSnapButton.setClickingTogglesState(true);
            scaleSnapButton.onClick = [this] { scaleSnapEnabled = scaleSnapButton.getToggleState(); };

            stepInputButton.setButtonText("KB Step");
            stepInputButton.setClickingTogglesState(true);
            stepInputButton.setToggleState(true, juce::dontSendNotification);
            stepInputButton.onClick = [this] { stepInputEnabled = stepInputButton.getToggleState(); };

            octaveDownButton.setButtonText("Oct-");
            octaveDownButton.onClick = [this]
            {
                lowestVisibleNote = juce::jlimit(0, 127 - visibleNoteCount, lowestVisibleNote - 12);
                updateScrollBars();
                repaint();
            };

            octaveUpButton.setButtonText("Oct+");
            octaveUpButton.onClick = [this]
            {
                lowestVisibleNote = juce::jlimit(0, 127 - visibleNoteCount, lowestVisibleNote + 12);
                updateScrollBars();
                repaint();
            };

            horizZoomOutButton.setButtonText("H-");
            horizZoomOutButton.onClick = [this] { zoomHorizontalBy(1.1); };
            horizZoomInButton.setButtonText("H+");
            horizZoomInButton.onClick = [this] { zoomHorizontalBy(0.9); };
            vertZoomOutButton.setButtonText("V-");
            vertZoomOutButton.onClick = [this] { zoomVerticalByRows(+2); };
            vertZoomInButton.setButtonText("V+");
            vertZoomInButton.onClick = [this] { zoomVerticalByRows(-2); };

            addAndMakeVisible(rootSelector);
            addAndMakeVisible(scaleSelector);
            addAndMakeVisible(snapSelector);
            addAndMakeVisible(swingSlider);
            addAndMakeVisible(lengthSelector);
            addAndMakeVisible(zoomSelector);
            addAndMakeVisible(selectToolButton);
            addAndMakeVisible(drawToolButton);
            addAndMakeVisible(eraseToolButton);
            addAndMakeVisible(scaleSnapButton);
            addAndMakeVisible(stepInputButton);
            addAndMakeVisible(octaveDownButton);
            addAndMakeVisible(octaveUpButton);
            addAndMakeVisible(horizZoomOutButton);
            addAndMakeVisible(horizZoomInButton);
            addAndMakeVisible(vertZoomOutButton);
            addAndMakeVisible(vertZoomInButton);
            addAndMakeVisible(velocitySlider);
            addAndMakeVisible(ccSelector);
            addAndMakeVisible(quantizeButton);
            addAndMakeVisible(humanizeButton);
            addAndMakeVisible(aiAssistButton);
            addAndMakeVisible(progressionSelector);
            addAndMakeVisible(generateProgressionButton);
            addAndMakeVisible(regenerateProgressionButton);
            addAndMakeVisible(horizontalScrollBar);
            addAndMakeVisible(verticalScrollBar);

            horizontalScrollBar.addListener(this);
            verticalScrollBar.addListener(this);
            horizontalScrollBar.setAutoHide(false);
            verticalScrollBar.setAutoHide(false);

            rootSelector.setTooltip("Root note for scale highlight and optional scale-snap.");
            scaleSelector.setTooltip("Scale mode used for highlight and scale-snap.");
            snapSelector.setTooltip("Grid snap resolution.");
            swingSlider.setTooltip("Swing amount for quantize grid (50% = straight).");
            lengthSelector.setTooltip("Default inserted note length.");
            zoomSelector.setTooltip("Visible note rows (vertical zoom).");
            selectToolButton.setTooltip("Select/move/resize notes. Drag empty area to marquee-select.");
            drawToolButton.setTooltip("Brush draw notes with one drag gesture.");
            eraseToolButton.setTooltip("Erase notes by clicking or dragging.");
            scaleSnapButton.setTooltip("Constrain note pitch edits to selected scale.");
            stepInputButton.setTooltip("Enable computer keyboard step-note input.");
            octaveDownButton.setTooltip("Shift visible piano range down 1 octave.");
            octaveUpButton.setTooltip("Shift visible piano range up 1 octave.");
            horizZoomOutButton.setTooltip("Zoom piano roll timeline out.");
            horizZoomInButton.setTooltip("Zoom piano roll timeline in.");
            vertZoomOutButton.setTooltip("Show more note rows.");
            vertZoomInButton.setTooltip("Show fewer note rows.");
            velocitySlider.setTooltip("Default note velocity and selected-note velocity.");
            ccSelector.setTooltip("Active CC lane controller.");
            quantizeButton.setTooltip("Quantize note starts/ends and CC to grid.");
            humanizeButton.setTooltip("Add timing and velocity variation.");
            aiAssistButton.setTooltip("Scale-aware cleanup: snap pitch/timing and normalize lengths.");
            progressionSelector.setTooltip("Select one of 100 chord progression templates.");
            generateProgressionButton.setTooltip("Generate progression notes into the selected clip.");
            regenerateProgressionButton.setTooltip("Regenerate with a new voicing and rhythmic variation.");
        }

        void setClip(Clip* c, int index = -1)
        {
            clip = c;
            clipIndex = index;
            selectedNoteIndex = -1;
            selectedNoteIndices.clear();
            draggingNote = false;
            resizingNote = false;
            draggingCC = false;
            velocityDragging = false;
            marqueeSelecting = false;
            brushPainting = false;
            hoveredNoteIndex = -1;
            stepInputBeat = 0.0;
            viewStartBeat = 0.0;
            viewLengthBeats = clip != nullptr ? juce::jlimit(1.0, juce::jmax(1.0, clip->lengthBeats), 8.0)
                                              : 4.0;
            clampViewWindow();
            updateScrollBars();
            updateVelocitySliderFromSelection();
            repaint();
        }

        void setSnapBeat(double beats)
        {
            snapBeat = juce::jlimit(1.0 / 64.0, 4.0, beats);
            clampViewWindow();
            updateScrollBars();
            repaint();
        }

        double getSnapBeat() const { return snapBeat; }
        void setSwingPercent(int percent)
        {
            const int clamped = juce::jlimit(50, 75, percent);
            if (swingPercent == clamped)
                return;

            swingPercent = clamped;
            if (static_cast<int>(std::round(swingSlider.getValue())) != swingPercent)
                swingSlider.setValue(static_cast<double>(swingPercent), juce::dontSendNotification);

            if (onSwingChanged)
                onSwingChanged(swingPercent);
        }
        int getSwingPercent() const { return swingPercent; }
        int getRootNote() const { return rootNote; }
        int getScaleMode() const { return scaleMode; }
        bool isScaleSnapEnabled() const { return scaleSnapEnabled; }

        void setScaleContext(int newRootNote, int newScaleMode, bool enableScaleSnap)
        {
            rootNote = juce::jlimit(0, 11, newRootNote);
            scaleMode = juce::jlimit(0, 4, newScaleMode);
            scaleSnapEnabled = enableScaleSnap;

            rootSelector.setSelectedItemIndex(rootNote, juce::dontSendNotification);
            scaleSelector.setSelectedId(scaleMode + 1, juce::dontSendNotification);
            scaleSnapButton.setToggleState(scaleSnapEnabled, juce::dontSendNotification);
            repaint();
        }

        void zoomHorizontalBy(double factor)
        {
            if (clip == nullptr)
                return;

            clampViewWindow();
            const double minSpan = juce::jmax(snapBeat, 0.25);
            const double maxSpan = juce::jmax(minSpan, clip->lengthBeats);
            viewLengthBeats = juce::jlimit(minSpan, maxSpan, viewLengthBeats * factor);
            clampViewWindow();
            updateScrollBars();
            repaint();
        }

        void zoomVerticalByRows(int deltaRows)
        {
            visibleNoteCount = juce::jlimit(12, 72, visibleNoteCount + deltaRows);
            lowestVisibleNote = juce::jlimit(0, 127 - visibleNoteCount, lowestVisibleNote);
            updateScrollBars();
            repaint();
        }

        bool handleComputerKeyboardPress(const juce::KeyPress& key)
        {
            if (clip == nullptr)
                return false;

            if (!key.getModifiers().isAnyModifierKeyDown())
            {
                const juce::juce_wchar ch = static_cast<juce::juce_wchar>(std::tolower(static_cast<unsigned char>(key.getTextCharacter())));
                if (ch == '1')
                {
                    activeTool = EditTool::Select;
                    selectToolButton.setToggleState(true, juce::dontSendNotification);
                    repaint();
                    return true;
                }
                if (ch == '2')
                {
                    activeTool = EditTool::Draw;
                    drawToolButton.setToggleState(true, juce::dontSendNotification);
                    repaint();
                    return true;
                }
                if (ch == '3')
                {
                    activeTool = EditTool::Erase;
                    eraseToolButton.setToggleState(true, juce::dontSendNotification);
                    repaint();
                    return true;
                }
                if (ch == 'q')
                {
                    quantizeButton.triggerClick();
                    return true;
                }
            }

            if (!stepInputEnabled || key.getModifiers().isAnyModifierKeyDown())
                return false;

            if (key == juce::KeyPress::leftKey)
            {
                stepInputBeat = juce::jmax(0.0, stepInputBeat - snapBeat);
                ensureBeatVisible(stepInputBeat);
                repaint();
                return true;
            }

            if (key == juce::KeyPress::rightKey)
            {
                stepInputBeat = juce::jmin(juce::jmax(0.0, clip->lengthBeats - 0.0625), stepInputBeat + snapBeat);
                ensureBeatVisible(stepInputBeat);
                repaint();
                return true;
            }

            const juce::juce_wchar ch = static_cast<juce::juce_wchar>(std::tolower(static_cast<unsigned char>(key.getTextCharacter())));
            if (ch == 'z')
            {
                keyboardBaseNote = juce::jlimit(12, 108, keyboardBaseNote - 12);
                return true;
            }

            if (ch == 'x')
            {
                keyboardBaseNote = juce::jlimit(12, 108, keyboardBaseNote + 12);
                return true;
            }

            const int semitone = getKeyboardSemitoneOffset(ch);
            if (semitone < 0)
                return false;

            const int rawNote = juce::jlimit(0, 127, keyboardBaseNote + semitone);
            const int snappedNote = scaleSnapEnabled ? snapNoteToScale(rawNote) : rawNote;
            insertStepNote(snappedNote);
            return true;
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(theme::Colours::darker());

            const auto controlArea = getControlBounds();
            g.setColour(theme::Colours::panel());
            g.fillRoundedRectangle(controlArea.toFloat(), 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.14f));
            g.drawRoundedRectangle(controlArea.toFloat(), 4.0f, 1.0f);

            if (clip == nullptr)
            {
                g.setColour(juce::Colours::grey);
                g.setFont(20.0f);
                g.drawText("Select a MIDI Clip to Edit", getGridBounds(), juce::Justification::centred);
                return;
            }

            clampViewWindow();
            updateScrollBars();

            const auto fullGrid = getGridBounds();
            if (fullGrid.isEmpty())
                return;

            const auto pianoKeys = getPianoKeyBounds(fullGrid);
            const auto noteGrid = getNoteGridBounds(fullGrid);
            const auto velocityGrid = getVelocityLaneBounds(fullGrid);
            const auto ccGrid = getCCLaneBounds(fullGrid);

            paintPianoKeys(g, pianoKeys);
            paintNoteGrid(g, noteGrid);
            paintVelocityLane(g, velocityGrid);
            paintCCLane(g, ccGrid);

            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.setFont(10.0f);
            g.drawText("KB step @" + juce::String(stepInputBeat, 2) + "b",
                       noteGrid.getRight() - 130, noteGrid.getY() + 4, 126, 14,
                       juce::Justification::centredRight);

            if (marqueeSelecting && !marqueeRect.isEmpty())
            {
                g.setColour(theme::Colours::accent().withAlpha(0.2f));
                g.fillRect(marqueeRect);
                g.setColour(theme::Colours::accent().withAlpha(0.85f));
                g.drawRect(marqueeRect, 1.5f);
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (clip == nullptr)
                return;

            clampViewWindow();
            grabKeyboardFocus();

            const auto grid = getGridBounds();
            if (!grid.contains(e.getPosition()))
                return;

            const auto noteGrid = getNoteGridBounds(grid);
            const auto velocityGrid = getVelocityLaneBounds(grid);
            const auto ccGrid = getCCLaneBounds(grid);

            if (velocityGrid.contains(e.getPosition()))
            {
                handleVelocityLaneMouseDown(e, velocityGrid);
                return;
            }

            if (ccGrid.contains(e.getPosition()))
            {
                handleCCMouseDown(e, ccGrid);
                draggingCC = !e.mods.isRightButtonDown();
                return;
            }

            if (!noteGrid.contains(e.getPosition()))
                return;

            const auto [note, beat] = getNoteAndBeatAtPosition(e.position, noteGrid);
            ResizeEdge resizeEdge = ResizeEdge::None;
            const int hitIndex = findNoteAtPosition(e.position, noteGrid, &resizeEdge);
            const bool onResizeHandle = resizeEdge != ResizeEdge::None;

            const bool eraseGesture = e.mods.isRightButtonDown() || activeTool == EditTool::Erase;
            if (eraseGesture)
            {
                if (hitIndex >= 0)
                {
                    std::vector<int> toDelete;
                    if (!selectedNoteIndices.empty() && selectedNoteIndices.count(hitIndex) > 0)
                        toDelete.assign(selectedNoteIndices.begin(), selectedNoteIndices.end());
                    else
                        toDelete.push_back(hitIndex);

                    performClipEdit("Delete MIDI Note",
                                    [toDelete](Clip& target)
                                    {
                                        for (int i = static_cast<int>(toDelete.size()) - 1; i >= 0; --i)
                                        {
                                            const int idx = toDelete[static_cast<size_t>(i)];
                                            if (juce::isPositiveAndBelow(idx, static_cast<int>(target.events.size())))
                                                target.events.erase(target.events.begin() + idx);
                                        }
                                    });
                    clearNoteSelection();
                }
                brushPainting = true;
                brushEraseMode = true;
                lastBrushNote = -1;
                lastBrushBeat = -1.0;
                return;
            }

            if (activeTool == EditTool::Select && hitIndex >= 0)
            {
                if (onResizeHandle)
                {
                    selectSingleNote(hitIndex);
                }
                else if (e.mods.isShiftDown())
                    toggleNoteSelection(hitIndex);
                else if (selectedNoteIndices.count(hitIndex) == 0)
                    selectSingleNote(hitIndex);

                const bool duplicateGesture = e.mods.isAltDown();
                if (duplicateGesture && selectedNoteIndices.count(hitIndex) > 0)
                    duplicateSelectedNotesForDrag();
                else
                    duplicateDragInProgress = false;

                beginNoteDrag(beat, note, onResizeHandle, resizeEdge);
                return;
            }

            if (activeTool == EditTool::Select)
            {
                if (!e.mods.isShiftDown())
                    clearNoteSelection();

                marqueeSelecting = true;
                marqueeStart = e.position;
                marqueeRect = juce::Rectangle<float>(marqueeStart.x, marqueeStart.y, 0.0f, 0.0f);
                repaint();
                return;
            }

            brushPainting = true;
            brushEraseMode = false;
            lastBrushNote = -1;
            lastBrushBeat = -1.0;
            applyBrushAt(note, beat, false);
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (clip == nullptr)
                return;

            clampViewWindow();
            const auto grid = getGridBounds();
            const auto noteGrid = getNoteGridBounds(grid);
            const auto velocityGrid = getVelocityLaneBounds(grid);
            const auto ccGrid = getCCLaneBounds(grid);

            if (velocityDragging && velocityGrid.contains(e.getPosition()))
            {
                handleVelocityLaneMouseDrag(e, velocityGrid);
                return;
            }

            if (marqueeSelecting)
            {
                marqueeRect = juce::Rectangle<float>(
                    juce::jmin(marqueeStart.x, e.position.x),
                    juce::jmin(marqueeStart.y, e.position.y),
                    std::abs(marqueeStart.x - e.position.x),
                    std::abs(marqueeStart.y - e.position.y));
                updateMarqueeSelection(noteGrid);
                repaint();
                return;
            }

            if (draggingCC && ccGrid.contains(e.getPosition()))
            {
                handleCCMouseDown(e, ccGrid);
                return;
            }

            if (brushPainting && noteGrid.contains(e.getPosition()))
            {
                autoScrollAtMouseX(e.position.x, noteGrid);
                const auto [note, beat] = getNoteAndBeatAtPosition(e.position, noteGrid);
                applyBrushAt(note, beat, brushEraseMode);
                return;
            }

            if (!draggingNote || dragSourceIndices.empty())
                return;

            autoScrollAtMouseX(e.position.x, noteGrid);
            const auto [mouseNote, mouseBeat] = getNoteAndBeatAtPosition(e.position, noteGrid);
            if (resizingNote)
            {
                if (dragSourceIndices.empty() || dragSourceEvents.empty())
                    return;

                const int noteIndex = dragSourceIndices.front();
                const auto source = dragSourceEvents.front();
                const auto snapResizeBeat = [this, &e](double rawBeat, double anchorBeat)
                {
                    if (e.mods.isShiftDown())
                        return rawBeat;

                    const double step = juce::jmax(1.0 / 128.0, snapBeat);
                    if (rawBeat >= anchorBeat)
                        return std::ceil(rawBeat / step) * step;
                    return std::floor(rawBeat / step) * step;
                };
                if (resizingFromLeft)
                {
                    const double sourceEndBeat = source.startBeat + source.durationBeats;
                    const double rawStartBeat = getRawBeatForXInGrid(e.position.x, noteGrid);
                    const double snappedStartBeat = snapResizeBeat(rawStartBeat, source.startBeat);
                    const double proposedStart = juce::jlimit(0.0, sourceEndBeat - 0.0625, snappedStartBeat);
                    if (std::abs(proposedStart - lastDragStartBeat) < 0.0001)
                        return;

                    lastDragStartBeat = proposedStart;
                    const double proposedDuration = juce::jmax(0.0625, sourceEndBeat - proposedStart);
                    performClipEdit("Resize MIDI Note",
                                    [noteIndex, proposedStart, proposedDuration](Clip& target)
                                    {
                                        if (!juce::isPositiveAndBelow(noteIndex, static_cast<int>(target.events.size())))
                                            return;
                                        auto& event = target.events[static_cast<size_t>(noteIndex)];
                                        event.startBeat = proposedStart;
                                        event.durationBeats = proposedDuration;
                                    });
                }
                else
                {
                    const double maxDuration = juce::jmax(0.0625, clip->lengthBeats - source.startBeat);
                    const double rawResizeBeat = getRawBeatForXInGrid(e.position.x, noteGrid);
                    const double snappedResizeBeat = snapResizeBeat(rawResizeBeat, source.startBeat + source.durationBeats);
                    const double proposedDuration = juce::jlimit(0.0625, maxDuration, snappedResizeBeat - source.startBeat);
                    if (std::abs(proposedDuration - lastDragDuration) < 0.0001)
                        return;

                    lastDragDuration = proposedDuration;
                    performClipEdit("Resize MIDI Note",
                                    [noteIndex, proposedDuration](Clip& target)
                                    {
                                        if (!juce::isPositiveAndBelow(noteIndex, static_cast<int>(target.events.size())))
                                            return;
                                        target.events[static_cast<size_t>(noteIndex)].durationBeats = proposedDuration;
                                    });
                }
                return;
            }

            const double deltaBeat = std::round((mouseBeat - dragStartMouseBeat) / snapBeat) * snapBeat;
            const int deltaNote = mouseNote - dragStartMouseNote;
            if (std::abs(deltaBeat - lastDragDeltaBeat) < 0.0001 && deltaNote == lastDragDeltaNote)
                return;

            lastDragDeltaBeat = deltaBeat;
            lastDragDeltaNote = deltaNote;
            const auto indices = dragSourceIndices;
            const auto sources = dragSourceEvents;
            const bool useScaleSnap = scaleSnapEnabled;
            performClipEdit(duplicateDragInProgress ? "Duplicate + Move MIDI Notes" : "Move MIDI Notes",
                            [indices, sources, deltaBeat, deltaNote, useScaleSnap, this](Clip& target)
                            {
                                for (size_t i = 0; i < indices.size() && i < sources.size(); ++i)
                                {
                                    const int idx = indices[i];
                                    if (!juce::isPositiveAndBelow(idx, static_cast<int>(target.events.size())))
                                        continue;

                                    const auto& source = sources[i];
                                    auto& event = target.events[static_cast<size_t>(idx)];
                                    event.startBeat = juce::jlimit(0.0, juce::jmax(0.0, target.lengthBeats - 0.0625), source.startBeat + deltaBeat);

                                    int note = juce::jlimit(0, 127, source.noteNumber + deltaNote);
                                    if (useScaleSnap)
                                        note = snapNoteToScale(note);
                                    event.noteNumber = note;

                                    const double maxDur = juce::jmax(0.0625, target.lengthBeats - event.startBeat);
                                    event.durationBeats = juce::jmin(source.durationBeats, maxDur);
                                }
                            });
            if (!selectedNoteIndices.empty())
                selectedNoteIndex = *selectedNoteIndices.begin();
            repaint();
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const auto grid = getGridBounds();
            if (!grid.contains(e.getPosition()))
            {
                if (hoveredNoteIndex != -1)
                {
                    hoveredNoteIndex = -1;
                    repaint();
                }
                setMouseCursor(juce::MouseCursor::NormalCursor);
                return;
            }

            const auto noteGrid = getNoteGridBounds(grid);
            if (!noteGrid.contains(e.getPosition()))
            {
                if (hoveredNoteIndex != -1)
                {
                    hoveredNoteIndex = -1;
                    repaint();
                }
                setMouseCursor(juce::MouseCursor::NormalCursor);
                return;
            }

            ResizeEdge resizeEdge = ResizeEdge::None;
            const int hitIndex = findNoteAtPosition(e.position, noteGrid, &resizeEdge);
            if (hitIndex != hoveredNoteIndex)
            {
                hoveredNoteIndex = hitIndex;
                repaint();
            }

            if (activeTool == EditTool::Draw || activeTool == EditTool::Erase)
                setMouseCursor(juce::MouseCursor::CrosshairCursor);
            else if (hitIndex >= 0 && resizeEdge != ResizeEdge::None)
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            else if (hitIndex >= 0)
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            else
                setMouseCursor(juce::MouseCursor::NormalCursor);
        }

        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (clip == nullptr)
                return;

            const auto grid = getGridBounds();
            const auto noteGrid = getNoteGridBounds(grid);
            if (!noteGrid.contains(e.getPosition()))
                return;

            clampViewWindow();
            const double visibleSpan = getVisibleBeats();

            if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            {
                const double minSpan = juce::jmax(snapBeat, 0.25);
                const double maxSpan = juce::jmax(minSpan, clip->lengthBeats);
                const double factor = wheel.deltaY > 0.0f ? 0.9 : 1.1;
                const double beatAtCursor = getRawBeatForXInGrid(e.position.x, noteGrid);
                const float ratio = juce::jlimit(0.0f, 1.0f,
                                                 (e.position.x - static_cast<float>(noteGrid.getX()))
                                                     / static_cast<float>(juce::jmax(1, noteGrid.getWidth())));
                viewLengthBeats = juce::jlimit(minSpan, maxSpan, visibleSpan * factor);
                viewStartBeat = beatAtCursor - (viewLengthBeats * ratio);
                clampViewWindow();
                updateScrollBars();
                repaint();
                return;
            }

            if (e.mods.isShiftDown())
            {
                zoomVerticalByRows(wheel.deltaY > 0.0f ? -2 : +2);
                return;
            }

            const float deltaAxis = std::abs(wheel.deltaX) > 0.0001f ? wheel.deltaX : wheel.deltaY;
            viewStartBeat -= static_cast<double>(deltaAxis) * visibleSpan * 0.18;
            clampViewWindow();
            updateScrollBars();
            repaint();
        }

        void mouseUp(const juce::MouseEvent&) override
        {
            draggingNote = false;
            resizingNote = false;
            resizingFromLeft = false;
            draggingCC = false;
            velocityDragging = false;
            marqueeSelecting = false;
            brushPainting = false;
            draggedNoteIndex = -1;
            dragSourceIndices.clear();
            dragSourceEvents.clear();
            duplicateDragInProgress = false;
        }

        void resized() override
        {
            auto controls = getControlBounds().reduced(4, 2);
            auto row1 = controls.removeFromTop((controls.getHeight() - 4) / 2);
            controls.removeFromTop(4);
            auto row2 = controls;

            rootSelector.setBounds(row1.removeFromLeft(58));
            scaleSelector.setBounds(row1.removeFromLeft(108));
            snapSelector.setBounds(row1.removeFromLeft(96));
            row1.removeFromLeft(6);
            swingSlider.setBounds(row1.removeFromLeft(88));
            lengthSelector.setBounds(row1.removeFromLeft(82));
            row1.removeFromLeft(6);
            zoomSelector.setBounds(row1.removeFromLeft(90));
            row1.removeFromLeft(6);
            velocitySlider.setBounds(row1.removeFromLeft(120));
            row1.removeFromLeft(6);
            ccSelector.setBounds(row1.removeFromLeft(130));
            row1.removeFromLeft(6);
            quantizeButton.setBounds(row1.removeFromLeft(88));
            row1.removeFromLeft(6);
            humanizeButton.setBounds(row1.removeFromLeft(88));
            row1.removeFromLeft(6);
            aiAssistButton.setBounds(row1.removeFromLeft(88));
            row1.removeFromLeft(6);
            progressionSelector.setBounds(row1.removeFromLeft(124));
            row1.removeFromLeft(4);
            generateProgressionButton.setBounds(row1.removeFromLeft(78));
            row1.removeFromLeft(4);
            regenerateProgressionButton.setBounds(row1.removeFromLeft(66));

            selectToolButton.setBounds(row2.removeFromLeft(64));
            drawToolButton.setBounds(row2.removeFromLeft(58));
            eraseToolButton.setBounds(row2.removeFromLeft(58));
            row2.removeFromLeft(6);
            scaleSnapButton.setBounds(row2.removeFromLeft(86));
            stepInputButton.setBounds(row2.removeFromLeft(72));
            octaveDownButton.setBounds(row2.removeFromLeft(48));
            octaveUpButton.setBounds(row2.removeFromLeft(48));
            row2.removeFromLeft(6);
            horizZoomOutButton.setBounds(row2.removeFromLeft(38));
            horizZoomInButton.setBounds(row2.removeFromLeft(38));
            row2.removeFromLeft(4);
            vertZoomOutButton.setBounds(row2.removeFromLeft(38));
            vertZoomInButton.setBounds(row2.removeFromLeft(38));

            horizontalScrollBar.setBounds(getHorizontalScrollBarBounds());
            verticalScrollBar.setBounds(getVerticalScrollBarBounds());
            updateScrollBars();
        }

        void scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override
        {
            if (updatingScrollBars || clip == nullptr)
                return;

            if (scrollBarThatHasMoved == &horizontalScrollBar)
            {
                viewStartBeat = newRangeStart;
                clampViewWindow();
                updateScrollBars();
                repaint();
                return;
            }

            if (scrollBarThatHasMoved == &verticalScrollBar)
            {
                const int maxLowest = juce::jmax(0, 127 - visibleNoteCount);
                const int mappedLowest = maxLowest - static_cast<int>(std::round(newRangeStart));
                lowestVisibleNote = juce::jlimit(0, maxLowest, mappedLowest);
                updateScrollBars();
                repaint();
            }
        }

    private:
        void paintPianoKeys(juce::Graphics& g, juce::Rectangle<int> keys)
        {
            if (clip == nullptr || keys.isEmpty())
                return;

            const int highestVisibleNote = getHighestVisibleNote();
            const float noteHeight = static_cast<float>(keys.getHeight()) / static_cast<float>(visibleNoteCount);

            for (int row = 0; row < visibleNoteCount; ++row)
            {
                const int note = highestVisibleNote - row;
                const float y = keys.getY() + (row * noteHeight);
                const bool black = isBlackKey(note);

                g.setColour(black ? juce::Colour::fromRGB(28, 31, 36)
                                  : juce::Colour::fromRGB(54, 58, 64));
                g.fillRect(keys.getX(), static_cast<int>(y), keys.getWidth(), static_cast<int>(std::ceil(noteHeight)));

                if (note % 12 == 0)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.75f));
                    g.setFont(10.0f);
                    g.drawText("C" + juce::String((note / 12) - 1),
                               keys.getX() + 4,
                               static_cast<int>(y),
                               keys.getWidth() - 6,
                               static_cast<int>(std::ceil(noteHeight)),
                               juce::Justification::centredLeft);
                }

                g.setColour(juce::Colours::black.withAlpha(0.35f));
                g.drawHorizontalLine(static_cast<int>(y), static_cast<float>(keys.getX()), static_cast<float>(keys.getRight()));
            }

            g.setColour(juce::Colours::white.withAlpha(0.2f));
            g.drawRect(keys);
        }

        void paintNoteGrid(juce::Graphics& g, juce::Rectangle<int> grid)
        {
            const double visibleBeats = getVisibleBeats();
            const double viewEndBeat = viewStartBeat + visibleBeats;
            const int highestVisibleNote = getHighestVisibleNote();
            const float noteHeight = static_cast<float>(grid.getHeight()) / static_cast<float>(visibleNoteCount);
            const float beatWidth = static_cast<float>(grid.getWidth()) / static_cast<float>(visibleBeats);

            for (int row = 0; row < visibleNoteCount; ++row)
            {
                const int note = highestVisibleNote - row;
                const float y = grid.getY() + (row * noteHeight);

                juce::Colour rowColour = isBlackKey(note)
                    ? juce::Colour::fromRGB(28, 30, 34)
                    : juce::Colour::fromRGB(35, 38, 42);

                if (isInSelectedScale(note))
                    rowColour = rowColour.interpolatedWith(juce::Colour::fromRGB(56, 100, 77), 0.40f);

                if (((note % 12) + 12) % 12 == rootNote)
                    rowColour = rowColour.interpolatedWith(theme::Colours::accent(), 0.25f);

                g.setColour(rowColour);
                g.fillRect(static_cast<float>(grid.getX()), y, static_cast<float>(grid.getWidth()), noteHeight);

                g.setColour(juce::Colours::black.withAlpha(0.2f));
                g.drawLine(static_cast<float>(grid.getX()), y, static_cast<float>(grid.getRight()), y);
            }

            const double gridStartBeat = std::floor(viewStartBeat / snapBeat) * snapBeat;
            for (double beat = gridStartBeat; beat <= viewEndBeat + 0.0001; beat += snapBeat)
            {
                const float x = grid.getX() + static_cast<float>((beat - viewStartBeat) * beatWidth);
                const bool isBeatLine = std::abs(std::fmod(beat, 1.0)) < 0.0001;
                const bool isBarLine = std::abs(std::fmod(beat, 4.0)) < 0.0001;
                g.setColour(isBarLine ? juce::Colours::white.withAlpha(0.22f)
                                      : (isBeatLine ? juce::Colours::white.withAlpha(0.14f)
                                                    : juce::Colours::white.withAlpha(0.08f)));
                g.drawLine(x, static_cast<float>(grid.getY()), x, static_cast<float>(grid.getBottom()));
            }

            for (int i = 0; i < static_cast<int>(clip->events.size()); ++i)
            {
                const auto& ev = clip->events[static_cast<size_t>(i)];
                if (ev.noteNumber < lowestVisibleNote || ev.noteNumber > highestVisibleNote)
                    continue;
                const double eventEndBeat = ev.startBeat + ev.durationBeats;
                if (eventEndBeat < viewStartBeat || ev.startBeat > viewEndBeat)
                    continue;

                const float x = grid.getX() + static_cast<float>((ev.startBeat - viewStartBeat) * beatWidth);
                const float y = grid.getY() + static_cast<float>(highestVisibleNote - ev.noteNumber) * noteHeight;
                const float w = juce::jmax(2.0f, static_cast<float>(ev.durationBeats * beatWidth) - 1.0f);
                const float h = juce::jmax(2.0f, noteHeight - 1.0f);

                juce::Rectangle<float> noteRect(x, y, w, h);
                juce::Colour noteColour = theme::Colours::accent().withBrightness(0.55f + (static_cast<float>(ev.velocity) / 127.0f) * 0.45f);
                const bool selected = selectedNoteIndices.count(i) > 0;
                const bool hovered = i == hoveredNoteIndex;
                if (selected)
                    noteColour = noteColour.brighter(0.35f);

                g.setColour(noteColour);
                g.fillRoundedRectangle(noteRect, 2.0f);
                g.setColour(juce::Colours::white.withAlpha(selected ? 0.95f : (hovered ? 0.88f : 0.72f)));
                g.drawRoundedRectangle(noteRect, 2.0f, selected ? 1.8f : (hovered ? 1.4f : 1.0f));

                if (selected || hovered)
                {
                    const float handleWidth = juce::jlimit(4.0f, resizeHandleWidth, noteRect.getWidth() * 0.24f);
                    const bool canLeftResize = noteRect.getWidth() >= minNoteWidthForLeftResize;
                    juce::Rectangle<float> leftHandle(noteRect.getX(), noteRect.getY(), handleWidth, noteRect.getHeight());
                    juce::Rectangle<float> rightHandle(noteRect.getRight() - handleWidth, noteRect.getY(), handleWidth, noteRect.getHeight());
                    g.setColour(juce::Colours::white.withAlpha(selected ? 0.75f : 0.55f));
                    if (canLeftResize)
                        g.fillRect(leftHandle);
                    g.fillRect(rightHandle);
                }
            }

            const float stepX = grid.getX() + static_cast<float>((stepInputBeat - viewStartBeat) * beatWidth);
            if (stepX >= grid.getX() && stepX <= grid.getRight())
            {
                g.setColour(juce::Colours::yellow.withAlpha(0.85f));
                g.drawLine(stepX, static_cast<float>(grid.getY()), stepX, static_cast<float>(grid.getBottom()), 1.4f);
            }
        }

        void paintVelocityLane(juce::Graphics& g, juce::Rectangle<int> velGrid)
        {
            if (clip == nullptr || velGrid.isEmpty())
                return;

            g.setColour(theme::Colours::panel().darker(0.15f));
            g.fillRect(velGrid);

            const double visibleBeats = getVisibleBeats();
            const double viewEndBeat = viewStartBeat + visibleBeats;
            const float beatWidth = static_cast<float>(velGrid.getWidth()) / static_cast<float>(visibleBeats);
            const double gridStartBeat = std::floor(viewStartBeat / snapBeat) * snapBeat;
            for (double beat = gridStartBeat; beat <= viewEndBeat + 0.0001; beat += snapBeat)
            {
                const float x = velGrid.getX() + static_cast<float>((beat - viewStartBeat) * beatWidth);
                const bool isBeatLine = std::abs(std::fmod(beat, 1.0)) < 0.0001;
                const bool isBarLine = std::abs(std::fmod(beat, 4.0)) < 0.0001;
                g.setColour(isBarLine ? juce::Colours::white.withAlpha(0.2f)
                                      : (isBeatLine ? juce::Colours::white.withAlpha(0.11f)
                                                    : juce::Colours::white.withAlpha(0.06f)));
                g.drawLine(x, static_cast<float>(velGrid.getY()), x, static_cast<float>(velGrid.getBottom()));
            }

            for (int i = 0; i < static_cast<int>(clip->events.size()); ++i)
            {
                const auto& ev = clip->events[static_cast<size_t>(i)];
                if (ev.startBeat < viewStartBeat || ev.startBeat > viewEndBeat)
                    continue;
                const float x = velGrid.getX() + static_cast<float>((ev.startBeat - viewStartBeat) * beatWidth);
                const float valueNorm = static_cast<float>(ev.velocity) / 127.0f;
                const float y = velGrid.getBottom() - valueNorm * static_cast<float>(velGrid.getHeight());
                const bool selected = selectedNoteIndices.count(i) > 0;

                g.setColour(selected ? theme::Colours::accent().brighter(0.25f) : theme::Colours::accent().withAlpha(0.55f));
                g.drawLine(x, static_cast<float>(velGrid.getBottom()), x, y, selected ? 2.0f : 1.3f);
                g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
            }

            g.setColour(juce::Colours::white.withAlpha(0.2f));
            g.drawRect(velGrid);
        }

        void paintCCLane(juce::Graphics& g, juce::Rectangle<int> ccGrid)
        {
            g.setColour(theme::Colours::panel().darker(0.25f));
            g.fillRect(ccGrid);

            const double visibleBeats = getVisibleBeats();
            const double viewEndBeat = viewStartBeat + visibleBeats;
            const float beatWidth = static_cast<float>(ccGrid.getWidth()) / static_cast<float>(visibleBeats);

            const double gridStartBeat = std::floor(viewStartBeat / snapBeat) * snapBeat;
            for (double beat = gridStartBeat; beat <= viewEndBeat + 0.0001; beat += snapBeat)
            {
                const float x = ccGrid.getX() + static_cast<float>((beat - viewStartBeat) * beatWidth);
                const bool isBeatLine = std::abs(std::fmod(beat, 1.0)) < 0.0001;
                const bool isBarLine = std::abs(std::fmod(beat, 4.0)) < 0.0001;
                g.setColour(isBarLine ? juce::Colours::white.withAlpha(0.2f)
                                      : (isBeatLine ? juce::Colours::white.withAlpha(0.12f)
                                                    : juce::Colours::white.withAlpha(0.07f)));
                g.drawLine(x, static_cast<float>(ccGrid.getY()), x, static_cast<float>(ccGrid.getBottom()));
            }

            if (isControllerLaneSelected())
            {
                const int controller = getSelectedController();
                for (const auto& cc : clip->ccEvents)
                {
                    if (cc.controller != controller || cc.beat < viewStartBeat || cc.beat > viewEndBeat)
                        continue;
                    const float x = ccGrid.getX() + static_cast<float>((cc.beat - viewStartBeat) * beatWidth);
                    const float valueNorm = static_cast<float>(cc.value) / 127.0f;
                    const float h = valueNorm * static_cast<float>(ccGrid.getHeight());
                    g.setColour(theme::Colours::accent().withAlpha(0.78f));
                    g.fillRect(x - 2.0f, ccGrid.getBottom() - h, 4.0f, h);
                }
            }
            else if (isPitchBendLaneSelected())
            {
                for (const auto& bend : clip->pitchBendEvents)
                {
                    if (bend.beat < viewStartBeat || bend.beat > viewEndBeat)
                        continue;
                    const float x = ccGrid.getX() + static_cast<float>((bend.beat - viewStartBeat) * beatWidth);
                    const float valueNorm = static_cast<float>(juce::jlimit(0, 16383, bend.value)) / 16383.0f;
                    const float h = valueNorm * static_cast<float>(ccGrid.getHeight());
                    g.setColour(juce::Colours::orange.withAlpha(0.8f));
                    g.fillRect(x - 2.0f, ccGrid.getBottom() - h, 4.0f, h);
                }
            }
            else if (isChannelPressureLaneSelected())
            {
                for (const auto& pressure : clip->channelPressureEvents)
                {
                    if (pressure.beat < viewStartBeat || pressure.beat > viewEndBeat)
                        continue;
                    const float x = ccGrid.getX() + static_cast<float>((pressure.beat - viewStartBeat) * beatWidth);
                    const float valueNorm = static_cast<float>(pressure.pressure) / 127.0f;
                    const float h = valueNorm * static_cast<float>(ccGrid.getHeight());
                    g.setColour(juce::Colours::cyan.withAlpha(0.75f));
                    g.fillRect(x - 2.0f, ccGrid.getBottom() - h, 4.0f, h);
                }
            }
            else if (isPolyAftertouchLaneSelected())
            {
                for (const auto& poly : clip->polyAftertouchEvents)
                {
                    if (poly.beat < viewStartBeat || poly.beat > viewEndBeat)
                        continue;
                    const float x = ccGrid.getX() + static_cast<float>((poly.beat - viewStartBeat) * beatWidth);
                    const float valueNorm = static_cast<float>(poly.pressure) / 127.0f;
                    const float h = valueNorm * static_cast<float>(ccGrid.getHeight());
                    g.setColour(juce::Colours::violet.withAlpha(0.78f));
                    g.fillRect(x - 2.0f, ccGrid.getBottom() - h, 4.0f, h);
                }
            }
            else if (isProgramChangeLaneSelected())
            {
                for (const auto& program : clip->programChangeEvents)
                {
                    if (program.beat < viewStartBeat || program.beat > viewEndBeat || program.program < 0)
                        continue;
                    const float x = ccGrid.getX() + static_cast<float>((program.beat - viewStartBeat) * beatWidth);
                    const float valueNorm = static_cast<float>(program.program) / 127.0f;
                    const float h = valueNorm * static_cast<float>(ccGrid.getHeight());
                    g.setColour(juce::Colours::yellow.withAlpha(0.8f));
                    g.fillRect(x - 2.0f, ccGrid.getBottom() - h, 4.0f, h);
                }
            }

            g.setColour(juce::Colours::white.withAlpha(0.25f));
            g.drawRect(ccGrid);
        }

        void handleCCMouseDown(const juce::MouseEvent& e, juce::Rectangle<int> ccGrid)
        {
            const double visibleBeats = getVisibleBeats();
            const float beatWidth = static_cast<float>(ccGrid.getWidth()) / static_cast<float>(visibleBeats);
            const double rawBeat = viewStartBeat + ((e.position.x - ccGrid.getX()) / beatWidth);
            const double beat = juce::jlimit(0.0, clip->lengthBeats, quantizeBeatToGrid(rawBeat));
            const float normalized = juce::jlimit(0.0f, 1.0f, (ccGrid.getBottom() - e.position.y) / static_cast<float>(ccGrid.getHeight()));

            if (isControllerLaneSelected())
            {
                const uint8_t value = static_cast<uint8_t>(juce::jlimit(0, 127, static_cast<int>(std::round(normalized * 127.0f))));
                const int controller = getSelectedController();
                if (e.mods.isRightButtonDown())
                {
                    performClipEdit("Delete CC Event", [beat, controller, this](Clip& target)
                    {
                        auto it = std::remove_if(target.ccEvents.begin(), target.ccEvents.end(),
                                                 [&](const MidiCCEvent& cc)
                                                 {
                                                     return cc.controller == controller
                                                         && std::abs(cc.beat - beat) <= (snapBeat * 0.5);
                                                 });
                        target.ccEvents.erase(it, target.ccEvents.end());
                    });
                    return;
                }

                performClipEdit("Edit CC Lane", [beat, value, controller, this](Clip& target)
                {
                    bool updated = false;
                    for (auto& cc : target.ccEvents)
                    {
                        if (cc.controller == controller && std::abs(cc.beat - beat) <= (snapBeat * 0.5))
                        {
                            cc.beat = beat;
                            cc.value = value;
                            updated = true;
                            break;
                        }
                    }
                    if (!updated)
                        target.ccEvents.push_back({ beat, controller, value });
                    std::sort(target.ccEvents.begin(), target.ccEvents.end(),
                              [](const MidiCCEvent& a, const MidiCCEvent& b) { return a.beat < b.beat; });
                });
                return;
            }

            if (isPitchBendLaneSelected())
            {
                const int value = juce::jlimit(0, 16383, static_cast<int>(std::round(normalized * 16383.0f)));
                performClipEdit(e.mods.isRightButtonDown() ? "Delete Pitch Bend" : "Edit Pitch Bend", [beat, value, this, isDelete = e.mods.isRightButtonDown()](Clip& target)
                {
                    if (isDelete)
                    {
                        auto it = std::remove_if(target.pitchBendEvents.begin(), target.pitchBendEvents.end(),
                                                 [&](const MidiPitchBendEvent& ev) { return std::abs(ev.beat - beat) <= (snapBeat * 0.5); });
                        target.pitchBendEvents.erase(it, target.pitchBendEvents.end());
                    }
                    else
                    {
                        bool updated = false;
                        for (auto& ev : target.pitchBendEvents)
                        {
                            if (std::abs(ev.beat - beat) <= (snapBeat * 0.5))
                            {
                                ev.beat = beat;
                                ev.value = value;
                                updated = true;
                                break;
                            }
                        }
                        if (!updated)
                            target.pitchBendEvents.push_back({ beat, value });
                        std::sort(target.pitchBendEvents.begin(), target.pitchBendEvents.end(),
                                  [](const MidiPitchBendEvent& a, const MidiPitchBendEvent& b) { return a.beat < b.beat; });
                    }
                });
                return;
            }

            if (isChannelPressureLaneSelected())
            {
                const uint8_t value = static_cast<uint8_t>(juce::jlimit(0, 127, static_cast<int>(std::round(normalized * 127.0f))));
                performClipEdit(e.mods.isRightButtonDown() ? "Delete Channel Pressure" : "Edit Channel Pressure", [beat, value, this, isDelete = e.mods.isRightButtonDown()](Clip& target)
                {
                    if (isDelete)
                    {
                        auto it = std::remove_if(target.channelPressureEvents.begin(), target.channelPressureEvents.end(),
                                                 [&](const MidiChannelPressureEvent& ev) { return std::abs(ev.beat - beat) <= (snapBeat * 0.5); });
                        target.channelPressureEvents.erase(it, target.channelPressureEvents.end());
                    }
                    else
                    {
                        bool updated = false;
                        for (auto& ev : target.channelPressureEvents)
                        {
                            if (std::abs(ev.beat - beat) <= (snapBeat * 0.5))
                            {
                                ev.beat = beat;
                                ev.pressure = value;
                                updated = true;
                                break;
                            }
                        }
                        if (!updated)
                            target.channelPressureEvents.push_back({ beat, value });
                        std::sort(target.channelPressureEvents.begin(), target.channelPressureEvents.end(),
                                  [](const MidiChannelPressureEvent& a, const MidiChannelPressureEvent& b) { return a.beat < b.beat; });
                    }
                });
                return;
            }

            if (isPolyAftertouchLaneSelected())
            {
                const uint8_t value = static_cast<uint8_t>(juce::jlimit(0, 127, static_cast<int>(std::round(normalized * 127.0f))));
                const int note = (selectedNoteIndex >= 0 && selectedNoteIndex < static_cast<int>(clip->events.size()))
                    ? clip->events[static_cast<size_t>(selectedNoteIndex)].noteNumber
                    : 60;
                performClipEdit(e.mods.isRightButtonDown() ? "Delete Poly Aftertouch" : "Edit Poly Aftertouch", [beat, value, note, this, isDelete = e.mods.isRightButtonDown()](Clip& target)
                {
                    if (isDelete)
                    {
                        auto it = std::remove_if(target.polyAftertouchEvents.begin(), target.polyAftertouchEvents.end(),
                                                 [&](const MidiPolyAftertouchEvent& ev)
                                                 {
                                                     return std::abs(ev.beat - beat) <= (snapBeat * 0.5)
                                                         && ev.noteNumber == note;
                                                 });
                        target.polyAftertouchEvents.erase(it, target.polyAftertouchEvents.end());
                    }
                    else
                    {
                        bool updated = false;
                        for (auto& ev : target.polyAftertouchEvents)
                        {
                            if (ev.noteNumber == note && std::abs(ev.beat - beat) <= (snapBeat * 0.5))
                            {
                                ev.beat = beat;
                                ev.pressure = value;
                                updated = true;
                                break;
                            }
                        }
                        if (!updated)
                            target.polyAftertouchEvents.push_back({ beat, note, value });
                        std::sort(target.polyAftertouchEvents.begin(), target.polyAftertouchEvents.end(),
                                  [](const MidiPolyAftertouchEvent& a, const MidiPolyAftertouchEvent& b) { return a.beat < b.beat; });
                    }
                });
                return;
            }

            if (isProgramChangeLaneSelected())
            {
                const int value = juce::jlimit(0, 127, static_cast<int>(std::round(normalized * 127.0f)));
                performClipEdit(e.mods.isRightButtonDown() ? "Delete Program Change" : "Edit Program Change", [beat, value, this, isDelete = e.mods.isRightButtonDown()](Clip& target)
                {
                    if (isDelete)
                    {
                        auto it = std::remove_if(target.programChangeEvents.begin(), target.programChangeEvents.end(),
                                                 [&](const MidiProgramChangeEvent& ev) { return std::abs(ev.beat - beat) <= (snapBeat * 0.5); });
                        target.programChangeEvents.erase(it, target.programChangeEvents.end());
                    }
                    else
                    {
                        bool updated = false;
                        for (auto& ev : target.programChangeEvents)
                        {
                            if (std::abs(ev.beat - beat) <= (snapBeat * 0.5))
                            {
                                ev.beat = beat;
                                ev.program = value;
                                updated = true;
                                break;
                            }
                        }
                        if (!updated)
                        {
                            MidiProgramChangeEvent ev;
                            ev.beat = beat;
                            ev.program = value;
                            target.programChangeEvents.push_back(ev);
                        }
                        std::sort(target.programChangeEvents.begin(), target.programChangeEvents.end(),
                                  [](const MidiProgramChangeEvent& a, const MidiProgramChangeEvent& b) { return a.beat < b.beat; });
                    }
                });
            }
        }

        void handleVelocityLaneMouseDown(const juce::MouseEvent& e, juce::Rectangle<int> velocityGrid)
        {
            if (clip == nullptr || velocityGrid.isEmpty())
                return;

            const double beat = getBeatForXInGrid(e.position.x, velocityGrid);
            const int hit = findClosestEventAtBeat(beat, snapBeat * 0.65);
            if (hit < 0)
                return;

            if (selectedNoteIndices.count(hit) == 0)
                selectSingleNote(hit);

            velocityDragging = true;
            applyVelocityFromPosition(e.position.y, velocityGrid);
        }

        void handleVelocityLaneMouseDrag(const juce::MouseEvent& e, juce::Rectangle<int> velocityGrid)
        {
            if (!velocityDragging || clip == nullptr || velocityGrid.isEmpty())
                return;

            applyVelocityFromPosition(e.position.y, velocityGrid);
        }

        void applyVelocityFromPosition(float y, juce::Rectangle<int> velocityGrid)
        {
            if (selectedNoteIndices.empty() || clip == nullptr)
                return;

            const float normalized = juce::jlimit(0.0f, 1.0f,
                                                  (static_cast<float>(velocityGrid.getBottom()) - y)
                                                  / static_cast<float>(juce::jmax(1, velocityGrid.getHeight())));
            const int newVelocity = juce::jlimit(1, 127, static_cast<int>(std::round(normalized * 127.0f)));
            if (newVelocity == lastVelocityDragValue)
                return;
            lastVelocityDragValue = newVelocity;

            const auto selected = std::vector<int>(selectedNoteIndices.begin(), selectedNoteIndices.end());
            performClipEdit("Edit MIDI Velocity",
                            [selected, newVelocity](Clip& target)
                            {
                                for (int idx : selected)
                                {
                                    if (juce::isPositiveAndBelow(idx, static_cast<int>(target.events.size())))
                                        target.events[static_cast<size_t>(idx)].velocity = static_cast<uint8_t>(newVelocity);
                                }
                            });
            defaultVelocity = newVelocity;
        }

        void clearNoteSelection()
        {
            selectedNoteIndices.clear();
            selectedNoteIndex = -1;
            updateVelocitySliderFromSelection();
            repaint();
        }

        void selectSingleNote(int index)
        {
            selectedNoteIndices.clear();
            if (index >= 0)
                selectedNoteIndices.insert(index);
            selectedNoteIndex = index;
            updateVelocitySliderFromSelection();
            repaint();
        }

        void toggleNoteSelection(int index)
        {
            if (index < 0)
                return;

            if (selectedNoteIndices.count(index) > 0)
                selectedNoteIndices.erase(index);
            else
                selectedNoteIndices.insert(index);

            selectedNoteIndex = selectedNoteIndices.empty() ? -1 : *selectedNoteIndices.begin();
            updateVelocitySliderFromSelection();
            repaint();
        }

        void updateMarqueeSelection(juce::Rectangle<int> noteGrid)
        {
            selectedNoteIndices.clear();
            if (clip == nullptr)
                return;

            for (int i = 0; i < static_cast<int>(clip->events.size()); ++i)
            {
                const auto rect = getEventRect(clip->events[static_cast<size_t>(i)], noteGrid);
                if (!rect.isEmpty() && marqueeRect.intersects(rect))
                    selectedNoteIndices.insert(i);
            }

            selectedNoteIndex = selectedNoteIndices.empty() ? -1 : *selectedNoteIndices.begin();
            updateVelocitySliderFromSelection();
        }

        void beginNoteDrag(double mouseBeat, int mouseNote, bool resizeSingleNote, ResizeEdge resizeEdge)
        {
            draggingNote = true;
            resizingNote = resizeSingleNote;
            resizingFromLeft = resizeSingleNote && resizeEdge == ResizeEdge::Left;
            dragStartMouseBeat = mouseBeat;
            dragStartMouseNote = mouseNote;
            lastDragDeltaBeat = 99999.0;
            lastDragDeltaNote = std::numeric_limits<int>::max();
            lastDragDuration = -1.0;
            lastDragStartBeat = -1.0;
            dragSourceIndices.assign(selectedNoteIndices.begin(), selectedNoteIndices.end());
            dragSourceEvents.clear();
            dragSourceEvents.reserve(dragSourceIndices.size());
            for (int idx : dragSourceIndices)
            {
                if (juce::isPositiveAndBelow(idx, static_cast<int>(clip->events.size())))
                    dragSourceEvents.push_back(clip->events[static_cast<size_t>(idx)]);
            }
        }

        void duplicateSelectedNotesForDrag()
        {
            if (clip == nullptr || selectedNoteIndices.empty())
                return;

            const auto selected = std::vector<int>(selectedNoteIndices.begin(), selectedNoteIndices.end());
            std::vector<TimelineEvent> source;
            source.reserve(selected.size());
            for (int idx : selected)
            {
                if (juce::isPositiveAndBelow(idx, static_cast<int>(clip->events.size())))
                    source.push_back(clip->events[static_cast<size_t>(idx)]);
            }
            if (source.empty())
                return;

            const int oldSize = static_cast<int>(clip->events.size());
            performClipEdit("Duplicate MIDI Notes",
                            [source](Clip& target)
                            {
                                target.events.insert(target.events.end(), source.begin(), source.end());
                            });

            selectedNoteIndices.clear();
            for (int i = 0; i < static_cast<int>(source.size()); ++i)
                selectedNoteIndices.insert(oldSize + i);
            selectedNoteIndex = selectedNoteIndices.empty() ? -1 : *selectedNoteIndices.begin();
            duplicateDragInProgress = true;
            updateVelocitySliderFromSelection();
            repaint();
        }

        void applyBrushAt(int note, double beat, bool erase)
        {
            if (clip == nullptr)
                return;

            const int snappedNote = scaleSnapEnabled ? snapNoteToScale(note) : juce::jlimit(0, 127, note);
            const double maxBeat = juce::jmax(0.0, clip->lengthBeats - 0.0625);
            const double snappedBeat = juce::jlimit(0.0, maxBeat, quantizeBeatToGrid(beat));

            if (snappedNote == lastBrushNote && std::abs(snappedBeat - lastBrushBeat) < 0.0001)
                return;

            lastBrushNote = snappedNote;
            lastBrushBeat = snappedBeat;

            if (erase)
            {
                performClipEdit("Brush Erase MIDI",
                                [snappedNote, snappedBeat, this](Clip& target)
                                {
                                    auto it = std::remove_if(target.events.begin(), target.events.end(),
                                                             [&](const TimelineEvent& ev)
                                                             {
                                                                 return ev.noteNumber == snappedNote
                                                                     && std::abs(ev.startBeat - snappedBeat) <= (snapBeat * 0.25);
                                                             });
                                    target.events.erase(it, target.events.end());
                                });
                if (selectedNoteIndex >= 0 && selectedNoteIndices.count(selectedNoteIndex) == 0)
                    selectedNoteIndex = selectedNoteIndices.empty() ? -1 : *selectedNoteIndices.begin();
                repaint();
                return;
            }

            const int velocity = juce::jlimit(1, 127, static_cast<int>(std::round(velocitySlider.getValue())));
            performClipEdit("Brush Draw MIDI",
                            [snappedNote, snappedBeat, velocity, this](Clip& target)
                            {
                                auto existing = std::remove_if(target.events.begin(), target.events.end(),
                                                               [&](const TimelineEvent& ev)
                                                               {
                                                                   return ev.noteNumber == snappedNote
                                                                       && std::abs(ev.startBeat - snappedBeat) <= (snapBeat * 0.25);
                                                               });
                                target.events.erase(existing, target.events.end());

                                TimelineEvent ev;
                                ev.noteNumber = snappedNote;
                                ev.startBeat = snappedBeat;
                                const double remaining = juce::jmax(0.0625, target.lengthBeats - ev.startBeat);
                                ev.durationBeats = juce::jlimit(0.0625, remaining, noteLengthBeats);
                                ev.velocity = static_cast<uint8_t>(velocity);
                                target.events.push_back(ev);
                            });

            selectedNoteIndex = findNoteAt(snappedNote, snappedBeat);
            selectedNoteIndices.clear();
            if (selectedNoteIndex >= 0)
                selectedNoteIndices.insert(selectedNoteIndex);
            stepInputBeat = juce::jlimit(0.0, maxBeat, snappedBeat + noteLengthBeats);
            ensureBeatVisible(stepInputBeat);
            updateVelocitySliderFromSelection();
        }

        int findNoteAtPosition(juce::Point<float> position, juce::Rectangle<int> noteGrid, ResizeEdge* resizeEdge) const
        {
            if (clip == nullptr)
                return -1;

            for (int i = static_cast<int>(clip->events.size()) - 1; i >= 0; --i)
            {
                const auto& ev = clip->events[static_cast<size_t>(i)];
                const auto rect = getEventRect(ev, noteGrid);
                if (!rect.contains(position))
                    continue;

                if (resizeEdge != nullptr)
                {
                    const float handleWidth = juce::jlimit(4.0f, resizeHandleWidth, rect.getWidth() * 0.24f);
                    const bool canLeftResize = rect.getWidth() >= minNoteWidthForLeftResize;
                    const bool onLeft = canLeftResize && position.x <= (rect.getX() + handleWidth);
                    const bool onRight = position.x >= (rect.getRight() - handleWidth);
                    if (onLeft || onRight)
                    {
                        const float leftDistance = std::abs(position.x - rect.getX());
                        const float rightDistance = std::abs(rect.getRight() - position.x);
                        *resizeEdge = leftDistance <= rightDistance ? ResizeEdge::Left : ResizeEdge::Right;
                    }
                    else
                    {
                        *resizeEdge = ResizeEdge::None;
                    }
                }
                return i;
            }

            if (resizeEdge != nullptr)
                *resizeEdge = ResizeEdge::None;
            return -1;
        }

        double getVisibleBeats() const
        {
            if (clip == nullptr)
                return 4.0;

            const double minSpan = juce::jmax(snapBeat, 0.25);
            const double maxSpan = juce::jmax(minSpan, clip->lengthBeats);
            return juce::jlimit(minSpan, maxSpan, viewLengthBeats);
        }

        double getRawBeatForXInGrid(float x, juce::Rectangle<int> grid) const
        {
            const double visibleBeats = getVisibleBeats();
            const float beatWidth = static_cast<float>(grid.getWidth()) / static_cast<float>(visibleBeats);
            const double rawBeat = viewStartBeat + ((x - grid.getX()) / beatWidth);
            return juce::jlimit(0.0, juce::jmax(0.0, clip != nullptr ? clip->lengthBeats : 4.0), rawBeat);
        }

        double getSwingOffsetBeats() const
        {
            if (swingPercent <= 50)
                return 0.0;

            const double normalized = static_cast<double>(swingPercent - 50) / 25.0;
            return juce::jmax(0.0, snapBeat * 0.5 * normalized);
        }

        double applySwingToGridBeat(double straightGridBeat) const
        {
            const double step = juce::jmax(1.0 / 128.0, snapBeat);
            const double swingOffset = getSwingOffsetBeats();
            if (swingOffset <= 1.0e-9)
                return straightGridBeat;

            const int64_t gridIndex = static_cast<int64_t>(std::llround(straightGridBeat / step));
            const bool oddSubdivision = (std::abs(gridIndex) & 1LL) != 0LL;
            return oddSubdivision ? (straightGridBeat + swingOffset) : straightGridBeat;
        }

        double quantizeBeatToGrid(double rawBeat) const
        {
            const double step = juce::jmax(1.0 / 128.0, snapBeat);
            const int nearestIndex = juce::roundToInt(rawBeat / step);
            double bestBeat = applySwingToGridBeat(static_cast<double>(nearestIndex) * step);
            double bestDistance = std::abs(rawBeat - bestBeat);

            for (int idx = nearestIndex - 2; idx <= nearestIndex + 2; ++idx)
            {
                const double candidateBeat = applySwingToGridBeat(static_cast<double>(idx) * step);
                const double candidateDistance = std::abs(rawBeat - candidateBeat);
                if (candidateDistance < bestDistance)
                {
                    bestDistance = candidateDistance;
                    bestBeat = candidateBeat;
                }
            }

            return bestBeat;
        }

        void clampViewWindow()
        {
            if (clip == nullptr)
            {
                viewStartBeat = 0.0;
                viewLengthBeats = 4.0;
                return;
            }

            const double minSpan = juce::jmax(snapBeat, 0.25);
            const double maxSpan = juce::jmax(minSpan, clip->lengthBeats);
            viewLengthBeats = juce::jlimit(minSpan, maxSpan, viewLengthBeats);
            const double maxStart = juce::jmax(0.0, clip->lengthBeats - viewLengthBeats);
            viewStartBeat = juce::jlimit(0.0, maxStart, viewStartBeat);
            stepInputBeat = juce::jlimit(0.0, juce::jmax(0.0, clip->lengthBeats - 0.0625), stepInputBeat);
        }

        void ensureBeatVisible(double beat)
        {
            if (clip == nullptr)
                return;

            clampViewWindow();
            const double span = getVisibleBeats();
            if (beat < viewStartBeat)
                viewStartBeat = beat;
            else if (beat > viewStartBeat + span)
                viewStartBeat = beat - (span * 0.8);
            clampViewWindow();
            updateScrollBars();
        }

        void autoScrollAtMouseX(float x, juce::Rectangle<int> noteGrid)
        {
            if (clip == nullptr || noteGrid.isEmpty())
                return;

            const float edgeThreshold = 18.0f;
            const double span = getVisibleBeats();
            const float leftEdge = static_cast<float>(noteGrid.getX()) + edgeThreshold;
            const float rightEdge = static_cast<float>(noteGrid.getRight()) - edgeThreshold;

            if (x < leftEdge)
            {
                viewStartBeat -= span * 0.035;
                clampViewWindow();
                updateScrollBars();
            }
            else if (x > rightEdge)
            {
                viewStartBeat += span * 0.035;
                clampViewWindow();
                updateScrollBars();
            }
        }

        double getBeatForXInGrid(float x, juce::Rectangle<int> grid) const
        {
            if (clip == nullptr || grid.isEmpty())
                return 0.0;

            const double visibleBeats = getVisibleBeats();
            const float beatWidth = static_cast<float>(grid.getWidth()) / static_cast<float>(visibleBeats);
            const double rawBeat = viewStartBeat + ((x - grid.getX()) / beatWidth);
            const double maxBeat = juce::jmax(0.0, clip->lengthBeats - 0.0625);
            return juce::jlimit(0.0, maxBeat, quantizeBeatToGrid(rawBeat));
        }

        int findClosestEventAtBeat(double beat, double toleranceBeats) const
        {
            if (clip == nullptr)
                return -1;

            int closest = -1;
            double bestDistance = toleranceBeats;
            for (int i = 0; i < static_cast<int>(clip->events.size()); ++i)
            {
                const auto& ev = clip->events[static_cast<size_t>(i)];
                const double distance = std::abs(ev.startBeat - beat);
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    closest = i;
                }
            }
            return closest;
        }

        std::pair<int, double> getNoteAndBeatAtPosition(juce::Point<float> position, juce::Rectangle<int> noteGrid) const
        {
            const double visibleBeats = getVisibleBeats();
            const float noteHeight = static_cast<float>(noteGrid.getHeight()) / static_cast<float>(visibleNoteCount);
            const float beatWidth = static_cast<float>(noteGrid.getWidth()) / static_cast<float>(visibleBeats);

            const int row = juce::jlimit(0, visibleNoteCount - 1, static_cast<int>((position.y - noteGrid.getY()) / noteHeight));
            const int note = getHighestVisibleNote() - row;

            const double rawBeat = viewStartBeat + ((position.x - noteGrid.getX()) / beatWidth);
            const double quantisedBeat = quantizeBeatToGrid(rawBeat);
            const double maxBeat = juce::jmax(0.0, clip->lengthBeats - 0.0625);
            const double beat = juce::jlimit(0.0, maxBeat, quantisedBeat);

            return { note, beat };
        }

        int findNoteAt(int note, double beat) const
        {
            if (clip == nullptr)
                return -1;

            for (int i = 0; i < static_cast<int>(clip->events.size()); ++i)
            {
                const auto& ev = clip->events[static_cast<size_t>(i)];
                if (ev.noteNumber != note)
                    continue;
                if (beat >= ev.startBeat - (snapBeat * 0.25) && beat <= (ev.startBeat + ev.durationBeats + (snapBeat * 0.25)))
                    return i;
            }

            return -1;
        }

        void performClipEdit(const juce::String& actionName, std::function<void(Clip&)> editFn)
        {
            if (clip == nullptr)
                return;

            if (onRequestClipEdit && clipIndex >= 0)
                onRequestClipEdit(clipIndex, actionName, std::move(editFn));
            else
                editFn(*clip);

            ensureSelectionValid();
            updateVelocitySliderFromSelection();
            repaint();
        }

        void ensureSelectionValid()
        {
            if (clip == nullptr)
            {
                selectedNoteIndex = -1;
                selectedNoteIndices.clear();
                return;
            }

            for (auto it = selectedNoteIndices.begin(); it != selectedNoteIndices.end();)
            {
                if (!juce::isPositiveAndBelow(*it, static_cast<int>(clip->events.size())))
                    it = selectedNoteIndices.erase(it);
                else
                    ++it;
            }

            if (selectedNoteIndex >= static_cast<int>(clip->events.size()))
                selectedNoteIndex = -1;
            if (selectedNoteIndex < 0 && !selectedNoteIndices.empty())
                selectedNoteIndex = *selectedNoteIndices.begin();
            if (selectedNoteIndex >= 0)
                selectedNoteIndices.insert(selectedNoteIndex);
        }

        void updateVelocitySliderFromSelection()
        {
            updatingVelocitySlider = true;
            if (clip != nullptr && selectedNoteIndex >= 0 && selectedNoteIndex < static_cast<int>(clip->events.size()))
                velocitySlider.setValue(static_cast<double>(clip->events[static_cast<size_t>(selectedNoteIndex)].velocity),
                                        juce::dontSendNotification);
            else
                velocitySlider.setValue(static_cast<double>(defaultVelocity), juce::dontSendNotification);
            updatingVelocitySlider = false;
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

        juce::Rectangle<int> getScrollableGridBounds(juce::Rectangle<int> fullGrid) const
        {
            auto area = fullGrid;
            if (area.getWidth() > pianoKeyWidth + scrollBarSize + 16)
                area.removeFromRight(scrollBarSize + scrollBarGap);
            if (area.getHeight() > scrollBarSize + 16)
                area.removeFromBottom(scrollBarSize + scrollBarGap);
            return area;
        }

        juce::Rectangle<int> getHorizontalScrollBarBounds() const
        {
            auto fullGrid = getGridBounds();
            auto scrollable = getScrollableGridBounds(fullGrid);
            auto bar = juce::Rectangle<int>(scrollable.getX() + pianoKeyWidth,
                                            scrollable.getBottom() + scrollBarGap,
                                            juce::jmax(0, scrollable.getWidth() - pianoKeyWidth),
                                            scrollBarSize);
            return bar;
        }

        juce::Rectangle<int> getVerticalScrollBarBounds() const
        {
            auto fullGrid = getGridBounds();
            auto noteGrid = getNoteGridBounds(fullGrid);
            auto bar = fullGrid;
            bar.removeFromLeft(noteGrid.getRight() + scrollBarGap);
            bar = bar.withY(noteGrid.getY()).withHeight(noteGrid.getHeight()).withWidth(scrollBarSize);
            return bar;
        }

        juce::Rectangle<int> getPianoKeyBounds(juce::Rectangle<int> fullGrid) const
        {
            auto noteArea = getNoteGridBounds(fullGrid);
            return { fullGrid.getX(), noteArea.getY(), pianoKeyWidth, noteArea.getHeight() };
        }

        juce::Rectangle<int> getNoteGridBounds(juce::Rectangle<int> fullGrid) const
        {
            auto area = getScrollableGridBounds(fullGrid);
            area.removeFromLeft(pianoKeyWidth);
            const int ccHeight = getCCLaneHeight(area.getHeight());
            area.removeFromBottom(ccHeight);
            area.removeFromBottom(6);
            const int velocityHeight = getVelocityLaneHeight(area.getHeight());
            area.removeFromBottom(velocityHeight);
            return area;
        }

        juce::Rectangle<int> getVelocityLaneBounds(juce::Rectangle<int> fullGrid) const
        {
            auto area = getScrollableGridBounds(fullGrid);
            area.removeFromLeft(pianoKeyWidth);
            const int ccHeight = getCCLaneHeight(area.getHeight());
            area.removeFromBottom(ccHeight);
            area.removeFromBottom(6);
            const int velocityHeight = getVelocityLaneHeight(area.getHeight());
            return area.removeFromBottom(velocityHeight);
        }

        juce::Rectangle<int> getCCLaneBounds(juce::Rectangle<int> fullGrid) const
        {
            auto area = getScrollableGridBounds(fullGrid);
            area.removeFromLeft(pianoKeyWidth);
            const int ccHeight = getCCLaneHeight(area.getHeight());
            return area.removeFromBottom(ccHeight);
        }

        void updateScrollBars()
        {
            if (updatingScrollBars)
                return;

            const juce::ScopedValueSetter<bool> updateGuard(updatingScrollBars, true);
            const auto fullGrid = getGridBounds();
            const auto noteGrid = getNoteGridBounds(fullGrid);

            const bool allowScrollbars = clip != nullptr
                                      && !fullGrid.isEmpty()
                                      && noteGrid.getWidth() > 16
                                      && noteGrid.getHeight() > 16;
            horizontalScrollBar.setVisible(allowScrollbars);
            verticalScrollBar.setVisible(allowScrollbars);
            if (!allowScrollbars)
                return;

            horizontalScrollBar.setBounds(getHorizontalScrollBarBounds());
            verticalScrollBar.setBounds(getVerticalScrollBarBounds());

            const double span = getVisibleBeats();
            const double totalBeats = juce::jmax(span, clip != nullptr ? juce::jmax(1.0, clip->lengthBeats) : 4.0);
            horizontalScrollBar.setRangeLimits(0.0, totalBeats);
            horizontalScrollBar.setCurrentRange(viewStartBeat, span);
            horizontalScrollBar.setSingleStepSize(juce::jmax(1.0 / 64.0, snapBeat));

            const int maxLowest = juce::jmax(0, 127 - visibleNoteCount);
            verticalScrollBar.setRangeLimits(0.0, static_cast<double>(juce::jmax(1, maxLowest + 1)));
            verticalScrollBar.setCurrentRange(static_cast<double>(maxLowest - lowestVisibleNote), 1.0);
            verticalScrollBar.setSingleStepSize(1.0);
        }

        int getVelocityLaneHeight(int remainingHeightAfterCC) const
        {
            return juce::jlimit(56, 112, remainingHeightAfterCC / 4);
        }

        int getCCLaneHeight(int fullGridHeight) const
        {
            return juce::jlimit(56, 128, fullGridHeight / 4);
        }

        bool isInSelectedScale(int note) const
        {
            const int pitchClass = ((note % 12) + 12) % 12;
            const auto& intervals = getScaleIntervals();
            for (int interval : intervals)
            {
                if (((rootNote + interval) % 12) == pitchClass)
                    return true;
            }
            return false;
        }

        const std::vector<int>& getScaleIntervals() const
        {
            static const std::array<std::vector<int>, 5> scales = {
                std::vector<int>{ 0, 2, 4, 5, 7, 9, 11 }, // Major
                std::vector<int>{ 0, 2, 3, 5, 7, 8, 10 }, // Minor
                std::vector<int>{ 0, 2, 3, 5, 7, 9, 10 }, // Dorian
                std::vector<int>{ 0, 2, 4, 5, 7, 9, 10 }, // Mixolydian
                std::vector<int>{ 0, 3, 5, 7, 10 }        // Minor Pentatonic
            };

            return scales[static_cast<size_t>(juce::jlimit(0, 4, scaleMode))];
        }

        int getHighestVisibleNote() const
        {
            return juce::jlimit(0, 127, lowestVisibleNote + visibleNoteCount - 1);
        }

        juce::Rectangle<float> getEventRect(const TimelineEvent& ev, juce::Rectangle<int> noteGrid) const
        {
            const int highestVisibleNote = getHighestVisibleNote();
            if (ev.noteNumber < lowestVisibleNote || ev.noteNumber > highestVisibleNote)
                return {};

            const double eventEndBeat = ev.startBeat + ev.durationBeats;
            const double visibleEndBeat = viewStartBeat + getVisibleBeats();
            if (eventEndBeat < viewStartBeat || ev.startBeat > visibleEndBeat)
                return {};

            const double visibleBeats = getVisibleBeats();
            const float noteHeight = static_cast<float>(noteGrid.getHeight()) / static_cast<float>(visibleNoteCount);
            const float beatWidth = static_cast<float>(noteGrid.getWidth()) / static_cast<float>(visibleBeats);
            const float x = noteGrid.getX() + static_cast<float>((ev.startBeat - viewStartBeat) * beatWidth);
            const float y = noteGrid.getY() + static_cast<float>(highestVisibleNote - ev.noteNumber) * noteHeight;
            const float w = juce::jmax(2.0f, static_cast<float>(ev.durationBeats * beatWidth) - 1.0f);
            const float h = juce::jmax(2.0f, noteHeight - 1.0f);
            return { x, y, w, h };
        }

        int snapNoteToScale(int note) const
        {
            const int clamped = juce::jlimit(0, 127, note);
            if (isInSelectedScale(clamped))
                return clamped;

            for (int distance = 1; distance < 12; ++distance)
            {
                const int up = juce::jlimit(0, 127, clamped + distance);
                if (isInSelectedScale(up))
                    return up;

                const int down = juce::jlimit(0, 127, clamped - distance);
                if (isInSelectedScale(down))
                    return down;
            }

            return clamped;
        }

        int getKeyboardSemitoneOffset(juce::juce_wchar ch) const
        {
            switch (ch)
            {
                case 'a': return 0;
                case 'w': return 1;
                case 's': return 2;
                case 'e': return 3;
                case 'd': return 4;
                case 'f': return 5;
                case 't': return 6;
                case 'g': return 7;
                case 'y': return 8;
                case 'h': return 9;
                case 'u': return 10;
                case 'j': return 11;
                case 'k': return 12;
                case 'o': return 13;
                case 'l': return 14;
                case 'p': return 15;
                case ';': return 16;
                default: return -1;
            }
        }

        std::vector<int> buildProgressionDegrees(int progressionId, bool regenerate) const
        {
            static const std::array<std::array<int, 4>, 25> baseProgressions =
            {{
                {{ 1, 5, 6, 4 }}, {{ 1, 4, 5, 1 }}, {{ 6, 4, 1, 5 }}, {{ 2, 5, 1, 6 }}, {{ 1, 6, 2, 5 }},
                {{ 1, 3, 6, 4 }}, {{ 1, 5, 4, 5 }}, {{ 4, 1, 5, 6 }}, {{ 6, 1, 4, 5 }}, {{ 1, 2, 6, 5 }},
                {{ 1, 7, 6, 5 }}, {{ 1, 4, 6, 5 }}, {{ 2, 6, 1, 5 }}, {{ 6, 5, 4, 5 }}, {{ 1, 4, 1, 5 }},
                {{ 1, 5, 3, 4 }}, {{ 1, 2, 4, 5 }}, {{ 6, 2, 5, 1 }}, {{ 1, 4, 2, 5 }}, {{ 1, 5, 6, 3 }},
                {{ 1, 4, 7, 3 }}, {{ 1, 6, 4, 5 }}, {{ 1, 5, 2, 4 }}, {{ 4, 5, 1, 1 }}, {{ 1, 1, 4, 5 }}
            }};

            progressionId = juce::jlimit(1, 100, progressionId);
            const int bankIndex = (progressionId - 1) % static_cast<int>(baseProgressions.size());
            const int variantGroup = (progressionId - 1) / static_cast<int>(baseProgressions.size()); // 0..3

            std::vector<int> out(baseProgressions[static_cast<size_t>(bankIndex)].begin(),
                                 baseProgressions[static_cast<size_t>(bankIndex)].end());

            if (variantGroup == 1)
            {
                std::rotate(out.begin(), out.begin() + 1, out.end());
            }
            else if (variantGroup == 2)
            {
                std::swap(out[1], out[2]);
            }
            else if (variantGroup == 3)
            {
                out.push_back(out[2]);
                out.push_back(5);
                out.push_back(out[0]);
                out.push_back(4);
            }

            if (regenerate)
            {
                juce::Random random;
                const int edits = juce::jlimit(1, 2, static_cast<int>(out.size() / 4));
                for (int i = 0; i < edits; ++i)
                {
                    const int index = random.nextInt(static_cast<int>(out.size()));
                    const int step = random.nextBool() ? 1 : -1;
                    out[static_cast<size_t>(index)] = juce::jlimit(1, 7, out[static_cast<size_t>(index)] + step);
                }
            }

            return out;
        }

        void generateChordProgression(bool regenerate)
        {
            if (clip == nullptr)
                return;

            const int progressionId = juce::jlimit(1, 100, juce::jmax(1, progressionSelector.getSelectedId()));
            const auto progressionDegrees = buildProgressionDegrees(progressionId, regenerate);
            const auto intervals = getScaleIntervals();
            if (progressionDegrees.empty() || intervals.empty())
                return;

            const int localRoot = rootNote;
            const double localSnap = snapBeat;
            const int baseVelocity = juce::jlimit(1, 127, static_cast<int>(std::round(velocitySlider.getValue())));
            const int regenerationToken = regenerate ? static_cast<int>(juce::Time::getMillisecondCounter())
                                                     : progressionId * 911;

            performClipEdit(regenerate ? "Regenerate Chord Progression" : "Generate Chord Progression",
                            [progressionId, progressionDegrees, intervals, localRoot, localSnap, baseVelocity, regenerationToken](Clip& target)
                            {
                                juce::Random random(static_cast<int64_t>(regenerationToken));

                                auto degreeToMidi = [localRoot, &intervals](int degree, int baseOctave)
                                {
                                    const int safeDegree = juce::jmax(1, degree);
                                    const int idx0 = safeDegree - 1;
                                    const int intervalIndex = idx0 % static_cast<int>(intervals.size());
                                    const int octaveShift = idx0 / static_cast<int>(intervals.size());
                                    const int midi = ((baseOctave + octaveShift + 1) * 12)
                                                     + localRoot
                                                     + intervals[static_cast<size_t>(intervalIndex)];
                                    return juce::jlimit(0, 127, midi);
                                };

                                const double clipLength = juce::jmax(4.0, target.lengthBeats);
                                target.lengthBeats = clipLength;
                                target.events.clear();

                                const int chordSteps = juce::jmax(4, static_cast<int>(std::round(clipLength)));
                                const double chordSpan = juce::jmax(localSnap, clipLength / static_cast<double>(chordSteps));
                                const double gate = 0.92;
                                std::vector<int> previousVoicing;

                                for (int step = 0; step < chordSteps; ++step)
                                {
                                    const int degree = progressionDegrees[static_cast<size_t>(step % static_cast<int>(progressionDegrees.size()))];
                                    const bool addSeventh = ((progressionId + step + random.nextInt(3)) % 3) == 0;

                                    std::vector<int> chord;
                                    chord.reserve(addSeventh ? 4 : 3);
                                    chord.push_back(degreeToMidi(degree, 3));
                                    chord.push_back(degreeToMidi(degree + 2, 3));
                                    chord.push_back(degreeToMidi(degree + 4, 3));
                                    if (addSeventh)
                                        chord.push_back(degreeToMidi(degree + 6, 3));

                                    std::vector<int> chosenVoicing = chord;
                                    if (!previousVoicing.empty())
                                    {
                                        double bestCost = std::numeric_limits<double>::max();
                                        for (int inversion = 0; inversion < static_cast<int>(chord.size()); ++inversion)
                                        {
                                            std::vector<int> candidate = chord;
                                            for (int r = 0; r < inversion; ++r)
                                            {
                                                const int lifted = candidate.front();
                                                candidate.erase(candidate.begin());
                                                candidate.push_back(lifted + 12);
                                            }

                                            double cost = 0.0;
                                            for (int note : candidate)
                                            {
                                                int nearest = 128;
                                                for (int previous : previousVoicing)
                                                    nearest = juce::jmin(nearest, std::abs(note - previous));
                                                cost += static_cast<double>(nearest);
                                            }

                                            if (cost < bestCost)
                                            {
                                                bestCost = cost;
                                                chosenVoicing = std::move(candidate);
                                            }
                                        }
                                    }

                                    const double startBeatBase = static_cast<double>(step) * chordSpan;
                                    const double startJitter = random.nextDouble() * localSnap * 0.1 - (localSnap * 0.05);
                                    const double startBeat = juce::jlimit(0.0,
                                                                          juce::jmax(0.0, clipLength - 0.0625),
                                                                          startBeatBase + startJitter);
                                    const double rawDuration = juce::jmax(0.125, chordSpan * gate);

                                    for (size_t noteIndex = 0; noteIndex < chosenVoicing.size(); ++noteIndex)
                                    {
                                        TimelineEvent ev;
                                        ev.startBeat = startBeat;
                                        const double maxDur = juce::jmax(0.0625, clipLength - ev.startBeat);
                                        ev.durationBeats = juce::jlimit(0.0625, maxDur, rawDuration);
                                        ev.noteNumber = juce::jlimit(0, 127, chosenVoicing[noteIndex]);

                                        const int accent = (step % 4 == 0) ? 5 : 0;
                                        const int bassBias = (noteIndex == 0) ? -6 : 0;
                                        const int velJitter = random.nextInt(11) - 5;
                                        const int velocity = juce::jlimit(1, 127, baseVelocity + accent + bassBias + velJitter);
                                        ev.velocity = static_cast<uint8_t>(velocity);
                                        target.events.push_back(ev);
                                    }

                                    previousVoicing = chosenVoicing;
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

        void insertStepNote(int note)
        {
            if (clip == nullptr)
                return;

            const int finalNote = juce::jlimit(0, 127, note);
            const int targetTrackIndex = clip->trackIndex;
            const double maxBeat = juce::jmax(0.0, clip->lengthBeats - 0.0625);
            const double beat = juce::jlimit(0.0, maxBeat, quantizeBeatToGrid(stepInputBeat));
            const int velocity = juce::jlimit(1, 127, static_cast<int>(std::round(velocitySlider.getValue())));

            performClipEdit("Step Input Note",
                            [this, beat, finalNote, velocity](Clip& target)
                            {
                                TimelineEvent ev;
                                ev.noteNumber = finalNote;
                                ev.startBeat = juce::jlimit(0.0, juce::jmax(0.0, target.lengthBeats - 0.0625), beat);
                                const double remaining = juce::jmax(0.0625, target.lengthBeats - ev.startBeat);
                                ev.durationBeats = juce::jlimit(0.0625, remaining, noteLengthBeats);
                                ev.velocity = static_cast<uint8_t>(velocity);
                                target.events.push_back(ev);

                                std::sort(target.events.begin(), target.events.end(),
                                          [](const TimelineEvent& a, const TimelineEvent& b)
                                          {
                                              if (std::abs(a.startBeat - b.startBeat) > 0.0001)
                                                  return a.startBeat < b.startBeat;
                                              return a.noteNumber < b.noteNumber;
                                          });
                            });

            if (onPreviewStepNote != nullptr && targetTrackIndex >= 0)
                onPreviewStepNote(targetTrackIndex, finalNote, velocity);

            stepInputBeat = juce::jlimit(0.0, maxBeat, beat + noteLengthBeats);
            ensureBeatVisible(stepInputBeat);
            selectedNoteIndex = findNoteAt(finalNote, beat);
            selectedNoteIndices.clear();
            if (selectedNoteIndex >= 0)
                selectedNoteIndices.insert(selectedNoteIndex);
            updateVelocitySliderFromSelection();
            repaint();
        }

        int getSelectedController() const
        {
            const int index = juce::jlimit(0, static_cast<int>(ccControllers.size()) - 1, ccLaneIndex);
            return ccControllers[static_cast<size_t>(index)];
        }

        int getTotalLaneCount() const
        {
            return static_cast<int>(ccControllers.size()) + 4;
        }

        bool isControllerLaneSelected() const { return ccLaneIndex < static_cast<int>(ccControllers.size()); }
        bool isPitchBendLaneSelected() const { return ccLaneIndex == static_cast<int>(ccControllers.size()); }
        bool isChannelPressureLaneSelected() const { return ccLaneIndex == static_cast<int>(ccControllers.size()) + 1; }
        bool isPolyAftertouchLaneSelected() const { return ccLaneIndex == static_cast<int>(ccControllers.size()) + 2; }
        bool isProgramChangeLaneSelected() const { return ccLaneIndex == static_cast<int>(ccControllers.size()) + 3; }

        bool isBlackKey(int note) const
        {
            const int n = note % 12;
            return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
        }

        Clip* clip = nullptr;
        int clipIndex = -1;
        int selectedNoteIndex = -1;
        std::set<int> selectedNoteIndices;

        enum class EditTool
        {
            Select,
            Draw,
            Erase
        };

        juce::ComboBox rootSelector;
        juce::ComboBox scaleSelector;
        juce::ComboBox snapSelector;
        juce::Slider swingSlider;
        juce::ComboBox lengthSelector;
        juce::ComboBox zoomSelector;
        juce::TextButton selectToolButton;
        juce::TextButton drawToolButton;
        juce::TextButton eraseToolButton;
        juce::TextButton scaleSnapButton;
        juce::TextButton stepInputButton;
        juce::TextButton octaveDownButton;
        juce::TextButton octaveUpButton;
        juce::TextButton horizZoomOutButton;
        juce::TextButton horizZoomInButton;
        juce::TextButton vertZoomOutButton;
        juce::TextButton vertZoomInButton;
        juce::Slider velocitySlider;
        juce::ComboBox ccSelector;
        juce::TextButton quantizeButton;
        juce::TextButton humanizeButton;
        juce::TextButton aiAssistButton;
        juce::ComboBox progressionSelector;
        juce::TextButton generateProgressionButton;
        juce::TextButton regenerateProgressionButton;
        juce::ScrollBar horizontalScrollBar { false };
        juce::ScrollBar verticalScrollBar { true };

        int rootNote = 0;
        int scaleMode = 0;
        int defaultVelocity = 100;
        int ccLaneIndex = 0;
        bool updatingVelocitySlider = false;
        bool scaleSnapEnabled = false;
        bool stepInputEnabled = true;
        EditTool activeTool = EditTool::Select;

        double snapBeat = 0.25;
        int swingPercent = 50;
        double noteLengthBeats = 1.0;
        double stepInputBeat = 0.0;
        double viewStartBeat = 0.0;
        double viewLengthBeats = 8.0;

        int lowestVisibleNote = 36;
        int visibleNoteCount = 24;
        int keyboardBaseNote = 60;

        bool draggingNote = false;
        bool resizingNote = false;
        bool resizingFromLeft = false;
        bool draggingCC = false;
        bool velocityDragging = false;
        bool marqueeSelecting = false;
        bool brushPainting = false;
        bool brushEraseMode = false;
        bool duplicateDragInProgress = false;
        int draggedNoteIndex = -1;
        int hoveredNoteIndex = -1;
        int lastVelocityDragValue = -1;
        int lastBrushNote = -1;
        double lastBrushBeat = -1.0;
        juce::Point<float> marqueeStart;
        juce::Rectangle<float> marqueeRect;

        double dragStartMouseBeat = 0.0;
        int dragStartMouseNote = 0;
        double lastDragDeltaBeat = 99999.0;
        int lastDragDeltaNote = std::numeric_limits<int>::max();
        double lastDragDuration = -1.0;
        double lastDragStartBeat = -1.0;
        std::vector<int> dragSourceIndices;
        std::vector<TimelineEvent> dragSourceEvents;

        static constexpr int controlHeight = 66;
        static constexpr int pianoKeyWidth = 54;
        static constexpr int scrollBarSize = 12;
        static constexpr int scrollBarGap = 2;
        static constexpr float resizeHandleWidth = 12.0f;
        static constexpr float minNoteWidthForLeftResize = 18.0f;
        const std::array<int, 6> ccControllers { 1, 7, 10, 11, 64, 74 };
        bool updatingScrollBars = false;
    };
}
