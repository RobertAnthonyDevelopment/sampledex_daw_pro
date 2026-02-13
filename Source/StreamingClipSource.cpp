#include "StreamingClipSource.h"

namespace sampledex
{
    StreamingClipSource::StreamingClipSource(const juce::File& sourceFile,
                                             juce::AudioFormatManager& formatManager,
                                             juce::TimeSliceThread& readThread,
                                             int readAheadSamples)
        : file(sourceFile)
    {
        if (!file.existsAsFile())
            return;

        auto sourceReader = std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(file));
        if (sourceReader == nullptr
            || sourceReader->numChannels <= 0
            || sourceReader->lengthInSamples <= 0)
            return;

        numChannels = static_cast<int>(sourceReader->numChannels);
        numSamples = static_cast<int64>(sourceReader->lengthInSamples);
        sampleRate = juce::jmax(1.0, sourceReader->sampleRate);
        bufferingReader = std::make_unique<juce::BufferingAudioReader>(sourceReader.get(),
                                                                        readThread,
                                                                        juce::jmax(8192, readAheadSamples));
        if (bufferingReader == nullptr)
            return;

        // BufferingAudioReader takes ownership of the source reader.
        sourceReader.release();
        ready = true;
    }

    StreamingClipSource::~StreamingClipSource()
    {
        shuttingDown.store(true, std::memory_order_release);
        const juce::ScopedLock sl(readLock);
        ready = false;
        bufferingReader.reset();
    }

    bool StreamingClipSource::readSamples(juce::AudioBuffer<float>& destination,
                                          int64 sourceStartSample,
                                          int numSamplesToRead) const
    {
        if (numSamplesToRead <= 0
            || destination.getNumChannels() < numChannels
            || destination.getNumSamples() < numSamplesToRead
            || shuttingDown.load(std::memory_order_acquire))
        {
            destination.clear();
            return false;
        }

        const int64 start = juce::jlimit<int64>(0, juce::jmax<int64>(0, numSamples - 1), sourceStartSample);
        const int64 end = juce::jlimit<int64>(0, numSamples, start + static_cast<int64>(numSamplesToRead));
        const int samplesToRead = static_cast<int>(end - start);
        if (samplesToRead <= 0)
        {
            destination.clear();
            return false;
        }

        destination.clear();

        const juce::ScopedLock sl(readLock);
        if (!ready
            || bufferingReader == nullptr
            || shuttingDown.load(std::memory_order_relaxed))
        {
            destination.clear();
            return false;
        }
        if (!bufferingReader->read(&destination, 0, samplesToRead, start, true, true))
            return false;

        if (samplesToRead < numSamplesToRead)
            destination.clear(0, samplesToRead, numSamplesToRead - samplesToRead);

        return true;
    }
}
