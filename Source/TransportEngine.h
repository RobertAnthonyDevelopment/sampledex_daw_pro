#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

namespace sampledex
{
    class TransportEngine : public juce::AudioPlayHead
    {
    public:
        enum class SyncSource : int
        {
            Internal = 0,
            MidiClock = 1,
            MidiTimecode = 2
        };

        struct BlockRange
        {
            double startBeat = 0.0;
            double endBeat = 0.0;
            int64_t startSample = 0;
            int64_t endSample = 0;
            bool wrapped = false;
        };

        TransportEngine()
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.resetToDefault();
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();
        }

        void prepare(double newSampleRate)
        {
            juce::ScopedLock lock(stateLock);
            if (newSampleRate > 0.0)
                sampleRate = newSampleRate;
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();
        }

        void play()
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.isPlaying = true;
            syncRealtimeAtomicsLocked();
        }

        void stop()
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.isPlaying = false;
            positionInfo.isRecording = false;
            syncRealtimeAtomicsLocked();
        }

        bool playing() const
        {
            return isPlayingRt.load(std::memory_order_relaxed);
        }

        bool recording() const
        {
            return isRecordingRt.load(std::memory_order_relaxed);
        }

        void setRecording(bool shouldRecord)
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.isRecording = shouldRecord;
            if (shouldRecord)
                positionInfo.isPlaying = true;
            syncRealtimeAtomicsLocked();
        }

        void setTempo(double newBpm)
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.bpm = juce::jmax(1.0, newBpm);
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();
        }

        double getTempo() const
        {
            return tempoRt.load(std::memory_order_relaxed);
        }

        void setSyncSource(SyncSource source) noexcept
        {
            syncSourceRt.store(static_cast<int>(source), std::memory_order_relaxed);
        }

        SyncSource getSyncSource() const noexcept
        {
            const int raw = syncSourceRt.load(std::memory_order_relaxed);
            switch (raw)
            {
                case static_cast<int>(SyncSource::MidiClock): return SyncSource::MidiClock;
                case static_cast<int>(SyncSource::MidiTimecode): return SyncSource::MidiTimecode;
                default: return SyncSource::Internal;
            }
        }

        juce::String getSyncSourceLabel() const
        {
            switch (getSyncSource())
            {
                case SyncSource::MidiClock: return "MIDI";
                case SyncSource::MidiTimecode: return "MTC";
                case SyncSource::Internal:
                default: return "INT";
            }
        }

        void setTimeSignature(int numerator, int denominator)
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.timeSigNumerator = juce::jmax(1, numerator);
            positionInfo.timeSigDenominator = juce::jmax(1, denominator);
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();
        }

        void setPosition(double beat)
        {
            setPositionBeats(beat);
        }

        void setPositionBeats(double beat)
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.ppqPosition = juce::jmax(0.0, beat);
            updateSampleFromBeatLocked();
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();
        }

        void setPositionSamples(int64_t samplePosition)
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.timeInSamples = juce::jmax<int64_t>(0, samplePosition);
            positionInfo.ppqPosition = beatsPerSample * static_cast<double>(positionInfo.timeInSamples);
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();
        }

        BlockRange advance(int numSamples)
        {
            juce::ScopedLock lock(stateLock);

            BlockRange block;
            block.startBeat = positionInfo.ppqPosition;
            block.startSample = positionInfo.timeInSamples;

            if (!positionInfo.isPlaying || numSamples <= 0)
            {
                block.endBeat = block.startBeat;
                block.endSample = block.startSample;
                return block;
            }

            const auto deltaSamples = static_cast<int64_t>(numSamples);
            const auto deltaBeats = beatsPerSample * static_cast<double>(numSamples);

            auto nextSample = positionInfo.timeInSamples + deltaSamples;
            auto nextBeat = positionInfo.ppqPosition + deltaBeats;

            if (positionInfo.isLooping && positionInfo.ppqLoopEnd > positionInfo.ppqLoopStart)
            {
                const auto loopLength = positionInfo.ppqLoopEnd - positionInfo.ppqLoopStart;
                if (loopLength > 0.0)
                {
                    while (nextBeat >= positionInfo.ppqLoopEnd)
                    {
                        nextBeat -= loopLength;
                        block.wrapped = true;
                    }
                    while (nextBeat < positionInfo.ppqLoopStart)
                    {
                        nextBeat += loopLength;
                        block.wrapped = true;
                    }
                }
            }

            positionInfo.timeInSamples = nextSample;
            positionInfo.ppqPosition = nextBeat;
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();

            block.endBeat = positionInfo.ppqPosition;
            block.endSample = positionInfo.timeInSamples;
            return block;
        }

        BlockRange advanceWithTempo(int numSamples, double newBpm)
        {
            juce::ScopedLock lock(stateLock);

            const double clampedTempo = juce::jmax(1.0, newBpm);
            if (std::abs(positionInfo.bpm - clampedTempo) > 1.0e-9)
            {
                positionInfo.bpm = clampedTempo;
                updateDerivedFieldsLocked();
            }

            BlockRange block;
            block.startBeat = positionInfo.ppqPosition;
            block.startSample = positionInfo.timeInSamples;

            if (!positionInfo.isPlaying || numSamples <= 0)
            {
                syncRealtimeAtomicsLocked();
                block.endBeat = block.startBeat;
                block.endSample = block.startSample;
                return block;
            }

            const auto deltaSamples = static_cast<int64_t>(numSamples);
            const auto deltaBeats = beatsPerSample * static_cast<double>(numSamples);

            const auto nextSample = positionInfo.timeInSamples + deltaSamples;
            auto nextBeat = positionInfo.ppqPosition + deltaBeats;

            if (positionInfo.isLooping && positionInfo.ppqLoopEnd > positionInfo.ppqLoopStart)
            {
                const auto loopLength = positionInfo.ppqLoopEnd - positionInfo.ppqLoopStart;
                if (loopLength > 0.0)
                {
                    while (nextBeat >= positionInfo.ppqLoopEnd)
                    {
                        nextBeat -= loopLength;
                        block.wrapped = true;
                    }
                    while (nextBeat < positionInfo.ppqLoopStart)
                    {
                        nextBeat += loopLength;
                        block.wrapped = true;
                    }
                }
            }

            positionInfo.timeInSamples = nextSample;
            positionInfo.ppqPosition = nextBeat;
            updateDerivedFieldsLocked();
            syncRealtimeAtomicsLocked();

            block.endBeat = positionInfo.ppqPosition;
            block.endSample = positionInfo.timeInSamples;
            return block;
        }

        void setLoop(bool enable, double startBeat, double endBeat)
        {
            juce::ScopedLock lock(stateLock);
            positionInfo.isLooping = enable;
            positionInfo.ppqLoopStart = juce::jmax(0.0, startBeat);
            positionInfo.ppqLoopEnd = juce::jmax(positionInfo.ppqLoopStart + 0.0001, endBeat);
            syncRealtimeAtomicsLocked();
        }

        bool isLooping() const
        {
            return isLoopingRt.load(std::memory_order_relaxed);
        }

        double getLoopStartBeat() const
        {
            return loopStartBeatRt.load(std::memory_order_relaxed);
        }

        double getLoopEndBeat() const
        {
            return loopEndBeatRt.load(std::memory_order_relaxed);
        }

        double getCurrentBeat() const
        {
            return currentBeatRt.load(std::memory_order_relaxed);
        }

        int64_t getCurrentSample() const
        {
            return currentSampleRt.load(std::memory_order_relaxed);
        }

        double getBeatsPerSample() const
        {
            return beatsPerSampleRt.load(std::memory_order_relaxed);
        }

        int getLookaheadSamplesForBeats(double lookaheadBeats) const
        {
            juce::ScopedLock lock(stateLock);
            return juce::jmax(0, juce::roundToInt(lookaheadBeats * samplesPerBeat));
        }

        juce::AudioPlayHead::CurrentPositionInfo getCurrentPositionInfo() const
        {
            juce::ScopedLock lock(stateLock);
            return positionInfo;
        }

        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::ScopedLock lock(stateLock);

            juce::AudioPlayHead::PositionInfo info;
            info.setTimeInSamples(positionInfo.timeInSamples);
            info.setTimeInSeconds(positionInfo.timeInSeconds);
            info.setPpqPosition(positionInfo.ppqPosition);
            info.setPpqPositionOfLastBarStart(positionInfo.ppqPositionOfLastBarStart);
            info.setEditOriginTime(positionInfo.editOriginTime);
            info.setBpm(positionInfo.bpm);
            info.setTimeSignature(juce::AudioPlayHead::TimeSignature{
                positionInfo.timeSigNumerator,
                positionInfo.timeSigDenominator
            });
            if (positionInfo.isLooping && positionInfo.ppqLoopEnd > positionInfo.ppqLoopStart)
            {
                info.setLoopPoints(juce::AudioPlayHead::LoopPoints{
                    positionInfo.ppqLoopStart,
                    positionInfo.ppqLoopEnd
                });
            }
            info.setFrameRate(positionInfo.frameRate);
            info.setIsPlaying(positionInfo.isPlaying);
            info.setIsRecording(positionInfo.isRecording);
            info.setIsLooping(positionInfo.isLooping);
            return info;
        }

    private:
        void updateSampleFromBeatLocked()
        {
            const auto samplePos = positionInfo.ppqPosition * samplesPerBeat;
            positionInfo.timeInSamples = juce::jmax<int64_t>(0, static_cast<int64_t>(std::llround(samplePos)));
        }

        void updateDerivedFieldsLocked()
        {
            samplesPerBeat = (60.0 / positionInfo.bpm) * sampleRate;
            beatsPerSample = samplesPerBeat > 0.0 ? (1.0 / samplesPerBeat) : 0.0;
            positionInfo.timeInSeconds = static_cast<double>(positionInfo.timeInSamples) / sampleRate;

            const auto beatsPerBar = static_cast<double>(positionInfo.timeSigNumerator) * (4.0 / static_cast<double>(positionInfo.timeSigDenominator));
            if (beatsPerBar > 0.0)
                positionInfo.ppqPositionOfLastBarStart = std::floor(positionInfo.ppqPosition / beatsPerBar) * beatsPerBar;
            else
                positionInfo.ppqPositionOfLastBarStart = 0.0;
        }

        void syncRealtimeAtomicsLocked()
        {
            currentBeatRt.store(positionInfo.ppqPosition, std::memory_order_relaxed);
            currentSampleRt.store(positionInfo.timeInSamples, std::memory_order_relaxed);
            tempoRt.store(positionInfo.bpm, std::memory_order_relaxed);
            beatsPerSampleRt.store(beatsPerSample, std::memory_order_relaxed);
            isPlayingRt.store(positionInfo.isPlaying, std::memory_order_relaxed);
            isRecordingRt.store(positionInfo.isRecording, std::memory_order_relaxed);
            isLoopingRt.store(positionInfo.isLooping, std::memory_order_relaxed);
            loopStartBeatRt.store(positionInfo.ppqLoopStart, std::memory_order_relaxed);
            loopEndBeatRt.store(positionInfo.ppqLoopEnd, std::memory_order_relaxed);
        }

        mutable juce::CriticalSection stateLock;
        juce::AudioPlayHead::CurrentPositionInfo positionInfo;
        double sampleRate = 44100.0;
        double samplesPerBeat = (60.0 / 120.0) * 44100.0;
        double beatsPerSample = 1.0 / samplesPerBeat;
        std::atomic<double> currentBeatRt { 0.0 };
        std::atomic<int64_t> currentSampleRt { 0 };
        std::atomic<double> tempoRt { 120.0 };
        std::atomic<double> beatsPerSampleRt { 1.0 / ((60.0 / 120.0) * 44100.0) };
        std::atomic<bool> isPlayingRt { false };
        std::atomic<bool> isRecordingRt { false };
        std::atomic<bool> isLoopingRt { false };
        std::atomic<double> loopStartBeatRt { 0.0 };
        std::atomic<double> loopEndBeatRt { 8.0 };
        std::atomic<int> syncSourceRt { static_cast<int>(SyncSource::Internal) };
    };
}
