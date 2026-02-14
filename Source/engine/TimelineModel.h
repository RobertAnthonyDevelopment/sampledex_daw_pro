#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <tuple>
#include <vector>
#include <memory>

namespace sampledex
{
    // --- Data Types ---
    struct TimelineEvent
    {
        double startBeat;
        double durationBeats;
        int noteNumber;
        uint8_t velocity;

        bool operator==(const TimelineEvent& other) const
        {
            return std::tie(startBeat, durationBeats, noteNumber, velocity)
                == std::tie(other.startBeat, other.durationBeats, other.noteNumber, other.velocity);
        }
    };

    struct MidiCCEvent
    {
        double beat = 0.0;
        int controller = 1;
        uint8_t value = 0;

        bool operator==(const MidiCCEvent& other) const
        {
            return std::tie(beat, controller, value)
                == std::tie(other.beat, other.controller, other.value);
        }
    };

    struct MidiPitchBendEvent
    {
        double beat = 0.0;
        int value = 8192; // 14-bit MIDI pitch wheel value (0..16383), center=8192

        bool operator==(const MidiPitchBendEvent& other) const
        {
            return std::tie(beat, value) == std::tie(other.beat, other.value);
        }
    };

    struct MidiChannelPressureEvent
    {
        double beat = 0.0;
        uint8_t pressure = 0;

        bool operator==(const MidiChannelPressureEvent& other) const
        {
            return std::tie(beat, pressure) == std::tie(other.beat, other.pressure);
        }
    };

    struct MidiPolyAftertouchEvent
    {
        double beat = 0.0;
        int noteNumber = 60;
        uint8_t pressure = 0;

        bool operator==(const MidiPolyAftertouchEvent& other) const
        {
            return std::tie(beat, noteNumber, pressure) == std::tie(other.beat, other.noteNumber, other.pressure);
        }
    };

    struct MidiProgramChangeEvent
    {
        double beat = 0.0;
        int bankMsb = -1; // -1 means no bank MSB update
        int bankLsb = -1; // -1 means no bank LSB update
        int program = -1; // -1 means no program update

        bool operator==(const MidiProgramChangeEvent& other) const
        {
            return std::tie(beat, bankMsb, bankLsb, program)
                == std::tie(other.beat, other.bankMsb, other.bankLsb, other.program);
        }
    };

    struct MidiRawEvent
    {
        double beat = 0.0;
        uint8_t status = 0x90;
        uint8_t data1 = 0;
        uint8_t data2 = 0;

        bool operator==(const MidiRawEvent& other) const
        {
            return std::tie(beat, status, data1, data2)
                == std::tie(other.beat, other.status, other.data1, other.data2);
        }
    };

    enum class AutomationTarget : int
    {
        TrackVolume = 0,
        TrackPan = 1,
        TrackSend = 2,
        MasterOutput = 3
    };

    enum class AutomationMode : int
    {
        Read = 0,
        Touch = 1,
        Latch = 2,
        Write = 3
    };

    struct AutomationPoint
    {
        double beat = 0.0;
        float value = 0.0f;

        bool operator==(const AutomationPoint& other) const
        {
            return std::tie(beat, value) == std::tie(other.beat, other.value);
        }
    };

    struct AutomationLane
    {
        int laneId = 0;
        AutomationTarget target = AutomationTarget::TrackVolume;
        int trackIndex = -1; // -1 is global/master targets
        AutomationMode mode = AutomationMode::Read;
        bool enabled = true;
        std::vector<AutomationPoint> points;

        bool operator==(const AutomationLane& other) const
        {
            return std::tie(laneId, target, trackIndex, mode, enabled, points)
                == std::tie(other.laneId, other.target, other.trackIndex, other.mode, other.enabled, other.points);
        }
    };

    enum class ClipType { MIDI, Audio };

    enum class ClipStretchMode : int
    {
        Tape = 0,
        BeatWarp = 1,
        OneShot = 2
    };

    struct WarpMarker
    {
        double clipBeat = 0.0;
        double sourceBeat = 0.0;
        float strength = 1.0f;
        bool transientAnchor = false;

        bool operator==(const WarpMarker& other) const
        {
            return std::tie(clipBeat, sourceBeat, strength, transientAnchor)
                == std::tie(other.clipBeat, other.sourceBeat, other.strength, other.transientAnchor);
        }
    };

    struct Clip
    {
        juce::String name;
        ClipType type = ClipType::MIDI;
        
        double startBeat;
        double lengthBeats;
        double offsetBeats = 0.0;
        int trackIndex;
        
        // MIDI Content
        std::vector<TimelineEvent> events;
        std::vector<MidiCCEvent> ccEvents;
        std::vector<MidiPitchBendEvent> pitchBendEvents;
        std::vector<MidiChannelPressureEvent> channelPressureEvents;
        std::vector<MidiPolyAftertouchEvent> polyAftertouchEvents;
        std::vector<MidiProgramChangeEvent> programChangeEvents;
        std::vector<MidiRawEvent> rawEvents;
        
        // Audio Content (RAM Cache)
        // We use shared_ptr so we can pass this around efficiently without copying heavy audio data
        std::shared_ptr<juce::AudioBuffer<float>> audioData;
        juce::String audioFilePath;
        double audioSampleRate = 44100.0; 
        float gainLinear = 1.0f;
        double fadeInBeats = 0.0;
        double fadeOutBeats = 0.0;
        double crossfadeInBeats = 0.0;
        double crossfadeOutBeats = 0.0;
        double detectedTempoBpm = 0.0;
        ClipStretchMode stretchMode = ClipStretchMode::Tape;
        double originalTempoBpm = 0.0;
        std::vector<WarpMarker> warpMarkers;
        bool formantPreserve = false;
        bool oneShot = false;

        bool operator==(const Clip& other) const
        {
            return std::tie(name,
                            type,
                            startBeat,
                            lengthBeats,
                            offsetBeats,
                            trackIndex,
                            events,
                            ccEvents,
                            pitchBendEvents,
                            channelPressureEvents,
                            polyAftertouchEvents,
                            programChangeEvents,
                            rawEvents,
                            audioData,
                            audioFilePath,
                            audioSampleRate,
                            gainLinear,
                            fadeInBeats,
                            fadeOutBeats,
                            crossfadeInBeats,
                            crossfadeOutBeats,
                            detectedTempoBpm,
                            stretchMode,
                            originalTempoBpm,
                            warpMarkers,
                            formantPreserve,
                            oneShot)
                == std::tie(other.name,
                            other.type,
                            other.startBeat,
                            other.lengthBeats,
                            other.offsetBeats,
                            other.trackIndex,
                            other.events,
                            other.ccEvents,
                            other.pitchBendEvents,
                            other.channelPressureEvents,
                            other.polyAftertouchEvents,
                            other.programChangeEvents,
                            other.rawEvents,
                            other.audioData,
                            other.audioFilePath,
                            other.audioSampleRate,
                            other.gainLinear,
                            other.fadeInBeats,
                            other.fadeOutBeats,
                            other.crossfadeInBeats,
                            other.crossfadeOutBeats,
                            other.detectedTempoBpm,
                            other.stretchMode,
                            other.originalTempoBpm,
                            other.warpMarkers,
                            other.formantPreserve,
                            other.oneShot);
        }

        // --- MIDI Helper ---
        void getEventsInRange(double fromBeat,
                              double toBeat,
                              juce::MidiBuffer& dest,
                              double bpm,
                              double sampleRate,
                              int blockNumSamples = -1,
                              bool chaseNotesAtBlockStart = false,
                              int midiChannel = 1,
                              int transposeSemitones = 0) const
        {
            if (type != ClipType::MIDI || toBeat <= fromBeat || bpm <= 0.0 || sampleRate <= 0.0)
                return;

            const double secondsPerBeat = 60.0 / bpm;
            const int channel = juce::jlimit(1, 16, midiChannel);
            const int transpose = juce::jlimit(-48, 48, transposeSemitones);
            const int estimatedSamples = blockNumSamples > 0
                ? blockNumSamples
                : juce::jmax(1, static_cast<int>(std::ceil((toBeat - fromBeat) * secondsPerBeat * sampleRate)));
            const auto beatToSample = [&](double absoluteBeat)
            {
                const double timeInBlockSeconds = (absoluteBeat - fromBeat) * secondsPerBeat;
                const int sampleOffset = static_cast<int>(std::llround(timeInBlockSeconds * sampleRate));
                return juce::jlimit(0, juce::jmax(0, estimatedSamples - 1), sampleOffset);
            };

            for (const auto& ev : events)
            {
                const double noteAbsStart = startBeat + ev.startBeat - offsetBeats;
                const double noteAbsEnd = noteAbsStart + ev.durationBeats;
                const int noteNumber = juce::jlimit(0, 127, ev.noteNumber + transpose);

                if (chaseNotesAtBlockStart && noteAbsStart < fromBeat && noteAbsEnd > fromBeat)
                    dest.addEvent(juce::MidiMessage::noteOn(channel, noteNumber, ev.velocity), 0);

                if (noteAbsStart >= fromBeat && noteAbsStart < toBeat)
                {
                    dest.addEvent(juce::MidiMessage::noteOn(channel, noteNumber, ev.velocity),
                                  beatToSample(noteAbsStart));
                }

                if (noteAbsEnd >= fromBeat && noteAbsEnd < toBeat)
                {
                    dest.addEvent(juce::MidiMessage::noteOff(channel, noteNumber),
                                  beatToSample(noteAbsEnd));
                }
            }

            for (const auto& cc : ccEvents)
            {
                const double ccAbsBeat = startBeat + cc.beat - offsetBeats;
                if (ccAbsBeat >= fromBeat && ccAbsBeat < toBeat)
                {
                    dest.addEvent(juce::MidiMessage::controllerEvent(channel, cc.controller, cc.value),
                                  beatToSample(ccAbsBeat));
                }
            }

            for (const auto& bend : pitchBendEvents)
            {
                const double absBeat = startBeat + bend.beat - offsetBeats;
                if (absBeat >= fromBeat && absBeat < toBeat)
                {
                    dest.addEvent(juce::MidiMessage::pitchWheel(channel, juce::jlimit(0, 16383, bend.value)),
                                  beatToSample(absBeat));
                }
            }

            for (const auto& pressure : channelPressureEvents)
            {
                const double absBeat = startBeat + pressure.beat - offsetBeats;
                if (absBeat >= fromBeat && absBeat < toBeat)
                {
                    dest.addEvent(juce::MidiMessage::channelPressureChange(channel, pressure.pressure),
                                  beatToSample(absBeat));
                }
            }

            for (const auto& poly : polyAftertouchEvents)
            {
                const double absBeat = startBeat + poly.beat - offsetBeats;
                if (absBeat >= fromBeat && absBeat < toBeat)
                {
                    const int note = juce::jlimit(0, 127, poly.noteNumber + transpose);
                    dest.addEvent(juce::MidiMessage::aftertouchChange(channel, note, poly.pressure),
                                  beatToSample(absBeat));
                }
            }

            for (const auto& program : programChangeEvents)
            {
                const double absBeat = startBeat + program.beat - offsetBeats;
                if (absBeat < fromBeat || absBeat >= toBeat)
                    continue;

                const int sampleOffset = beatToSample(absBeat);
                if (program.bankMsb >= 0)
                    dest.addEvent(juce::MidiMessage::controllerEvent(channel, 0, juce::jlimit(0, 127, program.bankMsb)),
                                  sampleOffset);
                if (program.bankLsb >= 0)
                    dest.addEvent(juce::MidiMessage::controllerEvent(channel, 32, juce::jlimit(0, 127, program.bankLsb)),
                                  sampleOffset);
                if (program.program >= 0)
                    dest.addEvent(juce::MidiMessage::programChange(channel, juce::jlimit(0, 127, program.program)),
                                  sampleOffset);
            }

            for (const auto& raw : rawEvents)
            {
                const double absBeat = startBeat + raw.beat - offsetBeats;
                if (absBeat >= fromBeat && absBeat < toBeat)
                {
                    dest.addEvent(juce::MidiMessage(static_cast<int>(raw.status),
                                                    static_cast<int>(raw.data1),
                                                    static_cast<int>(raw.data2)),
                                  beatToSample(absBeat));
                }
            }
        }
    };

    namespace ArrangementEditing
    {
        inline bool splitClipAtBeat(Clip& left, Clip& rightOut, double splitBeat)
        {
            const double splitLocalBeat = splitBeat - left.startBeat;
            if (splitLocalBeat <= 0.0001 || splitLocalBeat >= left.lengthBeats - 0.0001)
                return false;

            rightOut = left;
            rightOut.startBeat = splitBeat;
            rightOut.lengthBeats = left.lengthBeats - splitLocalBeat;
            left.lengthBeats = splitLocalBeat;

            if (left.type != ClipType::MIDI)
                return true;

            std::vector<TimelineEvent> leftEvents;
            std::vector<TimelineEvent> rightEvents;
            leftEvents.reserve(left.events.size());
            rightEvents.reserve(left.events.size());

            for (const auto& ev : left.events)
            {
                const double evStart = ev.startBeat;
                const double evEnd = ev.startBeat + ev.durationBeats;

                if (evStart < splitLocalBeat)
                {
                    TimelineEvent clippedLeft = ev;
                    clippedLeft.durationBeats = juce::jmax(0.001, juce::jmin(ev.durationBeats, splitLocalBeat - evStart));
                    leftEvents.push_back(clippedLeft);

                    if (evEnd > splitLocalBeat)
                    {
                        TimelineEvent rightCarry = ev;
                        rightCarry.startBeat = 0.0;
                        rightCarry.durationBeats = juce::jmax(0.001, evEnd - splitLocalBeat);
                        rightEvents.push_back(rightCarry);
                    }
                }
                else
                {
                    TimelineEvent shifted = ev;
                    shifted.startBeat = juce::jmax(0.0, ev.startBeat - splitLocalBeat);
                    rightEvents.push_back(shifted);
                }
            }

            std::vector<MidiCCEvent> leftCC;
            std::vector<MidiCCEvent> rightCC;
            leftCC.reserve(left.ccEvents.size());
            rightCC.reserve(left.ccEvents.size());
            for (const auto& cc : left.ccEvents)
            {
                if (cc.beat < splitLocalBeat)
                    leftCC.push_back(cc);
                else
                {
                    MidiCCEvent shifted = cc;
                    shifted.beat = juce::jmax(0.0, shifted.beat - splitLocalBeat);
                    rightCC.push_back(shifted);
                }
            }

            auto splitByBeat = [splitLocalBeat](auto& source, auto& leftOut, auto& rightOut)
            {
                leftOut.reserve(source.size());
                rightOut.reserve(source.size());
                for (const auto& event : source)
                {
                    if (event.beat < splitLocalBeat)
                        leftOut.push_back(event);
                    else
                    {
                        auto shifted = event;
                        shifted.beat = juce::jmax(0.0, shifted.beat - splitLocalBeat);
                        rightOut.push_back(shifted);
                    }
                }
            };

            std::vector<MidiPitchBendEvent> leftPitch;
            std::vector<MidiPitchBendEvent> rightPitch;
            splitByBeat(left.pitchBendEvents, leftPitch, rightPitch);

            std::vector<MidiChannelPressureEvent> leftChannelPressure;
            std::vector<MidiChannelPressureEvent> rightChannelPressure;
            splitByBeat(left.channelPressureEvents, leftChannelPressure, rightChannelPressure);

            std::vector<MidiPolyAftertouchEvent> leftPoly;
            std::vector<MidiPolyAftertouchEvent> rightPoly;
            splitByBeat(left.polyAftertouchEvents, leftPoly, rightPoly);

            std::vector<MidiProgramChangeEvent> leftProgram;
            std::vector<MidiProgramChangeEvent> rightProgram;
            splitByBeat(left.programChangeEvents, leftProgram, rightProgram);

            std::vector<MidiRawEvent> leftRaw;
            std::vector<MidiRawEvent> rightRaw;
            splitByBeat(left.rawEvents, leftRaw, rightRaw);

            left.events = std::move(leftEvents);
            left.ccEvents = std::move(leftCC);
            rightOut.events = std::move(rightEvents);
            rightOut.ccEvents = std::move(rightCC);
            left.pitchBendEvents = std::move(leftPitch);
            rightOut.pitchBendEvents = std::move(rightPitch);
            left.channelPressureEvents = std::move(leftChannelPressure);
            rightOut.channelPressureEvents = std::move(rightChannelPressure);
            left.polyAftertouchEvents = std::move(leftPoly);
            rightOut.polyAftertouchEvents = std::move(rightPoly);
            left.programChangeEvents = std::move(leftProgram);
            rightOut.programChangeEvents = std::move(rightProgram);
            left.rawEvents = std::move(leftRaw);
            rightOut.rawEvents = std::move(rightRaw);
            return true;
        }

        inline void applySymmetricCrossfade(Clip& left, Clip& right, double fadeBeats)
        {
            if (left.trackIndex != right.trackIndex)
                return;

            const double clamped = juce::jmax(0.0, fadeBeats);
            const double maxByLeft = juce::jmax(0.0, left.lengthBeats * 0.49);
            const double maxByRight = juce::jmax(0.0, right.lengthBeats * 0.49);
            const double applied = juce::jmin(clamped, juce::jmin(maxByLeft, maxByRight));
            left.crossfadeOutBeats = applied;
            right.crossfadeInBeats = applied;
            left.fadeOutBeats = juce::jmax(left.fadeOutBeats, applied);
            right.fadeInBeats = juce::jmax(right.fadeInBeats, applied);
        }
    }

}
