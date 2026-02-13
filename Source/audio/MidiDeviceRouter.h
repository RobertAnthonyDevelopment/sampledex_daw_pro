#pragma once
#include <JuceHeader.h>
#include <functional>

namespace sampledex
{
    // Thin wrapper around JUCE MIDI I/O with safe switching.
    class MidiDeviceRouter final
    {
    public:
        MidiDeviceRouter();
        ~MidiDeviceRouter();

        std::vector<juce::MidiDeviceInfo> getInputs()  const;
        std::vector<juce::MidiDeviceInfo> getOutputs() const;

        // Opens the input device at index (from getInputs()).
        // If index is out of range, input is closed.
        void setInputByIndex (int index, juce::MidiInputCallback* callback);

        // Opens the output device at index (from getOutputs()).
        // If index is out of range, output is closed (unless virtual is enabled).
        void setOutputByIndex (int index);

        // Creates a virtual MIDI output device (best workflow for routing into a DAW).
        // On platforms that don't support it, this becomes a no-op and returns false.
        bool setVirtualOutputEnabled (bool enabled, const juce::String& deviceName);

        bool isVirtualOutputEnabled() const { return virtualEnabled; }

        // Sends immediately on the currently active output (virtual takes precedence if enabled).
        void sendNow (const juce::MidiMessage& msg);

        juce::String getActiveOutputName() const;

    private:
        void closeInput();
        void closeOutput();

        bool virtualEnabled = false;

        std::unique_ptr<juce::MidiInput>  input;
        std::unique_ptr<juce::MidiOutput> output;
        std::unique_ptr<juce::MidiOutput> virtualOutput;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDeviceRouter)
    };
} // namespace sampledex
