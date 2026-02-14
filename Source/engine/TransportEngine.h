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
            publishRtStateLocked();
        }

        void prepare(double newSampleRate)
        {
            juce::ScopedLock lock(stateLock);
            if (newSampleRate > 0.0)
                sampleRate = newSampleRate;
            updateDerivedFieldsLocked();
            publishRtStateLocked();
        }

        void play()
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            positionInfo.isPlaying = true;
            publishRtStateLocked();
        }

        void stop()
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            positionInfo.isPlaying = false;
            positionInfo.isRecording = false;
            publishRtStateLocked();
        }

        void playRt() noexcept
        {
            isPlayingRt.store(true, std::memory_order_relaxed);
        }

        void stopRt() noexcept
        {
            isPlayingRt.store(false, std::memory_order_relaxed);
            isRecordingRt.store(false, std::memory_order_relaxed);
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
            refreshFromRtLocked();
            positionInfo.isRecording = shouldRecord;
            if (shouldRecord)
                positionInfo.isPlaying = true;
            publishRtStateLocked();
        }

        void setTempo(double newBpm)
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            positionInfo.bpm = juce::jmax(1.0, newBpm);
            updateDerivedFieldsLocked();
            publishRtStateLocked();
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
            refreshFromRtLocked();
            positionInfo.timeSigNumerator = juce::jmax(1, numerator);
            positionInfo.timeSigDenominator = juce::jmax(1, denominator);
            updateDerivedFieldsLocked();
            publishRtStateLocked();
        }

        void setPosition(double beat)
        {
            setPositionBeats(beat);
        }

        void setPositionBeats(double beat)
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            positionInfo.ppqPosition = juce::jmax(0.0, beat);
            updateSampleFromBeatLocked();
            updateDerivedFieldsLocked();
            publishRtStateLocked();
        }

        void setPositionBeatsRt(double beat) noexcept
        {
            const auto targetBeat = juce::jmax(0.0, beat);
            const auto localSamplesPerBeat = samplesPerBeatRt.load(std::memory_order_relaxed);
            const auto samplePos = targetBeat * localSamplesPerBeat;
            currentBeatRt.store(targetBeat, std::memory_order_relaxed);
            currentSampleRt.store(juce::jmax<int64_t>(0, static_cast<int64_t>(std::llround(samplePos))), std::memory_order_relaxed);
        }

        void setPositionSamples(int64_t samplePosition)
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            positionInfo.timeInSamples = juce::jmax<int64_t>(0, samplePosition);
            positionInfo.ppqPosition = beatsPerSample * static_cast<double>(positionInfo.timeInSamples);
            updateDerivedFieldsLocked();
            publishRtStateLocked();
        }

        BlockRange advance(int numSamples)
        {
            BlockRange block;
            block.startBeat = currentBeatRt.load(std::memory_order_relaxed);
            block.startSample = currentSampleRt.load(std::memory_order_relaxed);

            if (!isPlayingRt.load(std::memory_order_relaxed) || numSamples <= 0)
            {
                block.endBeat = block.startBeat;
                block.endSample = block.startSample;
                return block;
            }

            const auto deltaSamples = static_cast<int64_t>(numSamples);
            const auto beatsPerSampleLocal = beatsPerSampleRt.load(std::memory_order_relaxed);
            const auto deltaBeats = beatsPerSampleLocal * static_cast<double>(numSamples);

            auto nextSample = block.startSample + deltaSamples;
            auto nextBeat = block.startBeat + deltaBeats;

            const bool looping = isLoopingRt.load(std::memory_order_relaxed);
            const double loopStart = loopStartBeatRt.load(std::memory_order_relaxed);
            const double loopEnd = loopEndBeatRt.load(std::memory_order_relaxed);
            if (looping && loopEnd > loopStart)
            {
                const auto loopLength = loopEnd - loopStart;
                if (loopLength > 0.0)
                {
                    while (nextBeat >= loopEnd)
                    {
                        nextBeat -= loopLength;
                        block.wrapped = true;
                    }
                    while (nextBeat < loopStart)
                    {
                        nextBeat += loopLength;
                        block.wrapped = true;
                    }
                }
            }

            currentSampleRt.store(nextSample, std::memory_order_relaxed);
            currentBeatRt.store(nextBeat, std::memory_order_relaxed);

            block.endBeat = nextBeat;
            block.endSample = nextSample;
            return block;
        }

        BlockRange advanceWithTempo(int numSamples, double newBpm)
        {
            const double clampedTempo = juce::jmax(1.0, newBpm);
            double beatsPerSampleLocal = beatsPerSampleRt.load(std::memory_order_relaxed);
            if (std::abs(tempoRt.load(std::memory_order_relaxed) - clampedTempo) > 1.0e-9)
            {
                const auto localSampleRate = sampleRateRt.load(std::memory_order_relaxed);
                const auto localSamplesPerBeat = (60.0 / clampedTempo) * localSampleRate;
                beatsPerSampleLocal = localSamplesPerBeat > 0.0 ? (1.0 / localSamplesPerBeat) : 0.0;
                tempoRt.store(clampedTempo, std::memory_order_relaxed);
                samplesPerBeatRt.store(localSamplesPerBeat, std::memory_order_relaxed);
                beatsPerSampleRt.store(beatsPerSampleLocal, std::memory_order_relaxed);
            }

            BlockRange block;
            block.startBeat = currentBeatRt.load(std::memory_order_relaxed);
            block.startSample = currentSampleRt.load(std::memory_order_relaxed);

            if (!isPlayingRt.load(std::memory_order_relaxed) || numSamples <= 0)
            {
                block.endBeat = block.startBeat;
                block.endSample = block.startSample;
                return block;
            }

            const auto deltaSamples = static_cast<int64_t>(numSamples);
            const auto deltaBeats = beatsPerSampleLocal * static_cast<double>(numSamples);

            const auto nextSample = block.startSample + deltaSamples;
            auto nextBeat = block.startBeat + deltaBeats;

            const bool looping = isLoopingRt.load(std::memory_order_relaxed);
            const double loopStart = loopStartBeatRt.load(std::memory_order_relaxed);
            const double loopEnd = loopEndBeatRt.load(std::memory_order_relaxed);
            if (looping && loopEnd > loopStart)
            {
                const auto loopLength = loopEnd - loopStart;
                if (loopLength > 0.0)
                {
                    while (nextBeat >= loopEnd)
                    {
                        nextBeat -= loopLength;
                        block.wrapped = true;
                    }
                    while (nextBeat < loopStart)
                    {
                        nextBeat += loopLength;
                        block.wrapped = true;
                    }
                }
            }

            currentSampleRt.store(nextSample, std::memory_order_relaxed);
            currentBeatRt.store(nextBeat, std::memory_order_relaxed);

            block.endBeat = nextBeat;
            block.endSample = nextSample;
            return block;
        }

        void setLoop(bool enable, double startBeat, double endBeat)
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            positionInfo.isLooping = enable;
            positionInfo.ppqLoopStart = juce::jmax(0.0, startBeat);
            positionInfo.ppqLoopEnd = juce::jmax(positionInfo.ppqLoopStart + 0.0001, endBeat);
            publishRtStateLocked();
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
            const auto localSamplesPerBeat = samplesPerBeatRt.load(std::memory_order_relaxed);
            return juce::jmax(0, juce::roundToInt(lookaheadBeats * localSamplesPerBeat));
        }

        juce::AudioPlayHead::CurrentPositionInfo getCurrentPositionInfo() const
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();
            return positionInfo;
        }

        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
        {
            juce::ScopedLock lock(stateLock);
            refreshFromRtLocked();

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

        void updateDerivedFieldsLocked() const
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

        void publishRtStateLocked()
        {
            currentBeatRt.store(positionInfo.ppqPosition, std::memory_order_relaxed);
            currentSampleRt.store(positionInfo.timeInSamples, std::memory_order_relaxed);
            tempoRt.store(positionInfo.bpm, std::memory_order_relaxed);
            samplesPerBeatRt.store(samplesPerBeat, std::memory_order_relaxed);
            beatsPerSampleRt.store(beatsPerSample, std::memory_order_relaxed);
            isPlayingRt.store(positionInfo.isPlaying, std::memory_order_relaxed);
            isRecordingRt.store(positionInfo.isRecording, std::memory_order_relaxed);
            isLoopingRt.store(positionInfo.isLooping, std::memory_order_relaxed);
            loopStartBeatRt.store(positionInfo.ppqLoopStart, std::memory_order_relaxed);
            loopEndBeatRt.store(positionInfo.ppqLoopEnd, std::memory_order_relaxed);
            sampleRateRt.store(sampleRate, std::memory_order_relaxed);
        }

        void refreshFromRtLocked() const
        {
            positionInfo.ppqPosition = currentBeatRt.load(std::memory_order_relaxed);
            positionInfo.timeInSamples = currentSampleRt.load(std::memory_order_relaxed);
            positionInfo.bpm = tempoRt.load(std::memory_order_relaxed);
            positionInfo.isPlaying = isPlayingRt.load(std::memory_order_relaxed);
            positionInfo.isRecording = isRecordingRt.load(std::memory_order_relaxed);
            positionInfo.isLooping = isLoopingRt.load(std::memory_order_relaxed);
            positionInfo.ppqLoopStart = loopStartBeatRt.load(std::memory_order_relaxed);
            positionInfo.ppqLoopEnd = loopEndBeatRt.load(std::memory_order_relaxed);
            sampleRate = sampleRateRt.load(std::memory_order_relaxed);
            samplesPerBeat = samplesPerBeatRt.load(std::memory_order_relaxed);
            beatsPerSample = beatsPerSampleRt.load(std::memory_order_relaxed);
            updateDerivedFieldsLocked();
        }

        mutable juce::CriticalSection stateLock;
        mutable juce::AudioPlayHead::CurrentPositionInfo positionInfo;
        mutable double sampleRate = 44100.0;
        mutable double samplesPerBeat = (60.0 / 120.0) * 44100.0;
        mutable double beatsPerSample = 1.0 / samplesPerBeat;
        std::atomic<double> currentBeatRt { 0.0 };
        std::atomic<int64_t> currentSampleRt { 0 };
        std::atomic<double> tempoRt { 120.0 };
        std::atomic<double> sampleRateRt { 44100.0 };
        std::atomic<double> samplesPerBeatRt { (60.0 / 120.0) * 44100.0 };
        std::atomic<double> beatsPerSampleRt { 1.0 / ((60.0 / 120.0) * 44100.0) };
        std::atomic<bool> isPlayingRt { false };
        std::atomic<bool> isRecordingRt { false };
        std::atomic<bool> isLoopingRt { false };
        std::atomic<double> loopStartBeatRt { 0.0 };
        std::atomic<double> loopEndBeatRt { 8.0 };
        std::atomic<int> syncSourceRt { static_cast<int>(SyncSource::Internal) };
    };
}
