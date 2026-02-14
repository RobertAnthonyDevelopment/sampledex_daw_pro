#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <vector>
#include "TimelineModel.h"
#include "Track.h"

namespace sampledex
{
    class SmfPipeline
    {
    public:
        enum class ImportMode
        {
            SingleMergedClip = 0,
            PreserveSourceTracks = 1
        };

        struct TempoPoint
        {
            double beat = 0.0;
            double bpm = 120.0;
        };

        struct TimeSignaturePoint
        {
            double beat = 0.0;
            int numerator = 4;
            int denominator = 4;
        };

        struct ImportedClip
        {
            Clip clip;
            int sourceTrackIndex = 0;
            int sourceChannel = -1;
            juce::String sourceTrackName;
        };

        struct ImportResult
        {
            std::vector<ImportedClip> clips;
            std::vector<TempoPoint> tempoMap;
            std::vector<TimeSignaturePoint> timeSignatureMap;
            double detectedTempoBpm = 0.0;
        };

        struct ExportSelection
        {
            std::vector<int> trackIndices;
            bool mergeToSingleTrack = false;
            juce::String singleTrackName;
        };

        static bool importSmfFile(const juce::File& file, ImportMode mode, ImportResult& out)
        {
            out = {};
            if (!file.existsAsFile())
                return false;

            juce::FileInputStream input(file);
            if (!input.openedOk())
                return false;

            juce::MidiFile midi;
            if (!midi.readFrom(input))
                return false;

            const double ppq = juce::jmax(1.0, static_cast<double>(midi.getTimeFormat()));
            const auto tickToBeat = [ppq](double ticks)
            {
                return juce::jmax(0.0, ticks / ppq);
            };

            struct EventBucket
            {
                std::vector<TimelineEvent> notes;
                std::vector<MidiCCEvent> cc;
                std::vector<MidiPitchBendEvent> pitch;
                std::vector<MidiChannelPressureEvent> chPressure;
                std::vector<MidiPolyAftertouchEvent> polyAftertouch;
                std::vector<MidiProgramChangeEvent> program;
                std::vector<MidiRawEvent> raw;
                double endBeat = 0.0;
            };

            std::map<std::tuple<int, int>, EventBucket> buckets;
            std::map<int, juce::String> sourceTrackNames;

            for (int trackIdx = 0; trackIdx < midi.getNumTracks(); ++trackIdx)
            {
                const auto* seq = midi.getTrack(trackIdx);
                if (seq == nullptr)
                    continue;

                std::array<std::array<bool, 128>, 16> active {};
                std::array<std::array<double, 128>, 16> activeStartBeat {};
                std::array<std::array<uint8_t, 128>, 16> activeVelocity {};
                std::array<int, 16> activeBankMsb;
                std::array<int, 16> activeBankLsb;
                activeBankMsb.fill(-1);
                activeBankLsb.fill(-1);
                juce::String trackName;

                for (int i = 0; i < seq->getNumEvents(); ++i)
                {
                    const auto& msg = seq->getEventPointer(i)->message;
                    const double beat = tickToBeat(msg.getTimeStamp());

                    if (msg.isTrackNameEvent() && trackName.isEmpty())
                    {
                        trackName = msg.getTextFromTextMetaEvent();
                        sourceTrackNames[trackIdx] = trackName;
                        continue;
                    }

                    if (msg.isTempoMetaEvent())
                    {
                        const auto secPerQuarter = msg.getTempoSecondsPerQuarterNote();
                        if (secPerQuarter > 0.0)
                        {
                            const auto bpm = 60.0 / secPerQuarter;
                            out.tempoMap.push_back({ beat, bpm });
                            if (out.detectedTempoBpm <= 0.0)
                                out.detectedTempoBpm = bpm;
                        }
                        continue;
                    }

                    if (msg.isTimeSignatureMetaEvent())
                    {
                        int numerator = 4;
                        int denominator = 4;
                        msg.getTimeSignatureInfo(numerator, denominator);
                        out.timeSignatureMap.push_back({ beat, juce::jmax(1, numerator), juce::jmax(1, denominator) });
                        continue;
                    }

                    if (msg.isMetaEvent() || msg.getChannel() <= 0)
                        continue;

                    const int channel = juce::jlimit(1, 16, msg.getChannel()) - 1;
                    const int bucketChannel = mode == ImportMode::SingleMergedClip ? -1 : channel;
                    auto& bucket = buckets[{ trackIdx, bucketChannel }];
                    bucket.endBeat = juce::jmax(bucket.endBeat, beat);
                    const int note = juce::jlimit(0, 127, msg.getNoteNumber());

                    if (msg.isNoteOn())
                    {
                        active[static_cast<size_t>(channel)][static_cast<size_t>(note)] = true;
                        activeStartBeat[static_cast<size_t>(channel)][static_cast<size_t>(note)] = beat;
                        const float v = static_cast<float>(msg.getVelocity());
                        const int vel = v <= 1.0f ? static_cast<int>(std::lround(v * 127.0f)) : static_cast<int>(std::lround(v));
                        activeVelocity[static_cast<size_t>(channel)][static_cast<size_t>(note)] = static_cast<uint8_t>(juce::jlimit(1, 127, vel));
                    }
                    else if (msg.isNoteOff())
                    {
                        if (!active[static_cast<size_t>(channel)][static_cast<size_t>(note)])
                            continue;
                        const auto startBeat = activeStartBeat[static_cast<size_t>(channel)][static_cast<size_t>(note)];
                        bucket.notes.push_back({ startBeat, juce::jmax(0.0625, beat - startBeat), note,
                                                 activeVelocity[static_cast<size_t>(channel)][static_cast<size_t>(note)] });
                        active[static_cast<size_t>(channel)][static_cast<size_t>(note)] = false;
                    }
                    else if (msg.isController())
                    {
                        const int controller = juce::jlimit(0, 127, msg.getControllerNumber());
                        const auto value = static_cast<uint8_t>(juce::jlimit(0, 127, msg.getControllerValue()));
                        bucket.cc.push_back({ beat, controller, value });

                        if (controller == 0)
                            activeBankMsb[static_cast<size_t>(channel)] = value;
                        else if (controller == 32)
                            activeBankLsb[static_cast<size_t>(channel)] = value;
                    }
                    else if (msg.isPitchWheel())
                    {
                        bucket.pitch.push_back({ beat, juce::jlimit(0, 16383, msg.getPitchWheelValue()) });
                    }
                    else if (msg.isChannelPressure())
                    {
                        bucket.chPressure.push_back({ beat, static_cast<uint8_t>(juce::jlimit(0, 127, msg.getChannelPressureValue())) });
                    }
                    else if (msg.isAftertouch())
                    {
                        bucket.polyAftertouch.push_back({ beat,
                                                          juce::jlimit(0, 127, msg.getNoteNumber()),
                                                          static_cast<uint8_t>(juce::jlimit(0, 127, msg.getAfterTouchValue())) });
                    }
                    else if (msg.isProgramChange())
                    {
                        bucket.program.push_back({ beat,
                                                   activeBankMsb[static_cast<size_t>(channel)],
                                                   activeBankLsb[static_cast<size_t>(channel)],
                                                   juce::jlimit(0, 127, msg.getProgramChangeNumber()) });
                    }

                    if (msg.getRawDataSize() > 0)
                    {
                        const auto* d = msg.getRawData();
                        bucket.raw.push_back({ beat,
                                               d[0],
                                               static_cast<uint8_t>(msg.getRawDataSize() > 1 ? d[1] : 0),
                                               static_cast<uint8_t>(msg.getRawDataSize() > 2 ? d[2] : 0) });
                    }
                }

                for (int ch = 0; ch < 16; ++ch)
                {
                    for (int note = 0; note < 128; ++note)
                    {
                        if (!active[static_cast<size_t>(ch)][static_cast<size_t>(note)])
                            continue;

                        const int bucketChannel = mode == ImportMode::SingleMergedClip ? -1 : ch;
                        auto& bucket = buckets[{ trackIdx, bucketChannel }];
                        const auto startBeat = activeStartBeat[static_cast<size_t>(ch)][static_cast<size_t>(note)];
                        const auto endBeat = juce::jmax(startBeat + 0.0625, bucket.endBeat);
                        bucket.notes.push_back({ startBeat, endBeat - startBeat, note,
                                                 activeVelocity[static_cast<size_t>(ch)][static_cast<size_t>(note)] });
                    }
                }

            }

            std::sort(out.tempoMap.begin(), out.tempoMap.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
            std::sort(out.timeSignatureMap.begin(), out.timeSignatureMap.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });

            for (const auto& [key, bucket] : buckets)
            {
                if (bucket.notes.empty() && bucket.cc.empty() && bucket.pitch.empty() && bucket.chPressure.empty()
                    && bucket.polyAftertouch.empty() && bucket.program.empty() && bucket.raw.empty())
                {
                    continue;
                }

                ImportedClip imported;
                imported.sourceTrackIndex = std::get<0>(key);
                imported.sourceChannel = std::get<1>(key);
                const auto itName = sourceTrackNames.find(imported.sourceTrackIndex);
                imported.sourceTrackName = itName != sourceTrackNames.end() && itName->second.isNotEmpty()
                    ? itName->second
                    : ("MIDI " + juce::String(imported.sourceTrackIndex + 1));
                imported.clip.type = ClipType::MIDI;
                imported.clip.name = imported.sourceTrackName;
                imported.clip.lengthBeats = juce::jmax(0.25, bucket.endBeat + 0.25);
                imported.clip.events = bucket.notes;
                imported.clip.ccEvents = bucket.cc;
                imported.clip.pitchBendEvents = bucket.pitch;
                imported.clip.channelPressureEvents = bucket.chPressure;
                imported.clip.polyAftertouchEvents = bucket.polyAftertouch;
                imported.clip.programChangeEvents = bucket.program;
                imported.clip.rawEvents = bucket.raw;
                imported.clip.sourceMidiChannel = imported.sourceChannel >= 0 ? imported.sourceChannel + 1 : -1;
                imported.clip.sourceTrackName = imported.sourceTrackName;
                out.clips.push_back(std::move(imported));
            }

            std::sort(out.clips.begin(), out.clips.end(), [](const ImportedClip& a, const ImportedClip& b)
            {
                if (a.sourceTrackIndex != b.sourceTrackIndex)
                    return a.sourceTrackIndex < b.sourceTrackIndex;
                return a.sourceChannel < b.sourceChannel;
            });

            return !out.clips.empty();
        }

        static bool exportSmfFile(const juce::File& file,
                                  const std::vector<Clip>& arrangement,
                                  const juce::OwnedArray<Track>& tracks,
                                  const std::vector<TempoPoint>& tempoMap,
                                  const std::vector<TimeSignaturePoint>& timeSignatureMap,
                                  const ExportSelection& selection)
        {
            if (file == juce::File{})
                return false;

            juce::MidiFile midi;
            midi.setTicksPerQuarterNote(960);

            auto tempoTrack = std::make_unique<juce::MidiMessageSequence>();
            for (const auto& tempo : tempoMap)
            {
                const auto bpm = juce::jmax(1.0, tempo.bpm);
                juce::MidiMessage msg = juce::MidiMessage::tempoMetaEvent(juce::jmax(1, static_cast<int>(std::lround(60000000.0 / bpm))));
                msg.setTimeStamp(tempo.beat * 960.0);
                tempoTrack->addEvent(msg);
            }
            for (const auto& sig : timeSignatureMap)
            {
                juce::MidiMessage msg = juce::MidiMessage::timeSignatureMetaEvent(juce::jmax(1, sig.numerator), juce::jmax(1, sig.denominator));
                msg.setTimeStamp(sig.beat * 960.0);
                tempoTrack->addEvent(msg);
            }
            midi.addTrack(*tempoTrack);

            const auto shouldIncludeTrack = [&selection](int trackIndex)
            {
                return std::find(selection.trackIndices.begin(), selection.trackIndices.end(), trackIndex) != selection.trackIndices.end();
            };

            const auto appendClipEvents = [](juce::MidiMessageSequence& seq, const Clip& clip)
            {
                const int channel = juce::jlimit(1, 16, clip.sourceMidiChannel > 0 ? clip.sourceMidiChannel : 1);
                for (const auto& note : clip.events)
                {
                    juce::MidiMessage on = juce::MidiMessage::noteOn(channel,
                                                                     juce::jlimit(0, 127, note.noteNumber),
                                                                     static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(note.velocity))));
                    on.setTimeStamp((clip.startBeat + note.startBeat) * 960.0);
                    seq.addEvent(on);

                    juce::MidiMessage off = juce::MidiMessage::noteOff(channel, juce::jlimit(0, 127, note.noteNumber));
                    off.setTimeStamp((clip.startBeat + note.startBeat + juce::jmax(0.0625, note.durationBeats)) * 960.0);
                    seq.addEvent(off);
                }

                for (const auto& cc : clip.ccEvents)
                {
                    juce::MidiMessage msg = juce::MidiMessage::controllerEvent(channel, cc.controller, cc.value);
                    msg.setTimeStamp((clip.startBeat + cc.beat) * 960.0);
                    seq.addEvent(msg);
                }

                for (const auto& program : clip.programChangeEvents)
                {
                    if (program.bankMsb >= 0)
                    {
                        juce::MidiMessage msg = juce::MidiMessage::controllerEvent(channel, 0, program.bankMsb);
                        msg.setTimeStamp((clip.startBeat + program.beat) * 960.0);
                        seq.addEvent(msg);
                    }
                    if (program.bankLsb >= 0)
                    {
                        juce::MidiMessage msg = juce::MidiMessage::controllerEvent(channel, 32, program.bankLsb);
                        msg.setTimeStamp((clip.startBeat + program.beat) * 960.0);
                        seq.addEvent(msg);
                    }
                    if (program.program >= 0)
                    {
                        juce::MidiMessage msg = juce::MidiMessage::programChange(channel, program.program);
                        msg.setTimeStamp((clip.startBeat + program.beat) * 960.0);
                        seq.addEvent(msg);
                    }
                }
            };

            if (selection.mergeToSingleTrack)
            {
                juce::MidiMessageSequence seq;
                for (const auto& clip : arrangement)
                    if (clip.type == ClipType::MIDI && shouldIncludeTrack(clip.trackIndex))
                        appendClipEvents(seq, clip);
                midi.addTrack(seq);
            }
            else
            {
                for (int trackIndex : selection.trackIndices)
                {
                    juce::MidiMessageSequence seq;
                    if (juce::isPositiveAndBelow(trackIndex, tracks.size()))
                    {
                        auto name = juce::MidiMessage::textMetaEvent(0x03, tracks[trackIndex]->getTrackName());
                        name.setTimeStamp(0);
                        seq.addEvent(name);
                    }
                    for (const auto& clip : arrangement)
                        if (clip.type == ClipType::MIDI && clip.trackIndex == trackIndex)
                            appendClipEvents(seq, clip);
                    midi.addTrack(seq);
                }
            }

            juce::FileOutputStream output(file);
            if (!output.openedOk())
                return false;
            return midi.writeTo(output, 1);
        }
    };
}
