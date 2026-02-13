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
        
        // Audio Content (RAM Cache)
        // We use shared_ptr so we can pass this around efficiently without copying heavy audio data
        std::shared_ptr<juce::AudioBuffer<float>> audioData;
        juce::String audioFilePath;
        double audioSampleRate = 44100.0; 
        float gainLinear = 1.0f;
        double fadeInBeats = 0.0;
        double fadeOutBeats = 0.0;
        double detectedTempoBpm = 0.0;

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
                            audioData,
                            audioFilePath,
                            audioSampleRate,
                            gainLinear,
                            fadeInBeats,
                            fadeOutBeats,
                            detectedTempoBpm)
                == std::tie(other.name,
                            other.type,
                            other.startBeat,
                            other.lengthBeats,
                            other.offsetBeats,
                            other.trackIndex,
                            other.events,
                            other.ccEvents,
                            other.audioData,
                            other.audioFilePath,
                            other.audioSampleRate,
                            other.gainLinear,
                            other.fadeInBeats,
                            other.fadeOutBeats,
                            other.detectedTempoBpm);
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
        }
    };

}
