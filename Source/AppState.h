#pragma once
#include <JuceHeader.h>

#include "MidiDeviceRouter.h"
#include "ScheduledMidiOutput.h"
#include "ChordEngine.h"
#include "MidiRecorder.h"
#include "PracticeEngine.h"
#include "PluginHost.h"

namespace sampledex
{
    // Shared engine state for all tabs.
    struct AppState final
    {
        MidiDeviceRouter router;
        PluginHost pluginHost;
        MidiRecorder recorder;
        PracticeEngine practice;

        // Routing toggles
        std::atomic<bool> routeToMidiOut { true };
        std::atomic<bool> routeToPlugin  { false };

        // UI hooks (set by MainComponent)
        std::function<void(const juce::String&)> setStatus;
        std::function<void()> pulseMidiIn;
        std::function<void()> pulseMidiOut;
        std::function<void(bool)> setRecordingLed;
        std::function<void(bool)> setPluginLed;

        ScheduledMidiOutput scheduler;
        ChordEngine chordEngine;

        AppState()
            : scheduler([this] (const juce::MidiMessage& m)
                {
                    if (routeToMidiOut.load())
                        router.sendNow (m);

                    recorder.onMidiOut (m);

                    if (routeToPlugin.load())
                        pluginHost.sendMidi (m);

                    if (pulseMidiOut)
                        pulseMidiOut();
                })
        {
        }

        void panic()
        {
            chordEngine.panic (scheduler);
            if (setStatus)
                setStatus ("Panic: all notes off");
        }
    };
}
