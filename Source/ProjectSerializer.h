#pragma once
#include <JuceHeader.h>
#include <cstdint>
#include <utility>
#include <vector>
#include "TimelineModel.h"
#include "Track.h"

namespace sampledex
{
    class ProjectSerializer
    {
    public:
        struct TempoPoint
        {
            double beat = 0.0;
            double bpm = 120.0;
        };

        struct PluginSlotState
        {
            int slotIndex = 0;
            bool bypassed = false;
            juce::PluginDescription description;
            bool hasDescription = false;
            juce::String encodedState;
        };

        struct TrackState
        {
            juce::String name;
            float volume = 0.8f;
            float pan = 0.0f;
            float sendLevel = 0.0f;
            int sendTapMode = 1;
            int sendTargetBus = 0;
            bool mute = false;
            bool solo = false;
            bool arm = false;
            bool inputMonitoring = false;
            int inputSourcePair = -1;
            float inputMonitorGain = 0.68f;
            int monitorTapMode = 1;
            int channelType = static_cast<int>(Track::ChannelType::Instrument);
            int outputTargetType = static_cast<int>(Track::OutputTargetType::Master);
            int outputTargetBus = 0;
            bool eqEnabled = true;
            float eqLowGainDb = 0.0f;
            float eqMidGainDb = 0.0f;
            float eqHighGainDb = 0.0f;
            bool frozenPlaybackOnly = false;
            juce::String frozenRenderPath;
            int builtInInstrumentMode = static_cast<int>(Track::BuiltInInstrument::BasicSynth);
            juce::String samplerSamplePath;
            std::uint32_t builtInFxMask = 0u;
            std::vector<PluginSlotState> pluginSlots;
        };

        struct ProjectState
        {
            double bpm = 120.0;
            int keyRoot = 0;
            int scaleMode = 0;
            int transposeSemitones = 0;
            int lcdPositionMode = 1;
            bool loopEnabled = false;
            double loopStartBeat = 0.0;
            double loopEndBeat = 8.0;
            std::vector<TempoPoint> tempoMap;
            std::vector<TrackState> tracks;
            std::vector<AutomationLane> automationLanes;
            std::vector<Clip> arrangement;
        };

        static bool saveProject(const juce::File& file,
                                const ProjectState& project,
                                juce::String& errorMessage)
        {
            if (file == juce::File{})
            {
                errorMessage = "No output file was selected.";
                return false;
            }

            auto parent = file.getParentDirectory();
            if (!parent.exists() && !parent.createDirectory())
            {
                errorMessage = "Unable to create project folder:\n" + parent.getFullPathName();
                return false;
            }

            juce::XmlElement root("SAMPLEDEX_PROJECT");
            root.setAttribute("version", "2.3.0");
            root.setAttribute("bpm", project.bpm);
            root.setAttribute("keyRoot", project.keyRoot);
            root.setAttribute("scaleMode", project.scaleMode);
            root.setAttribute("transpose", project.transposeSemitones);
            root.setAttribute("lcdMode", project.lcdPositionMode);
            root.setAttribute("loopEnabled", project.loopEnabled);
            root.setAttribute("loopStart", project.loopStartBeat);
            root.setAttribute("loopEnd", project.loopEndBeat);

            auto* tempoXml = root.createNewChildElement("TEMPO_MAP");
            for (const auto& point : project.tempoMap)
            {
                auto* t = tempoXml->createNewChildElement("TEMPO");
                t->setAttribute("beat", point.beat);
                t->setAttribute("bpm", point.bpm);
            }

            auto* tracksXml = root.createNewChildElement("TRACKS");
            for (const auto& track : project.tracks)
            {
                auto* tXml = tracksXml->createNewChildElement("TRACK");
                tXml->setAttribute("name", track.name);
                tXml->setAttribute("volume", track.volume);
                tXml->setAttribute("pan", track.pan);
                tXml->setAttribute("sendLevel", track.sendLevel);
                tXml->setAttribute("sendTapMode", track.sendTapMode);
                tXml->setAttribute("sendTargetBus", track.sendTargetBus);
                tXml->setAttribute("mute", track.mute);
                tXml->setAttribute("solo", track.solo);
                tXml->setAttribute("arm", track.arm);
                tXml->setAttribute("inputMonitoring", track.inputMonitoring);
                tXml->setAttribute("inputSourcePair", track.inputSourcePair);
                tXml->setAttribute("inputMonitorGain", track.inputMonitorGain);
                tXml->setAttribute("monitorTapMode", track.monitorTapMode);
                tXml->setAttribute("channelType", track.channelType);
                tXml->setAttribute("outputTargetType", track.outputTargetType);
                tXml->setAttribute("outputTargetBus", track.outputTargetBus);
                tXml->setAttribute("eqEnabled", track.eqEnabled);
                tXml->setAttribute("eqLowGainDb", track.eqLowGainDb);
                tXml->setAttribute("eqMidGainDb", track.eqMidGainDb);
                tXml->setAttribute("eqHighGainDb", track.eqHighGainDb);
                tXml->setAttribute("frozenPlaybackOnly", track.frozenPlaybackOnly);
                tXml->setAttribute("frozenRenderPath", track.frozenRenderPath);
                tXml->setAttribute("builtInInstrumentMode", track.builtInInstrumentMode);
                tXml->setAttribute("samplerSamplePath", track.samplerSamplePath);
                tXml->setAttribute("builtInFxMask", static_cast<int>(track.builtInFxMask));

                auto* slotsXml = tXml->createNewChildElement("PLUGIN_SLOTS");
                for (const auto& slot : track.pluginSlots)
                {
                    auto* slotXml = slotsXml->createNewChildElement("PLUGIN_SLOT");
                    slotXml->setAttribute("slotIndex", slot.slotIndex);
                    slotXml->setAttribute("bypassed", slot.bypassed);

                    if (slot.hasDescription)
                    {
                        if (auto descXml = slot.description.createXml())
                        {
                            descXml->setTagName("PLUGIN_DESCRIPTION");
                            slotXml->addChildElement(descXml.release());
                        }
                    }

                    if (slot.encodedState.isNotEmpty())
                    {
                        auto* stateXml = slotXml->createNewChildElement("STATE");
                        stateXml->addTextElement(slot.encodedState);
                    }
                }
            }

            auto* automationXml = root.createNewChildElement("AUTOMATION");
            for (const auto& lane : project.automationLanes)
            {
                auto* laneXml = automationXml->createNewChildElement("LANE");
                laneXml->setAttribute("id", lane.laneId);
                laneXml->setAttribute("target", static_cast<int>(lane.target));
                laneXml->setAttribute("track", lane.trackIndex);
                laneXml->setAttribute("mode", static_cast<int>(lane.mode));
                laneXml->setAttribute("enabled", lane.enabled);

                for (const auto& point : lane.points)
                {
                    auto* p = laneXml->createNewChildElement("PT");
                    p->setAttribute("beat", point.beat);
                    p->setAttribute("value", static_cast<double>(point.value));
                }
            }

            auto* clipsXml = root.createNewChildElement("CLIPS");
            const auto projectDir = file.getParentDirectory();
            for (const auto& clip : project.arrangement)
            {
                auto* cXml = clipsXml->createNewChildElement("CLIP");
                cXml->setAttribute("name", clip.name);
                cXml->setAttribute("type", clip.type == ClipType::Audio ? "audio" : "midi");
                cXml->setAttribute("start", clip.startBeat);
                cXml->setAttribute("length", clip.lengthBeats);
                cXml->setAttribute("offset", clip.offsetBeats);
                cXml->setAttribute("track", clip.trackIndex);
                cXml->setAttribute("gain", clip.gainLinear);
                cXml->setAttribute("fadeIn", clip.fadeInBeats);
                cXml->setAttribute("fadeOut", clip.fadeOutBeats);
                cXml->setAttribute("audioSampleRate", clip.audioSampleRate);
                cXml->setAttribute("detectedTempoBpm", clip.detectedTempoBpm);

                if (clip.type == ClipType::Audio)
                {
                    const juce::File audioFile(clip.audioFilePath);
                    cXml->setAttribute("audioPathAbsolute", audioFile.getFullPathName());
                    cXml->setAttribute("audioPathRelative", audioFile.getRelativePathFrom(projectDir));
                }
                else
                {
                    auto* eventsXml = cXml->createNewChildElement("EVENTS");
                    for (const auto& event : clip.events)
                    {
                        auto* eXml = eventsXml->createNewChildElement("EV");
                        eXml->setAttribute("start", event.startBeat);
                        eXml->setAttribute("length", event.durationBeats);
                        eXml->setAttribute("note", event.noteNumber);
                        eXml->setAttribute("velocity", static_cast<int>(event.velocity));
                    }

                    auto* ccXml = cXml->createNewChildElement("CC");
                    for (const auto& cc : clip.ccEvents)
                    {
                        auto* c = ccXml->createNewChildElement("CCEV");
                        c->setAttribute("beat", cc.beat);
                        c->setAttribute("controller", cc.controller);
                        c->setAttribute("value", static_cast<int>(cc.value));
                    }
                }
            }

            juce::TemporaryFile temp(file);
            if (!temp.getFile().deleteFile() && temp.getFile().existsAsFile())
            {
                errorMessage = "Unable to prepare temporary save file:\n" + temp.getFile().getFullPathName();
                return false;
            }

            std::unique_ptr<juce::FileOutputStream> out(temp.getFile().createOutputStream());
            if (out == nullptr || !out->openedOk())
            {
                errorMessage = "Unable to open project file for writing:\n" + file.getFullPathName();
                return false;
            }

            root.writeTo(*out, {});
            out->flush();
            if (out->getStatus().failed())
            {
                errorMessage = "Project data write failed:\n" + file.getFullPathName();
                return false;
            }

            if (!temp.overwriteTargetFileWithTemporary())
            {
                errorMessage = "Unable to finalize project save:\n" + file.getFullPathName();
                return false;
            }

            errorMessage.clear();
            return true;
        }

        static bool loadProject(const juce::File& file,
                                ProjectState& outProject,
                                juce::AudioFormatManager& audioFormatManager,
                                juce::String& errorMessage)
        {
            outProject = {};
            if (!file.existsAsFile())
            {
                errorMessage = "Project file does not exist.";
                return false;
            }

            std::unique_ptr<juce::XmlElement> root(juce::XmlDocument::parse(file));
            if (root == nullptr || !root->hasTagName("SAMPLEDEX_PROJECT"))
            {
                errorMessage = "Invalid or unsupported project file.";
                return false;
            }

            outProject.bpm = juce::jmax(1.0, root->getDoubleAttribute("bpm", 120.0));
            outProject.keyRoot = juce::jlimit(0, 11, root->getIntAttribute("keyRoot", 0));
            outProject.scaleMode = juce::jmax(0, root->getIntAttribute("scaleMode", 0));
            outProject.transposeSemitones = juce::jlimit(-24, 24, root->getIntAttribute("transpose", 0));
            outProject.lcdPositionMode = juce::jlimit(1, 3, root->getIntAttribute("lcdMode", 1));
            outProject.loopEnabled = root->getBoolAttribute("loopEnabled", false);
            outProject.loopStartBeat = juce::jmax(0.0, root->getDoubleAttribute("loopStart", 0.0));
            outProject.loopEndBeat = juce::jmax(outProject.loopStartBeat + 0.25,
                                                root->getDoubleAttribute("loopEnd", 8.0));

            if (auto* tempoXml = root->getChildByName("TEMPO_MAP"))
            {
                for (auto* tempo = tempoXml->getFirstChildElement(); tempo != nullptr; tempo = tempo->getNextElement())
                {
                    if (!tempo->hasTagName("TEMPO"))
                        continue;
                    TempoPoint point;
                    point.beat = juce::jmax(0.0, tempo->getDoubleAttribute("beat", 0.0));
                    point.bpm = juce::jmax(1.0, tempo->getDoubleAttribute("bpm", outProject.bpm));
                    outProject.tempoMap.push_back(point);
                }
            }
            if (outProject.tempoMap.empty())
                outProject.tempoMap.push_back({ 0.0, outProject.bpm });

            if (auto* tracksXml = root->getChildByName("TRACKS"))
            {
                for (auto* tXml = tracksXml->getFirstChildElement(); tXml != nullptr; tXml = tXml->getNextElement())
                {
                    if (!tXml->hasTagName("TRACK"))
                        continue;

                    TrackState track;
                    track.name = tXml->getStringAttribute("name", "Track");
                    track.volume = static_cast<float>(tXml->getDoubleAttribute("volume", 0.8));
                    track.pan = static_cast<float>(tXml->getDoubleAttribute("pan", 0.0));
                    track.sendLevel = static_cast<float>(tXml->getDoubleAttribute("sendLevel", 0.0));
                    track.sendTapMode = tXml->getIntAttribute("sendTapMode", 1);
                    track.sendTargetBus = tXml->getIntAttribute("sendTargetBus", 0);
                    track.mute = tXml->getBoolAttribute("mute", false);
                    track.solo = tXml->getBoolAttribute("solo", false);
                    track.arm = tXml->getBoolAttribute("arm", false);
                    track.inputMonitoring = tXml->getBoolAttribute("inputMonitoring", false);
                    track.inputSourcePair = tXml->getIntAttribute("inputSourcePair", -1);
                    track.inputMonitorGain = static_cast<float>(tXml->getDoubleAttribute("inputMonitorGain", 0.68));
                    track.monitorTapMode = tXml->getIntAttribute("monitorTapMode", 1);
                    track.channelType = tXml->getIntAttribute("channelType", static_cast<int>(Track::ChannelType::Instrument));
                    track.outputTargetType = tXml->getIntAttribute("outputTargetType", static_cast<int>(Track::OutputTargetType::Master));
                    track.outputTargetBus = tXml->getIntAttribute("outputTargetBus", 0);
                    track.eqEnabled = tXml->getBoolAttribute("eqEnabled", true);
                    track.eqLowGainDb = static_cast<float>(tXml->getDoubleAttribute("eqLowGainDb", 0.0));
                    track.eqMidGainDb = static_cast<float>(tXml->getDoubleAttribute("eqMidGainDb", 0.0));
                    track.eqHighGainDb = static_cast<float>(tXml->getDoubleAttribute("eqHighGainDb", 0.0));
                    track.frozenPlaybackOnly = tXml->getBoolAttribute("frozenPlaybackOnly", false);
                    track.frozenRenderPath = tXml->getStringAttribute("frozenRenderPath");
                    track.builtInInstrumentMode = tXml->getIntAttribute("builtInInstrumentMode",
                        static_cast<int>(Track::BuiltInInstrument::BasicSynth));
                    track.samplerSamplePath = tXml->getStringAttribute("samplerSamplePath");
                    track.builtInFxMask = static_cast<std::uint32_t>(
                        tXml->getIntAttribute("builtInFxMask", 0)) & ((1u << Track::builtInEffectCount) - 1u);

                    if (auto* slotsXml = tXml->getChildByName("PLUGIN_SLOTS"))
                    {
                        for (auto* slotXml = slotsXml->getFirstChildElement(); slotXml != nullptr; slotXml = slotXml->getNextElement())
                        {
                            if (!slotXml->hasTagName("PLUGIN_SLOT"))
                                continue;
                            PluginSlotState slot;
                            slot.slotIndex = slotXml->getIntAttribute("slotIndex", 0);
                            slot.bypassed = slotXml->getBoolAttribute("bypassed", false);

                            if (auto* descXml = slotXml->getChildByName("PLUGIN_DESCRIPTION"))
                                slot.hasDescription = slot.description.loadFromXml(*descXml);
                            else if (auto* legacyDesc = slotXml->getChildByName("PLUGIN"))
                                slot.hasDescription = slot.description.loadFromXml(*legacyDesc);

                            if (auto* stateXml = slotXml->getChildByName("STATE"))
                                slot.encodedState = stateXml->getAllSubText().trim();

                            track.pluginSlots.push_back(std::move(slot));
                        }
                    }
                    else if (auto* legacyPlugin = tXml->getChildByName("PLUGIN"))
                    {
                        // Backward compatibility with older single-plugin schema.
                        PluginSlotState slot;
                        slot.slotIndex = 0;
                        slot.bypassed = false;
                        slot.description.fileOrIdentifier = legacyPlugin->getStringAttribute("id");
                        slot.description.pluginFormatName = legacyPlugin->getStringAttribute("format");
                        slot.description.name = slot.description.fileOrIdentifier;
                        slot.hasDescription = slot.description.fileOrIdentifier.isNotEmpty();
                        slot.encodedState = legacyPlugin->getStringAttribute("state");
                        track.pluginSlots.push_back(std::move(slot));
                    }

                    outProject.tracks.push_back(std::move(track));
                }
            }

            if (auto* automationXml = root->getChildByName("AUTOMATION"))
            {
                for (auto* laneXml = automationXml->getFirstChildElement(); laneXml != nullptr; laneXml = laneXml->getNextElement())
                {
                    if (!laneXml->hasTagName("LANE"))
                        continue;

                    AutomationLane lane;
                    lane.laneId = laneXml->getIntAttribute("id", 0);
                    lane.target = static_cast<AutomationTarget>(juce::jlimit(0, 3, laneXml->getIntAttribute("target", 0)));
                    lane.trackIndex = laneXml->getIntAttribute("track", -1);
                    lane.mode = static_cast<AutomationMode>(juce::jlimit(0, 3, laneXml->getIntAttribute("mode", 0)));
                    lane.enabled = laneXml->getBoolAttribute("enabled", true);
                    for (auto* pointXml = laneXml->getFirstChildElement(); pointXml != nullptr; pointXml = pointXml->getNextElement())
                    {
                        if (!pointXml->hasTagName("PT"))
                            continue;
                        AutomationPoint point;
                        point.beat = juce::jmax(0.0, pointXml->getDoubleAttribute("beat", 0.0));
                        point.value = static_cast<float>(pointXml->getDoubleAttribute("value", 0.0));
                        lane.points.push_back(point);
                    }

                    std::sort(lane.points.begin(), lane.points.end(),
                              [](const AutomationPoint& a, const AutomationPoint& b)
                              {
                                  return a.beat < b.beat;
                              });
                    outProject.automationLanes.push_back(std::move(lane));
                }
            }

            const auto projectDir = file.getParentDirectory();
            if (auto* clipsXml = root->getChildByName("CLIPS"))
            {
                for (auto* cXml = clipsXml->getFirstChildElement(); cXml != nullptr; cXml = cXml->getNextElement())
                {
                    if (!cXml->hasTagName("CLIP"))
                        continue;

                    Clip clip;
                    clip.name = cXml->getStringAttribute("name", "Clip");
                    clip.type = cXml->getStringAttribute("type", "midi").equalsIgnoreCase("audio")
                        ? ClipType::Audio
                        : ClipType::MIDI;
                    clip.startBeat = juce::jmax(0.0, cXml->getDoubleAttribute("start", 0.0));
                    clip.lengthBeats = juce::jmax(0.0625, cXml->getDoubleAttribute("length", 1.0));
                    clip.offsetBeats = juce::jmax(0.0, cXml->getDoubleAttribute("offset", 0.0));
                    clip.trackIndex = juce::jmax(0, cXml->getIntAttribute("track", 0));
                    clip.gainLinear = static_cast<float>(cXml->getDoubleAttribute("gain", 1.0));
                    clip.fadeInBeats = juce::jmax(0.0, cXml->getDoubleAttribute("fadeIn", 0.0));
                    clip.fadeOutBeats = juce::jmax(0.0, cXml->getDoubleAttribute("fadeOut", 0.0));
                    clip.audioSampleRate = juce::jmax(1.0, cXml->getDoubleAttribute("audioSampleRate", 44100.0));
                    clip.detectedTempoBpm = juce::jmax(0.0, cXml->getDoubleAttribute("detectedTempoBpm", 0.0));

                    if (clip.type == ClipType::Audio)
                    {
                        juce::String absolutePath = cXml->getStringAttribute("audioPathAbsolute");
                        const juce::String relativePath = cXml->getStringAttribute("audioPathRelative");
                        juce::File audioFile(absolutePath);

                        if ((!audioFile.existsAsFile()) && relativePath.isNotEmpty())
                        {
                            audioFile = projectDir.getChildFile(relativePath);
                            absolutePath = audioFile.getFullPathName();
                        }

                        clip.audioFilePath = absolutePath;
                        if (audioFile.existsAsFile())
                        {
                            std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(audioFile));
                            if (reader != nullptr && reader->lengthInSamples > 0 && reader->numChannels > 0)
                            {
                                // Project load keeps audio clips disk-backed. Audio data is streamed at playback.
                                clip.audioData.reset();
                                clip.audioSampleRate = juce::jmax(1.0, reader->sampleRate);
                            }
                        }
                    }
                    else
                    {
                        if (auto* eventsXml = cXml->getChildByName("EVENTS"))
                        {
                            for (auto* eXml = eventsXml->getFirstChildElement(); eXml != nullptr; eXml = eXml->getNextElement())
                            {
                                if (!eXml->hasTagName("EV"))
                                    continue;

                                TimelineEvent event;
                                event.startBeat = juce::jmax(0.0, eXml->getDoubleAttribute("start", 0.0));
                                event.durationBeats = juce::jmax(0.001, eXml->getDoubleAttribute("length", 1.0));
                                event.noteNumber = juce::jlimit(0, 127, eXml->getIntAttribute("note", 60));
                                event.velocity = static_cast<uint8_t>(juce::jlimit(1, 127, eXml->getIntAttribute("velocity", 100)));
                                clip.events.push_back(event);
                            }
                        }

                        if (auto* ccXml = cXml->getChildByName("CC"))
                        {
                            for (auto* ccEvent = ccXml->getFirstChildElement(); ccEvent != nullptr; ccEvent = ccEvent->getNextElement())
                            {
                                if (!ccEvent->hasTagName("CCEV"))
                                    continue;

                                MidiCCEvent cc;
                                cc.beat = juce::jmax(0.0, ccEvent->getDoubleAttribute("beat", 0.0));
                                cc.controller = juce::jlimit(0, 127, ccEvent->getIntAttribute("controller", 1));
                                cc.value = static_cast<uint8_t>(juce::jlimit(0, 127, ccEvent->getIntAttribute("value", 0)));
                                clip.ccEvents.push_back(cc);
                            }
                        }
                    }

                    outProject.arrangement.push_back(std::move(clip));
                }
            }

            errorMessage.clear();
            return true;
        }
    };
}
