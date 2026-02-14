#include "MidiDeviceRouter.h"

namespace sampledex
{
    MidiDeviceRouter::MidiDeviceRouter() = default;

    MidiDeviceRouter::~MidiDeviceRouter()
    {
        closeInput();
        closeOutput();
    }

    bool MidiDeviceRouter::isVirtualOutputEnabled() const
    {
        const juce::ScopedLock lock (outputStateLock);
        return virtualEnabled;
    }

    std::vector<juce::MidiDeviceInfo> MidiDeviceRouter::getInputs() const
    {
		// JUCE returns a juce::Array<MidiDeviceInfo> here in newer versions.
		// Convert to std::vector so the rest of the app can remain STL-based.
		const auto devices = juce::MidiInput::getAvailableDevices();
		std::vector<juce::MidiDeviceInfo> out;
		out.reserve(static_cast<size_t> (devices.size()));
		for (int i = 0; i < devices.size(); ++i)
			out.push_back(devices.getReference(i));
		return out;
    }

    std::vector<juce::MidiDeviceInfo> MidiDeviceRouter::getOutputs() const
    {
		const auto devices = juce::MidiOutput::getAvailableDevices();
		std::vector<juce::MidiDeviceInfo> out;
		out.reserve(static_cast<size_t> (devices.size()));
		for (int i = 0; i < devices.size(); ++i)
			out.push_back(devices.getReference(i));
		return out;
    }

    void MidiDeviceRouter::closeInput()
    {
        if (input != nullptr)
        {
            input->stop();
            input.reset();
        }
    }

    void MidiDeviceRouter::closeOutput()
    {
        const juce::ScopedLock lock (outputStateLock);
        output.reset();
        virtualOutput.reset();
        virtualEnabled = false;
    }

    void MidiDeviceRouter::setInputByIndex (int index, juce::MidiInputCallback* callback)
    {
        closeInput();

        auto inputs = getInputs();
        if (index < 0 || index >= (int) inputs.size() || callback == nullptr)
            return;

        input = juce::MidiInput::openDevice (inputs[(size_t) index].identifier, callback);
        if (input != nullptr)
            input->start();
    }

    void MidiDeviceRouter::setOutputByIndex (int index)
    {
        auto outputs = getOutputs();

        std::unique_ptr<juce::MidiOutput> newOutput;
        if (index < 0 || index >= (int) outputs.size())
        {
            const juce::ScopedLock lock (outputStateLock);
            output.reset();
            return;
        }

        newOutput = juce::MidiOutput::openDevice (outputs[(size_t) index].identifier);

        const juce::ScopedLock lock (outputStateLock);
        output = std::move (newOutput);
    }

    bool MidiDeviceRouter::setVirtualOutputEnabled (bool enabled, const juce::String& deviceName)
    {
        const juce::ScopedLock lock (outputStateLock);

        if (enabled == virtualEnabled)
            return virtualEnabled;

        // Turning off: destroy device.
        if (! enabled)
        {
            virtualOutput.reset();
            virtualEnabled = false;
            return false;
        }

        // Turning on.
        virtualOutput.reset();

       #if JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX
        virtualOutput = juce::MidiOutput::createNewDevice (deviceName.isNotEmpty() ? deviceName : "Sampledex ChordLab");
        virtualEnabled = (virtualOutput != nullptr);
        return virtualEnabled;
       #else
        juce::ignoreUnused (deviceName);
        virtualEnabled = false;
        return false;
       #endif
    }

    void MidiDeviceRouter::sendNow (const juce::MidiMessage& msg)
    {
        const juce::ScopedLock lock (outputStateLock);

        if (virtualEnabled && virtualOutput != nullptr)
        {
            virtualOutput->sendMessageNow (msg);
            return;
        }

        if (output != nullptr)
            output->sendMessageNow (msg);
    }

    juce::String MidiDeviceRouter::getActiveOutputName() const
    {
        const juce::ScopedLock lock (outputStateLock);

        if (virtualEnabled && virtualOutput != nullptr)
            return "Virtual: " + virtualOutput->getName();

        if (output != nullptr)
            return output->getName();

        return "None";
    }
} // namespace sampledex
