#pragma once

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <atomic>

#include "RealtimeGraphScheduler.h"
#include "Track.h"

namespace sampledex
{
    struct TransportBlockContext
    {
        int numSamples = 0;
        double sampleRate = 44100.0;
        bool lowLatencyProcessing = false;
        bool offlineRenderActive = false;
    };

    struct RealtimeTrackGraphJob
    {
        Track* track = nullptr;
        juce::AudioBuffer<float>* mainBuffer = nullptr;
        const juce::AudioBuffer<float>* sourceAudio = nullptr;
        juce::AudioBuffer<float>* sendBuffer = nullptr;
        juce::MidiBuffer* midi = nullptr;
        const juce::AudioBuffer<float>* monitorInput = nullptr;
        int blockSamples = 0;
        bool processTrack = false;
        bool monitorSafeInput = false;
    };

    struct RealtimeMixInputs
    {
        static constexpr int maxViewChannels = 16;
        static constexpr int hardOutputClamp = 1;

        int activeTrackCount = 0;
        int auxBusCount = 0;
        bool builtInFailSafe = false;
        bool pdcReady = false;
        bool limiterEnabled = false;
        bool useSoftClip = false;
        float targetMasterGain = 1.0f;
        float masterGainDezipperCoeff = 0.0015f;
        float softClipDrive = 1.18f;
        float softClipNormaliser = 1.0f;
        float limiterCeiling = 0.985f;
        float limiterAttack = 0.45f;
        float limiterRelease = 0.0025f;
        float limiterRecovery = 0.015f;
        std::array<bool, 128>* trackGraphAudible = nullptr;
        std::array<bool, 128>* trackMonitorInputUsed = nullptr;
        std::array<bool, 128>* trackSendFeedbackBlocked = nullptr;
        std::array<bool, 128>* trackOutputToBus = nullptr;
        std::array<int, 128>* trackSendBusIndex = nullptr;
        std::array<int, 128>* trackOutputBusIndex = nullptr;
        std::array<int, 128>* trackMainPathLatencySamples = nullptr;
        std::array<int, 128>* trackSendPathLatencySamples = nullptr;
        std::array<bool, 128>* trackSendPathActive = nullptr;
        int maxGraphLatencySamples = 0;
        bool outputDcHighPassEnabled = true;
    };

    struct RealtimeMixOutputs
    {
        bool severeOutputFault = false;
        int outputChannels = 0;
    };

    class RealtimeAudioEngine
    {
    public:
        using PdcFn = std::function<void(int, int, int, int, juce::AudioBuffer<float>&, juce::AudioBuffer<float>&)>;

        static void runTrackGraph(RealtimeGraphScheduler& scheduler,
                                  const TransportBlockContext& context,
                                  RealtimeMixInputs& mixInputs,
                                  std::array<RealtimeTrackGraphJob, 128>& jobs,
                                  juce::AudioBuffer<float>& tempMixingBuffer,
                                  std::array<juce::AudioBuffer<float>, Track::maxSendBuses>& auxBusBuffers,
                                  const PdcFn& pdcFn);

        static void applyOutputLimiting(const TransportBlockContext& context,
                                        RealtimeMixInputs& mixInputs,
                                        juce::AudioBuffer<float>& outputBuffer,
                                        int startSample,
                                        float& masterGainSmoothingState,
                                        float& masterLimiterGainState,
                                        std::array<float, 2>& masterLimiterPrevInput,
                                        std::array<float, 2>& masterTruePeakMidpointPrevInput,
                                        std::array<float, 2>& outputDcPrevInput,
                                        std::array<float, 2>& outputDcPrevOutput,
                                        RealtimeMixOutputs& mixOutputs);
    };

    static_assert(!std::is_same_v<juce::CriticalSection, TransportBlockContext>, "RT path must not expose UI lock types");
}
