#pragma once
#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>
#include "TimelineModel.h"

namespace sampledex
{
    class BasicSynthSound : public juce::SynthesiserSound
    {
    public:
        bool appliesToNote(int) override { return true; }
        bool appliesToChannel(int) override { return true; }
    };

    class BasicSynthVoice : public juce::SynthesiserVoice
    {
    public:
        bool canPlaySound(juce::SynthesiserSound* sound) override
        {
            return dynamic_cast<BasicSynthSound*> (sound) != nullptr;
        }

        void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int) override
        {
            currentAngle = 0.0;
            level = velocity * 0.18f;
            tailOff = 0.0;
            const auto cyclesPerSample = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber)
                                       / juce::jmax(1.0, getSampleRate());
            angleDelta = cyclesPerSample * juce::MathConstants<double>::twoPi;
        }

        void stopNote(float, bool allowTailOff) override
        {
            if (allowTailOff)
            {
                if (tailOff == 0.0)
                    tailOff = 1.0;
            }
            else
            {
                clearCurrentNote();
                angleDelta = 0.0;
            }
        }

        void pitchWheelMoved(int) override {}
        void controllerMoved(int, int) override {}

        void renderNextBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
        {
            if (angleDelta == 0.0)
                return;

            if (tailOff > 0.0)
            {
                while (--numSamples >= 0)
                {
                    const float currentSample = static_cast<float>(std::sin(currentAngle) * level * tailOff);
                    for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
                        outputBuffer.addSample(channel, startSample, currentSample);

                    currentAngle += angleDelta;
                    ++startSample;
                    tailOff *= 0.992;
                    if (tailOff <= 0.005)
                    {
                        clearCurrentNote();
                        angleDelta = 0.0;
                        break;
                    }
                }
            }
            else
            {
                while (--numSamples >= 0)
                {
                    const float currentSample = static_cast<float>(std::sin(currentAngle) * level);
                    for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
                        outputBuffer.addSample(channel, startSample, currentSample);
                    currentAngle += angleDelta;
                    ++startSample;
                }
            }
        }

    private:
        double currentAngle = 0.0;
        double angleDelta = 0.0;
        float level = 0.0f;
        double tailOff = 0.0;
    };

    class Track : public juce::AudioProcessor
    {
    public:
        static constexpr int instrumentSlotIndex = -1;
        static constexpr int maxInsertSlots = 4;
        static constexpr int maxSendBuses = 4;
        static constexpr int maxInputSourcePairs = 64;

        enum class SendTapMode : int
        {
            PreFader = 0,
            PostFader = 1,
            PostPan = 2
        };

        enum class MonitorTapMode : int
        {
            PreInserts = 0,
            PostInserts = 1
        };

        enum class BuiltInInstrument
        {
            None,
            BasicSynth,
            Sampler
        };

        enum class BuiltInEffect : int
        {
            Compressor = 0,
            Limiter,
            Gate,
            Reverb,
            Delay,
            Saturation,
            Chorus,
            Flanger,
            Phaser,
            Count
        };
        static constexpr int builtInEffectCount = static_cast<int>(BuiltInEffect::Count);

        enum class ChannelType : int
        {
            Instrument = 0,
            Audio = 1,
            Aux = 2,
            Master = 3
        };

        enum class OutputTargetType : int
        {
            Master = 0,
            Bus = 1
        };

        enum class RenderTaskType : int
        {
            None = 0,
            Freeze = 1,
            Commit = 2,
            Export = 3
        };

        Track(const juce::String& trackName, juce::AudioPluginFormatManager& formatManager)
            : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
              name(trackName), fmtMgr(formatManager)
        {
            for (int i = 0; i < 8; ++i)
                fallbackSynth.addVoice(new BasicSynthVoice());
            fallbackSynth.addSound(new BasicSynthSound());

            for (int i = 0; i < 24; ++i)
                samplerSynth.addVoice(new juce::SamplerVoice());

            for (auto& cachedLoaded : cachedInsertSlotLoaded)
                cachedLoaded.store(false, std::memory_order_relaxed);
            for (auto& cachedBypassed : cachedInsertSlotBypassed)
                cachedBypassed.store(false, std::memory_order_relaxed);
            updatePluginUiCacheLocked();
        }

        ~Track() override {}

        juce::String getTrackName() const { return name; }
        void setTrackName(const juce::String& newName)
        {
            const auto trimmed = newName.trim();
            if (trimmed.isNotEmpty())
                name = trimmed;
        }

        // --- Mixer Controls ---
        void setVolume(float gain) { volume.store(gain); }
        float getVolume() const { return volume.load(); }
        void setPan(float p) { pan.store(p); } 
        float getPan() const { return pan.load(); }
        void setSendLevel(float level) { sendLevel.store(juce::jlimit(0.0f, 1.0f, level)); }
        float getSendLevel() const { return sendLevel.load(); }
        void setSendTapMode(SendTapMode mode)
        {
            sendTapMode.store(static_cast<int>(mode), std::memory_order_relaxed);
        }
        SendTapMode getSendTapMode() const
        {
            const int raw = juce::jlimit(0, 2, sendTapMode.load(std::memory_order_relaxed));
            return static_cast<SendTapMode>(raw);
        }
        void setSendPreFader(bool shouldBePre)
        {
            setSendTapMode(shouldBePre ? SendTapMode::PreFader : SendTapMode::PostFader);
        }
        bool isSendPreFader() const { return getSendTapMode() == SendTapMode::PreFader; }
        void setSendTargetBus(int busIndex)
        {
            sendTargetBus.store(juce::jlimit(0, maxSendBuses - 1, busIndex), std::memory_order_relaxed);
        }
        int getSendTargetBus() const
        {
            return juce::jlimit(0, maxSendBuses - 1, sendTargetBus.load(std::memory_order_relaxed));
        }
        void setMute(bool m) { mute.store(m); }
        bool isMuted() const { return mute.load(); }
        void setSolo(bool s) { solo.store(s); }
        bool isSolo() const { return solo.load(); }
        void setArm(bool a) { arm.store(a); }
        bool isArmed() const { return arm.load(); }
        void setInputMonitoring(bool enabled) { inputMonitoring.store(enabled); }
        bool isInputMonitoringEnabled() const { return inputMonitoring.load(); }
        void setChannelType(ChannelType type)
        {
            channelType.store(static_cast<int>(type), std::memory_order_relaxed);
        }
        ChannelType getChannelType() const
        {
            const int raw = juce::jlimit(0, 3, channelType.load(std::memory_order_relaxed));
            return static_cast<ChannelType>(raw);
        }
        void setOutputTargetType(OutputTargetType type)
        {
            outputTargetType.store(static_cast<int>(type), std::memory_order_relaxed);
        }
        OutputTargetType getOutputTargetType() const
        {
            const int raw = juce::jlimit(0, 1, outputTargetType.load(std::memory_order_relaxed));
            return static_cast<OutputTargetType>(raw);
        }
        void setOutputTargetBus(int busIndex)
        {
            outputTargetBus.store(juce::jlimit(0, maxSendBuses - 1, busIndex), std::memory_order_relaxed);
        }
        int getOutputTargetBus() const
        {
            return juce::jlimit(0, maxSendBuses - 1, outputTargetBus.load(std::memory_order_relaxed));
        }
        void routeOutputToMaster()
        {
            setOutputTargetType(OutputTargetType::Master);
        }
        void routeOutputToBus(int busIndex)
        {
            setOutputTargetType(OutputTargetType::Bus);
            setOutputTargetBus(busIndex);
        }
        void setFrozenPlaybackOnly(bool shouldFreeze)
        {
            frozenPlaybackOnly.store(shouldFreeze, std::memory_order_relaxed);
        }
        bool isFrozenPlaybackOnly() const
        {
            return frozenPlaybackOnly.load(std::memory_order_relaxed);
        }
        void setFrozenRenderPath(const juce::String& path)
        {
            juce::ScopedLock sl(processLock);
            frozenRenderPath = path;
        }
        juce::String getFrozenRenderPath() const
        {
            juce::ScopedLock sl(processLock);
            return frozenRenderPath;
        }
        void setRenderTaskState(RenderTaskType taskType, bool active, float progress)
        {
            renderTaskType.store(static_cast<int>(taskType), std::memory_order_relaxed);
            renderTaskActive.store(active, std::memory_order_relaxed);
            renderTaskProgress.store(juce::jlimit(0.0f, 1.0f, progress), std::memory_order_relaxed);
        }
        bool isRenderTaskActive() const
        {
            return renderTaskActive.load(std::memory_order_relaxed);
        }
        float getRenderTaskProgress() const
        {
            return renderTaskProgress.load(std::memory_order_relaxed);
        }
        RenderTaskType getRenderTaskType() const
        {
            const int raw = juce::jlimit(0, 3, renderTaskType.load(std::memory_order_relaxed));
            return static_cast<RenderTaskType>(raw);
        }
        juce::String getRenderTaskLabel() const
        {
            switch (getRenderTaskType())
            {
                case RenderTaskType::Freeze: return "Freeze";
                case RenderTaskType::Commit: return "Commit";
                case RenderTaskType::Export: return "Export";
                case RenderTaskType::None:
                default: return {};
            }
        }
        void setInputSourcePair(int pairIndex)
        {
            inputSourcePair.store(juce::jlimit(-1, maxInputSourcePairs - 1, pairIndex), std::memory_order_relaxed);
        }
        int getInputSourcePair() const
        {
            return juce::jlimit(-1, maxInputSourcePairs - 1, inputSourcePair.load(std::memory_order_relaxed));
        }
        int getInputSourceStartChannel() const
        {
            const int pair = getInputSourcePair();
            return pair < 0 ? -1 : pair * 2;
        }
        void setInputMonitorGain(float gain)
        {
            inputMonitorGain.store(juce::jlimit(0.0f, 2.0f, gain), std::memory_order_relaxed);
        }
        float getInputMonitorGain() const
        {
            return inputMonitorGain.load(std::memory_order_relaxed);
        }
        void setMonitorTapMode(MonitorTapMode mode)
        {
            monitorTapMode.store(static_cast<int>(mode), std::memory_order_relaxed);
        }
        MonitorTapMode getMonitorTapMode() const
        {
            const int raw = juce::jlimit(0, 1, monitorTapMode.load(std::memory_order_relaxed));
            return static_cast<MonitorTapMode>(raw);
        }
        float getMeterLevel() const { return currentLevel.load(); }
        float getMeterPeakLevel() const { return meterPeakLevel.load(std::memory_order_relaxed); }
        float getMeterRmsLevel() const { return meterRmsLevel.load(std::memory_order_relaxed); }
        float getPostFaderOutputPeak() const { return postFaderOutputPeak.load(std::memory_order_relaxed); }
        bool isMeterClipping() const { return meterClipHoldFrames.load(std::memory_order_relaxed) > 0; }
        int getTotalPluginLatencySamples() const
        {
            juce::ScopedLock sl(processLock);
            int total = 0;
            if (instrumentSlot.instance != nullptr && !instrumentSlot.bypassed)
                total += juce::jmax(0, instrumentSlot.instance->getLatencySamples());
            for (const auto& slot : pluginSlots)
            {
                if (slot.instance == nullptr || slot.bypassed)
                    continue;
                total += juce::jmax(0, slot.instance->getLatencySamples());
            }
            return juce::jmax(0, total);
        }
        int getInsertPluginLatencySamples() const
        {
            juce::ScopedLock sl(processLock);
            int total = 0;
            for (const auto& slot : pluginSlots)
            {
                if (slot.instance == nullptr || slot.bypassed)
                    continue;
                total += juce::jmax(0, slot.instance->getLatencySamples());
            }
            return juce::jmax(0, total);
        }
        float getInputMeterPeakLevel() const { return inputMeterPeakLevel.load(std::memory_order_relaxed); }
        float getInputMeterRmsLevel() const { return inputMeterRmsLevel.load(std::memory_order_relaxed); }
        float getInputPeakHoldLevel() const { return inputMeterHoldLevel.load(std::memory_order_relaxed); }
        bool isInputMeterClipping() const { return inputMeterClipHoldFrames.load(std::memory_order_relaxed) > 0; }
        void clearInputPeakHold()
        {
            inputMeterPeakLevel.store(0.0f, std::memory_order_relaxed);
            inputMeterRmsLevel.store(0.0f, std::memory_order_relaxed);
            inputMeterHoldLevel.store(0.0f, std::memory_order_relaxed);
            inputMeterClipHoldFrames.store(0, std::memory_order_relaxed);
        }
        void setEqEnabled(bool shouldEnable)
        {
            eqEnabled.store(shouldEnable, std::memory_order_relaxed);
            eqDirty.store(true, std::memory_order_relaxed);
        }
        bool isEqEnabled() const { return eqEnabled.load(std::memory_order_relaxed); }
        void setEqBandGains(float lowGainDbIn, float midGainDbIn, float highGainDbIn)
        {
            eqLowGainDb.store(juce::jlimit(-24.0f, 24.0f, lowGainDbIn), std::memory_order_relaxed);
            eqMidGainDb.store(juce::jlimit(-24.0f, 24.0f, midGainDbIn), std::memory_order_relaxed);
            eqHighGainDb.store(juce::jlimit(-24.0f, 24.0f, highGainDbIn), std::memory_order_relaxed);
            eqDirty.store(true, std::memory_order_relaxed);
        }
        float getEqLowGainDb() const { return eqLowGainDb.load(std::memory_order_relaxed); }
        float getEqMidGainDb() const { return eqMidGainDb.load(std::memory_order_relaxed); }
        float getEqHighGainDb() const { return eqHighGainDb.load(std::memory_order_relaxed); }

        static juce::String getBuiltInEffectDisplayName(BuiltInEffect effect)
        {
            switch (effect)
            {
                case BuiltInEffect::Compressor: return "Compressor";
                case BuiltInEffect::Limiter: return "Limiter";
                case BuiltInEffect::Gate: return "Gate";
                case BuiltInEffect::Reverb: return "Reverb";
                case BuiltInEffect::Delay: return "Delay";
                case BuiltInEffect::Saturation: return "Saturation";
                case BuiltInEffect::Chorus: return "Chorus";
                case BuiltInEffect::Flanger: return "Flanger";
                case BuiltInEffect::Phaser: return "Phaser";
                case BuiltInEffect::Count:
                default: return "Unknown";
            }
        }

        void setBuiltInEffectEnabled(BuiltInEffect effect, bool shouldEnable)
        {
            const auto bit = getBuiltInEffectBit(effect);
            if (bit == 0u)
                return;

            auto mask = builtInEffectMask.load(std::memory_order_relaxed);
            if (shouldEnable)
                mask |= bit;
            else
                mask &= ~bit;

            builtInEffectMask.store(mask & builtInEffectAllMask, std::memory_order_relaxed);
        }

        bool isBuiltInEffectEnabled(BuiltInEffect effect) const
        {
            const auto bit = getBuiltInEffectBit(effect);
            if (bit == 0u)
                return false;
            return (builtInEffectMask.load(std::memory_order_relaxed) & bit) != 0u;
        }

        void setBuiltInEffectsMask(std::uint32_t mask)
        {
            builtInEffectMask.store(mask & builtInEffectAllMask, std::memory_order_relaxed);
        }

        std::uint32_t getBuiltInEffectsMask() const
        {
            return builtInEffectMask.load(std::memory_order_relaxed) & builtInEffectAllMask;
        }

        juce::String getBuiltInEffectsSummary() const
        {
            const auto mask = getBuiltInEffectsMask();
            juce::StringArray names;
            for (int i = 0; i < builtInEffectCount; ++i)
            {
                const auto effect = static_cast<BuiltInEffect>(i);
                const auto bit = getBuiltInEffectBit(effect);
                if ((mask & bit) != 0u)
                    names.add(getBuiltInEffectDisplayName(effect));
            }
            return names.joinIntoString(", ");
        }

        BuiltInInstrument getBuiltInInstrumentMode() const
        {
            juce::ScopedLock sl(processLock);
            return builtInInstrumentMode;
        }

        void useBuiltInSynthInstrument()
        {
            std::unique_ptr<juce::AudioPluginInstance> oldInstrument;
            {
                juce::ScopedLock sl(processLock);
                oldInstrument = std::move(instrumentSlot.instance);
                instrumentSlot.description = {};
                instrumentSlot.hasDescription = false;
                instrumentSlot.bypassed = false;
                builtInInstrumentMode = BuiltInInstrument::BasicSynth;
            }

            if (oldInstrument)
                oldInstrument->releaseResources();
        }

        void disableBuiltInInstrument()
        {
            juce::ScopedLock sl(processLock);
            builtInInstrumentMode = BuiltInInstrument::None;
        }

        bool loadSamplerSoundFromFile(const juce::File& file, juce::String& errorMsg)
        {
            if (!file.existsAsFile())
            {
                errorMsg = "Sampler file does not exist.";
                return false;
            }

            static std::once_flag formatInitFlag;
            static juce::AudioFormatManager samplerFormatManager;
            std::call_once(formatInitFlag, []
            {
                samplerFormatManager.registerBasicFormats();
            });

            std::unique_ptr<juce::AudioFormatReader> reader(samplerFormatManager.createReaderFor(file));
            if (reader == nullptr)
            {
                errorMsg = "Unsupported sample file.";
                return false;
            }

            juce::BigInteger noteRange;
            noteRange.setRange(0, 128, true);

            auto sound = std::make_unique<juce::SamplerSound>(
                file.getFileNameWithoutExtension(),
                *reader,
                noteRange,
                60,
                0.003,
                0.18,
                20.0);

            std::unique_ptr<juce::AudioPluginInstance> oldInstrument;
            {
                juce::ScopedLock sl(processLock);
                oldInstrument = std::move(instrumentSlot.instance);
                instrumentSlot.description = {};
                instrumentSlot.hasDescription = false;
                instrumentSlot.bypassed = false;
                samplerSynth.clearSounds();
                samplerSynth.addSound(sound.release());
                samplerSamplePath = file.getFullPathName();
                builtInInstrumentMode = BuiltInInstrument::Sampler;
            }

            if (oldInstrument)
                oldInstrument->releaseResources();

            errorMsg.clear();
            return true;
        }

        juce::String getSamplerSamplePath() const
        {
            juce::ScopedLock sl(processLock);
            return samplerSamplePath;
        }

        bool hasSamplerSoundLoaded() const
        {
            juce::ScopedLock sl(processLock);
            return samplerSynth.getNumSounds() > 0;
        }

        // --- Plugins ---
        bool hasInstrumentPlugin() const
        {
            juce::ScopedLock sl(processLock);
            return instrumentSlot.instance != nullptr;
        }

        juce::String getInstrumentName() const
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance != nullptr)
                return instrumentSlot.description.name;
            if (builtInInstrumentMode == BuiltInInstrument::Sampler && samplerSynth.getNumSounds() > 0)
                return "Built-in Sampler";
            if (builtInInstrumentMode == BuiltInInstrument::BasicSynth)
                return "Built-in Synth";
            return "None";
        }

        bool loadInstrumentPlugin(const juce::PluginDescription& desc, juce::String& errorMsg)
        {
            double sampleRateToUse = 44100.0;
            int blockSizeToUse = 512;
            juce::AudioPlayHead* playHeadToUse = nullptr;
            {
                juce::ScopedLock sl(processLock);
                sampleRateToUse = preparedSampleRate > 0.0 ? preparedSampleRate
                                  : (juce::AudioProcessor::getSampleRate() > 0.0 ? juce::AudioProcessor::getSampleRate() : 44100.0);
                blockSizeToUse = preparedBlockSize > 0 ? preparedBlockSize
                                 : (juce::AudioProcessor::getBlockSize() > 0 ? juce::AudioProcessor::getBlockSize() : 512);
                playHeadToUse = transportPlayHead;
            }

            std::unique_ptr<juce::AudioPluginInstance> instance =
                fmtMgr.createPluginInstance(desc, sampleRateToUse, blockSizeToUse, errorMsg);
            if (instance == nullptr)
                return false;

            if (!configurePluginBusLayout(*instance, true))
            {
                errorMsg = "Plugin bus layout is incompatible with track hosting.";
                return false;
            }

            const int instrumentInputs = juce::jmax(0, juce::jmin(2, instance->getMainBusNumInputChannels()));
            const int instrumentOutputs = getUsableMainOutputChannels(*instance);
            if (instrumentOutputs <= 0)
            {
                errorMsg = "Instrument plugin does not expose a usable output bus.";
                return false;
            }
            instance->setPlayConfigDetails(instrumentInputs, instrumentOutputs, sampleRateToUse, blockSizeToUse);
            instance->setRateAndBufferSizeDetails(sampleRateToUse, blockSizeToUse);
            instance->setPlayHead(playHeadToUse);
            instance->prepareToPlay(sampleRateToUse, blockSizeToUse);
            instance->setNonRealtime(false);
            if (!validatePluginInstanceSafety(*instance, true, blockSizeToUse, errorMsg))
            {
                instance->releaseResources();
                return false;
            }

            std::unique_ptr<juce::AudioPluginInstance> oldInstrument;
            {
                juce::ScopedLock sl(processLock);
                oldInstrument = std::move(instrumentSlot.instance);
                instrumentSlot.instance = std::move(instance);
                instrumentSlot.description = desc;
                instrumentSlot.hasDescription = true;
                instrumentSlot.bypassed = false;
                builtInInstrumentMode = BuiltInInstrument::None;

                const int requiredChannels = getRequiredPluginChannelsLocked(2);
                ensurePluginProcessBufferCapacityLocked(requiredChannels, juce::jmax(8192, blockSizeToUse));
            }

            if (oldInstrument)
                oldInstrument->releaseResources();

            return true;
        }

        void loadPlugin(const juce::PluginDescription& desc, juce::String& errorMsg)
        {
            if (desc.isInstrument)
                loadInstrumentPlugin(desc, errorMsg);
            else
                loadPluginInSlot(0, desc, errorMsg);
        }

        bool loadPluginInSlot(int slotIndex, const juce::PluginDescription& desc, juce::String& errorMsg)
        {
            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
            {
                errorMsg = "Invalid insert slot index.";
                return false;
            }

            double sampleRateToUse = 44100.0;
            int blockSizeToUse = 512;
            juce::AudioPlayHead* playHeadToUse = nullptr;
            {
                juce::ScopedLock sl(processLock);
                sampleRateToUse = preparedSampleRate > 0.0 ? preparedSampleRate
                                  : (juce::AudioProcessor::getSampleRate() > 0.0 ? juce::AudioProcessor::getSampleRate() : 44100.0);
                blockSizeToUse = preparedBlockSize > 0 ? preparedBlockSize
                                 : (juce::AudioProcessor::getBlockSize() > 0 ? juce::AudioProcessor::getBlockSize() : 512);
                playHeadToUse = transportPlayHead;
            }

            std::unique_ptr<juce::AudioPluginInstance> instance =
                fmtMgr.createPluginInstance(desc, sampleRateToUse, blockSizeToUse, errorMsg);
            if (instance == nullptr)
                return false;

            if (!configurePluginBusLayout(*instance, false))
            {
                errorMsg = "Plugin bus layout is incompatible with insert hosting.";
                return false;
            }

            const int effectInputs = juce::jmax(1, juce::jmin(2, instance->getMainBusNumInputChannels()));
            const int effectOutputs = juce::jmax(effectInputs, getUsableMainOutputChannels(*instance));
            if (effectOutputs <= 0)
            {
                errorMsg = "Effect plugin does not expose a usable output bus.";
                return false;
            }
            instance->setPlayConfigDetails(effectInputs, effectOutputs, sampleRateToUse, blockSizeToUse);
            instance->setRateAndBufferSizeDetails(sampleRateToUse, blockSizeToUse);
            instance->setPlayHead(playHeadToUse);
            instance->prepareToPlay(sampleRateToUse, blockSizeToUse);
            instance->setNonRealtime(false);
            if (!validatePluginInstanceSafety(*instance, false, blockSizeToUse, errorMsg))
            {
                instance->releaseResources();
                return false;
            }

            std::unique_ptr<juce::AudioPluginInstance> oldInsert;
            {
                juce::ScopedLock sl(processLock);
                auto& slot = pluginSlots[static_cast<size_t>(slotIndex)];
                oldInsert = std::move(slot.instance);
                slot.instance = std::move(instance);
                slot.description = desc;
                slot.hasDescription = true;
                slot.bypassed = false;

                const int requiredChannels = getRequiredPluginChannelsLocked(2);
                ensurePluginProcessBufferCapacityLocked(requiredChannels, juce::jmax(8192, blockSizeToUse));
            }

            if (oldInsert)
                oldInsert->releaseResources();

            return true;
        }

        void setTransportPlayHead(juce::AudioPlayHead* newPlayHead)
        {
            juce::ScopedLock sl(processLock);
            transportPlayHead = newPlayHead;
            if (instrumentSlot.instance)
                instrumentSlot.instance->setPlayHead(transportPlayHead);
            for (auto& slot : pluginSlots)
                if (slot.instance)
                    slot.instance->setPlayHead(transportPlayHead);
        }

        juce::AudioProcessorEditor* createPluginEditor()
        {
            return createPluginEditorForSlot(getFirstLoadedPluginSlot());
        }

        juce::AudioProcessorEditor* createPluginEditorForSlot(int slotIndex)
        {
            juce::ScopedLock sl(processLock);
            if (slotIndex == instrumentSlotIndex)
            {
                if (instrumentSlot.instance)
                    return instrumentSlot.instance->createEditorIfNeeded();
                return nullptr;
            }

            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return nullptr;

            auto& slot = pluginSlots[static_cast<size_t>(slotIndex)];
            return slot.instance ? slot.instance->createEditorIfNeeded() : nullptr;
        }

        bool hasPlugin() const
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance)
                return true;
            for (const auto& slot : pluginSlots)
                if (slot.instance)
                    return true;
            return false;
        }

        bool hasPluginInSlot(int slotIndex) const
        {
            juce::ScopedLock sl(processLock);
            updatePluginUiCacheLocked();
            return getSlotLoadedLocked(slotIndex);
        }

        bool hasPluginInSlotNonBlocking(int slotIndex) const
        {
            const juce::ScopedTryLock sl(processLock);
            if (sl.isLocked())
            {
                updatePluginUiCacheLocked();
                return getSlotLoadedLocked(slotIndex);
            }

            if (slotIndex == instrumentSlotIndex)
                return cachedInstrumentSlotLoaded.load(std::memory_order_relaxed);
            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return false;
            return cachedInsertSlotLoaded[static_cast<size_t>(slotIndex)].load(std::memory_order_relaxed);
        }

        int getPluginSlotCount() const { return maxInsertSlots; }

        int getFirstLoadedPluginSlot() const
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance)
                return instrumentSlotIndex;

            for (int i = 0; i < maxInsertSlots; ++i)
                if (pluginSlots[static_cast<size_t>(i)].instance != nullptr)
                    return i;

            return instrumentSlotIndex;
        }

        juce::String getPluginNameForSlot(int slotIndex) const
        {
            juce::ScopedLock sl(processLock);
            updatePluginUiCacheLocked();
            return getSlotNameLocked(slotIndex);
        }

        juce::String getPluginNameForSlotNonBlocking(int slotIndex) const
        {
            const juce::ScopedTryLock sl(processLock);
            if (sl.isLocked())
            {
                updatePluginUiCacheLocked();
                return getSlotNameLocked(slotIndex);
            }

            const juce::SpinLock::ScopedLockType cacheLock(pluginUiCacheLock);
            if (slotIndex == instrumentSlotIndex)
                return cachedInstrumentSlotName;
            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return {};
            return cachedInsertSlotNames[static_cast<size_t>(slotIndex)];
        }

        bool getPluginDescriptionForSlot(int slotIndex, juce::PluginDescription& outDescription) const
        {
            juce::ScopedLock sl(processLock);
            if (slotIndex == instrumentSlotIndex)
            {
                if (instrumentSlot.instance == nullptr)
                    return false;
                outDescription = instrumentSlot.description;
                return true;
            }

            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return false;

            const auto& slot = pluginSlots[static_cast<size_t>(slotIndex)];
            if (slot.instance == nullptr)
                return false;

            outDescription = slot.description;
            return true;
        }

        juce::String getPluginStateForSlot(int slotIndex) const
        {
            juce::ScopedLock sl(processLock);
            if (slotIndex == instrumentSlotIndex)
            {
                if (instrumentSlot.instance == nullptr)
                    return {};
                juce::MemoryBlock block;
                instrumentSlot.instance->getStateInformation(block);
                return block.toBase64Encoding();
            }

            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return {};

            const auto& slot = pluginSlots[static_cast<size_t>(slotIndex)];
            if (slot.instance == nullptr)
                return {};

            juce::MemoryBlock block;
            slot.instance->getStateInformation(block);
            return block.toBase64Encoding();
        }

        bool setPluginStateForSlot(int slotIndex, const juce::String& encodedState)
        {
            juce::ScopedLock sl(processLock);
            if (encodedState.isEmpty())
                return false;

            PluginSlot* slot = nullptr;
            if (slotIndex == instrumentSlotIndex)
            {
                if (instrumentSlot.instance == nullptr)
                    return false;
                slot = &instrumentSlot;
            }
            else
            {
                if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                    return false;
                slot = &pluginSlots[static_cast<size_t>(slotIndex)];
                if (slot->instance == nullptr)
                    return false;
            }

            juce::MemoryBlock block;
            if (!block.fromBase64Encoding(encodedState))
                return false;

            slot->instance->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
            return true;
        }

        juce::String getPluginSummary() const
        {
            juce::ScopedLock sl(processLock);
            juce::StringArray names;

            if (instrumentSlot.instance != nullptr)
                names.add("INST: " + instrumentSlot.description.name);
            else if (builtInInstrumentMode == BuiltInInstrument::Sampler && samplerSynth.getNumSounds() > 0)
                names.add("INST: Built-in Sampler");
            else if (builtInInstrumentMode == BuiltInInstrument::BasicSynth)
                names.add("INST: Built-in Synth");
            else
                names.add("INST: None");

            for (int i = 0; i < maxInsertSlots; ++i)
            {
                const auto& slot = pluginSlots[static_cast<size_t>(i)];
                if (slot.instance != nullptr)
                    names.add("I" + juce::String(i + 1) + ": " + slot.description.name);
            }

            const auto builtInFxSummary = getBuiltInEffectsSummary();
            if (builtInFxSummary.isNotEmpty())
                names.add("DSP: " + builtInFxSummary);

            return names.joinIntoString(" | ");
        }

        bool isPluginSlotBypassed(int slotIndex) const
        {
            juce::ScopedLock sl(processLock);
            updatePluginUiCacheLocked();
            return getSlotBypassedLocked(slotIndex);
        }

        bool isPluginSlotBypassedNonBlocking(int slotIndex) const
        {
            const juce::ScopedTryLock sl(processLock);
            if (sl.isLocked())
            {
                updatePluginUiCacheLocked();
                return getSlotBypassedLocked(slotIndex);
            }

            if (slotIndex == instrumentSlotIndex)
                return cachedInstrumentSlotBypassed.load(std::memory_order_relaxed);
            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return false;
            return cachedInsertSlotBypassed[static_cast<size_t>(slotIndex)].load(std::memory_order_relaxed);
        }

        void setPluginSlotBypassed(int slotIndex, bool shouldBypass)
        {
            juce::ScopedLock sl(processLock);
            if (slotIndex == instrumentSlotIndex)
            {
                instrumentSlot.bypassed = shouldBypass;
                updatePluginUiCacheLocked();
                return;
            }

            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return;
            pluginSlots[static_cast<size_t>(slotIndex)].bypassed = shouldBypass;
            updatePluginUiCacheLocked();
        }

        void clearPluginSlot(int slotIndex)
        {
            std::unique_ptr<juce::AudioPluginInstance> oldInstance;
            bool clearedInstrument = false;
            {
                juce::ScopedLock sl(processLock);
                if (slotIndex == instrumentSlotIndex)
                {
                    oldInstance = std::move(instrumentSlot.instance);
                    instrumentSlot.description = {};
                    instrumentSlot.hasDescription = false;
                    instrumentSlot.bypassed = false;
                    builtInInstrumentMode = BuiltInInstrument::BasicSynth;
                    samplerSamplePath.clear();
                    clearedInstrument = true;
                }
                else
                {
                    if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                        return;

                    auto& slot = pluginSlots[static_cast<size_t>(slotIndex)];
                    oldInstance = std::move(slot.instance);
                    slot.description = {};
                    slot.hasDescription = false;
                    slot.bypassed = false;
                }
            }

            if (oldInstance)
                oldInstance->releaseResources();
            juce::ignoreUnused(clearedInstrument);
        }

        bool movePluginSlot(int fromIndex, int toIndex)
        {
            juce::ScopedLock sl(processLock);
            if (!juce::isPositiveAndBelow(fromIndex, maxInsertSlots)
                || !juce::isPositiveAndBelow(toIndex, maxInsertSlots)
                || fromIndex == toIndex)
                return false;

            auto movedSlot = std::move(pluginSlots[static_cast<size_t>(fromIndex)]);
            if (fromIndex < toIndex)
            {
                for (int i = fromIndex; i < toIndex; ++i)
                    pluginSlots[static_cast<size_t>(i)] = std::move(pluginSlots[static_cast<size_t>(i + 1)]);
            }
            else
            {
                for (int i = fromIndex; i > toIndex; --i)
                    pluginSlots[static_cast<size_t>(i)] = std::move(pluginSlots[static_cast<size_t>(i - 1)]);
            }
            pluginSlots[static_cast<size_t>(toIndex)] = std::move(movedSlot);
            return true;
        }

        juce::String getPluginID() const
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance != nullptr)
                return instrumentSlot.description.fileOrIdentifier;
            for (const auto& slot : pluginSlots)
                if (slot.instance != nullptr)
                    return slot.description.fileOrIdentifier;
            return {};
        }

        juce::String getPluginFormat() const
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance != nullptr)
                return instrumentSlot.description.pluginFormatName;
            for (const auto& slot : pluginSlots)
                if (slot.instance != nullptr)
                    return slot.description.pluginFormatName;
            return {};
        }

        juce::String getPluginName() const
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance != nullptr)
                return instrumentSlot.description.name;
            for (const auto& slot : pluginSlots)
                if (slot.instance != nullptr)
                    return slot.description.name;
            if (builtInInstrumentMode == BuiltInInstrument::Sampler && samplerSynth.getNumSounds() > 0)
                return "Built-in Sampler";
            if (builtInInstrumentMode == BuiltInInstrument::BasicSynth)
                return "Built-in Synth";
            return {};
        }

        juce::String getPluginState()
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance)
            {
                juce::MemoryBlock block;
                instrumentSlot.instance->getStateInformation(block);
                return block.toBase64Encoding();
            }

            auto& slot = pluginSlots[0];
            if (!slot.instance)
                return {};

            juce::MemoryBlock block;
            slot.instance->getStateInformation(block);
            return block.toBase64Encoding();
        }

        void restorePluginState(const juce::String& stateParams,
                                const juce::PluginDescription& desc,
                                juce::String& errorMsg)
        {
            const bool loaded = desc.isInstrument
                ? loadInstrumentPlugin(desc, errorMsg)
                : loadPluginInSlot(0, desc, errorMsg);
            if (!loaded || errorMsg.isNotEmpty())
                return;

            if (stateParams.isNotEmpty())
                setPluginStateForSlot(desc.isInstrument ? instrumentSlotIndex : 0, stateParams);
        }

        // --- Recording ---
        void startRecording() 
        { 
            isRecordingActive.store(false, std::memory_order_release);
            for (auto& noteState : activeNotes)
                noteState.active = false;
            recordedReadIndex.store(0, std::memory_order_relaxed);
            recordedWriteIndex.store(0, std::memory_order_relaxed);
            droppedRecordedEvents.store(0, std::memory_order_relaxed);
            isRecordingActive.store(true, std::memory_order_release);
        }

        void stopRecording()
        {
            isRecordingActive.store(false, std::memory_order_release);
        }
        
        void addMidiToRecord(const juce::MidiMessage& m, double currentBeat)
        {
            if (!isRecordingActive.load(std::memory_order_acquire) || !arm.load())
                return;

            if (m.isNoteOn())
            {
                const int note = juce::jlimit(0, 127, m.getNoteNumber());
                auto& noteState = activeNotes[static_cast<size_t>(note)];
                noteState.startBeat = currentBeat;
                noteState.velocity = static_cast<int>(m.getVelocity());
                noteState.active = true;
            }
            else if (m.isNoteOff())
            {
                const int note = juce::jlimit(0, 127, m.getNoteNumber());
                auto& noteState = activeNotes[static_cast<size_t>(note)];
                if (!noteState.active)
                    return;

                TimelineEvent newEvent;
                newEvent.startBeat = noteState.startBeat;
                newEvent.durationBeats = juce::jmax(0.001, currentBeat - noteState.startBeat);
                newEvent.noteNumber = note;
                newEvent.velocity = static_cast<uint8_t>(juce::jlimit(0, 127, noteState.velocity));

                const int read = recordedReadIndex.load(std::memory_order_acquire);
                const int write = recordedWriteIndex.load(std::memory_order_relaxed);
                const int nextWrite = (write + 1) % recordedEventCapacity;
                if (nextWrite == read)
                {
                    droppedRecordedEvents.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    recordedEvents[static_cast<size_t>(write)] = newEvent;
                    recordedWriteIndex.store(nextWrite, std::memory_order_release);
                }

                noteState.active = false;
            }
        }
        
        void flushRecordingToClip(std::vector<TimelineEvent>& outEvents, double startBeat)
        {
            int read = recordedReadIndex.load(std::memory_order_relaxed);
            const int write = recordedWriteIndex.load(std::memory_order_acquire);
            while (read != write)
            {
                const auto& recordedEvent = recordedEvents[static_cast<size_t>(read)];
                TimelineEvent clipEvent = recordedEvent;
                clipEvent.startBeat -= startBeat;
                outEvents.push_back(clipEvent);
                read = (read + 1) % recordedEventCapacity;
            }
            recordedReadIndex.store(read, std::memory_order_release);
        }

        void panic()
        {
            juce::ScopedTryLock sl(processLock);
            if (!sl.isLocked())
                return;

            fallbackSynth.allNotesOff(0, false);
            samplerSynth.allNotesOff(0, false);
            for (auto& note : activeNotes)
                note.active = false;
        }

        // --- Audio Processing ---
        void prepareToPlay(double sampleRate, int samplesPerBlock) override
        {
            juce::ScopedLock sl(processLock);
            preparedSampleRate = sampleRate;
            preparedBlockSize = samplesPerBlock;
            prevLeftGain = volume.load();
            prevRightGain = volume.load();
            prevVolumeGain = volume.load();
            prevSendGain = sendLevel.load(std::memory_order_relaxed);
            monitorDcPrevInput = { 0.0f, 0.0f };
            monitorDcPrevOutput = { 0.0f, 0.0f };
            fallbackSynth.setCurrentPlaybackSampleRate(sampleRate);
            samplerSynth.setCurrentPlaybackSampleRate(sampleRate);
            eqDirty.store(true, std::memory_order_relaxed);
            builtInGateEnvelope = 0.0f;
            builtInDelayWritePosition = 0;
            builtInDelayLastSampleRate = juce::jmax(1.0, sampleRate);
            startupRampDurationSamples = juce::jmax(1, juce::roundToInt(sampleRate * 0.02));
            startupRampSamplesRemaining = startupRampDurationSamples;
            prepareBuiltInEffectsLocked(sampleRate, samplesPerBlock);

            if (instrumentSlot.instance)
            {
                const int instrumentInputs = juce::jmax(0, juce::jmin(2, instrumentSlot.instance->getMainBusNumInputChannels()));
                const int instrumentOutputs = getUsableMainOutputChannels(*instrumentSlot.instance);
                if (instrumentOutputs > 0)
                {
                    instrumentSlot.instance->setPlayConfigDetails(instrumentInputs,
                                                                  instrumentOutputs,
                                                                  sampleRate,
                                                                  samplesPerBlock);
                    instrumentSlot.instance->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
                    instrumentSlot.instance->setPlayHead(transportPlayHead);
                    instrumentSlot.instance->prepareToPlay(sampleRate, samplesPerBlock);
                    instrumentSlot.instance->setNonRealtime(false);
                    instrumentSlot.bypassed = false;
                }
                else
                {
                    instrumentSlot.bypassed = true;
                }
            }
            for (auto& slot : pluginSlots)
            {
                if (!slot.instance)
                    continue;
                const int effectInputs = juce::jmax(1, juce::jmin(2, slot.instance->getMainBusNumInputChannels()));
                const int effectOutputs = juce::jmax(effectInputs, getUsableMainOutputChannels(*slot.instance));
                if (effectOutputs <= 0)
                {
                    slot.bypassed = true;
                    continue;
                }
                slot.instance->setPlayConfigDetails(effectInputs,
                                                    effectOutputs,
                                                    sampleRate,
                                                    samplesPerBlock);
                slot.instance->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
                slot.instance->setPlayHead(transportPlayHead);
                slot.instance->prepareToPlay(sampleRate, samplesPerBlock);
                slot.instance->setNonRealtime(false);
                slot.bypassed = false;
            }

            const int requiredChannels = getRequiredPluginChannelsLocked(2);
            ensurePluginProcessBufferCapacityLocked(requiredChannels, juce::jmax(8192, samplesPerBlock));
            pluginProcessBuffer.clear();
            sendTapBuffer.clear();
            lastSuccessfulOutputBuffer.clear();
        }

        void releaseResources() override
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance)
                instrumentSlot.instance->releaseResources();
            for (auto& slot : pluginSlots)
                if (slot.instance)
                    slot.instance->releaseResources();
        }

        void setPluginsNonRealtime(bool shouldBeNonRealtime)
        {
            juce::ScopedLock sl(processLock);
            if (instrumentSlot.instance)
                instrumentSlot.instance->setNonRealtime(shouldBeNonRealtime);
            for (auto& slot : pluginSlots)
            {
                if (slot.instance)
                    slot.instance->setNonRealtime(shouldBeNonRealtime);
            }
        }

        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
        {
            juce::AudioBuffer<float> dummySend;
            processBlockAndSends(buffer, dummySend, midi, nullptr, nullptr, false);
        }

        void processBlockAndSends(juce::AudioBuffer<float>& mainBuffer,
                                  juce::AudioBuffer<float>& sendBuffer,
                                  juce::MidiBuffer& midi,
                                  const juce::AudioBuffer<float>* sourceAudio,
                                  const juce::AudioBuffer<float>* monitoredInput,
                                  bool monitorSafeInput)
        {
            juce::ScopedNoDenormals noDenormals;

            const auto updateMeterState = [&](const juce::AudioBuffer<float>& meterBuffer, bool clearFast = false)
            {
                float peak = 0.0f;
                float rms = 0.0f;
                for (int ch = 0; ch < meterBuffer.getNumChannels(); ++ch)
                {
                    peak = juce::jmax(peak, meterBuffer.getMagnitude(ch, 0, meterBuffer.getNumSamples()));
                    rms = juce::jmax(rms, meterBuffer.getRMSLevel(ch, 0, meterBuffer.getNumSamples()));
                }

                const float previousPeak = meterPeakLevel.load(std::memory_order_relaxed);
                const float peakDecay = clearFast ? 0.65f : 0.93f;
                const float newPeak = (peak > previousPeak)
                    ? peak
                    : juce::jmax(peak, previousPeak * peakDecay);
                meterPeakLevel.store(newPeak, std::memory_order_relaxed);

                const float previousRms = meterRmsLevel.load(std::memory_order_relaxed);
                const float rmsBlend = clearFast ? 0.35f : 0.18f;
                meterRmsLevel.store(previousRms + ((rms - previousRms) * rmsBlend), std::memory_order_relaxed);
                currentLevel.store(newPeak, std::memory_order_relaxed);

                if (peak >= 0.995f)
                    meterClipHoldFrames.store(48, std::memory_order_relaxed);
                else
                {
                    const int hold = meterClipHoldFrames.load(std::memory_order_relaxed);
                    if (hold > 0)
                        meterClipHoldFrames.store(hold - 1, std::memory_order_relaxed);
                }
            };

            const auto updateInputMeterState = [&](const juce::AudioBuffer<float>* meterBuffer, bool clearFast = false)
            {
                float peak = 0.0f;
                float rms = 0.0f;
                if (meterBuffer != nullptr)
                {
                    for (int ch = 0; ch < meterBuffer->getNumChannels(); ++ch)
                    {
                        peak = juce::jmax(peak, meterBuffer->getMagnitude(ch, 0, meterBuffer->getNumSamples()));
                        rms = juce::jmax(rms, meterBuffer->getRMSLevel(ch, 0, meterBuffer->getNumSamples()));
                    }
                }

                const float previousPeak = inputMeterPeakLevel.load(std::memory_order_relaxed);
                const float peakDecay = clearFast ? 0.78f : 0.94f;
                const float displayPeak = (peak > previousPeak)
                    ? peak
                    : juce::jmax(peak, previousPeak * peakDecay);
                inputMeterPeakLevel.store(displayPeak, std::memory_order_relaxed);

                const float previousRms = inputMeterRmsLevel.load(std::memory_order_relaxed);
                const float rmsBlend = clearFast ? 0.42f : 0.24f;
                inputMeterRmsLevel.store(previousRms + ((rms - previousRms) * rmsBlend), std::memory_order_relaxed);

                const float previousHold = inputMeterHoldLevel.load(std::memory_order_relaxed);
                const float holdDecay = clearFast ? 0.92f : 0.992f;
                const float hold = (peak > previousHold)
                    ? peak
                    : juce::jmax(displayPeak, previousHold * holdDecay);
                inputMeterHoldLevel.store(hold, std::memory_order_relaxed);

                if (peak >= 0.995f)
                    inputMeterClipHoldFrames.store(70, std::memory_order_relaxed);
                else
                {
                    const int holdFrames = inputMeterClipHoldFrames.load(std::memory_order_relaxed);
                    if (holdFrames > 0)
                        inputMeterClipHoldFrames.store(holdFrames - 1, std::memory_order_relaxed);
                }
            };

            const auto measurePeak = [](const juce::AudioBuffer<float>& source)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < source.getNumChannels(); ++ch)
                    peak = juce::jmax(peak, source.getMagnitude(ch, 0, source.getNumSamples()));
                return peak;
            };

            const auto storePostFaderPeak = [&](const juce::AudioBuffer<float>& source)
            {
                postFaderOutputPeak.store(measurePeak(source), std::memory_order_relaxed);
            };

            const auto applyLastGoodOutput = [&](float sendGain)
            {
                const int requiredSamples = mainBuffer.getNumSamples();
                const int fallbackChannels = juce::jmin(mainBuffer.getNumChannels(),
                                                        lastSuccessfulOutputBuffer.getNumChannels());
                const int fallbackSamples = juce::jmin(requiredSamples,
                                                       lastSuccessfulOutputBuffer.getNumSamples());
                mainBuffer.clear();
                if (fallbackChannels <= 0 || fallbackSamples <= 0)
                    return;

                for (int ch = 0; ch < fallbackChannels; ++ch)
                    mainBuffer.copyFrom(ch, 0, lastSuccessfulOutputBuffer, ch, 0, fallbackSamples);

                if (sendGain <= 0.0f || sendBuffer.getNumChannels() <= 0)
                    return;

                const int sendChannels = juce::jmin(sendBuffer.getNumChannels(), fallbackChannels);
                const int sendSamples = juce::jmin(sendBuffer.getNumSamples(), fallbackSamples);
                for (int ch = 0; ch < sendChannels; ++ch)
                    sendBuffer.addFrom(ch, 0, mainBuffer, ch, 0, sendSamples, sendGain);
            };

            const juce::ScopedTryLock sl(processLock);
            if (!sl.isLocked())
            {
                applyLastGoodOutput(sendLevel.load(std::memory_order_relaxed));
                updateMeterState(mainBuffer, true);
                storePostFaderPeak(mainBuffer);
                updateInputMeterState(monitoredInput, true);
                midi.clear();
                return;
            }

            if (frozenPlaybackOnly.load(std::memory_order_relaxed))
            {
                mainBuffer.clear();
                if (sendBuffer.getNumChannels() > 0)
                    sendBuffer.clear();
                updateMeterState(mainBuffer, true);
                storePostFaderPeak(mainBuffer);
                updateInputMeterState(nullptr, true);
                midi.clear();
                return;
            }

            const int requiredChannels = juce::jmax(mainBuffer.getNumChannels(),
                                                    getRequiredPluginChannelsLocked(2));
            const int requiredSamples = mainBuffer.getNumSamples();

            if (requiredChannels <= 0 || requiredSamples <= 0)
            {
                mainBuffer.clear();
                updateMeterState(mainBuffer, true);
                storePostFaderPeak(mainBuffer);
                updateInputMeterState(monitoredInput, true);
                midi.clear();
                return;
            }

            if (pluginProcessBuffer.getNumChannels() < requiredChannels
                || pluginProcessBuffer.getNumSamples() < requiredSamples)
            {
                applyLastGoodOutput(sendLevel.load(std::memory_order_relaxed));
                updateMeterState(mainBuffer, true);
                storePostFaderPeak(mainBuffer);
                updateInputMeterState(monitoredInput, true);
                midi.clear();
                return;
            }

            pluginProcessBuffer.clear();
            juce::MidiBuffer instrumentMidi;
            instrumentMidi.addEvents(midi, 0, requiredSamples, 0);
            juce::MidiBuffer insertMidi;
            insertMidi.addEvents(midi, 0, requiredSamples, 0);

            const bool monitorInputActive = inputMonitoring.load(std::memory_order_relaxed)
                                            && monitoredInput != nullptr
                                            && monitoredInput->getNumChannels() > 0
                                            && monitoredInput->getNumSamples() > 0;
            updateInputMeterState(monitorInputActive ? monitoredInput : nullptr);
            const float monitorGain = inputMonitorGain.load(std::memory_order_relaxed);
            const auto monitorTap = getMonitorTapMode();
            const auto mixSourceAudio = [&](juce::AudioBuffer<float>& destination)
            {
                if (sourceAudio == nullptr
                    || sourceAudio->getNumChannels() <= 0
                    || sourceAudio->getNumSamples() <= 0)
                {
                    return;
                }

                const int srcChannels = sourceAudio->getNumChannels();
                const int dstChannels = destination.getNumChannels();
                const int sampleCount = juce::jmin(destination.getNumSamples(), sourceAudio->getNumSamples());
                if (srcChannels <= 0 || dstChannels <= 0 || sampleCount <= 0)
                    return;

                const int directChannels = juce::jmin(srcChannels, dstChannels);
                for (int ch = 0; ch < directChannels; ++ch)
                    destination.addFrom(ch, 0, *sourceAudio, ch, 0, sampleCount);

                if (srcChannels == 1 && dstChannels > 1)
                {
                    for (int ch = 1; ch < dstChannels; ++ch)
                        destination.addFrom(ch, 0, *sourceAudio, 0, 0, sampleCount);
                }
            };
            const auto mixMonitoredInput = [&](juce::AudioBuffer<float>& destination)
            {
                if (!monitorInputActive || monitorGain <= 0.0001f)
                    return;

                const int srcChannels = monitoredInput->getNumChannels();
                const int dstChannels = destination.getNumChannels();
                const int sampleCount = juce::jmin(destination.getNumSamples(), monitoredInput->getNumSamples());
                if (srcChannels <= 0 || dstChannels <= 0 || sampleCount <= 0)
                    return;

                if (monitorSafeInput)
                {
                    constexpr float drive = 1.34f;
                    const float normalise = 1.0f / std::tanh(drive);
                    const float safeGain = monitorGain * 0.82f;
                    constexpr float dcBlockCoeff = 0.995f;

                    const int directChannels = juce::jmin(srcChannels, dstChannels);
                    for (int ch = 0; ch < directChannels; ++ch)
                    {
                        const auto* src = monitoredInput->getReadPointer(ch);
                        auto* dst = destination.getWritePointer(ch);
                        if (src == nullptr || dst == nullptr)
                            continue;

                        for (int i = 0; i < sampleCount; ++i)
                        {
                            const float dry = src[i] * safeGain;
                            float filtered = dry;
                            if (ch < 2)
                            {
                                filtered = dry - monitorDcPrevInput[static_cast<size_t>(ch)]
                                    + (dcBlockCoeff * monitorDcPrevOutput[static_cast<size_t>(ch)]);
                                monitorDcPrevInput[static_cast<size_t>(ch)] = dry;
                                monitorDcPrevOutput[static_cast<size_t>(ch)] = filtered;
                            }
                            dst[i] += std::tanh(filtered * drive) * normalise;
                        }
                    }

                    if (srcChannels == 1 && dstChannels > 1)
                    {
                        const auto* src = monitoredInput->getReadPointer(0);
                        if (src != nullptr)
                        {
                            for (int ch = 1; ch < dstChannels; ++ch)
                            {
                                auto* dst = destination.getWritePointer(ch);
                                if (dst == nullptr)
                                    continue;
                                for (int i = 0; i < sampleCount; ++i)
                                {
                                    const float dry = src[i] * safeGain;
                                    float filtered = dry;
                                    if (ch < 2)
                                    {
                                        filtered = dry - monitorDcPrevInput[static_cast<size_t>(ch)]
                                            + (dcBlockCoeff * monitorDcPrevOutput[static_cast<size_t>(ch)]);
                                        monitorDcPrevInput[static_cast<size_t>(ch)] = dry;
                                        monitorDcPrevOutput[static_cast<size_t>(ch)] = filtered;
                                    }
                                    dst[i] += std::tanh(filtered * drive) * normalise;
                                }
                            }
                        }
                    }
                    return;
                }

                const int directChannels = juce::jmin(srcChannels, dstChannels);
                for (int ch = 0; ch < directChannels; ++ch)
                    destination.addFrom(ch, 0, *monitoredInput, ch, 0, sampleCount, monitorGain);

                if (srcChannels == 1 && dstChannels > 1)
                {
                    for (int ch = 1; ch < dstChannels; ++ch)
                        destination.addFrom(ch, 0, *monitoredInput, 0, 0, sampleCount, monitorGain);
                }
            };

            // 1. Instrument stage (Instrument plugin > Sampler > Built-in synth)
            try
            {
                if (instrumentSlot.instance != nullptr && !instrumentSlot.bypassed)
                {
                    if (getUsableMainOutputChannels(*instrumentSlot.instance) > 0)
                        instrumentSlot.instance->processBlock(pluginProcessBuffer, instrumentMidi);
                    else
                        instrumentSlot.bypassed = true;
                }
                else if (builtInInstrumentMode == BuiltInInstrument::Sampler && samplerSynth.getNumSounds() > 0)
                {
                    samplerSynth.renderNextBlock(pluginProcessBuffer, midi, 0, requiredSamples);
                }
                else if (builtInInstrumentMode == BuiltInInstrument::BasicSynth)
                {
                    fallbackSynth.renderNextBlock(pluginProcessBuffer, midi, 0, requiredSamples);
                }

                // Timeline clip audio is injected before insert FX so third-party plugins process it.
                mixSourceAudio(pluginProcessBuffer);

                // Post-insert monitor mode feeds live input through insert FX + EQ.
                if (monitorTap == MonitorTapMode::PostInserts)
                    mixMonitoredInput(pluginProcessBuffer);

                // 2. Insert FX stage
                for (auto& slot : pluginSlots)
                {
                    if (slot.instance == nullptr || slot.bypassed)
                        continue;
                    if (getUsableMainOutputChannels(*slot.instance) <= 0)
                    {
                        slot.bypassed = true;
                        continue;
                    }
                    slot.instance->processBlock(pluginProcessBuffer, insertMidi);
                }

                // 2b. Built-in DSP essentials (toggleable track-local effects).
                applyBuiltInEffectsLocked(pluginProcessBuffer, requiredSamples);
            }
            catch (...)
            {
                applyLastGoodOutput(sendLevel.load(std::memory_order_relaxed));
                updateMeterState(mainBuffer, true);
                storePostFaderPeak(mainBuffer);
                updateInputMeterState(monitoredInput, true);
                midi.clear();
                return;
            }

            // Defensive sanitiser: protect the mixer from non-finite plugin output.
            for (int ch = 0; ch < pluginProcessBuffer.getNumChannels(); ++ch)
            {
                auto* write = pluginProcessBuffer.getWritePointer(ch);
                if (write == nullptr)
                    continue;
                for (int i = 0; i < requiredSamples; ++i)
                {
                    const float v = write[i];
                    if (!std::isfinite(v))
                        write[i] = 0.0f;
                    else
                        write[i] = juce::jlimit(-8.0f, 8.0f, v);
                }
            }

            const int pluginOutputChannels = pluginProcessBuffer.getNumChannels();
            const int copyChannels = juce::jmin(mainBuffer.getNumChannels(), pluginOutputChannels);
            if (copyChannels > 0 && pluginOutputChannels > copyChannels)
            {
                float primaryPeak = 0.0f;
                for (int ch = 0; ch < copyChannels; ++ch)
                    primaryPeak = juce::jmax(primaryPeak, pluginProcessBuffer.getMagnitude(ch, 0, requiredSamples));

                float extraPeak = 0.0f;
                for (int ch = copyChannels; ch < pluginOutputChannels; ++ch)
                    extraPeak = juce::jmax(extraPeak, pluginProcessBuffer.getMagnitude(ch, 0, requiredSamples));

                // Some multi-out plugins default to non-main bus channels. Fold them into main outputs when needed.
                if (primaryPeak < 1.0e-5f && extraPeak > 1.0e-5f)
                {
                    if (copyChannels == 1)
                    {
                        auto* mono = pluginProcessBuffer.getWritePointer(0);
                        if (mono != nullptr)
                        {
                            std::fill(mono, mono + requiredSamples, 0.0f);
                            for (int ch = 0; ch < pluginOutputChannels; ++ch)
                            {
                                const auto* read = pluginProcessBuffer.getReadPointer(ch);
                                if (read == nullptr)
                                    continue;
                                for (int i = 0; i < requiredSamples; ++i)
                                    mono[i] += read[i];
                            }
                            const float scale = 1.0f / static_cast<float>(juce::jmax(1, pluginOutputChannels));
                            for (int i = 0; i < requiredSamples; ++i)
                                mono[i] *= scale;
                        }
                    }
                    else
                    {
                        auto* left = pluginProcessBuffer.getWritePointer(0);
                        auto* right = pluginProcessBuffer.getWritePointer(1);
                        if (left != nullptr && right != nullptr)
                        {
                            std::fill(left, left + requiredSamples, 0.0f);
                            std::fill(right, right + requiredSamples, 0.0f);
                            for (int ch = 0; ch < pluginOutputChannels; ++ch)
                            {
                                const auto* read = pluginProcessBuffer.getReadPointer(ch);
                                if (read == nullptr)
                                    continue;
                                auto* target = (ch % 2 == 0) ? left : right;
                                for (int i = 0; i < requiredSamples; ++i)
                                    target[i] += read[i];
                            }
                            const float sideScale = 1.0f / static_cast<float>(juce::jmax(1, (pluginOutputChannels + 1) / 2));
                            for (int i = 0; i < requiredSamples; ++i)
                            {
                                left[i] *= sideScale;
                                right[i] *= sideScale;
                            }
                        }
                    }
                }
            }

            // 3. Copy plugin chain output to main buffer
            const int outputChannelsToCopy = juce::jmin(mainBuffer.getNumChannels(), pluginProcessBuffer.getNumChannels());
            for (int ch = 0; ch < outputChannelsToCopy; ++ch)
                mainBuffer.copyFrom(ch, 0, pluginProcessBuffer, ch, 0, requiredSamples);
            for (int ch = outputChannelsToCopy; ch < mainBuffer.getNumChannels(); ++ch)
                mainBuffer.clear(ch, 0, requiredSamples);

            // 4. In-DAW 3-band EQ stage
            if (eqEnabled.load(std::memory_order_relaxed))
            {
                updateEqFiltersIfNeededLocked();
                const int eqChannelCount = juce::jmin(mainBuffer.getNumChannels(), 2);
                for (int ch = 0; ch < eqChannelCount; ++ch)
                {
                    auto* write = mainBuffer.getWritePointer(ch);
                    if (write == nullptr)
                        continue;
                    eqLowFilters[static_cast<size_t>(ch)].processSamples(write, requiredSamples);
                    eqMidFilters[static_cast<size_t>(ch)].processSamples(write, requiredSamples);
                    eqHighFilters[static_cast<size_t>(ch)].processSamples(write, requiredSamples);
                }
            }

            // Pre-insert monitor mode keeps live input dry (bypasses insert chain + track EQ).
            if (monitorTap == MonitorTapMode::PreInserts)
                mixMonitoredInput(mainBuffer);

            if (startupRampSamplesRemaining > 0)
            {
                const int sampleCount = mainBuffer.getNumSamples();
                const int channelCount = mainBuffer.getNumChannels();
                const int rampStartRemaining = startupRampSamplesRemaining;
                const int rampSamplesToApply = juce::jmin(rampStartRemaining, sampleCount);

                for (int ch = 0; ch < channelCount; ++ch)
                {
                    auto* write = mainBuffer.getWritePointer(ch);
                    if (write == nullptr)
                        continue;

                    for (int i = 0; i < rampSamplesToApply; ++i)
                    {
                        const float gain = static_cast<float>(startupRampDurationSamples - rampStartRemaining + i + 1)
                                           / static_cast<float>(juce::jmax(1, startupRampDurationSamples));
                        write[i] *= juce::jlimit(0.0f, 1.0f, gain);
                    }
                }

                startupRampSamplesRemaining = juce::jmax(0, rampStartRemaining - sampleCount);
            }

            // 5. Mute
            const float currentSend = sendLevel.load(std::memory_order_relaxed);
            const auto sendTap = getSendTapMode();

            const auto copyToSendBus = [&](const juce::AudioBuffer<float>& sourceBuffer)
            {
                if (currentSend <= 0.0f || sendBuffer.getNumChannels() <= 0)
                    return;

                const int channelCount = juce::jmin(sourceBuffer.getNumChannels(), sendBuffer.getNumChannels());
                const int sampleCount = juce::jmin(sourceBuffer.getNumSamples(), sendBuffer.getNumSamples());
                for (int ch = 0; ch < channelCount; ++ch)
                {
                    auto* dst = sendBuffer.getWritePointer(ch);
                    const auto* src = sourceBuffer.getReadPointer(ch);
                    if (dst == nullptr || src == nullptr)
                        continue;

                    if (sampleCount == 1)
                    {
                        dst[0] += src[0] * currentSend;
                        continue;
                    }

                    const float gainStep = sampleCount > 1
                        ? (currentSend - prevSendGain) / static_cast<float>(sampleCount - 1)
                        : 0.0f;
                    float gain = prevSendGain;
                    for (int i = 0; i < sampleCount; ++i)
                    {
                        dst[i] += src[i] * gain;
                        gain += gainStep;
                    }
                }
            };

            if (sendTap == SendTapMode::PreFader)
                copyToSendBus(mainBuffer);

            if (mute.load())
            {
                mainBuffer.clear();
                updateMeterState(mainBuffer, true);
                storePostFaderPeak(mainBuffer);
                lastSuccessfulOutputBuffer.clear();
                return;
            }

            // 6. Volume & Pan
            const float vol = volume.load();
            const float p = juce::jlimit(-1.0f, 1.0f, pan.load());
            const float angle = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            
            const float leftGain = vol * std::cos(angle);
            const float rightGain = vol * std::sin(angle);

            // Keep a post-fader/pre-pan tap source to support a distinct post-fader mode.
            if (sendTap == SendTapMode::PostFader
                && sendTapBuffer.getNumChannels() >= mainBuffer.getNumChannels()
                && sendTapBuffer.getNumSamples() >= mainBuffer.getNumSamples())
            {
                for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
                {
                    sendTapBuffer.copyFrom(ch, 0, mainBuffer, ch, 0, mainBuffer.getNumSamples());
                    sendTapBuffer.applyGainRamp(ch, 0, mainBuffer.getNumSamples(), prevVolumeGain, vol);
                }
            }

            // 7. Apply to Main Buffer
            if (mainBuffer.getNumChannels() > 0)
                mainBuffer.applyGainRamp(0, 0, mainBuffer.getNumSamples(), prevLeftGain, leftGain);
            if (mainBuffer.getNumChannels() > 1)
                mainBuffer.applyGainRamp(1, 0, mainBuffer.getNumSamples(), prevRightGain, rightGain);
            for (int ch = 2; ch < mainBuffer.getNumChannels(); ++ch)
                mainBuffer.applyGain(ch, 0, mainBuffer.getNumSamples(), vol);

            prevLeftGain = leftGain;
            prevRightGain = rightGain;
            prevVolumeGain = vol;
            prevSendGain = currentSend;

            // 8. Post-fader send routing
            if (sendTap == SendTapMode::PostFader
                && sendTapBuffer.getNumChannels() >= mainBuffer.getNumChannels()
                && sendTapBuffer.getNumSamples() >= mainBuffer.getNumSamples())
            {
                copyToSendBus(sendTapBuffer);
            }
            else if (sendTap == SendTapMode::PostPan)
            {
                copyToSendBus(mainBuffer);
            }

            applyStartupRampLocked(mainBuffer);

            storePostFaderPeak(mainBuffer);

            // 9. Metering (post-fader/post-pan for real mixer feedback).
            updateMeterState(mainBuffer);

            if (lastSuccessfulOutputBuffer.getNumChannels() >= mainBuffer.getNumChannels()
                && lastSuccessfulOutputBuffer.getNumSamples() >= mainBuffer.getNumSamples())
            {
                const int channelCount = mainBuffer.getNumChannels();
                const int sampleCount = mainBuffer.getNumSamples();
                for (int ch = 0; ch < channelCount; ++ch)
                    lastSuccessfulOutputBuffer.copyFrom(ch, 0, mainBuffer, ch, 0, sampleCount);
                for (int ch = channelCount; ch < lastSuccessfulOutputBuffer.getNumChannels(); ++ch)
                    lastSuccessfulOutputBuffer.clear(ch, 0, sampleCount);
            }
        }

        // --- Boilerplate ---
        const juce::String getName() const override { return name; }
        bool hasEditor() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; } 
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return true; }
        double getTailLengthSeconds() const override { return 0.0; }
        int getNumPrograms() override { return 0; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}

    private:
        struct PluginSlot
        {
            std::unique_ptr<juce::AudioPluginInstance> instance;
            juce::PluginDescription description;
            bool hasDescription = false;
            bool bypassed = false;
        };

        bool getSlotLoadedLocked(int slotIndex) const
        {
            if (slotIndex == instrumentSlotIndex)
            {
                return instrumentSlot.instance != nullptr
                    || builtInInstrumentMode == BuiltInInstrument::BasicSynth
                    || (builtInInstrumentMode == BuiltInInstrument::Sampler && samplerSynth.getNumSounds() > 0);
            }

            return juce::isPositiveAndBelow(slotIndex, maxInsertSlots)
                && pluginSlots[static_cast<size_t>(slotIndex)].instance != nullptr;
        }

        juce::String getSlotNameLocked(int slotIndex) const
        {
            if (slotIndex == instrumentSlotIndex)
            {
                if (instrumentSlot.instance != nullptr)
                    return instrumentSlot.description.name;
                if (builtInInstrumentMode == BuiltInInstrument::Sampler && samplerSynth.getNumSounds() > 0)
                    return "Built-in Sampler";
                if (builtInInstrumentMode == BuiltInInstrument::BasicSynth)
                    return "Built-in Synth";
                return {};
            }

            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return {};

            const auto& slot = pluginSlots[static_cast<size_t>(slotIndex)];
            return slot.instance != nullptr ? slot.description.name : juce::String{};
        }

        bool getSlotBypassedLocked(int slotIndex) const
        {
            if (slotIndex == instrumentSlotIndex)
                return instrumentSlot.bypassed;
            if (!juce::isPositiveAndBelow(slotIndex, maxInsertSlots))
                return false;
            return pluginSlots[static_cast<size_t>(slotIndex)].bypassed;
        }

        void updatePluginUiCacheLocked() const
        {
            cachedInstrumentSlotLoaded.store(getSlotLoadedLocked(instrumentSlotIndex), std::memory_order_relaxed);
            cachedInstrumentSlotBypassed.store(getSlotBypassedLocked(instrumentSlotIndex), std::memory_order_relaxed);
            for (int slot = 0; slot < maxInsertSlots; ++slot)
            {
                cachedInsertSlotLoaded[static_cast<size_t>(slot)].store(getSlotLoadedLocked(slot), std::memory_order_relaxed);
                cachedInsertSlotBypassed[static_cast<size_t>(slot)].store(getSlotBypassedLocked(slot), std::memory_order_relaxed);
            }

            const juce::SpinLock::ScopedLockType cacheLock(pluginUiCacheLock);
            cachedInstrumentSlotName = getSlotNameLocked(instrumentSlotIndex);
            for (int slot = 0; slot < maxInsertSlots; ++slot)
                cachedInsertSlotNames[static_cast<size_t>(slot)] = getSlotNameLocked(slot);
        }

        static constexpr std::uint32_t getBuiltInEffectBit(BuiltInEffect effect) noexcept
        {
            const int index = static_cast<int>(effect);
            if (index < 0 || index >= builtInEffectCount)
                return 0u;
            return static_cast<std::uint32_t>(1u) << static_cast<std::uint32_t>(index);
        }

        static constexpr std::uint32_t builtInEffectAllMask =
            (builtInEffectCount >= 32)
                ? 0xffffffffu
                : ((static_cast<std::uint32_t>(1u) << static_cast<std::uint32_t>(builtInEffectCount)) - 1u);

        void prepareBuiltInEffectsLocked(double sampleRate, int blockSize)
        {
            const double safeSampleRate = juce::jmax(8000.0, sampleRate);
            const int safeBlockSize = juce::jlimit(64, 8192, blockSize > 0 ? blockSize : 512);
            juce::dsp::ProcessSpec spec
            {
                safeSampleRate,
                static_cast<juce::uint32>(safeBlockSize),
                static_cast<juce::uint32>(2)
            };

            builtInCompressor.reset();
            builtInCompressor.prepare(spec);
            builtInCompressor.setThreshold(-18.0f);
            builtInCompressor.setRatio(3.0f);
            builtInCompressor.setAttack(6.0f);
            builtInCompressor.setRelease(120.0f);

            builtInLimiter.reset();
            builtInLimiter.prepare(spec);
            builtInLimiter.setThreshold(-0.3f);
            builtInLimiter.setRelease(45.0f);

            builtInChorus.reset();
            builtInChorus.prepare(spec);
            builtInChorus.setRate(0.33f);
            builtInChorus.setDepth(0.34f);
            builtInChorus.setCentreDelay(8.0f);
            builtInChorus.setFeedback(0.09f);
            builtInChorus.setMix(0.24f);

            builtInFlanger.reset();
            builtInFlanger.prepare(spec);
            builtInFlanger.setRate(0.26f);
            builtInFlanger.setDepth(0.93f);
            builtInFlanger.setCentreDelay(2.3f);
            builtInFlanger.setFeedback(0.22f);
            builtInFlanger.setMix(0.34f);

            builtInPhaser.reset();
            builtInPhaser.prepare(spec);
            builtInPhaser.setRate(0.19f);
            builtInPhaser.setDepth(0.78f);
            builtInPhaser.setCentreFrequency(1100.0f);
            builtInPhaser.setFeedback(0.16f);
            builtInPhaser.setMix(0.28f);

            juce::Reverb::Parameters reverbParameters;
            reverbParameters.roomSize = 0.48f;
            reverbParameters.damping = 0.45f;
            reverbParameters.wetLevel = juce::jlimit(0.0f, 1.0f, builtInReverbMix.load(std::memory_order_relaxed));
            reverbParameters.dryLevel = 1.0f;
            reverbParameters.width = 0.82f;
            reverbParameters.freezeMode = 0.0f;
            builtInReverb.setSampleRate(safeSampleRate);
            builtInReverb.setParameters(reverbParameters);

            const int delayBufferSamples = juce::jlimit(2048,
                                                        262144,
                                                        static_cast<int>(std::round(safeSampleRate * 2.0)));
            builtInDelayBuffer.setSize(2, delayBufferSamples, false, false, true);
            builtInDelayBuffer.clear();
            builtInDelayWritePosition = 0;
            builtInDelayLastSampleRate = safeSampleRate;
            builtInDelayFeedbackLowpassState = { 0.0f, 0.0f };
            builtInDelayFeedbackDcBlockPrevInput = { 0.0f, 0.0f };
            builtInDelayFeedbackDcBlockPrevOutput = { 0.0f, 0.0f };
            builtInGateEnvelope = 0.0f;
            builtInSaturationSmoothedDrive = targetDriveForReset();
            builtInSaturationSmoothedMix = targetMixForReset();
            builtInSaturationDcPrevInput = { 0.0f, 0.0f };
            builtInSaturationDcPrevOutput = { 0.0f, 0.0f };
        }

        float targetDriveForReset() const
        {
            return juce::jlimit(1.0f, 8.0f, builtInSaturationDrive.load(std::memory_order_relaxed));
        }

        float targetMixForReset() const
        {
            return juce::jlimit(0.0f, 1.0f, builtInSaturationMix.load(std::memory_order_relaxed));
        }

        void applyBuiltInGateLocked(juce::AudioBuffer<float>& buffer, int channels, int samples)
        {
            if (samples <= 0 || channels <= 0)
                return;

            const float thresholdGain = juce::Decibels::decibelsToGain(
                builtInGateThresholdDb.load(std::memory_order_relaxed));
            const double sr = juce::jmax(8000.0, builtInDelayLastSampleRate);
            const float attackMs = juce::jlimit(0.1f, 80.0f, builtInGateAttackMs.load(std::memory_order_relaxed));
            const float releaseMs = juce::jlimit(5.0f, 400.0f, builtInGateReleaseMs.load(std::memory_order_relaxed));
            const float attackCoeff = std::exp(-1.0f / static_cast<float>(0.001 * attackMs * sr));
            const float releaseCoeff = std::exp(-1.0f / static_cast<float>(0.001 * releaseMs * sr));

            auto* left = buffer.getWritePointer(0);
            auto* right = channels > 1 ? buffer.getWritePointer(1) : nullptr;
            if (left == nullptr)
                return;

            float env = builtInGateEnvelope;
            for (int i = 0; i < samples; ++i)
            {
                const float inL = std::abs(left[i]);
                const float inR = right != nullptr ? std::abs(right[i]) : inL;
                const float detector = juce::jmax(inL, inR);
                const float target = detector >= thresholdGain ? 1.0f : 0.0f;
                const float coeff = target > env ? attackCoeff : releaseCoeff;
                env = ((1.0f - coeff) * target) + (coeff * env);
                const float gateGain = env * env;

                left[i] *= gateGain;
                if (right != nullptr)
                    right[i] *= gateGain;
            }
            builtInGateEnvelope = juce::jlimit(0.0f, 1.0f, env);
        }

        void applyBuiltInSaturationLocked(juce::AudioBuffer<float>& buffer, int channels, int samples)
        {
            if (samples <= 0 || channels <= 0)
                return;

            const float targetDrive = juce::jlimit(1.0f, 8.0f, builtInSaturationDrive.load(std::memory_order_relaxed));
            const float targetMix = juce::jlimit(0.0f, 1.0f, builtInSaturationMix.load(std::memory_order_relaxed));
            if (targetMix <= 0.0001f && builtInSaturationSmoothedMix <= 0.0001f)
                return;

            float smoothedDrive = builtInSaturationSmoothedDrive;
            float smoothedMix = builtInSaturationSmoothedMix;
            const float driveStep = (targetDrive - smoothedDrive) / static_cast<float>(samples);
            const float mixStep = (targetMix - smoothedMix) / static_cast<float>(samples);
            constexpr float dcReject = 0.995f;

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* write = buffer.getWritePointer(ch);
                if (write == nullptr)
                    continue;

                float prevInput = builtInSaturationDcPrevInput[static_cast<size_t>(ch)];
                float prevOutput = builtInSaturationDcPrevOutput[static_cast<size_t>(ch)];
                float channelDrive = smoothedDrive;
                float channelMix = smoothedMix;

                for (int i = 0; i < samples; ++i)
                {
                    channelDrive += driveStep;
                    channelMix += mixStep;

                    const float dry = write[i];
                    const float dcRemoved = dry - prevInput + (dcReject * prevOutput);
                    prevInput = dry;
                    prevOutput = dcRemoved;

                    const float safeDrive = juce::jmax(1.0e-4f, channelDrive);
                    const float normalise = 1.0f / std::tanh(safeDrive);
                    const float wet = std::tanh(dcRemoved * safeDrive) * normalise;
                    write[i] = dry + ((wet - dry) * juce::jlimit(0.0f, 1.0f, channelMix));
                }

                builtInSaturationDcPrevInput[static_cast<size_t>(ch)] = prevInput;
                builtInSaturationDcPrevOutput[static_cast<size_t>(ch)] = prevOutput;
            }

            builtInSaturationSmoothedDrive = targetDrive;
            builtInSaturationSmoothedMix = targetMix;
        }

        void applyBuiltInDelayLocked(juce::AudioBuffer<float>& buffer, int channels, int samples)
        {
            if (channels <= 0 || samples <= 0)
                return;
            if (builtInDelayBuffer.getNumChannels() < channels || builtInDelayBuffer.getNumSamples() <= 1)
                return;

            const double sr = juce::jmax(8000.0, builtInDelayLastSampleRate);
            const float delayMs = juce::jlimit(5.0f, 1800.0f, builtInDelayTimeMs.load(std::memory_order_relaxed));
            const float delaySamples = juce::jlimit(1.0f,
                                                    static_cast<float>(builtInDelayBuffer.getNumSamples() - 2),
                                                    delayMs * 0.001f * static_cast<float>(sr));
            const float feedback = juce::jlimit(0.0f, 0.95f, builtInDelayFeedback.load(std::memory_order_relaxed));
            const float mix = juce::jlimit(0.0f, 1.0f, builtInDelayMix.load(std::memory_order_relaxed));
            if (mix <= 0.0001f)
                return;

            const float dryGain = 1.0f - mix;
            const float wetGain = mix;

            const float lowpassCutoffHz = 9000.0f;
            const float lowpassAlpha = 1.0f - std::exp(-juce::MathConstants<float>::twoPi
                                                       * (lowpassCutoffHz / static_cast<float>(sr)));
            constexpr float dcBlockCoeff = 0.995f;

            int writePos = juce::jlimit(0,
                                        builtInDelayBuffer.getNumSamples() - 1,
                                        builtInDelayWritePosition);

            std::array<float*, 2> writePtrs { nullptr, nullptr };
            std::array<float*, 2> delayPtrs { nullptr, nullptr };
            for (int ch = 0; ch < channels; ++ch)
            {
                writePtrs[static_cast<size_t>(ch)] = buffer.getWritePointer(ch);
                delayPtrs[static_cast<size_t>(ch)] = builtInDelayBuffer.getWritePointer(ch);
            }

            for (int i = 0; i < samples; ++i)
            {
                float readIndex = static_cast<float>(writePos) - delaySamples;
                while (readIndex < 0.0f)
                    readIndex += static_cast<float>(builtInDelayBuffer.getNumSamples());

                const int readPosA = static_cast<int>(readIndex);
                const int readPosB = (readPosA + 1) % builtInDelayBuffer.getNumSamples();
                const float readFrac = readIndex - static_cast<float>(readPosA);

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* write = writePtrs[static_cast<size_t>(ch)];
                    auto* delayWrite = delayPtrs[static_cast<size_t>(ch)];
                    if (write == nullptr || delayWrite == nullptr)
                        continue;

                    const float dry = write[i];
                    const float delayedA = delayWrite[readPosA];
                    const float delayedB = delayWrite[readPosB];
                    const float delayed = delayedA + ((delayedB - delayedA) * readFrac);
                    write[i] = (dry * dryGain) + (delayed * wetGain);

                    const auto channelIndex = static_cast<size_t>(ch);
                    float feedbackSample = dry + (delayed * feedback);

                    auto& lowpassState = builtInDelayFeedbackLowpassState[channelIndex];
                    lowpassState += lowpassAlpha * (feedbackSample - lowpassState);
                    feedbackSample = lowpassState;

                    auto& prevIn = builtInDelayFeedbackDcBlockPrevInput[channelIndex];
                    auto& prevOut = builtInDelayFeedbackDcBlockPrevOutput[channelIndex];
                    const float dcBlocked = feedbackSample - prevIn + (dcBlockCoeff * prevOut);
                    prevIn = feedbackSample;
                    prevOut = dcBlocked;

                    delayWrite[writePos] = juce::jlimit(-1.25f, 1.25f, dcBlocked);
                }

                if (++writePos >= builtInDelayBuffer.getNumSamples())
                    writePos = 0;
            }

            builtInDelayWritePosition = writePos;
        }

        void applyBuiltInEffectsLocked(juce::AudioBuffer<float>& buffer, int samples)
        {
            const int channels = juce::jmin(2, buffer.getNumChannels());
            if (channels <= 0 || samples <= 0)
                return;

            const std::uint32_t fxMask = builtInEffectMask.load(std::memory_order_relaxed) & builtInEffectAllMask;
            if (fxMask == 0u)
                return;

            juce::dsp::AudioBlock<float> fullBlock(buffer);
            auto stereoBlock = fullBlock.getSubsetChannelBlock(0, static_cast<size_t>(channels));
            juce::dsp::ProcessContextReplacing<float> context(stereoBlock);

            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Gate)) != 0u)
                applyBuiltInGateLocked(buffer, channels, samples);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Compressor)) != 0u)
                builtInCompressor.process(context);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Saturation)) != 0u)
                applyBuiltInSaturationLocked(buffer, channels, samples);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Chorus)) != 0u)
                builtInChorus.process(context);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Flanger)) != 0u)
                builtInFlanger.process(context);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Phaser)) != 0u)
                builtInPhaser.process(context);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Delay)) != 0u)
                applyBuiltInDelayLocked(buffer, channels, samples);
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Reverb)) != 0u)
            {
                auto params = builtInReverb.getParameters();
                params.wetLevel = juce::jlimit(0.0f, 1.0f, builtInReverbMix.load(std::memory_order_relaxed));
                params.dryLevel = 1.0f;
                builtInReverb.setParameters(params);
                auto* left = buffer.getWritePointer(0);
                if (channels > 1)
                {
                    auto* right = buffer.getWritePointer(1);
                    if (left != nullptr && right != nullptr)
                        builtInReverb.processStereo(left, right, samples);
                }
                else if (left != nullptr)
                {
                    builtInReverb.processMono(left, samples);
                }
            }
            if ((fxMask & getBuiltInEffectBit(BuiltInEffect::Limiter)) != 0u)
                builtInLimiter.process(context);
        }

        int getRequiredPluginChannelsLocked(int minimumChannels) const
        {
            int requiredChannels = minimumChannels;
            if (instrumentSlot.instance)
            {
                requiredChannels = juce::jmax(requiredChannels,
                                              juce::jmax(juce::jmax(0, juce::jmin(2, instrumentSlot.instance->getMainBusNumInputChannels())),
                                                         juce::jmax(0, juce::jmin(2, instrumentSlot.instance->getMainBusNumOutputChannels()))));
            }

            for (const auto& slot : pluginSlots)
            {
                if (!slot.instance)
                    continue;

                requiredChannels = juce::jmax(requiredChannels,
                                              juce::jmax(juce::jmax(0, juce::jmin(2, slot.instance->getMainBusNumInputChannels())),
                                                         juce::jmax(0, juce::jmin(2, slot.instance->getMainBusNumOutputChannels()))));
            }
            return juce::jmax(2, requiredChannels);
        }

        void ensurePluginProcessBufferCapacityLocked(int channels, int samples)
        {
            channels = juce::jmax(2, channels);
            samples = juce::jmax(512, samples);
            pluginProcessBuffer.setSize(channels, samples, false, false, true);
            sendTapBuffer.setSize(channels, samples, false, false, true);
            lastSuccessfulOutputBuffer.setSize(channels, samples, false, false, true);
        }

        bool validatePluginInstanceSafety(juce::AudioPluginInstance& instance,
                                          bool isInstrument,
                                          int blockSize,
                                          juce::String& errorMsg) const
        {
            const int safeBlockSize = juce::jlimit(64, 2048, blockSize > 0 ? blockSize : 512);
            const int channels = juce::jmax(1,
                                            juce::jmax(juce::jmax(0, juce::jmin(2, instance.getMainBusNumInputChannels())),
                                                       juce::jmax(0, juce::jmin(2, instance.getMainBusNumOutputChannels()))));
            juce::AudioBuffer<float> testBuffer(channels, safeBlockSize);
            juce::MidiBuffer testMidi;

            for (int pass = 0; pass < 3; ++pass)
            {
                testBuffer.clear();
                testMidi.clear();
                if (isInstrument)
                {
                    if (pass == 0)
                        testMidi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)96), 0);
                    else if (pass == 1)
                        testMidi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                }

                try
                {
                    instance.processBlock(testBuffer, testMidi);
                }
                catch (const std::exception& e)
                {
                    errorMsg = "Plugin crashed during guarded startup check: " + juce::String(e.what());
                    return false;
                }
                catch (...)
                {
                    errorMsg = "Plugin crashed during guarded startup check.";
                    return false;
                }

                for (int ch = 0; ch < testBuffer.getNumChannels(); ++ch)
                {
                    const auto* read = testBuffer.getReadPointer(ch);
                    if (read == nullptr)
                        continue;

                    for (int i = 0; i < safeBlockSize; ++i)
                    {
                        const float s = read[i];
                        if (!std::isfinite(s))
                        {
                            errorMsg = "Plugin produced invalid audio data during safety validation.";
                            return false;
                        }
                    }
                }
            }

            return true;
        }

        bool configurePluginBusLayout(juce::AudioPluginInstance& instance, bool isInstrument) const
        {
            auto makeSet = [](int channels)
            {
                if (channels <= 0)
                    return juce::AudioChannelSet::disabled();
                if (channels == 1)
                    return juce::AudioChannelSet::mono();
                if (channels == 2)
                    return juce::AudioChannelSet::stereo();
                return juce::AudioChannelSet::discreteChannels(channels);
            };

            const auto tryLayout = [&](int inChannels, int outChannels)
            {
                juce::AudioProcessor::BusesLayout layout;
                if (instance.getBusCount(true) > 0)
                    layout.inputBuses.add(makeSet(inChannels));
                if (instance.getBusCount(false) > 0)
                    layout.outputBuses.add(makeSet(outChannels));
                if (layout.inputBuses.isEmpty() && layout.outputBuses.isEmpty())
                    return false;
                return instance.checkBusesLayoutSupported(layout)
                    && instance.setBusesLayout(layout);
            };

            instance.enableAllBuses();
            const auto currentLayout = instance.getBusesLayout();
            if (instance.checkBusesLayoutSupported(currentLayout)
                && instance.setBusesLayout(currentLayout))
            {
                if (instance.getMainBusNumOutputChannels() > 0)
                    return true;
            }

            if (isInstrument)
            {
                if (!tryLayout(0, 2)
                    && !tryLayout(0, 1)
                    && !tryLayout(1, 2)
                    && !tryLayout(2, 2)
                    && !tryLayout(1, 1)
                    && !tryLayout(2, 1))
                    return false;
                return instance.getMainBusNumOutputChannels() > 0;
            }

            if (!tryLayout(2, 2)
                && !tryLayout(1, 2)
                && !tryLayout(1, 1)
                && !tryLayout(2, 1)
                && !tryLayout(0, 2)
                && !tryLayout(0, 1))
                return false;

            return instance.getMainBusNumOutputChannels() > 0;
        }

        int getUsableMainOutputChannels(const juce::AudioPluginInstance& instance) const
        {
            return juce::jmax(0, juce::jmin(2, instance.getMainBusNumOutputChannels()));
        }

        void updateEqFiltersIfNeededLocked()
        {
            const float low = eqLowGainDb.load(std::memory_order_relaxed);
            const float mid = eqMidGainDb.load(std::memory_order_relaxed);
            const float high = eqHighGainDb.load(std::memory_order_relaxed);
            const bool mustRebuild = eqDirty.exchange(false, std::memory_order_relaxed)
                                     || std::abs(low - cachedEqLowGainDb) > 0.001f
                                     || std::abs(mid - cachedEqMidGainDb) > 0.001f
                                     || std::abs(high - cachedEqHighGainDb) > 0.001f;
            if (!mustRebuild)
                return;

            const double sr = preparedSampleRate > 1.0 ? preparedSampleRate
                                                       : (juce::AudioProcessor::getSampleRate() > 1.0 ? juce::AudioProcessor::getSampleRate() : 44100.0);
            const double safeSr = juce::jmax(8000.0, sr);
            const double lowGain = juce::Decibels::decibelsToGain(low);
            const double midGain = juce::Decibels::decibelsToGain(mid);
            const double highGain = juce::Decibels::decibelsToGain(high);

            for (int ch = 0; ch < 2; ++ch)
            {
                eqLowFilters[static_cast<size_t>(ch)].setCoefficients(
                    juce::IIRCoefficients::makeLowShelf(safeSr, 110.0, 0.707, static_cast<float>(lowGain)));
                eqMidFilters[static_cast<size_t>(ch)].setCoefficients(
                    juce::IIRCoefficients::makePeakFilter(safeSr, 1200.0, 0.95, static_cast<float>(midGain)));
                eqHighFilters[static_cast<size_t>(ch)].setCoefficients(
                    juce::IIRCoefficients::makeHighShelf(safeSr, 6800.0, 0.707, static_cast<float>(highGain)));
            }

            cachedEqLowGainDb = low;
            cachedEqMidGainDb = mid;
            cachedEqHighGainDb = high;
        }

        void applyStartupRampLocked(juce::AudioBuffer<float>& target)
        {
            if (startupRampSamplesRemaining <= 0)
                return;

            const int sampleCount = target.getNumSamples();
            if (sampleCount <= 0 || target.getNumChannels() <= 0)
                return;

            const int rampSamples = juce::jmin(sampleCount, startupRampSamplesRemaining);
            const float startGain = startupRampGain;
            const float progress = static_cast<float>(rampSamples)
                                 / static_cast<float>(juce::jmax(1, startupRampSamplesRemaining));
            const float endGain = startGain + ((1.0f - startGain) * progress);

            for (int ch = 0; ch < target.getNumChannels(); ++ch)
            {
                target.applyGainRamp(ch, 0, rampSamples, startGain, 1.0f);
                if (rampSamples < sampleCount)
                    target.applyGain(ch, rampSamples, sampleCount - rampSamples, 1.0f);
            }

            startupRampSamplesRemaining -= rampSamples;
            startupRampGain = (startupRampSamplesRemaining > 0) ? endGain : 1.0f;
        }

        struct ActiveNote
        {
            double startBeat = 0.0;
            int velocity = 0;
            bool active = false;
        };

        juce::String name;
        juce::AudioPluginFormatManager& fmtMgr;
        
        mutable juce::CriticalSection processLock;
        mutable juce::SpinLock pluginUiCacheLock;
        PluginSlot instrumentSlot;
        std::array<PluginSlot, static_cast<size_t>(maxInsertSlots)> pluginSlots;
        juce::AudioPlayHead* transportPlayHead = nullptr;
        BuiltInInstrument builtInInstrumentMode = BuiltInInstrument::BasicSynth;
        juce::String samplerSamplePath;
        mutable juce::String cachedInstrumentSlotName;
        mutable std::array<juce::String, static_cast<size_t>(maxInsertSlots)> cachedInsertSlotNames;
        mutable std::atomic<bool> cachedInstrumentSlotLoaded { true };
        mutable std::atomic<bool> cachedInstrumentSlotBypassed { false };
        mutable std::array<std::atomic<bool>, static_cast<size_t>(maxInsertSlots)> cachedInsertSlotLoaded;
        mutable std::array<std::atomic<bool>, static_cast<size_t>(maxInsertSlots)> cachedInsertSlotBypassed;

        std::atomic<float> volume { 0.8f };
        std::atomic<float> pan { 0.0f };
        std::atomic<float> sendLevel { 0.0f };
        std::atomic<int> sendTapMode { static_cast<int>(SendTapMode::PostFader) };
        std::atomic<int> sendTargetBus { 0 };
        std::atomic<bool> mute { false };
        std::atomic<bool> solo { false };
        std::atomic<bool> arm { false };
        std::atomic<bool> inputMonitoring { false };
        std::atomic<int> inputSourcePair { -1 };
        std::atomic<float> inputMonitorGain { 0.68f };
        std::atomic<int> monitorTapMode { static_cast<int>(MonitorTapMode::PostInserts) };
        std::atomic<int> channelType { static_cast<int>(ChannelType::Instrument) };
        std::atomic<int> outputTargetType { static_cast<int>(OutputTargetType::Master) };
        std::atomic<int> outputTargetBus { 0 };
        std::atomic<float> currentLevel { 0.0f };
        std::atomic<float> postFaderOutputPeak { 0.0f };
        std::atomic<float> meterPeakLevel { 0.0f };
        std::atomic<float> meterRmsLevel { 0.0f };
        std::atomic<int> meterClipHoldFrames { 0 };
        std::atomic<float> inputMeterPeakLevel { 0.0f };
        std::atomic<float> inputMeterRmsLevel { 0.0f };
        std::atomic<float> inputMeterHoldLevel { 0.0f };
        std::atomic<int> inputMeterClipHoldFrames { 0 };
        std::atomic<bool> eqEnabled { true };
        std::atomic<float> eqLowGainDb { 0.0f };
        std::atomic<float> eqMidGainDb { 0.0f };
        std::atomic<float> eqHighGainDb { 0.0f };
        std::atomic<bool> eqDirty { true };

        float prevLeftGain = 0.8f;
        float prevRightGain = 0.8f;
        float prevVolumeGain = 0.8f;
        float prevSendGain = 0.0f;
        int startupRampSamplesRemaining = 0;
        float startupRampGain = 0.0f;
        std::array<float, 2> monitorDcPrevInput { 0.0f, 0.0f };
        std::array<float, 2> monitorDcPrevOutput { 0.0f, 0.0f };
        double preparedSampleRate = 44100.0;
        int preparedBlockSize = 512;
        int startupRampDurationSamples = 1;
        juce::AudioBuffer<float> pluginProcessBuffer;
        juce::AudioBuffer<float> sendTapBuffer;
        juce::AudioBuffer<float> lastSuccessfulOutputBuffer;
        juce::Synthesiser fallbackSynth;
        juce::Synthesiser samplerSynth;
        std::array<juce::IIRFilter, 2> eqLowFilters;
        std::array<juce::IIRFilter, 2> eqMidFilters;
        std::array<juce::IIRFilter, 2> eqHighFilters;
        float cachedEqLowGainDb = 1000.0f;
        float cachedEqMidGainDb = 1000.0f;
        float cachedEqHighGainDb = 1000.0f;

        std::atomic<std::uint32_t> builtInEffectMask { 0u };
        std::atomic<float> builtInReverbMix { 0.24f };
        std::atomic<float> builtInDelayTimeMs { 340.0f };
        std::atomic<float> builtInDelayFeedback { 0.33f };
        std::atomic<float> builtInDelayMix { 0.22f };
        std::atomic<float> builtInSaturationDrive { 2.0f };
        std::atomic<float> builtInSaturationMix { 0.35f };
        float builtInSaturationSmoothedDrive = 2.0f;
        float builtInSaturationSmoothedMix = 0.35f;
        std::array<float, 2> builtInSaturationDcPrevInput { 0.0f, 0.0f };
        std::array<float, 2> builtInSaturationDcPrevOutput { 0.0f, 0.0f };
        std::atomic<float> builtInGateThresholdDb { -52.0f };
        std::atomic<float> builtInGateAttackMs { 4.0f };
        std::atomic<float> builtInGateReleaseMs { 75.0f };
        juce::dsp::Compressor<float> builtInCompressor;
        juce::dsp::Limiter<float> builtInLimiter;
        juce::dsp::Chorus<float> builtInChorus;
        juce::dsp::Chorus<float> builtInFlanger;
        juce::dsp::Phaser<float> builtInPhaser;
        juce::Reverb builtInReverb;
        juce::AudioBuffer<float> builtInDelayBuffer;
        int builtInDelayWritePosition = 0;
        double builtInDelayLastSampleRate = 44100.0;
        std::array<float, 2> builtInDelayFeedbackLowpassState { 0.0f, 0.0f };
        std::array<float, 2> builtInDelayFeedbackDcBlockPrevInput { 0.0f, 0.0f };
        std::array<float, 2> builtInDelayFeedbackDcBlockPrevOutput { 0.0f, 0.0f };
        float builtInGateEnvelope = 0.0f;

        std::atomic<bool> frozenPlaybackOnly { false };
        juce::String frozenRenderPath;
        std::atomic<bool> renderTaskActive { false };
        std::atomic<float> renderTaskProgress { 0.0f };
        std::atomic<int> renderTaskType { static_cast<int>(RenderTaskType::None) };

        std::atomic<bool> isRecordingActive { false };
        std::array<ActiveNote, 128> activeNotes;
        static constexpr int recordedEventCapacity = 4096;
        std::array<TimelineEvent, static_cast<size_t>(recordedEventCapacity)> recordedEvents;
        std::atomic<int> recordedReadIndex { 0 };
        std::atomic<int> recordedWriteIndex { 0 };
        std::atomic<int> droppedRecordedEvents { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Track)
    };
}
