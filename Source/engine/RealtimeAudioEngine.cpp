#include "RealtimeAudioEngine.h"

namespace sampledex
{
    static void sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer, int numSamples)
    {
        const int channels = buffer.getNumChannels();
        const int samples = juce::jlimit(0, buffer.getNumSamples(), numSamples);
        for (int ch = 0; ch < channels; ++ch)
        {
            float* write = buffer.getWritePointer(ch);
            if (write == nullptr)
                continue;

            for (int i = 0; i < samples; ++i)
            {
                float sample = write[i];
                if (!std::isfinite(sample) || std::abs(sample) < 1.0e-30f)
                    sample = 0.0f;
                write[i] = sample;
            }
        }
    }

    static float processSoftClipOversampled2x(float sample,
                                              float drive,
                                              float normaliser,
                                              float& previousInput) noexcept
    {
        const float midpoint = 0.5f * (previousInput + sample);
        const float clippedMid = std::tanh(midpoint * drive) * normaliser;
        const float clippedCurrent = std::tanh(sample * drive) * normaliser;
        previousInput = sample;
        return 0.5f * (clippedMid + clippedCurrent);
    }

    static void runRealtimeTrackGraphJob(void* context, int index)
    {
        auto* jobs = static_cast<RealtimeTrackGraphJob*>(context);
        if (jobs == nullptr || index < 0)
            return;

        auto& job = jobs[index];
        if (!job.processTrack || job.track == nullptr || job.mainBuffer == nullptr || job.sendBuffer == nullptr || job.midi == nullptr)
            return;

        job.mainBuffer->clear();
        job.sendBuffer->clear();
        job.track->processBlockAndSends(*job.mainBuffer,
                                        *job.sendBuffer,
                                        *job.midi,
                                        job.sourceAudio,
                                        job.monitorInput,
                                        job.monitorSafeInput);
        sanitizeAudioBuffer(*job.mainBuffer, job.blockSamples);
        sanitizeAudioBuffer(*job.sendBuffer, job.blockSamples);
    }

    void RealtimeAudioEngine::runTrackGraph(RealtimeGraphScheduler& scheduler,
                                            const TransportBlockContext& context,
                                            RealtimeMixInputs& mixInputs,
                                            std::array<RealtimeTrackGraphJob, 128>& jobs,
                                            juce::AudioBuffer<float>& tempMixingBuffer,
                                            std::array<juce::AudioBuffer<float>, Track::maxSendBuses>& auxBusBuffers,
                                            const PdcFn& pdcFn)
    {
        const bool useParallelGraph = !context.offlineRenderActive
            && !context.lowLatencyProcessing
            && context.numSamples >= 256
            && scheduler.getWorkerCount() > 0
            && mixInputs.activeTrackCount >= 4;

        if (useParallelGraph)
            scheduler.run(mixInputs.activeTrackCount, jobs.data(), &runRealtimeTrackGraphJob);
        else
            for (int i = 0; i < mixInputs.activeTrackCount; ++i)
                runRealtimeTrackGraphJob(jobs.data(), i);

        for (int i = 0; i < mixInputs.activeTrackCount; ++i)
        {
            const auto& job = jobs[static_cast<size_t>(i)];
            if (!job.processTrack || !(*mixInputs.trackGraphAudible)[static_cast<size_t>(i)] || job.mainBuffer == nullptr || job.sendBuffer == nullptr)
                continue;

            auto& processedTrackAudio = *job.mainBuffer;
            auto& processedTrackSend = *job.sendBuffer;

            if (mixInputs.builtInFailSafe && (*mixInputs.trackMonitorInputUsed)[static_cast<size_t>(i)])
                processedTrackSend.clear();

            if (mixInputs.pdcReady && pdcFn != nullptr)
            {
                const int mainDelaySamples = juce::jmax(0, mixInputs.maxGraphLatencySamples - (*mixInputs.trackMainPathLatencySamples)[static_cast<size_t>(i)]);
                const int sendDelaySamples = (*mixInputs.trackSendPathActive)[static_cast<size_t>(i)]
                    ? juce::jmax(0, mixInputs.maxGraphLatencySamples - (*mixInputs.trackSendPathLatencySamples)[static_cast<size_t>(i)])
                    : 0;
                pdcFn(i, mainDelaySamples, sendDelaySamples, context.numSamples, processedTrackAudio, processedTrackSend);
            }

            if (!(*mixInputs.trackSendFeedbackBlocked)[static_cast<size_t>(i)])
            {
                auto& targetAuxBus = auxBusBuffers[static_cast<size_t>((*mixInputs.trackSendBusIndex)[static_cast<size_t>(i)])];
                const int sendChannels = juce::jmin(targetAuxBus.getNumChannels(), processedTrackSend.getNumChannels());
                for (int ch = 0; ch < sendChannels; ++ch)
                    targetAuxBus.addFrom(ch, 0, processedTrackSend, ch, 0, context.numSamples);
            }

            if ((*mixInputs.trackOutputToBus)[static_cast<size_t>(i)])
            {
                auto& outputBus = auxBusBuffers[static_cast<size_t>((*mixInputs.trackOutputBusIndex)[static_cast<size_t>(i)])];
                const int mixChannels = juce::jmin(outputBus.getNumChannels(), processedTrackAudio.getNumChannels());
                for (int ch = 0; ch < mixChannels; ++ch)
                    outputBus.addFrom(ch, 0, processedTrackAudio, ch, 0, context.numSamples);
            }
            else
            {
                const int mixChannels = juce::jmin(tempMixingBuffer.getNumChannels(), processedTrackAudio.getNumChannels());
                for (int ch = 0; ch < mixChannels; ++ch)
                    tempMixingBuffer.addFrom(ch, 0, processedTrackAudio, ch, 0, context.numSamples);
            }
        }
    }

    void RealtimeAudioEngine::applyOutputLimiting(const TransportBlockContext& context,
                                                   RealtimeMixInputs& mixInputs,
                                                   juce::AudioBuffer<float>& outputBuffer,
                                                   int startSample,
                                                   float& masterGainSmoothingState,
                                                   float& masterLimiterGainState,
                                                   std::array<float, 2>& masterLimiterPrevInput,
                                                   std::array<float, 2>& masterTruePeakMidpointPrevInput,
                                                   std::array<float, 2>& outputDcPrevInput,
                                                   std::array<float, 2>& outputDcPrevOutput,
                                                   RealtimeMixOutputs& mixOutputs)
    {
        const int outputChannels = outputBuffer.getNumChannels();
        mixOutputs.outputChannels = outputChannels;
        for (int i = 0; i < context.numSamples; ++i)
        {
            masterGainSmoothingState += (mixInputs.targetMasterGain - masterGainSmoothingState) * mixInputs.masterGainDezipperCoeff;
            float overPeak = 0.0f;
            for (int ch = 0; ch < outputChannels; ++ch)
            {
                auto* write = outputBuffer.getWritePointer(ch, startSample);
                if (write == nullptr)
                    continue;
                float sample = write[i] * masterGainSmoothingState;
                if (mixInputs.useSoftClip)
                {
                    auto& prevIn = masterTruePeakMidpointPrevInput[static_cast<size_t>(juce::jmin(ch, 1))];
                    sample = processSoftClipOversampled2x(sample, mixInputs.softClipDrive, mixInputs.softClipNormaliser, prevIn);
                }
                write[i] = sample;
                overPeak = juce::jmax(overPeak, std::abs(sample));
            }

            const float targetGain = (mixInputs.limiterEnabled && overPeak > mixInputs.limiterCeiling) ? (mixInputs.limiterCeiling / overPeak) : 1.0f;
            if (targetGain < masterLimiterGainState)
                masterLimiterGainState += (targetGain - masterLimiterGainState) * mixInputs.limiterAttack;
            else
                masterLimiterGainState += (targetGain - masterLimiterGainState) * mixInputs.limiterRelease;

            for (int ch = 0; ch < outputChannels; ++ch)
            {
                auto* write = outputBuffer.getWritePointer(ch, startSample);
                if (write == nullptr)
                    continue;
                float limited = write[i] * masterLimiterGainState;
                limited = juce::jlimit(-mixInputs.limiterCeiling, mixInputs.limiterCeiling, limited);
                write[i] = limited;
                masterLimiterPrevInput[static_cast<size_t>(juce::jmin(ch, 1))] = limited;
            }
        }

        if (mixInputs.outputDcHighPassEnabled)
        {
            constexpr float dcBlockCoeff = 0.995f;
            for (int ch = 0; ch < juce::jmin(outputChannels, 2); ++ch)
            {
                auto* write = outputBuffer.getWritePointer(ch, startSample);
                if (write == nullptr)
                    continue;

                float prevIn = outputDcPrevInput[static_cast<size_t>(ch)];
                float prevOut = outputDcPrevOutput[static_cast<size_t>(ch)];
                for (int i = 0; i < context.numSamples; ++i)
                {
                    const float in = write[i];
                    const float out = in - prevIn + (dcBlockCoeff * prevOut);
                    write[i] = out;
                    prevIn = in;
                    prevOut = out;
                }
                outputDcPrevInput[static_cast<size_t>(ch)] = prevIn;
                outputDcPrevOutput[static_cast<size_t>(ch)] = prevOut;
            }
        }

        constexpr float hardOutputClamp = 1.25f;
        constexpr float faultThreshold = 24.0f;
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            auto* write = outputBuffer.getWritePointer(ch, startSample);
            if (write == nullptr)
                continue;
            for (int i = 0; i < context.numSamples; ++i)
            {
                float sample = write[i];
                if (!std::isfinite(sample) || std::abs(sample) > faultThreshold)
                {
                    mixOutputs.severeOutputFault = true;
                    sample = 0.0f;
                }
                else
                    sample = juce::jlimit(-hardOutputClamp, hardOutputClamp, sample);
                write[i] = sample;
            }
        }
    }
}
