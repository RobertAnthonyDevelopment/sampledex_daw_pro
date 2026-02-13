#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace sampledex
{
    class StreamingClipSource
    {
    public:
        StreamingClipSource(const juce::File& sourceFile,
                            juce::AudioFormatManager& formatManager,
                            juce::TimeSliceThread& readThread,
                            int readAheadSamples = 32768);
        ~StreamingClipSource();

        bool isReady() const noexcept { return ready; }
        int getNumChannels() const noexcept { return numChannels; }
        int64 getNumSamples() const noexcept { return numSamples; }
        double getSampleRate() const noexcept { return sampleRate; }
        const juce::File& getFile() const noexcept { return file; }

        // Reads a contiguous window from the source file into destination.
        // Destination must already be sized for at least numChannels x numSamples.
        bool readSamples(juce::AudioBuffer<float>& destination,
                         int64 sourceStartSample,
                         int numSamplesToRead) const;

    private:
        juce::File file;
        std::unique_ptr<juce::BufferingAudioReader> bufferingReader;
        int numChannels = 0;
        int64 numSamples = 0;
        double sampleRate = 44100.0;
        bool ready = false;
        std::atomic<bool> shuttingDown { false };
        mutable juce::CriticalSection readLock;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingClipSource)
    };
}
