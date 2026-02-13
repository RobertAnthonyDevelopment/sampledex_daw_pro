#pragma once
#include <JuceHeader.h>
#include <array>

namespace sampledex
{
    class ScheduledMidiOutput
    {
    public:
        ScheduledMidiOutput() = default;

        // Delay is in Milliseconds (matching ChordEngine units)
        void schedule(const juce::MidiMessage& msg, double delayMs, uint64_t tag = 0)
        {
            if (numEvents >= maxEvents)
                return;

            ScheduledEvent ev;
            ev.msg = msg;
            ev.deliveryTimeMs = currentTimeMs + delayMs;
            ev.tag = tag;
            events[static_cast<size_t>(numEvents++)] = std::move(ev);
        }

        void cancelTag(uint64_t tag)
        {
            if (tag == 0 || numEvents == 0)
                return;

            int write = 0;
            for (int i = 0; i < numEvents; ++i)
            {
                if (events[static_cast<size_t>(i)].tag == tag)
                    continue;
                events[static_cast<size_t>(write++)] = events[static_cast<size_t>(i)];
            }
            numEvents = write;
        }

        // Call this at the start of each audio block
        void process(int numSamples, double sampleRate, juce::MidiBuffer& outputBuffer)
        {
            if (numSamples <= 0 || sampleRate <= 0.0)
                return;

            const double msPerBlock = (numSamples / sampleRate) * 1000.0;
            const double endTimeMs = currentTimeMs + msPerBlock;
            int write = 0;
            for (int i = 0; i < numEvents; ++i)
            {
                const auto& ev = events[static_cast<size_t>(i)];
                if (ev.deliveryTimeMs <= endTimeMs)
                {
                    double offsetMs = ev.deliveryTimeMs - currentTimeMs;
                    int offsetSamples = juce::jlimit(0, numSamples - 1, static_cast<int>((offsetMs / 1000.0) * sampleRate));
                    outputBuffer.addEvent(ev.msg, offsetSamples);
                }
                else
                {
                    events[static_cast<size_t>(write++)] = ev;
                }
            }
            numEvents = write;
            currentTimeMs = endTimeMs;
        }

        void reset()
        {
            numEvents = 0;
            currentTimeMs = 0.0;
        }

    private:
        struct ScheduledEvent
        {
            juce::MidiMessage msg;
            double deliveryTimeMs;
            uint64_t tag;
        };

        static constexpr int maxEvents = 4096;
        std::array<ScheduledEvent, static_cast<size_t>(maxEvents)> events;
        int numEvents = 0;
        double currentTimeMs = 0.0;
    };
}
