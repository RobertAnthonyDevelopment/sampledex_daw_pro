#include "MainComponent.h"
#include "LcdDisplay.h"         
#include "NormalizeDialog.h"
#include "ProjectSerializer.h"  
#include "PianoRollComponent.h" 
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <set>
#include <thread>
#include "Theme.h"

namespace
{
    constexpr int menuIdOpenCurrentPluginEditor = 1;
    constexpr int menuIdClosePluginEditor = 2;
    constexpr int menuIdScanPlugins = 3;
    constexpr int menuIdToggleBypassCurrentInsert = 4;
    constexpr int menuIdClearCurrentInsert = 5;
    constexpr int menuIdOpenLoadedInstrument = 39;
    constexpr int menuIdOpenLoadedInsertBase = 40;
    constexpr int menuIdSwitchTargetInstrument = 79;
    constexpr int menuIdSwitchTargetInsertBase = 80;
    constexpr int menuIdUseBuiltInSynth = 900;
    constexpr int menuIdUseBuiltInSampler = 901;
    constexpr int menuIdToggleBuiltInDspBase = 930;
    constexpr int menuIdDisableBuiltInDsp = 979;
    constexpr int menuIdPluginBase = 1000;
    constexpr int menuIdUnquarantinePluginBase = 16000;
    constexpr int menuIdFileOpenProject = 20000;
    constexpr int menuIdFileSaveProject = 20001;
    constexpr int menuIdFileSaveProjectAs = 20009;
    constexpr int menuIdFileExportMix = 20002;
    constexpr int menuIdFileExportStems = 20003;
    constexpr int menuIdFileFreezeTrack = 20004;
    constexpr int menuIdFileUnfreezeTrack = 20005;
    constexpr int menuIdFileCommitTrack = 20006;
    constexpr int menuIdFileCancelRenderTask = 20007;
    constexpr int menuIdFileQuit = 20008;
    constexpr int menuIdViewAudioSettings = 20100;
    constexpr int menuIdViewHelp = 20101;
    constexpr int menuIdViewScanPlugins = 20102;
    constexpr int menuIdViewProjectSettings = 20103;
    constexpr int menuIdViewAutoScanPlugins = 20104;

    const juce::StringArray keyNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const juce::StringArray scaleNames { "Major", "Minor", "Dorian", "Mixolydian", "Pentatonic" };

    struct AcidLoopMetadata
    {
        bool valid = false;
        bool oneShot = false;
        double tempoBpm = 0.0;
        double beatCount = 0.0;
    };

    static std::uint32_t readLe32(const std::uint8_t* data) noexcept
    {
        return static_cast<std::uint32_t>(data[0])
             | (static_cast<std::uint32_t>(data[1]) << 8u)
             | (static_cast<std::uint32_t>(data[2]) << 16u)
             | (static_cast<std::uint32_t>(data[3]) << 24u);
    }

    static float readLeFloat(const std::uint8_t* data) noexcept
    {
        std::uint32_t raw = readLe32(data);
        float out = 0.0f;
        std::memcpy(&out, &raw, sizeof(float));
        return out;
    }

    static bool shouldQuarantinePluginLoadError(const juce::String& errorText)
    {
        const auto lower = errorText.toLowerCase();
        return lower.contains("guarded startup")
            || lower.contains("safety validation")
            || lower.contains("invalid audio data")
            || lower.contains("unstable output")
            || lower.contains("plugin crashed");
    }

    static bool formatNameLooksLikeAudioUnit(const juce::String& formatName)
    {
        return formatName.containsIgnoreCase("AudioUnit")
            || formatName.containsIgnoreCase("AU");
    }

    static bool formatNameLooksLikeVST3(const juce::String& formatName)
    {
        return formatName.containsIgnoreCase("VST3");
    }

    static constexpr int clipResamplerPhases = 128;
    static constexpr int clipResamplerTaps = 32;

    using PolyphaseCoefficients = std::array<std::array<float, clipResamplerTaps>, clipResamplerPhases>;

    static const PolyphaseCoefficients& getClipResamplerCoefficients()
    {
        static const PolyphaseCoefficients coeffs = []
        {
            PolyphaseCoefficients table {};
            constexpr int halfTaps = clipResamplerTaps / 2;
            constexpr double cutoff = 0.965;
            constexpr double pi = juce::MathConstants<double>::pi;

            for (int phase = 0; phase < clipResamplerPhases; ++phase)
            {
                const double frac = static_cast<double>(phase) / static_cast<double>(clipResamplerPhases);
                double norm = 0.0;
                for (int tap = 0; tap < clipResamplerTaps; ++tap)
                {
                    const double x = static_cast<double>(tap - halfTaps + 1) - frac;
                    const double sincArg = x * cutoff;
                    const double sinc = std::abs(sincArg) < 1.0e-8
                        ? 1.0
                        : std::sin(pi * sincArg) / (pi * sincArg);
                    const double window = 0.54 - (0.46 * std::cos((2.0 * pi * static_cast<double>(tap))
                                                                   / static_cast<double>(clipResamplerTaps - 1)));
                    const double v = cutoff * sinc * window;
                    table[static_cast<size_t>(phase)][static_cast<size_t>(tap)] = static_cast<float>(v);
                    norm += v;
                }
                const float normInv = static_cast<float>(1.0 / juce::jmax(1.0e-12, norm));
                for (int tap = 0; tap < clipResamplerTaps; ++tap)
                    table[static_cast<size_t>(phase)][static_cast<size_t>(tap)] *= normInv;
            }
            return table;
        }();

        return coeffs;
    }

    static inline float sampleBandlimited(const float* src, int srcLength, double sourcePosition) noexcept
    {
        if (src == nullptr || srcLength <= 0)
            return 0.0f;

        const auto& coeffs = getClipResamplerCoefficients();
        constexpr int halfTaps = clipResamplerTaps / 2;
        const int baseIndex = static_cast<int>(std::floor(sourcePosition));
        const double frac = sourcePosition - static_cast<double>(baseIndex);
        const double phasePosition = frac * static_cast<double>(clipResamplerPhases - 1);
        const int phaseA = juce::jlimit(0,
                                        clipResamplerPhases - 1,
                                        static_cast<int>(std::floor(phasePosition)));
        const int phaseB = juce::jlimit(0, clipResamplerPhases - 1, phaseA + 1);
        const float phaseMix = static_cast<float>(phasePosition - static_cast<double>(phaseA));
        const int start = baseIndex - halfTaps + 1;
        float out = 0.0f;
        for (int tap = 0; tap < clipResamplerTaps; ++tap)
        {
            const int srcIndex = juce::jlimit(0, srcLength - 1, start + tap);
            const float coeffA = coeffs[static_cast<size_t>(phaseA)][static_cast<size_t>(tap)];
            const float coeffB = coeffs[static_cast<size_t>(phaseB)][static_cast<size_t>(tap)];
            const float coeff = coeffA + ((coeffB - coeffA) * phaseMix);
            out += src[srcIndex] * coeff;
        }
        return out;
    }

    static inline float applyMicroFadeWindow(double beatInClip,
                                             double clipLengthBeats,
                                             double beatsPerSample,
                                             float inSample) noexcept
    {
        auto smoothFade = [] (double x) noexcept -> float
        {
            const double clamped = juce::jlimit(0.0, 1.0, x);
            return static_cast<float>(0.5 - (0.5 * std::cos(clamped * juce::MathConstants<double>::pi)));
        };

        constexpr int microFadeSamples = 48;
        const double microFadeBeats = static_cast<double>(microFadeSamples) * beatsPerSample;
        if (microFadeBeats <= 0.0)
            return inSample;

        float fade = 1.0f;
        if (beatInClip < microFadeBeats)
            fade = juce::jmin(fade, smoothFade(beatInClip / microFadeBeats));

        const double beatsToEnd = juce::jmax(0.0, clipLengthBeats - beatInClip);
        if (beatsToEnd < microFadeBeats)
            fade = juce::jmin(fade, smoothFade(beatsToEnd / microFadeBeats));

        return inSample * fade;
    }

    static inline float computeClipFadeGain(double beatInClip,
                                            double clipLengthBeats,
                                            double fadeInBeats,
                                            double fadeOutBeats) noexcept
    {
        auto equalPowerIn = [] (double x) noexcept -> float
        {
            const double clamped = juce::jlimit(0.0, 1.0, x);
            return static_cast<float>(std::sin(0.5 * juce::MathConstants<double>::pi * clamped));
        };

        float fadeGain = 1.0f;
        if (fadeInBeats > 0.0)
            fadeGain = juce::jmin(fadeGain, equalPowerIn(beatInClip / fadeInBeats));

        if (fadeOutBeats > 0.0)
        {
            const double beatsToEnd = juce::jmax(0.0, clipLengthBeats - beatInClip);
            fadeGain = juce::jmin(fadeGain, equalPowerIn(beatsToEnd / fadeOutBeats));
        }

        return fadeGain;
    }

    static inline float processSoftClipOversampled2x(float sample,
                                                     float drive,
                                                     float normaliser,
                                                     float& prevInput) noexcept
    {
        const float midpoint = 0.5f * (prevInput + sample);
        const float clippedMid = std::tanh(midpoint * drive) / normaliser;
        const float clippedNow = std::tanh(sample * drive) / normaliser;
        prevInput = sample;
        return 0.5f * (clippedMid + clippedNow);
    }

    static inline std::uint32_t nextDitherState(std::uint32_t& state) noexcept
    {
        state = (state * 1664525u) + 1013904223u;
        return state;
    }

    static void applyTpdfDither(juce::AudioBuffer<float>& buffer,
                                int startSample,
                                int numSamples,
                                int bitDepth,
                                std::uint32_t& rngState)
    {
        if (bitDepth <= 0 || bitDepth >= 24)
            return;

        const float scale = static_cast<float>(1u << juce::jmax(1, bitDepth - 1));
        const float invScale = 1.0f / scale;
        const float lsb = invScale;
        constexpr float invUint = 1.0f / 4294967295.0f;
        const int channels = buffer.getNumChannels();

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* write = buffer.getWritePointer(ch, startSample);
            if (write == nullptr)
                continue;
            for (int i = 0; i < numSamples; ++i)
            {
                const float r1 = static_cast<float>(nextDitherState(rngState)) * invUint;
                const float r2 = static_cast<float>(nextDitherState(rngState)) * invUint;
                const float dither = (r1 - r2) * lsb;
                const float withDither = juce::jlimit(-1.0f, 1.0f, write[i] + dither);
                write[i] = std::round(withDither * scale) * invScale;
            }
        }
    }

    static juce::String scanFormatDisplayName(const juce::String& formatName)
    {
        if (formatNameLooksLikeAudioUnit(formatName))
            return "AU";
        if (formatNameLooksLikeVST3(formatName))
            return "VST3";
        return formatName;
    }

    static float analyseBufferPeak(const juce::AudioBuffer<float>& buffer, bool removeDc)
    {
        if (buffer.getNumChannels() <= 0 || buffer.getNumSamples() <= 0)
            return 0.0f;

        if (!removeDc)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
            return peak;
        }

        std::vector<double> dcOffset(static_cast<size_t>(buffer.getNumChannels()), 0.0);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* read = buffer.getReadPointer(ch);
            if (read == nullptr)
                continue;
            double sum = 0.0;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                sum += static_cast<double>(read[i]);
            dcOffset[static_cast<size_t>(ch)] = sum / static_cast<double>(buffer.getNumSamples());
        }

        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* read = buffer.getReadPointer(ch);
            if (read == nullptr)
                continue;
            const float dc = static_cast<float>(dcOffset[static_cast<size_t>(ch)]);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                peak = juce::jmax(peak, std::abs(read[i] - dc));
        }
        return peak;
    }

    static float analyseClipPeak(const sampledex::Clip& clip,
                                 juce::AudioFormatManager& audioFormatManager,
                                 bool removeDc)
    {
        if (clip.type != sampledex::ClipType::Audio)
            return 0.0f;

        if (clip.audioData != nullptr)
            return analyseBufferPeak(*clip.audioData, removeDc);

        if (clip.audioFilePath.isEmpty())
            return 0.0f;

        const juce::File sourceFile(clip.audioFilePath);
        if (!sourceFile.existsAsFile())
            return 0.0f;

        auto reader = std::unique_ptr<juce::AudioFormatReader>(audioFormatManager.createReaderFor(sourceFile));
        if (reader == nullptr)
            return 0.0f;

        const int channelCount = static_cast<int>(juce::jmax<int64>(1, reader->numChannels));
        juce::AudioBuffer<float> scanBuffer(channelCount, 32768);

        if (!removeDc)
        {
            float peak = 0.0f;
            int64 position = 0;
            while (position < reader->lengthInSamples)
            {
                const int toRead = static_cast<int>(juce::jmin<int64>(scanBuffer.getNumSamples(),
                                                                      reader->lengthInSamples - position));
                if (!reader->read(&scanBuffer, 0, toRead, position, true, true))
                    break;
                for (int ch = 0; ch < scanBuffer.getNumChannels(); ++ch)
                    peak = juce::jmax(peak, scanBuffer.getMagnitude(ch, 0, toRead));
                position += toRead;
            }
            return peak;
        }

        std::vector<double> sums(static_cast<size_t>(channelCount), 0.0);
        int64 totalSamples = 0;
        int64 position = 0;
        while (position < reader->lengthInSamples)
        {
            const int toRead = static_cast<int>(juce::jmin<int64>(scanBuffer.getNumSamples(),
                                                                  reader->lengthInSamples - position));
            if (!reader->read(&scanBuffer, 0, toRead, position, true, true))
                break;
            for (int ch = 0; ch < channelCount; ++ch)
            {
                const auto* read = scanBuffer.getReadPointer(ch);
                if (read == nullptr)
                    continue;
                double sum = 0.0;
                for (int i = 0; i < toRead; ++i)
                    sum += static_cast<double>(read[i]);
                sums[static_cast<size_t>(ch)] += sum;
            }
            totalSamples += static_cast<int64>(toRead);
            position += toRead;
        }

        if (totalSamples <= 0)
            return 0.0f;

        std::vector<float> dcOffset(static_cast<size_t>(channelCount), 0.0f);
        for (int ch = 0; ch < channelCount; ++ch)
            dcOffset[static_cast<size_t>(ch)] = static_cast<float>(sums[static_cast<size_t>(ch)] / static_cast<double>(totalSamples));

        float peak = 0.0f;
        position = 0;
        while (position < reader->lengthInSamples)
        {
            const int toRead = static_cast<int>(juce::jmin<int64>(scanBuffer.getNumSamples(),
                                                                  reader->lengthInSamples - position));
            if (!reader->read(&scanBuffer, 0, toRead, position, true, true))
                break;
            for (int ch = 0; ch < channelCount; ++ch)
            {
                const auto* read = scanBuffer.getReadPointer(ch);
                if (read == nullptr)
                    continue;
                const float dc = dcOffset[static_cast<size_t>(ch)];
                for (int i = 0; i < toRead; ++i)
                    peak = juce::jmax(peak, std::abs(read[i] - dc));
            }
            position += toRead;
        }

        return peak;
    }

    static bool parseAcidMetadataFromWav(const juce::File& file, AcidLoopMetadata& outMeta)
    {
        outMeta = {};
        if (!file.existsAsFile() || !file.hasFileExtension("wav"))
            return false;

        juce::FileInputStream stream(file);
        if (!stream.openedOk())
            return false;

        if (stream.getTotalLength() < 12)
            return false;

        std::array<std::uint8_t, 12> riffHeader {};
        if (stream.read(riffHeader.data(), static_cast<int>(riffHeader.size())) != static_cast<int>(riffHeader.size()))
            return false;

        const bool isRiff = std::memcmp(riffHeader.data(), "RIFF", 4) == 0;
        const bool isWave = std::memcmp(riffHeader.data() + 8, "WAVE", 4) == 0;
        if (!isRiff || !isWave)
            return false;

        while (stream.getPosition() + 8 <= stream.getTotalLength())
        {
            std::array<std::uint8_t, 8> chunkHeader {};
            if (stream.read(chunkHeader.data(), static_cast<int>(chunkHeader.size())) != static_cast<int>(chunkHeader.size()))
                break;

            const std::uint32_t chunkSize = readLe32(chunkHeader.data() + 4);
            const juce::int64 chunkDataStart = stream.getPosition();
            const juce::int64 alignedChunkSize = static_cast<juce::int64>(chunkSize) + (chunkSize & 1u);
            if (chunkDataStart + alignedChunkSize > stream.getTotalLength())
                break;

            const bool isAcidChunk = std::memcmp(chunkHeader.data(), "acid", 4) == 0;
            if (!isAcidChunk)
            {
                stream.setPosition(chunkDataStart + alignedChunkSize);
                continue;
            }

            if (chunkSize < 20)
                return false;

            std::vector<std::uint8_t> payload(static_cast<size_t>(chunkSize), 0);
            if (stream.read(payload.data(), static_cast<int>(chunkSize)) != static_cast<int>(chunkSize))
                return false;

            const std::uint32_t flags = readLe32(payload.data());
            const std::uint32_t beatCount = readLe32(payload.data() + 8);
            float tempoBpm = readLeFloat(payload.data() + 16);
            if (!std::isfinite(tempoBpm))
                tempoBpm = 0.0f;

            outMeta.oneShot = (flags & 0x01u) != 0;
            outMeta.tempoBpm = juce::jlimit(0.0, 400.0, static_cast<double>(tempoBpm));
            outMeta.beatCount = juce::jmax(0.0, static_cast<double>(beatCount));
            outMeta.valid = outMeta.beatCount > 0.0 || outMeta.tempoBpm > 1.0;
            return outMeta.valid;
        }

        return false;
    }

    static void sanitizeAudioBuffer(juce::AudioBuffer<float>& buffer, int numSamples)
    {
        const int samples = juce::jmin(numSamples, buffer.getNumSamples());
        if (samples <= 0)
            return;

        constexpr float clampAbs = 6.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* write = buffer.getWritePointer(ch);
            if (write == nullptr)
                continue;
            for (int i = 0; i < samples; ++i)
            {
                float s = write[i];
                if (!std::isfinite(s))
                    s = 0.0f;
                write[i] = juce::jlimit(-clampAbs, clampAbs, s);
            }
        }
    }

    class AudioCallbackPerfScope final
    {
    public:
        AudioCallbackPerfScope(std::atomic<double>& sampleRateSource,
                               int blockSamplesIn,
                               std::atomic<float>& callbackLoadTarget,
                               std::atomic<int>& overloadTarget,
                               std::atomic<int>& xrunTarget,
                               std::atomic<int>& recoveryTarget) noexcept
            : sampleRateRt(sampleRateSource),
              blockSamples(juce::jmax(1, blockSamplesIn)),
              callbackLoadRt(callbackLoadTarget),
              overloadCountRt(overloadTarget),
              xrunCountRt(xrunTarget),
              recoveryBlocksRt(recoveryTarget),
              startTicks(juce::Time::getHighResolutionTicks())
        {
        }

        ~AudioCallbackPerfScope() noexcept
        {
            const auto elapsedTicks = juce::Time::getHighResolutionTicks() - startTicks;
            const double tickRate = static_cast<double>(juce::Time::getHighResolutionTicksPerSecond());
            if (elapsedTicks <= 0 || tickRate <= 0.0)
                return;

            const double elapsedSeconds = static_cast<double>(elapsedTicks) / tickRate;
            const double sampleRate = juce::jmax(1.0, sampleRateRt.load(std::memory_order_relaxed));
            const double blockSeconds = static_cast<double>(blockSamples) / sampleRate;
            if (blockSeconds <= 0.0)
                return;

            const float loadRatio = static_cast<float>(elapsedSeconds / blockSeconds);
            if (!std::isfinite(loadRatio))
                return;

            const float previousLoad = callbackLoadRt.load(std::memory_order_relaxed);
            callbackLoadRt.store(previousLoad + ((loadRatio - previousLoad) * 0.18f),
                                 std::memory_order_relaxed);

            int recoveryBlocks = recoveryBlocksRt.load(std::memory_order_relaxed);
            if (loadRatio >= 1.05f)
            {
                overloadCountRt.fetch_add(1, std::memory_order_relaxed);
                xrunCountRt.fetch_add(1, std::memory_order_relaxed);
                recoveryBlocksRt.store(juce::jmax(recoveryBlocks, 72), std::memory_order_relaxed);
            }
            else if (loadRatio >= 0.90f)
            {
                overloadCountRt.fetch_add(1, std::memory_order_relaxed);
                recoveryBlocksRt.store(juce::jmax(recoveryBlocks, 18), std::memory_order_relaxed);
            }
            else if (recoveryBlocks > 0)
            {
                recoveryBlocksRt.store(recoveryBlocks - 1, std::memory_order_relaxed);
            }
        }

    private:
        std::atomic<double>& sampleRateRt;
        int blockSamples = 0;
        std::atomic<float>& callbackLoadRt;
        std::atomic<int>& overloadCountRt;
        std::atomic<int>& xrunCountRt;
        std::atomic<int>& recoveryBlocksRt;
        int64_t startTicks = 0;
    };

    static bool beatFallsInRange(double beat,
                                 double startBeat,
                                 double endBeat,
                                 bool wrapped,
                                 double loopStartBeat,
                                 double loopEndBeat) noexcept
    {
        constexpr double eps = 1.0e-6;
        if (!wrapped)
            return beat + eps >= startBeat && beat < endBeat + eps;

        const bool inUpperSegment = beat + eps >= startBeat && beat < loopEndBeat + eps;
        const bool inLowerSegment = beat + eps >= loopStartBeat && beat < endBeat + eps;
        return inUpperSegment || inLowerSegment;
    }

    struct RealtimeTrackGraphJob
    {
        sampledex::Track* track = nullptr;
        juce::AudioBuffer<float>* mainBuffer = nullptr;
        const juce::AudioBuffer<float>* sourceAudio = nullptr;
        juce::AudioBuffer<float>* sendBuffer = nullptr;
        juce::MidiBuffer* midi = nullptr;
        const juce::AudioBuffer<float>* monitorInput = nullptr;
        int blockSamples = 0;
        bool processTrack = false;
        bool monitorSafeInput = false;
    };

    static void runRealtimeTrackGraphJob(void* context, int index)
    {
        if (context == nullptr || index < 0)
            return;

        auto* jobs = static_cast<RealtimeTrackGraphJob*>(context);
        auto& job = jobs[index];
        if (!job.processTrack
            || job.track == nullptr
            || job.mainBuffer == nullptr
            || job.sendBuffer == nullptr
            || job.midi == nullptr)
        {
            return;
        }

        const int blockSamples = juce::jmax(0, job.blockSamples);
        if (blockSamples <= 0)
        {
            job.mainBuffer->clear();
            job.sendBuffer->clear();
            return;
        }

        job.mainBuffer->clear();
        job.sendBuffer->clear();

        constexpr int maxViewChannels = 16;
        const int mainChannels = juce::jmin(job.mainBuffer->getNumChannels(), maxViewChannels);
        const int sendChannels = juce::jmin(job.sendBuffer->getNumChannels(), maxViewChannels);
        if (mainChannels <= 0 || sendChannels <= 0)
            return;

        std::array<float*, static_cast<size_t>(maxViewChannels)> mainPtrs {};
        std::array<float*, static_cast<size_t>(maxViewChannels)> sendPtrs {};
        for (int ch = 0; ch < mainChannels; ++ch)
            mainPtrs[static_cast<size_t>(ch)] = job.mainBuffer->getWritePointer(ch);
        for (int ch = 0; ch < sendChannels; ++ch)
            sendPtrs[static_cast<size_t>(ch)] = job.sendBuffer->getWritePointer(ch);

        juce::AudioBuffer<float> mainView(mainPtrs.data(), mainChannels, blockSamples);
        juce::AudioBuffer<float> sendView(sendPtrs.data(), sendChannels, blockSamples);

        juce::AudioBuffer<float> sourceView;
        const juce::AudioBuffer<float>* sourceViewPtr = nullptr;
        std::array<float*, static_cast<size_t>(maxViewChannels)> sourcePtrs {};
        if (job.sourceAudio != nullptr && job.sourceAudio->getNumChannels() > 0)
        {
            const int sourceChannels = juce::jmin(job.sourceAudio->getNumChannels(), maxViewChannels);
            const int sourceSamples = juce::jmin(job.sourceAudio->getNumSamples(), blockSamples);
            if (sourceChannels > 0 && sourceSamples > 0)
            {
                for (int ch = 0; ch < sourceChannels; ++ch)
                    sourcePtrs[static_cast<size_t>(ch)] = const_cast<float*>(job.sourceAudio->getReadPointer(ch));
                sourceView.setDataToReferTo(sourcePtrs.data(), sourceChannels, sourceSamples);
                sourceViewPtr = &sourceView;
            }
        }

        juce::AudioBuffer<float> monitorView;
        const juce::AudioBuffer<float>* monitorViewPtr = nullptr;
        std::array<float*, static_cast<size_t>(maxViewChannels)> monitorPtrs {};
        if (job.monitorInput != nullptr && job.monitorInput->getNumChannels() > 0)
        {
            const int monitorChannels = juce::jmin(job.monitorInput->getNumChannels(), maxViewChannels);
            const int monitorSamples = juce::jmin(job.monitorInput->getNumSamples(), blockSamples);
            if (monitorChannels > 0 && monitorSamples > 0)
            {
                for (int ch = 0; ch < monitorChannels; ++ch)
                    monitorPtrs[static_cast<size_t>(ch)] = const_cast<float*>(job.monitorInput->getReadPointer(ch));
                monitorView.setDataToReferTo(monitorPtrs.data(), monitorChannels, monitorSamples);
                monitorViewPtr = &monitorView;
            }
        }

        job.track->processBlockAndSends(mainView,
                                        sendView,
                                        *job.midi,
                                        sourceViewPtr,
                                        monitorViewPtr,
                                        job.monitorSafeInput);
        sanitizeAudioBuffer(mainView, blockSamples);
        sanitizeAudioBuffer(sendView, blockSamples);
    }

    constexpr int monitorAnalyzerFftOrder = 11;
    constexpr int monitorAnalyzerFftSize = 1 << monitorAnalyzerFftOrder;
    constexpr int monitorAnalyzerBinCount = monitorAnalyzerFftSize / 2;

    class MonitoringAnalyzerWidget final : public juce::Component, private juce::Timer
    {
    public:
        MonitoringAnalyzerWidget(std::atomic<float>& peakIn,
                                 std::atomic<float>& rmsIn,
                                 std::atomic<int>& clipHoldIn,
                                 std::atomic<float>& phaseIn,
                                 std::atomic<float>& loudnessLufsIn,
                                 std::array<std::array<float, monitorAnalyzerFftSize>, 2>& snapshotsIn,
                                 std::atomic<int>& readySnapshotIn,
                                 std::atomic<double>& sampleRateIn)
            : peakRt(peakIn),
              rmsRt(rmsIn),
              clipHoldRt(clipHoldIn),
              phaseRt(phaseIn),
              loudnessLufsRt(loudnessLufsIn),
              snapshots(snapshotsIn),
              readySnapshotRt(readySnapshotIn),
              sampleRateRt(sampleRateIn),
              fft(monitorAnalyzerFftOrder)
        {
            const float denom = static_cast<float>(juce::jmax(1, monitorAnalyzerFftSize - 1));
            for (int i = 0; i < monitorAnalyzerFftSize; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi * (static_cast<float>(i) / denom);
                fftWindowTable[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(phase));
            }
            spectrumDb.fill(-96.0f);
            startTimerHz(30);
        }

        void paint(juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
            g.setColour(sampledex::theme::Colours::panel().brighter(0.04f));
            g.fillRoundedRectangle(bounds, 5.0f);
            g.setColour(juce::Colours::white.withAlpha(0.14f));
            g.drawRoundedRectangle(bounds, 5.0f, 1.0f);

            auto inner = getLocalBounds().reduced(5);
            auto topRow = inner.removeFromTop(18);
            clipLatchBounds = topRow.removeFromLeft(58).reduced(1, 1);
            const bool clipLatched = clipLatchedDisplay;
            g.setColour(clipLatched ? juce::Colour::fromRGB(220, 64, 64)
                                    : juce::Colour::fromRGB(66, 72, 81));
            g.fillRoundedRectangle(clipLatchBounds.toFloat(), 3.0f);
            g.setColour(clipLatched ? juce::Colours::white
                                    : juce::Colours::white.withAlpha(0.72f));
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawFittedText("CLIP", clipLatchBounds, juce::Justification::centred, 1);

            auto lufsArea = topRow.reduced(4, 0);
            g.setColour(juce::Colours::white.withAlpha(0.78f));
            g.setFont(juce::Font(juce::FontOptions(10.4f, juce::Font::bold)));
            g.drawFittedText("LUFS " + juce::String(loudnessLufsDisplay, 1), lufsArea, juce::Justification::centredLeft, 1);

            inner.removeFromTop(3);
            auto phaseArea = inner.removeFromBottom(20).reduced(1, 1);
            auto meterArea = inner.removeFromLeft(34);
            inner.removeFromLeft(4);
            auto spectrumArea = inner.reduced(1, 1);

            drawMeterPair(g, meterArea);
            drawSpectrum(g, spectrumArea);
            drawPhaseMeter(g, phaseArea);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (!clipLatchBounds.contains(e.getPosition()))
                return;

            clipHoldRt.store(0, std::memory_order_relaxed);
            holdPeakDisplay = 0.0f;
            holdRmsDisplay = 0.0f;
            clipLatchedDisplay = false;
            repaint();
        }

    private:
        static float gainToNorm(float gain)
        {
            const float db = juce::Decibels::gainToDecibels(juce::jmax(0.000001f, gain), -60.0f);
            return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        }

        static void drawMeterBar(juce::Graphics& g,
                                 juce::Rectangle<int> area,
                                 float level,
                                 float hold,
                                 juce::Colour colour,
                                 const juce::String& label)
        {
            if (area.isEmpty())
                return;

            g.setColour(juce::Colour::fromRGB(24, 28, 35));
            g.fillRoundedRectangle(area.toFloat(), 2.2f);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawRoundedRectangle(area.toFloat(), 2.2f, 0.8f);

            const auto fillNorm = gainToNorm(level);
            const int fillHeight = juce::roundToInt(fillNorm * static_cast<float>(area.getHeight()));
            if (fillHeight > 0)
            {
                auto fill = area.withTop(area.getBottom() - fillHeight);
                g.setColour(colour.withAlpha(0.86f));
                g.fillRoundedRectangle(fill.toFloat(), 1.8f);
            }

            const auto holdNorm = gainToNorm(hold);
            const int holdY = area.getBottom() - juce::roundToInt(holdNorm * static_cast<float>(area.getHeight()));
            g.setColour(juce::Colours::white.withAlpha(0.78f));
            g.drawLine(static_cast<float>(area.getX()) + 1.0f,
                       static_cast<float>(holdY),
                       static_cast<float>(area.getRight()) - 1.0f,
                       static_cast<float>(holdY),
                       1.0f);

            g.setColour(juce::Colours::white.withAlpha(0.72f));
            g.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
            g.drawFittedText(label,
                             area.removeFromTop(12),
                             juce::Justification::centred,
                             1);
        }

        void drawMeterPair(juce::Graphics& g, juce::Rectangle<int> area) const
        {
            if (area.getWidth() < 18 || area.getHeight() < 30)
                return;

            const int gap = 3;
            auto rmsBar = area.removeFromLeft((area.getWidth() - gap) / 2).reduced(0, 1);
            area.removeFromLeft(gap);
            auto peakBar = area.reduced(0, 1);

            drawMeterBar(g, rmsBar, rmsDisplay, holdRmsDisplay, juce::Colour::fromRGB(83, 168, 230), "R");
            drawMeterBar(g, peakBar, peakDisplay, holdPeakDisplay, juce::Colour::fromRGB(255, 166, 41), "P");
        }

        void drawSpectrum(juce::Graphics& g, juce::Rectangle<int> area) const
        {
            if (area.getWidth() < 32 || area.getHeight() < 32)
                return;

            g.setColour(juce::Colour::fromRGB(17, 21, 27));
            g.fillRoundedRectangle(area.toFloat(), 2.4f);
            g.setColour(juce::Colours::white.withAlpha(0.11f));
            g.drawRoundedRectangle(area.toFloat(), 2.4f, 0.8f);

            for (int db = -72; db <= -12; db += 20)
            {
                const float norm = juce::jlimit(0.0f, 1.0f, (static_cast<float>(db) + 90.0f) / 90.0f);
                const float y = area.getBottom() - (norm * static_cast<float>(area.getHeight()));
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                g.drawHorizontalLine(juce::roundToInt(y), static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
            }

            juce::Path curve;
            const double sr = juce::jmax(8000.0, sampleRateRt.load(std::memory_order_relaxed));
            const double nyquist = juce::jmax(50.0, sr * 0.5);
            const double minFreq = 20.0;
            const double ratio = nyquist > minFreq ? nyquist / minFreq : 1.0;
            const int width = juce::jmax(1, area.getWidth());
            for (int x = 0; x < width; ++x)
            {
                const float xNorm = static_cast<float>(x) / static_cast<float>(juce::jmax(1, width - 1));
                const double freq = minFreq * std::pow(ratio, static_cast<double>(xNorm));
                const int bin = juce::jlimit(1,
                                             monitorAnalyzerBinCount - 1,
                                             static_cast<int>(std::round((freq / sr) * static_cast<double>(monitorAnalyzerFftSize))));
                const float db = spectrumDb[static_cast<size_t>(bin)];
                const float yNorm = juce::jlimit(0.0f, 1.0f, (db + 90.0f) / 90.0f);
                const float xPos = static_cast<float>(area.getX() + x);
                const float yPos = area.getBottom() - (yNorm * static_cast<float>(area.getHeight()));
                if (x == 0)
                    curve.startNewSubPath(xPos, yPos);
                else
                    curve.lineTo(xPos, yPos);
            }

            g.setColour(juce::Colour::fromRGB(90, 201, 168).withAlpha(0.95f));
            g.strokePath(curve, juce::PathStrokeType(1.4f));
        }

        void drawPhaseMeter(juce::Graphics& g, juce::Rectangle<int> area) const
        {
            if (area.getWidth() < 28 || area.getHeight() < 12)
                return;

            g.setColour(juce::Colour::fromRGB(17, 21, 27));
            g.fillRoundedRectangle(area.toFloat(), 2.2f);
            g.setColour(juce::Colours::white.withAlpha(0.11f));
            g.drawRoundedRectangle(area.toFloat(), 2.2f, 0.8f);

            const float centerX = area.getX() + (area.getWidth() * 0.5f);
            g.setColour(juce::Colours::white.withAlpha(0.20f));
            g.drawVerticalLine(juce::roundToInt(centerX), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));

            const float phaseNorm = juce::jlimit(0.0f, 1.0f, (phaseDisplay + 1.0f) * 0.5f);
            const float x = area.getX() + (phaseNorm * static_cast<float>(area.getWidth()));
            const juce::Colour indicator = phaseDisplay < -0.1f
                ? juce::Colour::fromRGB(220, 96, 86)
                : (phaseDisplay > 0.2f
                    ? juce::Colour::fromRGB(94, 204, 132)
                    : juce::Colour::fromRGB(233, 194, 86));
            g.setColour(indicator);
            g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(area.getY()) + 1.0f, static_cast<float>(area.getBottom()) - 1.0f);

            g.setColour(juce::Colours::white.withAlpha(0.78f));
            g.setFont(juce::Font(juce::FontOptions(9.2f, juce::Font::bold)));
            g.drawFittedText("Phase " + juce::String(phaseDisplay, 2), area.reduced(4, 0), juce::Justification::centredLeft, 1);
        }

        void refreshSpectrum()
        {
            for (size_t i = 1; i < spectrumDb.size(); ++i)
                spectrumDb[i] = juce::jmax(-96.0f, spectrumDb[i] - 0.32f);

            const int readyIndex = readySnapshotRt.load(std::memory_order_acquire);
            if (!juce::isPositiveAndBelow(readyIndex, static_cast<int>(snapshots.size()))
                || readyIndex == lastConsumedSnapshot)
            {
                return;
            }

            lastConsumedSnapshot = readyIndex;
            const auto& source = snapshots[static_cast<size_t>(readyIndex)];
            fftData.fill(0.0f);
            std::copy(source.begin(), source.end(), fftData.begin());
            for (int i = 0; i < monitorAnalyzerFftSize; ++i)
                fftData[static_cast<size_t>(i)] *= fftWindowTable[static_cast<size_t>(i)];
            fft.performFrequencyOnlyForwardTransform(fftData.data());

            for (int bin = 1; bin < monitorAnalyzerBinCount; ++bin)
            {
                const float mag = juce::jmax(1.0e-8f,
                                             fftData[static_cast<size_t>(bin)] / static_cast<float>(monitorAnalyzerFftSize));
                const float db = juce::jlimit(-96.0f, 6.0f, juce::Decibels::gainToDecibels(mag, -120.0f));
                auto& display = spectrumDb[static_cast<size_t>(bin)];
                display += (db - display) * 0.28f;
            }
        }

        void timerCallback() override
        {
            const float peak = peakRt.load(std::memory_order_relaxed);
            const float rms = rmsRt.load(std::memory_order_relaxed);
            const float phase = phaseRt.load(std::memory_order_relaxed);
            const float loudnessLufs = loudnessLufsRt.load(std::memory_order_relaxed);

            peakDisplay = juce::jmax(peak, peakDisplay * 0.92f);
            rmsDisplay += (rms - rmsDisplay) * 0.22f;
            phaseDisplay += (phase - phaseDisplay) * 0.24f;
            loudnessLufsDisplay += (loudnessLufs - loudnessLufsDisplay) * 0.16f;

            holdPeakDisplay = juce::jmax(peakDisplay, holdPeakDisplay * 0.985f);
            holdRmsDisplay = juce::jmax(rmsDisplay, holdRmsDisplay * 0.985f);
            clipLatchedDisplay = clipHoldRt.load(std::memory_order_relaxed) > 0;
            refreshSpectrum();
            repaint();
        }

        std::atomic<float>& peakRt;
        std::atomic<float>& rmsRt;
        std::atomic<int>& clipHoldRt;
        std::atomic<float>& phaseRt;
        std::atomic<float>& loudnessLufsRt;
        std::array<std::array<float, monitorAnalyzerFftSize>, 2>& snapshots;
        std::atomic<int>& readySnapshotRt;
        std::atomic<double>& sampleRateRt;
        juce::dsp::FFT fft;
        std::array<float, monitorAnalyzerFftSize> fftWindowTable {};
        std::array<float, monitorAnalyzerFftSize * 2> fftData {};
        std::array<float, monitorAnalyzerBinCount> spectrumDb {};
        int lastConsumedSnapshot = -1;
        float peakDisplay = 0.0f;
        float rmsDisplay = 0.0f;
        float phaseDisplay = 0.0f;
        float loudnessLufsDisplay = -96.0f;
        float holdPeakDisplay = 0.0f;
        float holdRmsDisplay = 0.0f;
        bool clipLatchedDisplay = false;
        juce::Rectangle<int> clipLatchBounds;
    };

    class LambdaRenderJob final : public juce::ThreadPoolJob
    {
    public:
        LambdaRenderJob(juce::String nameIn, std::function<void()> taskIn)
            : juce::ThreadPoolJob(std::move(nameIn)),
              task(std::move(taskIn))
        {
        }

        JobStatus runJob() override
        {
            if (task)
                task();
            return jobHasFinished;
        }

    private:
        std::function<void()> task;
    };

    class FloatingPluginWindow : public juce::DocumentWindow
    {
    public:
        FloatingPluginWindow(const juce::String& windowTitle,
                             juce::AudioProcessorEditor* editorToOwn,
                             std::function<void(FloatingPluginWindow*)> onCloseCallback)
            : juce::DocumentWindow(windowTitle,
                                   juce::Colours::black,
                                   juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
              onClose(std::move(onCloseCallback))
        {
            // Avoid native-titlebar recreation path on macOS that has caused plugin-window crashes.
            setUsingNativeTitleBar(false);
            setResizable(true, true);
            setContentOwned(editorToOwn, true);
            setResizeLimits(320, 220, 3840, 2160);
            setAlwaysOnTop(false);

            int width = 720;
            int height = 500;
            if (editorToOwn != nullptr)
            {
                width = juce::jmax(480, editorToOwn->getWidth() + 16);
                height = juce::jmax(320, editorToOwn->getHeight() + 40);
            }

            setSize(width, height);
            centreWithSize(width, height);
            setVisible(true);
            toFront(true);
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            if (onClose)
                onClose(this);
        }

    private:
        std::function<void(FloatingPluginWindow*)> onClose;
    };

    class ChannelRackContent : public juce::Component, private juce::Timer
    {
    public:
        static constexpr int slotCount = 5;

        std::function<void(int, int)> onLoadSlot;
        std::function<void(int, int)> onOpenSlot;
        std::function<void(int, int, bool)> onBypassChanged;
        std::function<void(int, int)> onClearSlot;
        std::function<void(int, int, int)> onMoveSlot;

        ChannelRackContent()
        {
            title.setJustificationType(juce::Justification::centredLeft);
            title.setColour(juce::Label::textColourId, juce::Colours::white);
            title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            addAndMakeVisible(title);

            sendLabel.setText("Send", juce::dontSendNotification);
            sendLabel.setJustificationType(juce::Justification::centredLeft);
            sendLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.86f));
            addAndMakeVisible(sendLabel);

            sendSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            sendSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            sendSlider.setRange(0.0, 1.0, 0.01);
            sendSlider.onValueChange = [this]
            {
                if (track != nullptr)
                    track->setSendLevel(static_cast<float>(sendSlider.getValue()));
            };
            sendSlider.setTooltip("Aux send level.");
            addAndMakeVisible(sendSlider);

            sendTapBox.addItem("Pre", 1);
            sendTapBox.addItem("Post", 2);
            sendTapBox.addItem("Post-Pan", 3);
            sendTapBox.onChange = [this]
            {
                if (track == nullptr)
                    return;
                const int selectedId = sendTapBox.getSelectedId();
                const auto mode = (selectedId == 1) ? sampledex::Track::SendTapMode::PreFader
                                                    : (selectedId == 3 ? sampledex::Track::SendTapMode::PostPan
                                                                       : sampledex::Track::SendTapMode::PostFader);
                track->setSendTapMode(mode);
            };
            sendTapBox.setTooltip("Send tap point.");
            addAndMakeVisible(sendTapBox);

            for (int bus = 0; bus < sampledex::Track::maxSendBuses; ++bus)
                sendBusBox.addItem("Bus " + juce::String(bus + 1), bus + 1);
            sendBusBox.onChange = [this]
            {
                if (track != nullptr)
                    track->setSendTargetBus(sendBusBox.getSelectedId() - 1);
            };
            sendBusBox.setTooltip("Send destination bus.");
            addAndMakeVisible(sendBusBox);

            outputRouteBox.addItem("Master", 1);
            for (int bus = 0; bus < sampledex::Track::maxSendBuses; ++bus)
                outputRouteBox.addItem("Bus " + juce::String(bus + 1), bus + 2);
            outputRouteBox.onChange = [this]
            {
                if (track == nullptr)
                    return;
                const int selected = outputRouteBox.getSelectedId();
                if (selected <= 1)
                    track->routeOutputToMaster();
                else
                    track->routeOutputToBus(selected - 2);
            };
            outputRouteBox.setTooltip("Track output route.");
            addAndMakeVisible(outputRouteBox);

            for (int i = 0; i < slotCount; ++i)
            {
                auto& slotLabel = slotLabels[static_cast<size_t>(i)];
                slotLabel.setJustificationType(juce::Justification::centredLeft);
                slotLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
                slotLabel.setInterceptsMouseClicks(true, false);
                slotLabel.addMouseListener(this, false);
                slotLabel.setTooltip("Plugin slot name. Left-click opens plugin UI, right-click loads/replaces plugin.");
                addAndMakeVisible(slotLabel);

                auto& loadButton = loadButtons[static_cast<size_t>(i)];
                loadButton.setButtonText("Load");
                loadButton.onClick = [this, i]
                {
                    if (onLoadSlot && trackIndex >= 0)
                        onLoadSlot(trackIndex, displayIndexToSlotIndex(i));
                };
                addAndMakeVisible(loadButton);

                auto& openButton = openButtons[static_cast<size_t>(i)];
                openButton.setButtonText("UI");
                openButton.onClick = [this, i]
                {
                    if (onOpenSlot && trackIndex >= 0)
                        onOpenSlot(trackIndex, displayIndexToSlotIndex(i));
                };
                addAndMakeVisible(openButton);

                auto& bypassButton = bypassButtons[static_cast<size_t>(i)];
                bypassButton.setButtonText("On");
                bypassButton.setClickingTogglesState(true);
                bypassButton.setTooltip("Plugin power. Off bypasses the plugin but keeps the UI available.");
                bypassButton.onClick = [this, i]
                {
                    if (onBypassChanged && trackIndex >= 0)
                        onBypassChanged(trackIndex,
                                        displayIndexToSlotIndex(i),
                                        bypassButtons[static_cast<size_t>(i)].getToggleState());
                };
                addAndMakeVisible(bypassButton);

                auto& clearButton = clearButtons[static_cast<size_t>(i)];
                clearButton.setButtonText("Clear");
                clearButton.onClick = [this, i]
                {
                    if (onClearSlot && trackIndex >= 0)
                        onClearSlot(trackIndex, displayIndexToSlotIndex(i));
                };
                addAndMakeVisible(clearButton);

                auto& moveUpButton = moveUpButtons[static_cast<size_t>(i)];
                moveUpButton.setButtonText("Up");
                moveUpButton.onClick = [this, i]
                {
                    if (onMoveSlot && trackIndex >= 0 && i > 1)
                        onMoveSlot(trackIndex, displayIndexToSlotIndex(i), displayIndexToSlotIndex(i - 1));
                };
                addAndMakeVisible(moveUpButton);

                auto& moveDownButton = moveDownButtons[static_cast<size_t>(i)];
                moveDownButton.setButtonText("Dn");
                moveDownButton.onClick = [this, i]
                {
                    if (onMoveSlot && trackIndex >= 0 && i > 0 && i < slotCount - 1)
                        onMoveSlot(trackIndex, displayIndexToSlotIndex(i), displayIndexToSlotIndex(i + 1));
                };
                addAndMakeVisible(moveDownButton);
            }

            startTimerHz(5);
        }

        void setTrack(sampledex::Track* newTrack, int newTrackIndex)
        {
            track = newTrack;
            trackIndex = newTrackIndex;
            slotDragCandidate = false;
            slotDragActive = false;
            slotDragSource = -1;
            slotDragTarget = -1;
            refreshFromTrack();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour::fromRGB(27, 30, 36));
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.drawRect(getLocalBounds(), 1);

            if (slotDragActive && juce::isPositiveAndBelow(slotDragTarget, slotCount))
            {
                g.setColour(sampledex::theme::Colours::accent().withAlpha(0.82f));
                g.drawRoundedRectangle(slotLabels[static_cast<size_t>(slotDragTarget)].getBounds().toFloat().expanded(2.0f),
                                       4.0f,
                                       1.8f);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            title.setBounds(r.removeFromTop(30));
            auto routeRow = r.removeFromTop(24);
            sendLabel.setBounds(routeRow.removeFromLeft(42));
            outputRouteBox.setBounds(routeRow.removeFromRight(94).reduced(2, 0));
            sendBusBox.setBounds(routeRow.removeFromRight(84).reduced(2, 0));
            sendTapBox.setBounds(routeRow.removeFromRight(84).reduced(2, 0));
            sendSlider.setBounds(routeRow.reduced(2, 0));
            r.removeFromTop(6);

            for (int i = 0; i < slotCount; ++i)
            {
                auto row = r.removeFromTop(34);
                slotLabels[static_cast<size_t>(i)].setBounds(row.removeFromLeft(190));
                loadButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(50).reduced(2));
                openButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(42).reduced(2));
                bypassButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(50).reduced(2));
                clearButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(56).reduced(2));
                moveUpButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(38).reduced(2));
                moveDownButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(38).reduced(2));
                r.removeFromTop(4);
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (track == nullptr || trackIndex < 0 || e.mods.isPopupMenu())
                return;

            for (int i = 0; i < slotCount; ++i)
            {
                if (e.eventComponent == &slotLabels[static_cast<size_t>(i)])
                {
                    if (i == 0)
                        return;
                    slotDragCandidate = true;
                    slotDragActive = false;
                    slotDragSource = i;
                    slotDragTarget = i;
                    slotDragStart = e.getEventRelativeTo(this).position;
                    return;
                }
            }
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (!slotDragCandidate || track == nullptr || trackIndex < 0 || e.mods.isPopupMenu())
                return;

            const auto localPos = e.getEventRelativeTo(this).position;
            if (!slotDragActive)
            {
                if (localPos.getDistanceFrom(slotDragStart) < 4.0f)
                    return;
                slotDragActive = true;
            }

            const int target = getClosestSlotIndexForY(localPos.y);
            if (target != slotDragTarget)
            {
                slotDragTarget = target;
                repaint();
            }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (track == nullptr || trackIndex < 0)
                return;

            if (slotDragCandidate)
            {
                const bool performReorder = slotDragActive
                                            && juce::isPositiveAndBelow(slotDragSource, slotCount)
                                            && juce::isPositiveAndBelow(slotDragTarget, slotCount)
                                            && slotDragSource > 0
                                            && slotDragTarget > 0
                                            && slotDragTarget != slotDragSource;
                const int source = slotDragSource;
                const int target = slotDragTarget;
                slotDragCandidate = false;
                slotDragActive = false;
                slotDragSource = -1;
                slotDragTarget = -1;
                repaint();

                if (performReorder)
                {
                    if (onMoveSlot)
                        onMoveSlot(trackIndex,
                                   displayIndexToSlotIndex(source),
                                   displayIndexToSlotIndex(target));
                    return;
                }
            }

            for (int i = 0; i < slotCount; ++i)
            {
                if (e.eventComponent != &slotLabels[static_cast<size_t>(i)])
                    continue;

                if (e.mods.isPopupMenu())
                {
                    showSlotContextMenu(i);
                }
                else
                {
                    const int slotIndex = displayIndexToSlotIndex(i);
                    if (!track->hasPluginInSlotNonBlocking(slotIndex))
                    {
                        if (onLoadSlot)
                            onLoadSlot(trackIndex, slotIndex);
                    }
                    else if (onOpenSlot)
                    {
                        onOpenSlot(trackIndex, slotIndex);
                    }
                }
                return;
            }
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            bool hoverAnyLabel = false;
            for (int i = 0; i < slotCount; ++i)
            {
                if (e.eventComponent == &slotLabels[static_cast<size_t>(i)])
                {
                    hoverAnyLabel = true;
                    break;
                }
            }
            setMouseCursor(hoverAnyLabel ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }

    private:
        static int displayIndexToSlotIndex(int displayIndex)
        {
            return displayIndex == 0 ? ::sampledex::Track::instrumentSlotIndex : (displayIndex - 1);
        }

        static juce::String displayIndexToLabel(int displayIndex)
        {
            return displayIndex == 0 ? juce::String("Instrument")
                                     : juce::String("Insert " + juce::String(displayIndex));
        }

        int getClosestSlotIndexForY(float y) const
        {
            int nearest = 0;
            float nearestDistance = std::numeric_limits<float>::max();
            for (int i = 0; i < slotCount; ++i)
            {
                const float centerY = static_cast<float>(slotLabels[static_cast<size_t>(i)].getBounds().getCentreY());
                const float d = std::abs(centerY - y);
                if (d < nearestDistance)
                {
                    nearest = i;
                    nearestDistance = d;
                }
            }
            return nearest;
        }

        void showSlotContextMenu(int slotIndex)
        {
            if (track == nullptr || trackIndex < 0 || !juce::isPositiveAndBelow(slotIndex, slotCount))
                return;

            const int mappedSlotIndex = displayIndexToSlotIndex(slotIndex);
            const bool hasPlugin = track->hasPluginInSlotNonBlocking(mappedSlotIndex);
            const bool isBypassed = hasPlugin && track->isPluginSlotBypassedNonBlocking(mappedSlotIndex);
            const bool isInstrumentSlot = slotIndex == 0;
            const juce::String slotLabel = displayIndexToLabel(slotIndex);

            juce::PopupMenu menu;
            menu.addItem(1, "Open Plugin UI", hasPlugin);
            menu.addItem(2, hasPlugin ? "Replace Plugin..." : "Load Plugin...");
            menu.addItem(3, "Toggle Bypass", hasPlugin, isBypassed);
            menu.addItem(4, "Clear Slot", hasPlugin);
            menu.addSeparator();
            menu.addItem(5, "Move Up", !isInstrumentSlot && slotIndex > 1);
            menu.addItem(6, "Move Down", !isInstrumentSlot && slotIndex < slotCount - 1);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                               [this, slotIndex, mappedSlotIndex, slotLabel](int selectedId)
                               {
                                   if (track == nullptr || trackIndex < 0)
                                       return;

                                   if (selectedId == 1)
                                   {
                                       if (onOpenSlot)
                                           onOpenSlot(trackIndex, mappedSlotIndex);
                                   }
                                   else if (selectedId == 2)
                                   {
                                       if (onLoadSlot)
                                           onLoadSlot(trackIndex, mappedSlotIndex);
                                   }
                                   else if (selectedId == 3)
                                   {
                                       if (onBypassChanged)
                                           onBypassChanged(trackIndex, mappedSlotIndex, !track->isPluginSlotBypassedNonBlocking(mappedSlotIndex));
                                   }
                                   else if (selectedId == 4)
                                   {
                                       if (onClearSlot)
                                           onClearSlot(trackIndex, mappedSlotIndex);
                                   }
                                   else if (selectedId == 5)
                                   {
                                       if (onMoveSlot && slotIndex > 1)
                                           onMoveSlot(trackIndex, mappedSlotIndex, displayIndexToSlotIndex(slotIndex - 1));
                                   }
                                   else if (selectedId == 6)
                                   {
                                       if (onMoveSlot && slotIndex > 0 && slotIndex < slotCount - 1)
                                           onMoveSlot(trackIndex, mappedSlotIndex, displayIndexToSlotIndex(slotIndex + 1));
                                   }
                                   juce::ignoreUnused(slotLabel);
                               });
        }

        void timerCallback() override
        {
            refreshFromTrack();
        }

        void refreshFromTrack()
        {
            if (track == nullptr || trackIndex < 0)
            {
                title.setText("Channel Rack", juce::dontSendNotification);
                sendSlider.setEnabled(false);
                sendTapBox.setEnabled(false);
                sendBusBox.setEnabled(false);
                outputRouteBox.setEnabled(false);
                sendSlider.setValue(0.0, juce::dontSendNotification);
                sendTapBox.setSelectedId(0, juce::dontSendNotification);
                sendBusBox.setSelectedId(0, juce::dontSendNotification);
                outputRouteBox.setSelectedId(0, juce::dontSendNotification);
                for (int i = 0; i < slotCount; ++i)
                {
                    slotLabels[static_cast<size_t>(i)].setText(displayIndexToLabel(i) + ": Empty",
                                                               juce::dontSendNotification);
                    openButtons[static_cast<size_t>(i)].setEnabled(false);
                    bypassButtons[static_cast<size_t>(i)].setEnabled(false);
                    bypassButtons[static_cast<size_t>(i)].setToggleState(false, juce::dontSendNotification);
                    bypassButtons[static_cast<size_t>(i)].setButtonText("Off");
                    clearButtons[static_cast<size_t>(i)].setEnabled(false);
                    moveUpButtons[static_cast<size_t>(i)].setEnabled(false);
                    moveDownButtons[static_cast<size_t>(i)].setEnabled(false);
                }
                return;
            }

            juce::String rackTitle = track->getTrackName() + "  |  " + track->getPluginSummary();
            if (track->isRenderTaskActive())
            {
                const int pct = juce::roundToInt(juce::jlimit(0.0f, 1.0f, track->getRenderTaskProgress()) * 100.0f);
                rackTitle << "  |  " << track->getRenderTaskLabel() << " " << juce::String(pct) << "%";
            }
            title.setText(rackTitle, juce::dontSendNotification);
            sendSlider.setEnabled(true);
            sendTapBox.setEnabled(true);
            sendBusBox.setEnabled(true);
            outputRouteBox.setEnabled(true);
            sendSlider.setValue(track->getSendLevel(), juce::dontSendNotification);
            sendTapBox.setSelectedId(track->getSendTapMode() == sampledex::Track::SendTapMode::PreFader
                                         ? 1
                                         : (track->getSendTapMode() == sampledex::Track::SendTapMode::PostPan ? 3 : 2),
                                     juce::dontSendNotification);
            sendBusBox.setSelectedId(track->getSendTargetBus() + 1, juce::dontSendNotification);
            outputRouteBox.setSelectedId(track->getOutputTargetType() == sampledex::Track::OutputTargetType::Master
                                             ? 1
                                             : (track->getOutputTargetBus() + 2),
                                         juce::dontSendNotification);
            for (int i = 0; i < slotCount; ++i)
            {
                const int mappedSlotIndex = displayIndexToSlotIndex(i);
                const auto name = track->getPluginNameForSlotNonBlocking(mappedSlotIndex);
                const bool hasPlugin = track->hasPluginInSlotNonBlocking(mappedSlotIndex);
                const bool bypassed = hasPlugin && track->isPluginSlotBypassedNonBlocking(mappedSlotIndex);
                slotLabels[static_cast<size_t>(i)].setText(displayIndexToLabel(i) + ": "
                                                           + (hasPlugin ? name : juce::String("Empty")),
                                                           juce::dontSendNotification);
                slotLabels[static_cast<size_t>(i)].setColour(juce::Label::textColourId,
                                                             hasPlugin ? sampledex::theme::Colours::accent().withAlpha(0.95f)
                                                                       : juce::Colours::white.withAlpha(0.70f));
                openButtons[static_cast<size_t>(i)].setEnabled(hasPlugin);
                clearButtons[static_cast<size_t>(i)].setEnabled(hasPlugin);
                bypassButtons[static_cast<size_t>(i)].setEnabled(hasPlugin);
                moveUpButtons[static_cast<size_t>(i)].setEnabled(hasPlugin && i > 1);
                moveDownButtons[static_cast<size_t>(i)].setEnabled(hasPlugin && i > 0 && i < slotCount - 1);
                bypassButtons[static_cast<size_t>(i)].setToggleState(bypassed, juce::dontSendNotification);
                bypassButtons[static_cast<size_t>(i)].setButtonText(!hasPlugin ? "Off" : (bypassed ? "Off" : "On"));
            }
        }

        sampledex::Track* track = nullptr;
        int trackIndex = -1;
        juce::Label title;
        juce::Label sendLabel;
        juce::Slider sendSlider;
        juce::ComboBox sendTapBox, sendBusBox, outputRouteBox;
        std::array<juce::Label, static_cast<size_t>(slotCount)> slotLabels;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> loadButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> openButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> bypassButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> clearButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> moveUpButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> moveDownButtons;
        bool slotDragCandidate = false;
        bool slotDragActive = false;
        int slotDragSource = -1;
        int slotDragTarget = -1;
        juce::Point<float> slotDragStart;
    };

    class TrackInspectorContent : public juce::Component, private juce::Timer
    {
    public:
        static constexpr int slotCount = 5;

        std::function<void(int)> onRenameTrack;
        std::function<void(int)> onDuplicateTrack;
        std::function<void(int)> onDeleteTrack;
        std::function<void(int)> onOpenFloatingRack;
        std::function<void(int, int)> onLoadSlot;
        std::function<void(int, int)> onOpenSlot;
        std::function<void(int, int, bool)> onBypassChanged;
        std::function<void(int, int)> onClearSlot;
        std::function<void(int, int, int)> onMoveSlot;
        std::function<bool(int, bool)> onInputMonitoringChanged;

        TrackInspectorContent()
        {
            title.setJustificationType(juce::Justification::centredLeft);
            title.setColour(juce::Label::textColourId, juce::Colours::white);
            title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            addAndMakeVisible(title);

            renameButton.setButtonText("Rename");
            renameButton.onClick = [this]
            {
                if (trackIndex >= 0 && onRenameTrack)
                    onRenameTrack(trackIndex);
            };
            addAndMakeVisible(renameButton);

            duplicateButton.setButtonText("Duplicate");
            duplicateButton.onClick = [this]
            {
                if (trackIndex >= 0 && onDuplicateTrack)
                    onDuplicateTrack(trackIndex);
            };
            addAndMakeVisible(duplicateButton);

            deleteButton.setButtonText("Delete");
            deleteButton.onClick = [this]
            {
                if (trackIndex >= 0 && onDeleteTrack)
                    onDeleteTrack(trackIndex);
            };
            addAndMakeVisible(deleteButton);

            floatingRackButton.setButtonText("Float Rack");
            floatingRackButton.onClick = [this]
            {
                if (trackIndex >= 0 && onOpenFloatingRack)
                    onOpenFloatingRack(trackIndex);
            };
            addAndMakeVisible(floatingRackButton);

            muteButton.setButtonText("M");
            muteButton.setClickingTogglesState(true);
            muteButton.onClick = [this] { if (track != nullptr) track->setMute(muteButton.getToggleState()); };
            addAndMakeVisible(muteButton);

            soloButton.setButtonText("S");
            soloButton.setClickingTogglesState(true);
            soloButton.onClick = [this] { if (track != nullptr) track->setSolo(soloButton.getToggleState()); };
            addAndMakeVisible(soloButton);

            armButton.setButtonText("R");
            armButton.setClickingTogglesState(true);
            armButton.onClick = [this] { if (track != nullptr) track->setArm(armButton.getToggleState()); };
            addAndMakeVisible(armButton);

            monitorButton.setButtonText("I");
            monitorButton.setClickingTogglesState(true);
            monitorButton.onClick = [this]
            {
                if (track == nullptr || trackIndex < 0)
                    return;

                const bool shouldEnable = monitorButton.getToggleState();
                bool applied = false;
                if (onInputMonitoringChanged)
                    applied = onInputMonitoringChanged(trackIndex, shouldEnable);
                else
                {
                    track->setInputMonitoring(shouldEnable);
                    applied = true;
                }

                if (!applied)
                    monitorButton.setToggleState(track->isInputMonitoringEnabled(), juce::dontSendNotification);
            };
            addAndMakeVisible(monitorButton);

            inputSourceLabel.setText("Input", juce::dontSendNotification);
            inputSourceLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(inputSourceLabel);
            inputSourceBox.onChange = [this]
            {
                if (track != nullptr)
                    track->setInputSourcePair(inputSourceBox.getSelectedId() <= 1
                                                  ? -1
                                                  : (inputSourceBox.getSelectedId() - 2));
            };
            inputSourceBox.setTooltip("Per-track audio input pair used for input monitoring. Auto follows the hottest input channel.");
            addAndMakeVisible(inputSourceBox);
            rebuildInputSourceChoices();

            monitorTapLabel.setText("Mon Tap", juce::dontSendNotification);
            monitorTapLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(monitorTapLabel);
            monitorTapBox.addItem("Pre Inserts", 1);
            monitorTapBox.addItem("Post Inserts", 2);
            monitorTapBox.onChange = [this]
            {
                if (track == nullptr)
                    return;
                const auto mode = monitorTapBox.getSelectedId() == 1
                    ? ::sampledex::Track::MonitorTapMode::PreInserts
                    : ::sampledex::Track::MonitorTapMode::PostInserts;
                track->setMonitorTapMode(mode);
            };
            monitorTapBox.setTooltip("Input monitor tap point: dry (pre insert) or through insert FX.");
            addAndMakeVisible(monitorTapBox);

            monitorGainLabel.setText("Mon Gain", juce::dontSendNotification);
            monitorGainLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(monitorGainLabel);
            monitorGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            monitorGainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            monitorGainSlider.setRange(0.0, 1.5, 0.01);
            monitorGainSlider.onValueChange = [this]
            {
                if (track != nullptr)
                    track->setInputMonitorGain(static_cast<float>(monitorGainSlider.getValue()));
            };
            monitorGainSlider.setTooltip("Input monitoring gain.");
            addAndMakeVisible(monitorGainSlider);

            volumeLabel.setText("Volume", juce::dontSendNotification);
            volumeLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(volumeLabel);
            volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            volumeSlider.setRange(0.0, 1.2, 0.01);
            volumeSlider.onValueChange = [this] { if (track != nullptr) track->setVolume(static_cast<float>(volumeSlider.getValue())); };
            addAndMakeVisible(volumeSlider);

            panLabel.setText("Pan", juce::dontSendNotification);
            panLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(panLabel);
            panSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            panSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            panSlider.setRange(-1.0, 1.0, 0.01);
            panSlider.onValueChange = [this] { if (track != nullptr) track->setPan(static_cast<float>(panSlider.getValue())); };
            addAndMakeVisible(panSlider);

            sendLabel.setText("Send", juce::dontSendNotification);
            sendLabel.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(sendLabel);
            sendSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            sendSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            sendSlider.setRange(0.0, 1.0, 0.01);
            sendSlider.onValueChange = [this] { if (track != nullptr) track->setSendLevel(static_cast<float>(sendSlider.getValue())); };
            addAndMakeVisible(sendSlider);
            sendModeBox.addItem("Pre", 1);
            sendModeBox.addItem("Post", 2);
            sendModeBox.addItem("Post-Pan", 3);
            sendModeBox.onChange = [this]
            {
                if (track != nullptr)
                {
                    const int selected = sendModeBox.getSelectedId();
                    const auto mode = (selected == 1) ? ::sampledex::Track::SendTapMode::PreFader
                                                      : (selected == 3 ? ::sampledex::Track::SendTapMode::PostPan
                                                                       : ::sampledex::Track::SendTapMode::PostFader);
                    track->setSendTapMode(mode);
                }
            };
            sendModeBox.setTooltip("Send tap point: Pre, Post-fader, or Post-pan.");
            addAndMakeVisible(sendModeBox);
            for (int bus = 0; bus < ::sampledex::Track::maxSendBuses; ++bus)
                sendBusBox.addItem("Bus " + juce::String(bus + 1), bus + 1);
            sendBusBox.onChange = [this]
            {
                if (track != nullptr)
                    track->setSendTargetBus(sendBusBox.getSelectedId() - 1);
            };
            sendBusBox.setTooltip("Send destination bus.");
            addAndMakeVisible(sendBusBox);

            for (int i = 0; i < slotCount; ++i)
            {
                auto& slotButton = slotButtons[static_cast<size_t>(i)];
                slotButton.setButtonText(displayIndexToLabel(i) + ": Empty");
                slotButton.addMouseListener(this, false);
                slotButton.setTooltip("Instrument/insert slot. Left-click open/load, right-click for slot actions.");
                slotButton.onClick = [this, i]
                {
                    if (trackIndex < 0)
                        return;
                    const int slotIndex = displayIndexToSlotIndex(i);
                    if (track != nullptr && track->hasPluginInSlotNonBlocking(slotIndex))
                    {
                        if (onOpenSlot)
                            onOpenSlot(trackIndex, slotIndex);
                    }
                    else if (onLoadSlot)
                    {
                        onLoadSlot(trackIndex, slotIndex);
                    }
                };
                addAndMakeVisible(slotButton);

                auto& loadButton = loadButtons[static_cast<size_t>(i)];
                loadButton.setButtonText("Load");
                loadButton.onClick = [this, i]
                {
                    if (trackIndex >= 0 && onLoadSlot)
                        onLoadSlot(trackIndex, displayIndexToSlotIndex(i));
                };
                addAndMakeVisible(loadButton);

                auto& bypassButton = bypassButtons[static_cast<size_t>(i)];
                bypassButton.setButtonText("On");
                bypassButton.setClickingTogglesState(true);
                bypassButton.setTooltip("Plugin power. Off bypasses the plugin but keeps the UI available.");
                bypassButton.onClick = [this, i]
                {
                    if (trackIndex >= 0 && onBypassChanged)
                        onBypassChanged(trackIndex,
                                        displayIndexToSlotIndex(i),
                                        bypassButtons[static_cast<size_t>(i)].getToggleState());
                };
                addAndMakeVisible(bypassButton);

                auto& clearButton = clearButtons[static_cast<size_t>(i)];
                clearButton.setButtonText("Clear");
                clearButton.onClick = [this, i]
                {
                    if (trackIndex >= 0 && onClearSlot)
                        onClearSlot(trackIndex, displayIndexToSlotIndex(i));
                };
                addAndMakeVisible(clearButton);

                auto& moveUpButton = moveUpButtons[static_cast<size_t>(i)];
                moveUpButton.setButtonText("Up");
                moveUpButton.onClick = [this, i]
                {
                    if (trackIndex >= 0 && onMoveSlot && i > 1)
                        onMoveSlot(trackIndex, displayIndexToSlotIndex(i), displayIndexToSlotIndex(i - 1));
                };
                addAndMakeVisible(moveUpButton);

                auto& moveDownButton = moveDownButtons[static_cast<size_t>(i)];
                moveDownButton.setButtonText("Dn");
                moveDownButton.onClick = [this, i]
                {
                    if (trackIndex >= 0 && onMoveSlot && i > 0 && i < slotCount - 1)
                        onMoveSlot(trackIndex, displayIndexToSlotIndex(i), displayIndexToSlotIndex(i + 1));
                };
                addAndMakeVisible(moveDownButton);
            }

            startTimerHz(10);
        }

        void setAvailableInputChannels(int channelCount)
        {
            const int clamped = juce::jmax(1, channelCount);
            if (clamped == availableInputChannels)
                return;

            availableInputChannels = clamped;
            rebuildInputSourceChoices();
            refreshFromTrack();
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (track == nullptr || trackIndex < 0)
                return;

            if (slotDragCandidate)
            {
                const bool performReorder = slotDragActive
                                            && juce::isPositiveAndBelow(slotDragSource, slotCount)
                                            && juce::isPositiveAndBelow(slotDragTarget, slotCount)
                                            && slotDragSource > 0
                                            && slotDragTarget > 0
                                            && slotDragTarget != slotDragSource;
                const int source = slotDragSource;
                const int target = slotDragTarget;
                slotDragCandidate = false;
                slotDragActive = false;
                slotDragSource = -1;
                slotDragTarget = -1;
                repaint();

                if (performReorder)
                {
                    if (onMoveSlot)
                        onMoveSlot(trackIndex,
                                   displayIndexToSlotIndex(source),
                                   displayIndexToSlotIndex(target));
                    return;
                }
            }

            if (!e.mods.isPopupMenu())
                return;

            for (int i = 0; i < slotCount; ++i)
            {
                if (e.eventComponent == &slotButtons[static_cast<size_t>(i)])
                {
                    showSlotContextMenu(i);
                    return;
                }
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (track == nullptr || trackIndex < 0 || e.mods.isPopupMenu())
                return;

            for (int i = 0; i < slotCount; ++i)
            {
                if (e.eventComponent == &slotButtons[static_cast<size_t>(i)])
                {
                    if (i == 0)
                        return;
                    slotDragCandidate = true;
                    slotDragActive = false;
                    slotDragSource = i;
                    slotDragTarget = i;
                    slotDragStart = e.getEventRelativeTo(this).position;
                    return;
                }
            }
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (!slotDragCandidate || track == nullptr || trackIndex < 0 || e.mods.isPopupMenu())
                return;

            const auto localPos = e.getEventRelativeTo(this).position;
            if (!slotDragActive)
            {
                if (localPos.getDistanceFrom(slotDragStart) < 4.0f)
                    return;
                slotDragActive = true;
            }

            const int target = getClosestSlotIndexForY(localPos.y);
            if (target != slotDragTarget)
            {
                slotDragTarget = target;
                repaint();
            }
        }

        void setTrack(sampledex::Track* newTrack, int newTrackIndex)
        {
            track = newTrack;
            trackIndex = newTrackIndex;
            slotDragCandidate = false;
            slotDragActive = false;
            slotDragSource = -1;
            slotDragTarget = -1;
            refreshFromTrack();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour::fromRGB(24, 28, 34));
            g.setColour(juce::Colours::white.withAlpha(0.14f));
            g.drawRect(getLocalBounds(), 1);

            if (slotDragActive && juce::isPositiveAndBelow(slotDragTarget, slotCount))
            {
                g.setColour(sampledex::theme::Colours::accent().withAlpha(0.82f));
                g.drawRoundedRectangle(slotButtons[static_cast<size_t>(slotDragTarget)].getBounds().toFloat().expanded(2.0f),
                                       4.0f,
                                       1.8f);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            auto topRow = r.removeFromTop(30);
            title.setBounds(topRow.removeFromLeft(300));
            renameButton.setBounds(topRow.removeFromLeft(72).reduced(2));
            duplicateButton.setBounds(topRow.removeFromLeft(86).reduced(2));
            deleteButton.setBounds(topRow.removeFromLeft(72).reduced(2));
            floatingRackButton.setBounds(topRow.removeFromLeft(90).reduced(2));

            r.removeFromTop(6);
            auto stateRow = r.removeFromTop(26);
            const int stateW = 48;
            muteButton.setBounds(stateRow.removeFromLeft(stateW).reduced(2));
            soloButton.setBounds(stateRow.removeFromLeft(stateW).reduced(2));
            armButton.setBounds(stateRow.removeFromLeft(stateW).reduced(2));
            monitorButton.setBounds(stateRow.removeFromLeft(stateW).reduced(2));

            r.removeFromTop(4);
            auto inputRow = r.removeFromTop(22);
            inputSourceLabel.setBounds(inputRow.removeFromLeft(62));
            monitorTapBox.setBounds(inputRow.removeFromRight(124).reduced(2, 0));
            monitorTapLabel.setBounds(inputRow.removeFromRight(72));
            inputSourceBox.setBounds(inputRow.reduced(2, 0));

            auto monitorGainRow = r.removeFromTop(22);
            monitorGainLabel.setBounds(monitorGainRow.removeFromLeft(62));
            monitorGainSlider.setBounds(monitorGainRow.reduced(2, 0));

            auto volRow = r.removeFromTop(22);
            volumeLabel.setBounds(volRow.removeFromLeft(62));
            volumeSlider.setBounds(volRow.reduced(2, 0));

            auto panRow = r.removeFromTop(22);
            panLabel.setBounds(panRow.removeFromLeft(62));
            panSlider.setBounds(panRow.reduced(2, 0));

            auto sendRow = r.removeFromTop(22);
            sendLabel.setBounds(sendRow.removeFromLeft(62));
            sendBusBox.setBounds(sendRow.removeFromRight(74).reduced(2, 0));
            sendModeBox.setBounds(sendRow.removeFromRight(88).reduced(2, 0));
            sendSlider.setBounds(sendRow.reduced(2, 0));

            r.removeFromTop(8);
            for (int i = 0; i < slotCount; ++i)
            {
                auto row = r.removeFromTop(30);
                slotButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(360).reduced(2));
                loadButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(56).reduced(2));
                bypassButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(52).reduced(2));
                clearButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(58).reduced(2));
                moveUpButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(42).reduced(2));
                moveDownButtons[static_cast<size_t>(i)].setBounds(row.removeFromLeft(42).reduced(2));
                r.removeFromTop(2);
            }
        }

    private:
        void rebuildInputSourceChoices()
        {
            const int currentPair = (track != nullptr) ? track->getInputSourcePair() : -1;
            inputSourceBox.clear(juce::dontSendNotification);
            inputSourceBox.addItem("Auto", 1);
            const int pairCount = juce::jmax(1, (availableInputChannels + 1) / 2);
            for (int pair = 0; pair < pairCount; ++pair)
            {
                const int left = pair * 2 + 1;
                const int right = juce::jmin(availableInputChannels, left + 1);
                inputSourceBox.addItem("In " + juce::String(left) + "/" + juce::String(right), pair + 2);
            }

            if (currentPair < 0)
            {
                inputSourceBox.setSelectedId(1, juce::dontSendNotification);
            }
            else
            {
                const int clampedPair = juce::jlimit(0, pairCount - 1, currentPair);
                inputSourceBox.setSelectedId(clampedPair + 2, juce::dontSendNotification);
            }
        }

        static int displayIndexToSlotIndex(int displayIndex)
        {
            return displayIndex == 0 ? ::sampledex::Track::instrumentSlotIndex : (displayIndex - 1);
        }

        static juce::String displayIndexToLabel(int displayIndex)
        {
            return displayIndex == 0 ? juce::String("Instrument")
                                     : juce::String("Insert " + juce::String(displayIndex));
        }

        void showSlotContextMenu(int slotIndex)
        {
            if (track == nullptr || trackIndex < 0 || !juce::isPositiveAndBelow(slotIndex, slotCount))
                return;

            const int mappedSlotIndex = displayIndexToSlotIndex(slotIndex);
            const bool hasPlugin = track->hasPluginInSlotNonBlocking(mappedSlotIndex);
            const bool bypassed = hasPlugin && track->isPluginSlotBypassedNonBlocking(mappedSlotIndex);
            const bool isInstrumentSlot = slotIndex == 0;
            juce::PopupMenu menu;
            menu.addItem(1, "Open Plugin UI", hasPlugin);
            menu.addItem(2, hasPlugin ? "Replace Plugin..." : "Load Plugin...");
            menu.addItem(3, "Toggle Bypass", hasPlugin, bypassed);
            menu.addItem(4, "Clear Slot", hasPlugin);
            menu.addSeparator();
            menu.addItem(5, "Move Up", !isInstrumentSlot && slotIndex > 1);
            menu.addItem(6, "Move Down", !isInstrumentSlot && slotIndex < slotCount - 1);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                               [this, slotIndex, mappedSlotIndex](int selectedId)
                               {
                                   if (track == nullptr || trackIndex < 0)
                                       return;

                                   if (selectedId == 1)
                                   {
                                       if (onOpenSlot)
                                           onOpenSlot(trackIndex, mappedSlotIndex);
                                   }
                                   else if (selectedId == 2)
                                   {
                                       if (onLoadSlot)
                                           onLoadSlot(trackIndex, mappedSlotIndex);
                                   }
                                   else if (selectedId == 3)
                                   {
                                       if (onBypassChanged)
                                           onBypassChanged(trackIndex, mappedSlotIndex, !track->isPluginSlotBypassedNonBlocking(mappedSlotIndex));
                                   }
                                   else if (selectedId == 4)
                                   {
                                       if (onClearSlot)
                                           onClearSlot(trackIndex, mappedSlotIndex);
                                   }
                                   else if (selectedId == 5)
                                   {
                                       if (onMoveSlot && slotIndex > 1)
                                           onMoveSlot(trackIndex, mappedSlotIndex, displayIndexToSlotIndex(slotIndex - 1));
                                   }
                                   else if (selectedId == 6)
                                   {
                                       if (onMoveSlot && slotIndex > 0 && slotIndex < slotCount - 1)
                                           onMoveSlot(trackIndex, mappedSlotIndex, displayIndexToSlotIndex(slotIndex + 1));
                                   }
                               });
        }

        void timerCallback() override
        {
            refreshFromTrack();
        }

        void setControlsEnabled(bool enabled)
        {
            renameButton.setEnabled(enabled);
            duplicateButton.setEnabled(enabled);
            deleteButton.setEnabled(enabled);
            floatingRackButton.setEnabled(enabled);
            muteButton.setEnabled(enabled);
            soloButton.setEnabled(enabled);
            armButton.setEnabled(enabled);
            monitorButton.setEnabled(enabled);
            inputSourceBox.setEnabled(enabled);
            monitorTapBox.setEnabled(enabled);
            monitorGainSlider.setEnabled(enabled);
            volumeSlider.setEnabled(enabled);
            panSlider.setEnabled(enabled);
            sendSlider.setEnabled(enabled);
            sendModeBox.setEnabled(enabled);
            sendBusBox.setEnabled(enabled);
            for (int i = 0; i < slotCount; ++i)
            {
                slotButtons[static_cast<size_t>(i)].setEnabled(enabled);
                loadButtons[static_cast<size_t>(i)].setEnabled(enabled);
                bypassButtons[static_cast<size_t>(i)].setEnabled(enabled);
                clearButtons[static_cast<size_t>(i)].setEnabled(enabled);
                moveUpButtons[static_cast<size_t>(i)].setEnabled(enabled);
                moveDownButtons[static_cast<size_t>(i)].setEnabled(enabled);
            }
        }

        int getClosestSlotIndexForY(float y) const
        {
            int nearest = 0;
            float nearestDistance = std::numeric_limits<float>::max();
            for (int i = 0; i < slotCount; ++i)
            {
                const float centerY = static_cast<float>(slotButtons[static_cast<size_t>(i)].getBounds().getCentreY());
                const float d = std::abs(centerY - y);
                if (d < nearestDistance)
                {
                    nearest = i;
                    nearestDistance = d;
                }
            }
            return nearest;
        }

        void refreshFromTrack()
        {
            if (track == nullptr || trackIndex < 0)
            {
                title.setText("Inspector: No Track", juce::dontSendNotification);
                setControlsEnabled(false);
                inputSourceBox.setSelectedId(0, juce::dontSendNotification);
                monitorTapBox.setSelectedId(0, juce::dontSendNotification);
                monitorGainSlider.setValue(0.0, juce::dontSendNotification);
                sendModeBox.setSelectedId(0, juce::dontSendNotification);
                sendBusBox.setSelectedId(0, juce::dontSendNotification);
                for (int i = 0; i < slotCount; ++i)
                {
                    slotButtons[static_cast<size_t>(i)].setButtonText(displayIndexToLabel(i) + ": Empty");
                    bypassButtons[static_cast<size_t>(i)].setToggleState(false, juce::dontSendNotification);
                    bypassButtons[static_cast<size_t>(i)].setButtonText("Off");
                }
                return;
            }

            setControlsEnabled(true);
            title.setText("Inspector: " + track->getTrackName(), juce::dontSendNotification);

            muteButton.setToggleState(track->isMuted(), juce::dontSendNotification);
            soloButton.setToggleState(track->isSolo(), juce::dontSendNotification);
            armButton.setToggleState(track->isArmed(), juce::dontSendNotification);
            monitorButton.setToggleState(track->isInputMonitoringEnabled(), juce::dontSendNotification);
            const int pairCount = juce::jmax(1, (availableInputChannels + 1) / 2);
            const int pair = track->getInputSourcePair();
            inputSourceBox.setSelectedId(pair < 0
                                             ? 1
                                             : (juce::jlimit(0, pairCount - 1, pair) + 2),
                                         juce::dontSendNotification);
            monitorTapBox.setSelectedId(track->getMonitorTapMode() == ::sampledex::Track::MonitorTapMode::PreInserts ? 1 : 2,
                                        juce::dontSendNotification);
            monitorGainSlider.setValue(track->getInputMonitorGain(), juce::dontSendNotification);
            volumeSlider.setValue(track->getVolume(), juce::dontSendNotification);
            panSlider.setValue(track->getPan(), juce::dontSendNotification);
            sendSlider.setValue(track->getSendLevel(), juce::dontSendNotification);
            int sendModeId = 2;
            switch (track->getSendTapMode())
            {
                case ::sampledex::Track::SendTapMode::PreFader: sendModeId = 1; break;
                case ::sampledex::Track::SendTapMode::PostPan: sendModeId = 3; break;
                case ::sampledex::Track::SendTapMode::PostFader: sendModeId = 2; break;
                default: break;
            }
            sendModeBox.setSelectedId(sendModeId, juce::dontSendNotification);
            sendBusBox.setSelectedId(track->getSendTargetBus() + 1, juce::dontSendNotification);

            for (int i = 0; i < slotCount; ++i)
            {
                const int mappedSlotIndex = displayIndexToSlotIndex(i);
                const bool hasPlugin = track->hasPluginInSlotNonBlocking(mappedSlotIndex);
                const bool bypassed = hasPlugin && track->isPluginSlotBypassedNonBlocking(mappedSlotIndex);
                const auto name = hasPlugin ? track->getPluginNameForSlotNonBlocking(mappedSlotIndex) : juce::String("Empty");
                slotButtons[static_cast<size_t>(i)].setButtonText(displayIndexToLabel(i) + ": " + name);
                slotButtons[static_cast<size_t>(i)].setColour(juce::TextButton::buttonColourId,
                                                              hasPlugin ? sampledex::theme::Colours::accent().withAlpha(0.30f)
                                                                        : juce::Colours::black.withAlpha(0.22f));
                bypassButtons[static_cast<size_t>(i)].setEnabled(hasPlugin);
                clearButtons[static_cast<size_t>(i)].setEnabled(hasPlugin);
                moveUpButtons[static_cast<size_t>(i)].setEnabled(hasPlugin && i > 1);
                moveDownButtons[static_cast<size_t>(i)].setEnabled(hasPlugin && i > 0 && i < slotCount - 1);
                bypassButtons[static_cast<size_t>(i)].setToggleState(bypassed, juce::dontSendNotification);
                bypassButtons[static_cast<size_t>(i)].setButtonText(!hasPlugin ? "Off" : (bypassed ? "Off" : "On"));
            }
        }

        sampledex::Track* track = nullptr;
        int trackIndex = -1;
        int availableInputChannels = 2;

        juce::Label title;
        juce::TextButton renameButton, duplicateButton, deleteButton, floatingRackButton;
        juce::TextButton muteButton, soloButton, armButton, monitorButton;
        juce::Label inputSourceLabel, monitorTapLabel, monitorGainLabel, volumeLabel, panLabel, sendLabel;
        juce::Slider monitorGainSlider, volumeSlider, panSlider, sendSlider;
        juce::ComboBox inputSourceBox, monitorTapBox, sendModeBox;
        juce::ComboBox sendBusBox;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> slotButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> loadButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> bypassButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> clearButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> moveUpButtons;
        std::array<juce::TextButton, static_cast<size_t>(slotCount)> moveDownButtons;
        bool slotDragCandidate = false;
        bool slotDragActive = false;
        int slotDragSource = -1;
        int slotDragTarget = -1;
        juce::Point<float> slotDragStart;
    };

    class RecordingPanelContent : public juce::Component, private juce::Timer
    {
    public:
        std::function<void()> onCalibrateInput;
        std::function<void()> onResetHolds;
        std::function<void(bool)> onMonitorSafeChanged;
        std::function<void(int)> onCountInBarsChanged;
        std::function<void(int)> onManualOffsetSamplesChanged;
        std::function<void(int)> onInputMonitoringModeChanged;
        std::function<void(bool)> onPunchEnabledChanged;
        std::function<void(double)> onPunchInChanged;
        std::function<void(double)> onPunchOutChanged;
        std::function<void(int)> onPreRollBarsChanged;
        std::function<void(int)> onPostRollBarsChanged;
        std::function<juce::String()> getLatencyText;
        std::function<int()> getCountInBars;
        std::function<int()> getManualOffsetSamples;
        std::function<int()> getInputMonitoringMode;
        std::function<bool()> getPunchEnabled;
        std::function<double()> getPunchInBeat;
        std::function<double()> getPunchOutBeat;
        std::function<int()> getPreRollBars;
        std::function<int()> getPostRollBars;
        std::function<int()> getTrackCount;
        std::function<juce::String(int)> getTrackName;
        std::function<float(int)> getTrackInputPeak;
        std::function<float(int)> getTrackInputRms;
        std::function<float(int)> getTrackInputHold;
        std::function<bool(int)> getTrackInputClip;
        std::function<bool(int)> getTrackMonitoringEnabled;
        std::function<float()> getLiveInputPeak;
        std::function<float()> getLiveInputRms;
        std::function<void(int)> onTrackRowClicked;
        std::function<void(bool)> onOverdubChanged;
        std::function<bool()> getOverdubEnabled;

        RecordingPanelContent()
        {
            title.setText("Recording Setup", juce::dontSendNotification);
            title.setJustificationType(juce::Justification::centredLeft);
            title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            title.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
            addAndMakeVisible(title);

            calibrateButton.setButtonText("Input Calibrate");
            calibrateButton.setTooltip("Auto-trim monitor gain based on live input level.");
            calibrateButton.onClick = [this]
            {
                if (onCalibrateInput)
                    onCalibrateInput();
            };
            addAndMakeVisible(calibrateButton);

            resetHoldButton.setButtonText("Reset Holds");
            resetHoldButton.setTooltip("Reset per-track input peak hold and clip latches.");
            resetHoldButton.onClick = [this]
            {
                if (onResetHolds)
                    onResetHolds();
            };
            addAndMakeVisible(resetHoldButton);

            monitorSafeToggle.setButtonText("Monitor Safe");
            monitorSafeToggle.setTooltip("Safer monitor defaults: dry monitor tap and constrained monitor gain.");
            monitorSafeToggle.onClick = [this]
            {
                if (onMonitorSafeChanged)
                    onMonitorSafeChanged(monitorSafeToggle.getToggleState());
            };
            addAndMakeVisible(monitorSafeToggle);

            countInLabel.setText("Count-In", juce::dontSendNotification);
            countInLabel.setJustificationType(juce::Justification::centredLeft);
            countInLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(countInLabel);

            countInBox.addItem("Off", 1);
            countInBox.addItem("1 Bar", 2);
            countInBox.addItem("2 Bars", 3);
            countInBox.setTooltip("Recording count-in before capture starts.");
            countInBox.onChange = [this]
            {
                if (!onCountInBarsChanged)
                    return;
                const int selectedId = countInBox.getSelectedId();
                const int bars = (selectedId <= 1) ? 0 : (selectedId - 1);
                onCountInBarsChanged(juce::jlimit(0, 2, bars));
            };
            addAndMakeVisible(countInBox);

            monitorModeLabel.setText("Monitor", juce::dontSendNotification);
            monitorModeLabel.setJustificationType(juce::Justification::centredLeft);
            monitorModeLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(monitorModeLabel);

            monitorModeBox.addItem("Arm-only", 1);
            monitorModeBox.addItem("Monitor-only", 2);
            monitorModeBox.addItem("Auto-monitor", 3);
            monitorModeBox.onChange = [this]
            {
                if (onInputMonitoringModeChanged)
                    onInputMonitoringModeChanged(juce::jmax(0, monitorModeBox.getSelectedId() - 1));
            };
            addAndMakeVisible(monitorModeBox);

            overdubToggle.setButtonText("Overdub");
            overdubToggle.setTooltip("When disabled, new takes replace overlapping clips on armed tracks.");
            overdubToggle.onClick = [this]
            {
                if (onOverdubChanged)
                    onOverdubChanged(overdubToggle.getToggleState());
            };
            addAndMakeVisible(overdubToggle);

            punchToggle.setButtonText("Punch");
            punchToggle.setTooltip("Enable punch-in / punch-out recording.");
            punchToggle.onClick = [this]
            {
                if (onPunchEnabledChanged)
                    onPunchEnabledChanged(punchToggle.getToggleState());
            };
            addAndMakeVisible(punchToggle);

            punchInLabel.setText("In", juce::dontSendNotification);
            punchInLabel.setJustificationType(juce::Justification::centredLeft);
            punchInLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(punchInLabel);
            punchInSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            punchInSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 18);
            punchInSlider.setRange(0.0, 512.0, 0.25);
            punchInSlider.setSkewFactorFromMidPoint(32.0);
            punchInSlider.onValueChange = [this]
            {
                if (onPunchInChanged)
                    onPunchInChanged(punchInSlider.getValue());
            };
            addAndMakeVisible(punchInSlider);

            punchOutLabel.setText("Out", juce::dontSendNotification);
            punchOutLabel.setJustificationType(juce::Justification::centredLeft);
            punchOutLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(punchOutLabel);
            punchOutSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            punchOutSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 18);
            punchOutSlider.setRange(0.25, 640.0, 0.25);
            punchOutSlider.setSkewFactorFromMidPoint(48.0);
            punchOutSlider.onValueChange = [this]
            {
                if (onPunchOutChanged)
                    onPunchOutChanged(punchOutSlider.getValue());
            };
            addAndMakeVisible(punchOutSlider);

            preRollLabel.setText("Pre", juce::dontSendNotification);
            preRollLabel.setJustificationType(juce::Justification::centredLeft);
            preRollLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(preRollLabel);
            preRollBox.addItem("Off", 1);
            preRollBox.addItem("1 Bar", 2);
            preRollBox.addItem("2 Bars", 3);
            preRollBox.addItem("4 Bars", 4);
            preRollBox.onChange = [this]
            {
                if (!onPreRollBarsChanged)
                    return;
                const int id = preRollBox.getSelectedId();
                const int bars = (id == 2 ? 1 : (id == 3 ? 2 : (id == 4 ? 4 : 0)));
                onPreRollBarsChanged(bars);
            };
            addAndMakeVisible(preRollBox);

            postRollLabel.setText("Post", juce::dontSendNotification);
            postRollLabel.setJustificationType(juce::Justification::centredLeft);
            postRollLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(postRollLabel);
            postRollBox.addItem("Off", 1);
            postRollBox.addItem("1 Bar", 2);
            postRollBox.addItem("2 Bars", 3);
            postRollBox.addItem("4 Bars", 4);
            postRollBox.onChange = [this]
            {
                if (!onPostRollBarsChanged)
                    return;
                const int id = postRollBox.getSelectedId();
                const int bars = (id == 2 ? 1 : (id == 3 ? 2 : (id == 4 ? 4 : 0)));
                onPostRollBarsChanged(bars);
            };
            addAndMakeVisible(postRollBox);

            offsetLabel.setText("Offset", juce::dontSendNotification);
            offsetLabel.setJustificationType(juce::Justification::centredLeft);
            offsetLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
            addAndMakeVisible(offsetLabel);
            offsetSlider.setSliderStyle(juce::Slider::LinearHorizontal);
            offsetSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 18);
            offsetSlider.setRange(-4000.0, 4000.0, 1.0);
            offsetSlider.setTextValueSuffix(" smp");
            offsetSlider.onValueChange = [this]
            {
                if (onManualOffsetSamplesChanged)
                    onManualOffsetSamplesChanged(juce::roundToInt(offsetSlider.getValue()));
            };
            addAndMakeVisible(offsetSlider);

            latencyLabel.setJustificationType(juce::Justification::centredLeft);
            latencyLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
            latencyLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
            addAndMakeVisible(latencyLabel);

            startTimerHz(14);
        }

        void setMonitorSafeState(bool enabled)
        {
            monitorSafeToggle.setToggleState(enabled, juce::dontSendNotification);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (rowsBounds.isEmpty() || !rowsBounds.contains(e.getPosition()) || !onTrackRowClicked)
                return;

            const int rowHeight = 24;
            const int row = (e.getPosition().y - rowsBounds.getY()) / rowHeight;
            if (row < 0)
                return;
            onTrackRowClicked(row);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour::fromRGB(22, 27, 34));
            g.setColour(juce::Colours::white.withAlpha(0.14f));
            g.drawRect(getLocalBounds(), 1);

            auto content = getLocalBounds().reduced(10);
            content.removeFromTop(140);

            auto liveBarArea = content.removeFromTop(24);
            drawLiveInputBar(g, liveBarArea);

            content.removeFromTop(6);
            rowsBounds = content;
            g.setColour(juce::Colour::fromRGB(17, 21, 27));
            g.fillRoundedRectangle(rowsBounds.toFloat(), 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawRoundedRectangle(rowsBounds.toFloat(), 4.0f, 1.0f);

            const int totalTracks = getTrackCount ? juce::jmax(0, getTrackCount()) : 0;
            if (totalTracks <= 0)
            {
                g.setColour(juce::Colours::white.withAlpha(0.56f));
                g.setFont(juce::Font(juce::FontOptions(13.0f)));
                g.drawFittedText("No tracks", rowsBounds, juce::Justification::centred, 1);
                return;
            }

            const int rowHeight = 24;
            const int visibleRows = juce::jmin(totalTracks, juce::jmax(1, rowsBounds.getHeight() / rowHeight));
            auto rowArea = rowsBounds;
            for (int row = 0; row < visibleRows; ++row)
            {
                auto rowRect = rowArea.removeFromTop(rowHeight);
                if (rowRect.getBottom() > rowsBounds.getBottom())
                    break;

                const bool alt = (row % 2) != 0;
                g.setColour(alt ? juce::Colours::white.withAlpha(0.02f) : juce::Colours::transparentBlack);
                g.fillRect(rowRect);

                drawTrackRow(g, rowRect.reduced(6, 3), row);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            auto headerRow = r.removeFromTop(28);
            title.setBounds(headerRow.removeFromLeft(170));
            calibrateButton.setBounds(headerRow.removeFromLeft(122).reduced(2));
            resetHoldButton.setBounds(headerRow.removeFromLeft(96).reduced(2));
            monitorSafeToggle.setBounds(headerRow.removeFromLeft(136));

            auto countInRow = r.removeFromTop(22);
            countInLabel.setBounds(countInRow.removeFromLeft(62));
            countInBox.setBounds(countInRow.removeFromLeft(100).reduced(2, 0));
            monitorModeLabel.setBounds(countInRow.removeFromLeft(58));
            monitorModeBox.setBounds(countInRow.removeFromLeft(124).reduced(2, 0));
            overdubToggle.setBounds(countInRow.removeFromLeft(92));
            punchToggle.setBounds(countInRow.removeFromLeft(70));
            preRollLabel.setBounds(countInRow.removeFromLeft(34));
            preRollBox.setBounds(countInRow.removeFromLeft(82).reduced(2, 0));
            postRollLabel.setBounds(countInRow.removeFromLeft(40));
            postRollBox.setBounds(countInRow.removeFromLeft(82).reduced(2, 0));

            auto punchInRow = r.removeFromTop(24);
            punchInLabel.setBounds(punchInRow.removeFromLeft(26));
            punchInSlider.setBounds(punchInRow.reduced(2, 0));

            auto punchOutRow = r.removeFromTop(24);
            punchOutLabel.setBounds(punchOutRow.removeFromLeft(26));
            punchOutSlider.setBounds(punchOutRow.reduced(2, 0));

            r.removeFromTop(2);
            auto offsetRow = r.removeFromTop(24);
            offsetLabel.setBounds(offsetRow.removeFromLeft(56));
            offsetSlider.setBounds(offsetRow.reduced(2, 0));
            latencyLabel.setBounds(r.removeFromTop(24));
        }

    private:
        static float levelToNorm(float gain)
        {
            const float dB = juce::Decibels::gainToDecibels(juce::jmax(0.000001f, gain), -60.0f);
            return juce::jlimit(0.0f, 1.0f, (dB + 60.0f) / 60.0f);
        }

        static juce::String gainToDbString(float gain)
        {
            const float dB = juce::Decibels::gainToDecibels(juce::jmax(0.000001f, gain), -60.0f);
            return juce::String(dB, 1) + " dB";
        }

        void drawLiveInputBar(juce::Graphics& g, juce::Rectangle<int> bounds)
        {
            if (bounds.isEmpty())
                return;

            const float livePeak = getLiveInputPeak ? getLiveInputPeak() : 0.0f;
            const float liveRms = getLiveInputRms ? getLiveInputRms() : 0.0f;
            const float peakNorm = levelToNorm(livePeak);
            const float rmsNorm = levelToNorm(liveRms);

            g.setColour(juce::Colours::white.withAlpha(0.68f));
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawFittedText("Live In", bounds.removeFromLeft(56), juce::Justification::centredLeft, 1);

            auto bar = bounds.reduced(2, 3);
            g.setColour(juce::Colour::fromRGB(16, 20, 26));
            g.fillRoundedRectangle(bar.toFloat(), 3.0f);
            g.setColour(juce::Colours::white.withAlpha(0.09f));
            g.drawRoundedRectangle(bar.toFloat(), 3.0f, 1.0f);

            auto rmsFill = bar.withWidth(juce::roundToInt(static_cast<float>(bar.getWidth()) * rmsNorm));
            g.setColour(juce::Colour::fromRGB(74, 158, 228).withAlpha(0.72f));
            g.fillRoundedRectangle(rmsFill.toFloat(), 2.5f);

            auto peakFill = bar.withWidth(juce::roundToInt(static_cast<float>(bar.getWidth()) * peakNorm));
            g.setColour(juce::Colour::fromRGB(255, 174, 72).withAlpha(0.82f));
            g.fillRoundedRectangle(peakFill.toFloat(), 2.5f);

            g.setColour(juce::Colours::white.withAlpha(0.82f));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawFittedText(gainToDbString(livePeak), bar.withTrimmedLeft(bar.getWidth() - 70), juce::Justification::centredRight, 1);
        }

        void drawTrackRow(juce::Graphics& g, juce::Rectangle<int> rowRect, int trackIndex)
        {
            const juce::String trackName = getTrackName ? getTrackName(trackIndex) : ("Track " + juce::String(trackIndex + 1));
            const float peak = getTrackInputPeak ? getTrackInputPeak(trackIndex) : 0.0f;
            const float rms = getTrackInputRms ? getTrackInputRms(trackIndex) : 0.0f;
            const float hold = getTrackInputHold ? getTrackInputHold(trackIndex) : peak;
            const bool clip = getTrackInputClip && getTrackInputClip(trackIndex);
            const bool monitoring = getTrackMonitoringEnabled && getTrackMonitoringEnabled(trackIndex);

            auto nameArea = rowRect.removeFromLeft(182);
            g.setColour(monitoring ? juce::Colour::fromRGB(82, 202, 140).withAlpha(0.9f)
                                   : juce::Colours::white.withAlpha(0.78f));
            g.setFont(juce::Font(juce::FontOptions(12.0f, monitoring ? juce::Font::bold : juce::Font::plain)));
            g.drawFittedText((monitoring ? "[MON] " : "") + trackName, nameArea, juce::Justification::centredLeft, 1);

            auto clipTag = rowRect.removeFromRight(44);
            g.setColour(clip ? juce::Colour::fromRGB(220, 64, 64)
                             : juce::Colour::fromRGB(63, 69, 79));
            g.fillRoundedRectangle(clipTag.toFloat(), 2.0f);
            g.setColour(juce::Colours::white.withAlpha(clip ? 1.0f : 0.7f));
            g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawFittedText("CLIP", clipTag, juce::Justification::centred, 1);

            auto dbArea = rowRect.removeFromRight(74);
            g.setColour(juce::Colours::white.withAlpha(0.72f));
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawFittedText(gainToDbString(peak), dbArea, juce::Justification::centredRight, 1);

            auto meterArea = rowRect.reduced(4, 2);
            g.setColour(juce::Colour::fromRGB(16, 20, 26));
            g.fillRoundedRectangle(meterArea.toFloat(), 2.4f);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawRoundedRectangle(meterArea.toFloat(), 2.4f, 0.8f);

            const float rmsNorm = levelToNorm(rms);
            const float peakNorm = levelToNorm(peak);
            const float holdNorm = levelToNorm(hold);
            auto rmsFill = meterArea.withWidth(juce::roundToInt(static_cast<float>(meterArea.getWidth()) * rmsNorm));
            auto peakFill = meterArea.withWidth(juce::roundToInt(static_cast<float>(meterArea.getWidth()) * peakNorm));
            g.setColour(juce::Colour::fromRGB(74, 158, 228).withAlpha(0.68f));
            g.fillRoundedRectangle(rmsFill.toFloat(), 1.8f);
            g.setColour(juce::Colour::fromRGB(255, 174, 72).withAlpha(0.80f));
            g.fillRoundedRectangle(peakFill.toFloat(), 1.8f);

            const int holdX = meterArea.getX() + juce::roundToInt(static_cast<float>(meterArea.getWidth()) * holdNorm);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawLine(static_cast<float>(holdX),
                       static_cast<float>(meterArea.getY()),
                       static_cast<float>(holdX),
                       static_cast<float>(meterArea.getBottom()),
                       1.2f);
        }

        void timerCallback() override
        {
            if (getLatencyText)
                latencyLabel.setText(getLatencyText(), juce::dontSendNotification);

            if (getCountInBars)
            {
                const int bars = juce::jlimit(0, 2, getCountInBars());
                const int selectedId = (bars <= 0) ? 1 : (bars + 1);
                if (countInBox.getSelectedId() != selectedId)
                    countInBox.setSelectedId(selectedId, juce::dontSendNotification);
            }

            if (getInputMonitoringMode)
                monitorModeBox.setSelectedId(juce::jmax(1, juce::jmin(3, getInputMonitoringMode() + 1)), juce::dontSendNotification);
            if (getOverdubEnabled)
                overdubToggle.setToggleState(getOverdubEnabled(), juce::dontSendNotification);
            if (getPunchEnabled)
                punchToggle.setToggleState(getPunchEnabled(), juce::dontSendNotification);
            if (getPunchInBeat)
                punchInSlider.setValue(getPunchInBeat(), juce::dontSendNotification);
            if (getPunchOutBeat)
                punchOutSlider.setValue(getPunchOutBeat(), juce::dontSendNotification);

            const auto rollIdForBars = [](int bars)
            {
                if (bars <= 0) return 1;
                if (bars == 1) return 2;
                if (bars == 2) return 3;
                return 4;
            };
            if (getPreRollBars)
                preRollBox.setSelectedId(rollIdForBars(getPreRollBars()), juce::dontSendNotification);
            if (getPostRollBars)
                postRollBox.setSelectedId(rollIdForBars(getPostRollBars()), juce::dontSendNotification);
            if (getManualOffsetSamples)
                offsetSlider.setValue(static_cast<double>(getManualOffsetSamples()), juce::dontSendNotification);
            repaint(rowsBounds);
        }

        juce::Label title;
        juce::TextButton calibrateButton;
        juce::TextButton resetHoldButton;
        juce::ToggleButton monitorSafeToggle;
        juce::Label countInLabel;
        juce::ComboBox countInBox;
        juce::Label monitorModeLabel;
        juce::ComboBox monitorModeBox;
        juce::ToggleButton overdubToggle;
        juce::ToggleButton punchToggle;
        juce::Label punchInLabel;
        juce::Slider punchInSlider;
        juce::Label punchOutLabel;
        juce::Slider punchOutSlider;
        juce::Label preRollLabel;
        juce::ComboBox preRollBox;
        juce::Label postRollLabel;
        juce::ComboBox postRollBox;
        juce::Label offsetLabel;
        juce::Slider offsetSlider;
        juce::Label latencyLabel;
        juce::Rectangle<int> rowsBounds;
    };

    class FloatingChannelRackWindow : public juce::DocumentWindow
    {
    public:
        FloatingChannelRackWindow(const juce::String& titleText,
                                  juce::Component* contentToOwn,
                                  std::function<void(FloatingChannelRackWindow*)> onCloseCallback)
            : juce::DocumentWindow(titleText,
                                   juce::Colours::black,
                                   juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
              onClose(std::move(onCloseCallback))
        {
            setUsingNativeTitleBar(true);
            setResizable(true, true);
            setResizeLimits(360, 280, 720, 860);
            setContentOwned(contentToOwn, true);
            setSize(560, 340);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
            toFront(false);
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            if (onClose)
                onClose(this);
        }

    private:
        std::function<void(FloatingChannelRackWindow*)> onClose;
    };

    class TrackEqContent : public juce::Component, private juce::Timer
    {
    public:
        TrackEqContent()
        {
            title.setJustificationType(juce::Justification::centredLeft);
            title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            addAndMakeVisible(title);

            enabledToggle.setButtonText("EQ Enabled");
            enabledToggle.onClick = [this]
            {
                if (track != nullptr)
                    track->setEqEnabled(enabledToggle.getToggleState());
            };
            enabledToggle.setTooltip("Toggle built-in 3-band EQ for this track.");
            addAndMakeVisible(enabledToggle);

            setupBand(lowLabel, lowSlider, "Low");
            setupBand(midLabel, midSlider, "Mid");
            setupBand(highLabel, highSlider, "High");

            resetButton.setButtonText("Reset");
            resetButton.onClick = [this]
            {
                if (track == nullptr)
                    return;
                track->setEqBandGains(0.0f, 0.0f, 0.0f);
                refreshFromTrack();
            };
            resetButton.setTooltip("Reset EQ band gains to 0 dB.");
            addAndMakeVisible(resetButton);

            startTimerHz(12);
            refreshFromTrack();
        }

        void setTrack(sampledex::Track* newTrack, int newTrackIndex)
        {
            track = newTrack;
            trackIndex = newTrackIndex;
            refreshFromTrack();
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(12);
            auto top = r.removeFromTop(30);
            title.setBounds(top.removeFromLeft(320));
            enabledToggle.setBounds(top.removeFromLeft(120));
            resetButton.setBounds(top.removeFromLeft(72).reduced(2));

            r.removeFromTop(8);
            auto bandRow = r.removeFromTop(80);
            auto low = bandRow.removeFromLeft(juce::jmax(120, bandRow.getWidth() / 3));
            lowLabel.setBounds(low.removeFromTop(18));
            lowSlider.setBounds(low.reduced(2, 0));

            auto mid = bandRow.removeFromLeft(juce::jmax(120, bandRow.getWidth() / 2));
            midLabel.setBounds(mid.removeFromTop(18));
            midSlider.setBounds(mid.reduced(2, 0));

            auto high = bandRow;
            highLabel.setBounds(high.removeFromTop(18));
            highSlider.setBounds(high.reduced(2, 0));
        }

    private:
        void setupBand(juce::Label& label, juce::Slider& slider, const juce::String& text)
        {
            label.setText(text, juce::dontSendNotification);
            label.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(label);

            slider.setRange(-24.0, 24.0, 0.1);
            slider.setSliderStyle(juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
            slider.setTextValueSuffix(" dB");
            slider.onValueChange = [this]
            {
                if (track == nullptr || suppressSliderCallbacks)
                    return;
                track->setEqBandGains(static_cast<float>(lowSlider.getValue()),
                                      static_cast<float>(midSlider.getValue()),
                                      static_cast<float>(highSlider.getValue()));
            };
            addAndMakeVisible(slider);
        }

        void timerCallback() override
        {
            refreshFromTrack();
        }

        void refreshFromTrack()
        {
            if (track == nullptr || trackIndex < 0)
            {
                title.setText("Track EQ", juce::dontSendNotification);
                enabledToggle.setEnabled(false);
                lowSlider.setEnabled(false);
                midSlider.setEnabled(false);
                highSlider.setEnabled(false);
                resetButton.setEnabled(false);
                return;
            }

            title.setText("Track EQ - " + track->getTrackName(), juce::dontSendNotification);

            suppressSliderCallbacks = true;
            enabledToggle.setEnabled(true);
            enabledToggle.setToggleState(track->isEqEnabled(), juce::dontSendNotification);
            lowSlider.setEnabled(true);
            midSlider.setEnabled(true);
            highSlider.setEnabled(true);
            resetButton.setEnabled(true);
            lowSlider.setValue(track->getEqLowGainDb(), juce::dontSendNotification);
            midSlider.setValue(track->getEqMidGainDb(), juce::dontSendNotification);
            highSlider.setValue(track->getEqHighGainDb(), juce::dontSendNotification);
            suppressSliderCallbacks = false;
        }

        sampledex::Track* track = nullptr;
        int trackIndex = -1;
        bool suppressSliderCallbacks = false;

        juce::Label title;
        juce::ToggleButton enabledToggle;
        juce::Label lowLabel, midLabel, highLabel;
        juce::Slider lowSlider, midSlider, highSlider;
        juce::TextButton resetButton;
    };

    class FloatingEqWindow : public juce::DocumentWindow
    {
    public:
        FloatingEqWindow(const juce::String& titleText,
                         juce::Component* contentToOwn,
                         std::function<void(FloatingEqWindow*)> onCloseCallback)
            : juce::DocumentWindow(titleText,
                                   juce::Colours::black,
                                   juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
              onClose(std::move(onCloseCallback))
        {
            setUsingNativeTitleBar(true);
            setResizable(true, true);
            setResizeLimits(420, 170, 1080, 420);
            setContentOwned(contentToOwn, true);
            setSize(620, 230);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
            toFront(false);
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            if (onClose)
                onClose(this);
        }

    private:
        std::function<void(FloatingEqWindow*)> onClose;
    };

    class ArrangementStateAction : public juce::UndoableAction
    {
    public:
        ArrangementStateAction(std::vector<sampledex::Clip>& targetIn,
                               std::vector<sampledex::Clip> beforeIn,
                               std::vector<sampledex::Clip> afterIn,
                               int beforeSelectionIn,
                               int afterSelectionIn,
                               std::function<void(int)> onChangedIn)
            : target(targetIn),
              before(std::move(beforeIn)),
              after(std::move(afterIn)),
              beforeSelection(beforeSelectionIn),
              afterSelection(afterSelectionIn),
              onChanged(std::move(onChangedIn))
        {
        }

        bool perform() override
        {
            target = after;
            if (onChanged) onChanged(afterSelection);
            return true;
        }

        bool undo() override
        {
            target = before;
            if (onChanged) onChanged(beforeSelection);
            return true;
        }

        int getSizeInUnits() override
        {
            return static_cast<int>(before.size() + after.size() + 1);
        }

    private:
        std::vector<sampledex::Clip>& target;
        std::vector<sampledex::Clip> before;
        std::vector<sampledex::Clip> after;
        int beforeSelection = -1;
        int afterSelection = -1;
        std::function<void(int)> onChanged;
    };
}

namespace sampledex
{
    static juce::String buildPrimaryShortcutMapText()
    {
        return "Shortcuts: Space play/stop, R record, Cmd/Ctrl+Z undo, Cmd/Ctrl+D duplicate, S split, Del delete.";
    }

    MainComponent::MainComponent(bool startInSafeMode)
        : safeModeStartup(startInSafeMode),
          timeline(transport, arrangement, tracks)
    {
        setLookAndFeel(&theme::ThemeManager::instance().lookAndFeel());
        setWantsKeyboardFocus(true);
        // Avoid repeated microphone permission prompts and feedback on startup.
        setAudioChannels(0, 2);
        enforceStartupOutputOnlyMode();
        deviceManager.addAudioCallback(this);
        monitorSafeModeRt.store(monitorSafeMode, std::memory_order_relaxed);
        tooltipWindow = std::make_unique<juce::TooltipWindow>(this, 800);

        for (auto& midiBuffer : trackMidiBuffers)
            midiBuffer.ensureSize(2048);
        for (auto& midiBuffer : previewMidiBuffers)
            midiBuffer.ensureSize(128);
        
        // 1. Formats (Enable WAV/MP3)
        formatManager.addDefaultFormats();
        audioFormatManager.registerBasicFormats(); 

        for (auto& lastValue : controlSurfaceLastCcValue)
            lastValue = -1;

        // 2. Saved state and plugin metadata
        appDataDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Sampledex")
            .getChildFile("ChordLab");
        appDataDir.createDirectory();
        knownPluginListFile = appDataDir.getChildFile("known_plugins.xml");
        startupSettingsFile = appDataDir.getChildFile("startup_settings.txt");
        pluginScanDeadMansPedalFile = appDataDir.getChildFile("plugin_scan_dead_mans_pedal.txt");
        pluginSessionGuardFile = appDataDir.getChildFile("plugin_session_guard.json");
        quarantinedPluginsFile = appDataDir.getChildFile("plugin_quarantine.txt");
        midiLearnMappingsFile = appDataDir.getChildFile("midi_learn_mappings.txt");
        toolbarLayoutSettingsFile = appDataDir.getChildFile("toolbar_layout.json");
        autosaveProjectFile = appDataDir.getChildFile("recovery.sampledex");
        // Startup now opens a fresh project session by default.
        autosaveProjectFile.deleteFile();
        recoveryPromptPending = false;
        loadStartupPreferences();
        saveStartupPreferences();
        if (knownPluginListFile.existsAsFile()) {
            std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(knownPluginListFile));
            if (xml) knownPluginList.recreateFromXml(*xml);
        }
        loadQuarantinedPlugins();
        handleUncleanPluginSessionRecovery();
        writePluginSessionGuard(false);
        loadMidiLearnMappings();
        if (!streamingAudioReadThread.isThreadRunning())
            streamingAudioReadThread.startThread();
        if (!audioRecordDiskThread.isThreadRunning())
            audioRecordDiskThread.startThread();

        // 3. MIDI Devices
        refreshMidiInputSelector();
        refreshMidiOutputSelector();
        refreshControlSurfaceInputSelector();
        externalMidiClockSyncEnabledRt.store(externalMidiClockSyncEnabled, std::memory_order_relaxed);
        backgroundRenderingEnabledRt.store(backgroundRenderingEnabled, std::memory_order_relaxed);
        lowLatencyMode = safeModeStartup;
        lowLatencyModeRt.store(lowLatencyMode, std::memory_order_relaxed);
        const unsigned int hardwareThreads = std::thread::hardware_concurrency();
        const int suggestedWorkers = hardwareThreads > 4
            ? static_cast<int>(hardwareThreads) - 2
            : (hardwareThreads > 2 ? 1 : 0);
        realtimeGraphScheduler.setWorkerCount(safeModeStartup ? 0 : juce::jlimit(0, 6, suggestedWorkers));

        // 4. Aux FX
        reverbParams.roomSize = 0.6f;
        reverbParams.damping = 0.5f;
        reverbParams.wetLevel = 1.0f; 
        reverbParams.dryLevel = 0.0f;
        for (auto& auxReverb : auxReverbs)
            auxReverb.setParameters(reverbParams);

        // 5. UI Layout
        lcdDisplay = std::make_unique<LcdDisplay>(transport, deviceManager);
        lcdDisplay->setStatusProvider([this]
        {
            LcdDisplay::DashboardStatus status;
            status.gridStepBeats = gridStepBeats;
            status.tripletGrid = std::abs(gridStepBeats - (1.0 / 3.0)) < 1.0e-3
                              || std::abs(gridStepBeats - (1.0 / 6.0)) < 1.0e-3;
            status.swingPercent = pianoRoll.getSwingPercent();
            status.punchEnabled = punchEnabled;
            status.recordArmed = recordButton.getToggleState();
            status.monitorSafeEnabled = monitorSafeMode;
            for (auto* track : tracks)
            {
                if (track != nullptr && track->isInputMonitoringEnabled())
                {
                    status.monitorInputActive = true;
                    break;
                }
            }
            const double deviceCpu = cpuUsageRt.load(std::memory_order_relaxed) * 100.0;
            const double callbackCpu = static_cast<double>(audioCallbackLoadRt.load(std::memory_order_relaxed)) * 100.0;
            status.cpuPercent = juce::jmax(deviceCpu, callbackCpu);
            status.guardDropCount = audioGuardDropCountRt.load(std::memory_order_relaxed)
                                  + audioXrunCountRt.load(std::memory_order_relaxed);
            status.syncSource = transport.getSyncSourceLabel();
            status.tempoMapped = tempoEvents.size() > 1;
            const double currentBeat = transport.getCurrentBeat();
            for (const auto& event : tempoEvents)
            {
                if (event.beat < currentBeat - 1.0e-6)
                    status.hasPreviousTempoEvent = true;
                else if (event.beat > currentBeat + 1.0e-6)
                {
                    status.nextTempoEventBeat = event.beat;
                    status.nextTempoEventBpm = juce::jmax(1.0, event.bpm);
                    status.hasNextTempoEvent = true;
                    break;
                }
            }
            const auto freeBytes = appDataDir.getBytesFreeOnVolume();
            status.diskSpaceLow = freeBytes > 0 && freeBytes < (250LL * 1024LL * 1024LL);
            return status;
        });
        lcdDisplay->onRequestSetTempoBpm = [this](double newTempoBpm)
        {
            setTempoBpm(newTempoBpm);
            refreshStatusText();
        };
        lcdDisplay->onRequestSetTimeSignature = [this](int numerator, int denominator)
        {
            transport.setTimeSignature(numerator, denominator);
            refreshStatusText();
        };
        lcdDisplay->onRequestJumpToBeat = [this](double beat)
        {
            transport.setPositionBeats(juce::jmax(0.0, beat));
            refreshStatusText();
        };
        lcdDisplay->onRequestJumpToSample = [this](int64_t samplePosition)
        {
            transport.setPositionSamples(juce::jmax<int64_t>(0, samplePosition));
            refreshStatusText();
        };
        lcdDisplay->onRequestJumpToPreviousTempoEvent = [this]
        {
            jumpToPreviousTempoEvent();
        };
        lcdDisplay->onRequestJumpToNextTempoEvent = [this]
        {
            jumpToNextTempoEvent();
        };
        addAndMakeVisible(lcdDisplay.get());
        addAndMakeVisible(transportBar);
        addAndMakeVisible(browserPanel);
        addAndMakeVisible(trackListView);
        addAndMakeVisible(timelineView);
        
        playButton.setButtonText("▶ Play");
        addAndMakeVisible(playButton); playButton.onClick = [this] { togglePlayback(); };
        playButton.setTooltip("Start/stop transport. Shortcut: Space. " + buildPrimaryShortcutMapText());
        stopButton.setButtonText("■ Stop");
        addAndMakeVisible(stopButton);
        stopButton.setTooltip("Stop transport and recording state.");
        stopButton.onClick = [this]
        {
            if (recordButton.getToggleState())
            {
                recordButton.setToggleState(false, juce::dontSendNotification);
                toggleRecord();
            }

            transport.stop();
            transport.setRecording(false);
            for (auto* track : tracks)
                track->stopRecording();
            recordStartPending = false;
            recordStartPendingRt.store(false, std::memory_order_relaxed);
            recordEnabledRt.store(false, std::memory_order_relaxed);
            recordButton.setToggleState(false, juce::dontSendNotification);
            if (forcedMetronomeForCountIn)
            {
                forcedMetronomeForCountIn = false;
                metronomeEnabled = metronomeStateBeforeCountIn;
                metronomeEnabledRt.store(metronomeEnabled, std::memory_order_relaxed);
                metroButton.setToggleState(metronomeEnabled, juce::dontSendNotification);
            }
            autoStopAfterBeat = -1.0;
            autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);
            stopTransportAfterAutoPunch = false;
            panicAllNotes();
            refreshStatusText();
        };
        
        recordButton.setButtonText("● Rec");
        addAndMakeVisible(recordButton);
        recordButton.setClickingTogglesState(true);
        recordButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::red);
        recordButton.setTooltip("Global record enable for armed tracks. Shortcut: R. " + buildPrimaryShortcutMapText());
        recordButton.onClick = [this] { toggleRecord(); };

        addAndMakeVisible(panicButton);
        panicButton.setTooltip("Send all-notes-off to all tracks and stop hanging notes.");
        panicButton.onClick = [this] { panicAllNotes(); };
        
        addAndMakeVisible(loopButton);
        loopButton.setClickingTogglesState(true);
        loopButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::orange);
        loopButton.setTooltip("Toggle loop playback.");
        loopButton.onClick = [this] { toggleLoop(); };

        addAndMakeVisible(metroButton);
        metroButton.setClickingTogglesState(true);
        metroButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::purple);
        metroButton.setTooltip("Toggle metronome click.");
        metroButton.onClick = [this] { toggleMetronome(); };

        addAndMakeVisible(tempoMenuButton);
        tempoMenuButton.setTooltip("Project tempo map and BPM actions.");
        tempoMenuButton.onClick = [this] { showTempoMenu(); };

        addAndMakeVisible(clipToolsButton);
        clipToolsButton.setTooltip("Selected clip tools: normalize, fades, gain. Shortcut: Cmd/Ctrl+Shift+T.");
        clipToolsButton.onClick = [this] { showClipToolsMenu(); };

        addAndMakeVisible(transportStartButton);
        transportStartButton.setTooltip("Jump transport to project start.");
        transportStartButton.onClick = [this]
        {
            transport.setPositionBeats(0.0);
            refreshStatusText();
        };

        addAndMakeVisible(transportPrevBarButton);
        transportPrevBarButton.setTooltip("Jump transport to previous bar.");
        transportPrevBarButton.onClick = [this]
        {
            const auto info = transport.getCurrentPositionInfo();
            const double beatsPerBar = juce::jmax(1.0,
                                                  static_cast<double>(info.timeSigNumerator)
                                                      * (4.0 / static_cast<double>(juce::jmax(1, info.timeSigDenominator))));
            const double currentBeat = transport.getCurrentBeat();
            const double previousBarBeat = juce::jmax(0.0, std::floor((currentBeat - 1.0e-6) / beatsPerBar) * beatsPerBar);
            transport.setPositionBeats(previousBarBeat);
            refreshStatusText();
        };

        addAndMakeVisible(transportNextBarButton);
        transportNextBarButton.setTooltip("Jump transport to next bar.");
        transportNextBarButton.onClick = [this]
        {
            const auto info = transport.getCurrentPositionInfo();
            const double beatsPerBar = juce::jmax(1.0,
                                                  static_cast<double>(info.timeSigNumerator)
                                                      * (4.0 / static_cast<double>(juce::jmax(1, info.timeSigDenominator))));
            const double currentBeat = transport.getCurrentBeat();
            const double nextBarBeat = juce::jmax(0.0, (std::floor(currentBeat / beatsPerBar) + 1.0) * beatsPerBar);
            transport.setPositionBeats(nextBarBeat);
            refreshStatusText();
        };

        followPlayheadButton.setClickingTogglesState(true);
        followPlayheadButton.setToggleState(timeline.isAutoFollowPlayheadEnabled(), juce::dontSendNotification);
        followPlayheadButton.setTooltip("Auto-follow playhead in timeline.");
        followPlayheadButton.onClick = [this]
        {
            timeline.setAutoFollowPlayhead(followPlayheadButton.getToggleState());
            refreshStatusText();
        };
        addAndMakeVisible(followPlayheadButton);

        undoButton.setButtonText("↶ Undo");
        addAndMakeVisible(undoButton);
        undoButton.setTooltip("Undo last edit (Cmd/Ctrl+Z). " + buildPrimaryShortcutMapText());
        redoButton.setButtonText("↷ Redo");
        addAndMakeVisible(redoButton);
        redoButton.setTooltip("Redo edit (Cmd+Shift+Z).");
        undoButton.onClick = [this] { undoManager.undo(); rebuildRealtimeSnapshot(); };
        redoButton.onClick = [this] { undoManager.redo(); rebuildRealtimeSnapshot(); };

        addAndMakeVisible(timelineZoomOutButton);
        timelineZoomOutButton.onClick = [this]
        {
            timeline.zoomHorizontalBy(0.9f);
            timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
        };
        timelineZoomOutButton.setTooltip("Zoom timeline out.");
        addAndMakeVisible(timelineZoomInButton);
        timelineZoomInButton.onClick = [this]
        {
            timeline.zoomHorizontalBy(1.1f);
            timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
        };
        timelineZoomInButton.setTooltip("Zoom timeline in.");
        addAndMakeVisible(trackZoomOutButton);
        trackZoomOutButton.onClick = [this]
        {
            timeline.zoomTrackHeightBy(-8.0f);
            trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
        };
        trackZoomOutButton.setTooltip("Reduce track row height.");
        addAndMakeVisible(trackZoomInButton);
        trackZoomInButton.onClick = [this]
        {
            timeline.zoomTrackHeightBy(8.0f);
            trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
        };
        trackZoomInButton.setTooltip("Increase track row height.");

        addAndMakeVisible(resetZoomButton);
        resetZoomButton.setTooltip("Reset timeline zoom and lane height.");
        resetZoomButton.onClick = [this]
        {
            timeline.setPixelsPerBeat(80.0f);
            timeline.setTrackHeight(124.0f);
            timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
            trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
        };

        timelineZoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        timelineZoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        timelineZoomSlider.setRange(30.0, 420.0, 1.0);
        timelineZoomSlider.setSkewFactorFromMidPoint(96.0);
        timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
        timelineZoomSlider.setTooltip("Continuous timeline zoom.");
        timelineZoomSlider.onValueChange = [this]
        {
            timeline.setPixelsPerBeat(static_cast<float>(timelineZoomSlider.getValue()));
        };
        addAndMakeVisible(timelineZoomSlider);

        trackZoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        trackZoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        trackZoomSlider.setRange(84.0, 280.0, 1.0);
        trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
        trackZoomSlider.setTooltip("Continuous timeline track-height zoom.");
        trackZoomSlider.onValueChange = [this]
        {
            timeline.setTrackHeight(static_cast<float>(trackZoomSlider.getValue()));
        };
        addAndMakeVisible(trackZoomSlider);

        gridSelector.addItem("Grid 1/4", 1);
        gridSelector.addItem("Grid 1/8", 2);
        gridSelector.addItem("Grid 1/16", 3);
        gridSelector.addItem("Grid 1/32", 4);
        gridSelector.addItem("Grid 1/8T", 5);
        gridSelector.addItem("Grid 1/16T", 6);
        gridSelector.setSelectedId(selectorIdFromGridStep(gridStepBeats), juce::dontSendNotification);
        gridSelector.onChange = [this] { applyGridStep(gridStepFromSelectorId(gridSelector.getSelectedId())); };
        gridSelector.setTooltip("Global grid resolution for timeline and editors.");
        addAndMakeVisible(gridSelector);

        sampleRateSelector.addItem("44.1k", 1);
        sampleRateSelector.addItem("48k", 2);
        sampleRateSelector.addItem("88.2k", 3);
        sampleRateSelector.addItem("96k", 4);
        sampleRateSelector.addItem("176.4k", 5);
        sampleRateSelector.addItem("192k", 6);
        sampleRateSelector.onChange = [this]
        {
            applySampleRateSelection(sampleRateSelector.getSelectedId());
        };
        sampleRateSelector.setTooltip("Engine sample rate quick-select.");
        addAndMakeVisible(sampleRateSelector);

        bufferSizeSelector.addItem("32", 1);
        bufferSizeSelector.addItem("64", 2);
        bufferSizeSelector.addItem("128", 3);
        bufferSizeSelector.addItem("256", 4);
        bufferSizeSelector.addItem("512", 5);
        bufferSizeSelector.addItem("1024", 6);
        bufferSizeSelector.addItem("2048", 7);
        bufferSizeSelector.onChange = [this]
        {
            applyBufferSizeSelection(bufferSizeSelector.getSelectedId());
        };
        bufferSizeSelector.setTooltip("Audio device buffer size quick-select.");
        addAndMakeVisible(bufferSizeSelector);

        lowLatencyToggle.setClickingTogglesState(true);
        lowLatencyToggle.setToggleState(lowLatencyMode, juce::dontSendNotification);
        lowLatencyToggle.onClick = [this]
        {
            applyLowLatencyMode(lowLatencyToggle.getToggleState());
        };
        lowLatencyToggle.setTooltip("Low-latency mode: prioritizes monitoring stability and faster callbacks.");
        addAndMakeVisible(lowLatencyToggle);

        keySelector.addItemList(keyNames, 1);
        keySelector.setSelectedId(projectKeyRoot + 1, juce::dontSendNotification);
        keySelector.onChange = [this]
        {
            projectKeyRoot = juce::jlimit(0, 11, keySelector.getSelectedId() - 1);
            applyProjectScaleToEngines();
            refreshStatusText();
        };
        keySelector.setTooltip("Project key root for chord engine and piano-roll highlighting.");
        addAndMakeVisible(keySelector);

        scaleSelector.addItemList(scaleNames, 1);
        scaleSelector.setSelectedId(projectScaleMode + 1, juce::dontSendNotification);
        scaleSelector.onChange = [this]
        {
            projectScaleMode = juce::jlimit(0, scaleNames.size() - 1, scaleSelector.getSelectedId() - 1);
            applyProjectScaleToEngines();
            refreshStatusText();
        };
        scaleSelector.setTooltip("Project scale mode for chord generation/highlighting.");
        addAndMakeVisible(scaleSelector);

        transposeSelector.addItem("-24", 1);
        for (int semitone = -23; semitone <= 24; ++semitone)
            transposeSelector.addItem(juce::String(semitone > 0 ? "+" : "") + juce::String(semitone), semitone + 25);
        transposeSelector.setSelectedId(25, juce::dontSendNotification);
        transposeSelector.onChange = [this]
        {
            const int semitones = transposeSelector.getSelectedId() - 25;
            applyGlobalTransposeToSelection(semitones);
        };
        transposeSelector.setTooltip("Global non-destructive MIDI transpose (playback and selected-clip edit option).");
        addAndMakeVisible(transposeSelector);

        midiInputSelector.onChange = [this]
        {
            applyMidiInputSelection(midiInputSelector.getSelectedId());
        };
        midiInputSelector.setTooltip("Select one active MIDI input device.");
        addAndMakeVisible(midiInputSelector);

        midiOutputSelector.onChange = [this]
        {
            applyMidiOutputSelection(midiOutputSelector.getSelectedId());
        };
        midiOutputSelector.setTooltip("Select MIDI output for external devices and MIDI thru.");
        addAndMakeVisible(midiOutputSelector);

        controlSurfaceInputSelector.onChange = [this]
        {
            applyControlSurfaceInputSelection(controlSurfaceInputSelector.getSelectedId());
        };
        controlSurfaceInputSelector.setTooltip("Dedicated control-surface MIDI input (faders/transport).");
        addAndMakeVisible(controlSurfaceInputSelector);

        midiLearnTargetSelector.addItem("Learn: Track Volume", 1);
        midiLearnTargetSelector.addItem("Learn: Track Pan", 2);
        midiLearnTargetSelector.addItem("Learn: Track Send", 3);
        midiLearnTargetSelector.addItem("Learn: Track Mute Toggle", 4);
        midiLearnTargetSelector.addItem("Learn: Track Solo Toggle", 5);
        midiLearnTargetSelector.addItem("Learn: Track Arm Toggle", 6);
        midiLearnTargetSelector.addItem("Learn: Track Monitor Toggle", 7);
        midiLearnTargetSelector.addItem("Learn: Master Output", 8);
        midiLearnTargetSelector.addItem("Learn: Transport Play", 9);
        midiLearnTargetSelector.addItem("Learn: Transport Stop", 10);
        midiLearnTargetSelector.addItem("Learn: Transport Record", 11);
        midiLearnTargetSelector.setSelectedId(1, juce::dontSendNotification);
        midiLearnTargetSelector.onChange = [this]
        {
            pendingMidiLearnTarget = midiLearnTargetFromId(midiLearnTargetSelector.getSelectedId());
            if (midiLearnArmed)
                pendingMidiLearnTrackIndex = selectedTrackIndex;
        };
        midiLearnTargetSelector.setTooltip("Parameter target for the next MIDI Learn assignment.");
        addAndMakeVisible(midiLearnTargetSelector);

        midiLearnArmToggle.setClickingTogglesState(true);
        midiLearnArmToggle.setTooltip("Arm MIDI Learn. Move a hardware knob/fader to bind it to the selected target.");
        midiLearnArmToggle.onClick = [this]
        {
            if (midiLearnArmToggle.getToggleState())
                armMidiLearnForSelectedTarget();
            else
                clearMidiLearnArm();
            refreshStatusText();
        };
        addAndMakeVisible(midiLearnArmToggle);

        midiThruToggle.setClickingTogglesState(true);
        midiThruToggle.setToggleState(false, juce::dontSendNotification);
        midiThruEnabledRt.store(false, std::memory_order_relaxed);
        midiThruToggle.onClick = [this]
        {
            const bool enabled = midiThruToggle.getToggleState();
            midiThruEnabledRt.store(enabled, std::memory_order_relaxed);
            refreshStatusText();
        };
        midiThruToggle.setTooltip("Route incoming MIDI notes/CC to the selected MIDI Out destination.");
        addAndMakeVisible(midiThruToggle);

        addAndMakeVisible(scanButton); scanButton.onClick = [this] { scanForPlugins(); };
        scanButton.setTooltip("Scan AU/VST3 plugins.");
        addAndMakeVisible(addTrackButton); addTrackButton.onClick = [this] { showAddTrackMenu(); };
        addTrackButton.setTooltip("Create track from template.");
        addAndMakeVisible(showEditorButton); showEditorButton.onClick = [this] { showPluginListMenu(selectedTrackIndex, &showEditorButton); };
        showEditorButton.setTooltip("Load plugin / open current plugin UI for selected track.");
        addAndMakeVisible(freezeButton);
        freezeButton.onClick = [this]
        {
            if (!juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
                return;

            juce::PopupMenu menu;
            const bool hasFrozenTrack = tracks[selectedTrackIndex]->isFrozenPlaybackOnly();
            menu.addItem(1, hasFrozenTrack ? "Update Freeze Render" : "Freeze Track");
            menu.addItem(2, "Unfreeze Track", hasFrozenTrack);
            menu.addItem(3, "Commit Track To Audio");
            menu.addItem(4, "Cancel Active Render", backgroundRenderBusyRt.load(std::memory_order_relaxed));
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&freezeButton),
                               [this](int selectedId)
                               {
                                   if (!juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
                                       return;

                                   if (selectedId == 1)
                                       freezeTrackToAudio(selectedTrackIndex);
                                   else if (selectedId == 2)
                                       unfreezeTrack(selectedTrackIndex);
                                   else if (selectedId == 3)
                                       commitTrackToAudio(selectedTrackIndex);
                                   else if (selectedId == 4)
                                       cancelActiveRenderTask();
                               });
        };
        freezeButton.setTooltip("Freeze/unfreeze/commit selected track.");

        externalClockToggle.setToggleState(externalMidiClockSyncEnabled, juce::dontSendNotification);
        externalClockToggle.onClick = [this]
        {
            externalMidiClockSyncEnabled = externalClockToggle.getToggleState();
            externalMidiClockSyncEnabledRt.store(externalMidiClockSyncEnabled, std::memory_order_relaxed);
            if (!externalMidiClockSyncEnabled)
            {
                externalMidiClockActiveRt.store(false, std::memory_order_relaxed);
                externalMidiClockTransportRunningRt.store(false, std::memory_order_relaxed);
                externalMidiClockTickCounterRt.store(0, std::memory_order_relaxed);
                externalMidiClockBeatRt.store(0.0, std::memory_order_relaxed);
                externalMidiClockBeatOffsetRt.store(0.0, std::memory_order_relaxed);
                externalMidiClockLastTickMsRt.store(-1.0, std::memory_order_relaxed);
                externalMidiClockWasRunning = false;
                lastAppliedExternalClockGeneration = -1;
            }
            else
            {
                externalMidiClockGenerationRt.fetch_add(1, std::memory_order_relaxed);
            }
            refreshStatusText();
        };
        externalClockToggle.setTooltip("When enabled, external MIDI clock can drive transport position and tempo.");
        addAndMakeVisible(externalClockToggle);

        backgroundRenderToggle.setToggleState(backgroundRenderingEnabled, juce::dontSendNotification);
        backgroundRenderToggle.onClick = [this]
        {
            backgroundRenderingEnabled = backgroundRenderToggle.getToggleState();
            backgroundRenderingEnabledRt.store(backgroundRenderingEnabled, std::memory_order_relaxed);
            refreshStatusText();
        };
        backgroundRenderToggle.setTooltip("Run freeze/commit renders in a background worker.");
        addAndMakeVisible(backgroundRenderToggle);

        addAndMakeVisible(rackButton);
        rackButton.onClick = [this]
        {
            bottomTabs.setCurrentTabIndex(3);
            refreshChannelRackWindow();
        };
        rackButton.setTooltip("Open Channel Rack tab for selected track.");
        addAndMakeVisible(inspectorButton);
        inspectorButton.onClick = [this]
        {
            bottomTabs.setCurrentTabIndex(4);
            refreshChannelRackWindow();
        };
        inspectorButton.setTooltip("Open Inspector tab for selected track.");
        addAndMakeVisible(recordSetupButton);
        recordSetupButton.onClick = [this]
        {
            if (bottomTabs.getNumTabs() > 0)
                bottomTabs.setCurrentTabIndex(bottomTabs.getNumTabs() - 1);
        };
        recordSetupButton.setTooltip("Open recording setup panel (input calibration, latency, monitor safety).");

        addAndMakeVisible(projectButton);
        projectButton.onClick = [this] { showProjectSettingsMenu(); };
        projectButton.setTooltip("Project settings and workflow shortcuts. Cmd/Ctrl+L toggles dark/light theme.");

        addAndMakeVisible(toolbarButton);
        toolbarButton.onClick = [this] { showToolbarConfigMenu(); };
        toolbarButton.setTooltip("Toolbar profiles and section visibility.");

        toolbarMoreButton.onClick = [this]
        {
            if (toolbarOverflowItems.empty())
                return;

            juce::PopupMenu menu;
            for (size_t i = 0; i < toolbarOverflowItems.size(); ++i)
                menu.addItem(static_cast<int>(i) + 1, toolbarOverflowItems[i].label);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&toolbarMoreButton),
                               [this](int selectedId)
                               {
                                   if (selectedId <= 0 || selectedId > static_cast<int>(toolbarOverflowItems.size()))
                                       return;
                                   const auto& item = toolbarOverflowItems[static_cast<size_t>(selectedId - 1)];
                                   if (item.invoke)
                                       item.invoke();
                               });
        };
        toolbarMoreButton.setTooltip("Overflow controls hidden due to limited width.");
        addAndMakeVisible(toolbarMoreButton);
        toolbarMoreButton.setVisible(false);

        addAndMakeVisible(settingsButton); settingsButton.onClick = [this] { showAudioSettings(); };
        settingsButton.setTooltip("Audio/MIDI device setup.");
        addAndMakeVisible(helpButton); helpButton.onClick = [this] { showHelpGuide(); };
        helpButton.setTooltip("Open keyboard + editing help.");
        addAndMakeVisible(exportButton); exportButton.onClick = [this] { showExportMenu(); };
        exportButton.setTooltip("Export mixdown or stems.");
        addAndMakeVisible(saveButton); saveButton.onClick = [this] { saveProject(); };
        saveButton.setTooltip("Save current project to the current project file.");

        masterOutLabel.setText("Master", juce::dontSendNotification);
        masterOutLabel.setJustificationType(juce::Justification::centredLeft);
        masterOutLabel.setTooltip("Main output gain.");
        addAndMakeVisible(masterOutLabel);

        masterOutSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        masterOutSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        masterOutSlider.setRange(0.0, 1.4, 0.01);
        masterOutSlider.setValue(masterOutputGain, juce::dontSendNotification);
        masterOutSlider.setTooltip("Main output gain before soft clip / limiter.");
        masterOutSlider.onValueChange = [this]
        {
            masterOutputGain = static_cast<float>(masterOutSlider.getValue());
            masterOutputGainRt.store(masterOutputGain, std::memory_order_relaxed);
            refreshStatusText();
        };
        masterOutSlider.onDragStart = [this]
        {
            masterAutomationTouchRt.store(true, std::memory_order_relaxed);
        };
        masterOutSlider.onDragEnd = [this]
        {
            masterAutomationTouchRt.store(false, std::memory_order_relaxed);
        };
        addAndMakeVisible(masterOutSlider);

        masterMeterWidget = std::make_unique<MonitoringAnalyzerWidget>(masterPeakMeterRt,
                                                                       masterRmsMeterRt,
                                                                       masterClipHoldRt,
                                                                       masterPhaseCorrelationRt,
                                                                       masterLoudnessLufsRt,
                                                                       masterAnalyzerSnapshots,
                                                                       masterAnalyzerReadySnapshot,
                                                                       sampleRateRt);
        addAndMakeVisible(masterMeterWidget.get());

        softClipButton.setToggleState(masterSoftClipEnabled, juce::dontSendNotification);
        softClipButton.onClick = [this]
        {
            masterSoftClipEnabled = softClipButton.getToggleState();
            masterSoftClipEnabledRt.store(masterSoftClipEnabled, std::memory_order_relaxed);
            refreshStatusText();
        };
        softClipButton.setTooltip("Enable smooth output saturation.");
        addAndMakeVisible(softClipButton);

        limiterButton.setToggleState(masterLimiterEnabled, juce::dontSendNotification);
        limiterButton.onClick = [this]
        {
            masterLimiterEnabled = limiterButton.getToggleState();
            masterLimiterEnabledRt.store(masterLimiterEnabled, std::memory_order_relaxed);
            refreshStatusText();
        };
        limiterButton.setTooltip("Enable output safety limiter to prevent harsh clipping.");
        addAndMakeVisible(limiterButton);

        auxEnableButton.setToggleState(auxFxEnabled, juce::dontSendNotification);
        auxEnableButton.onClick = [this]
        {
            auxFxEnabled = auxEnableButton.getToggleState();
            auxFxEnabledRt.store(auxFxEnabled, std::memory_order_relaxed);
            mixer.setAuxEnabled(auxFxEnabled);
            refreshStatusText();
        };
        auxEnableButton.setTooltip("Enable/disable aux FX return.");
        addAndMakeVisible(auxEnableButton);

        auxReturnLabel.setText("Aux Return", juce::dontSendNotification);
        auxReturnLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(auxReturnLabel);

        auxReturnSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        auxReturnSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        auxReturnSlider.setRange(0.0, 1.0, 0.01);
        auxReturnSlider.setChangeNotificationOnlyOnRelease(true);
        auxReturnSlider.setValue(auxReturnGain, juce::dontSendNotification);
        auxReturnSlider.onValueChange = [this]
        {
            auxReturnGain = static_cast<float>(auxReturnSlider.getValue());
            auxReturnGainRt.store(auxReturnGain, std::memory_order_relaxed);
            refreshStatusText();
        };
        auxReturnSlider.setTooltip("Aux return gain.");
        addAndMakeVisible(auxReturnSlider);

        refreshAudioEngineSelectors();

        pluginScanStatusBar.setTooltip("Plugin scan progress/status.");
        pluginScanStatusBar.setPercentageDisplay(false);
        pluginScanStatusBar.setVisible(false);
        addAndMakeVisible(pluginScanStatusBar);
        pluginScanStatusLabel.setJustificationType(juce::Justification::centredLeft);
        pluginScanStatusLabel.setInterceptsMouseClicks(false, false);
        pluginScanStatusLabel.setVisible(false);
        pluginScanStatusLabel.setTooltip("Current plugin scan pass status.");
        addAndMakeVisible(pluginScanStatusLabel);

        statusLabel.setJustificationType(juce::Justification::centredRight);
        statusLabel.setInterceptsMouseClicks(false, false);
        statusLabel.setTooltip("Current selection and quick shortcuts. " + buildPrimaryShortcutMapText());
        addAndMakeVisible(statusLabel);

        timeline.setBufferedToImage(true);
        bottomTabs.setBufferedToImage(true);
        addAndMakeVisible(timeline);

        timeline.onClipSelected = [this](Clip* c)
        {
            setSelectedClipIndex(findClipIndex(c), true);
        };
        timeline.onTrackSelected = [this](int idx)
        {
            setSelectedTrackIndex(idx);
            mixer.selectTrack(idx);
        };
        timeline.onTrackStateChanged = [this](int)
        {
            sanitizeRoutingConfiguration(false);
            rebuildRealtimeSnapshot();
            refreshStatusText();
        };
        timeline.onRenameTrack = [this](int trackIndex)
        {
            renameTrack(trackIndex);
        };
        timeline.onDuplicateTrack = [this](int trackIndex)
        {
            duplicateTrack(trackIndex);
        };
        timeline.onDeleteTrack = [this](int trackIndex)
        {
            deleteTrack(trackIndex);
        };
        timeline.onMoveTrackUp = [this](int trackIndex)
        {
            if (trackIndex > 0)
                reorderTracks(trackIndex, trackIndex - 1);
        };
        timeline.onMoveTrackDown = [this](int trackIndex)
        {
            if (juce::isPositiveAndBelow(trackIndex, tracks.size() - 1))
                reorderTracks(trackIndex, trackIndex + 1);
        };
        timeline.onOpenChannelRack = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            bottomTabs.setCurrentTabIndex(3);
            refreshChannelRackWindow();
        };
        timeline.onOpenInspector = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            bottomTabs.setCurrentTabIndex(4);
            refreshChannelRackWindow();
        };
        timeline.onOpenTrackEq = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            openEqWindowForTrack(trackIndex);
        };
        mixer.onTrackSelected = [this](int idx)
        {
            setSelectedTrackIndex(idx);
            timeline.selectTrack(idx);
        };
        mixer.onTrackStateChanged = [this](int)
        {
            sanitizeRoutingConfiguration(false);
            rebuildRealtimeSnapshot();
            refreshStatusText();
        };
        mixer.onTrackAutomationTouch = [this](int trackIndex, AutomationTarget target, bool isTouching)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;

            int targetIndex = 0;
            switch (target)
            {
                case AutomationTarget::TrackPan: targetIndex = 1; break;
                case AutomationTarget::TrackSend: targetIndex = 2; break;
                case AutomationTarget::MasterOutput:
                case AutomationTarget::TrackVolume:
                default: targetIndex = 0; break;
            }

            automationTouchStateRt[static_cast<size_t>(trackIndex)][static_cast<size_t>(targetIndex)]
                .store(isTouching, std::memory_order_relaxed);
            if (isTouching)
            {
                automationLatchStateRt[static_cast<size_t>(trackIndex)][static_cast<size_t>(targetIndex)]
                    .store(true, std::memory_order_relaxed);
            }
        };
        mixer.onTrackSetAutomationMode = [this](int trackIndex, AutomationTarget target, AutomationMode mode)
        {
            setAutomationModeForTrackTarget(trackIndex, target, mode);
        };
        mixer.getTrackAutomationMode = [this](int trackIndex, AutomationTarget target)
        {
            return getAutomationModeForTrackTarget(trackIndex, target);
        };
        mixer.onTrackRenameRequested = [this](int trackIndex)
        {
            renameTrack(trackIndex);
        };
        mixer.onTrackDuplicateRequested = [this](int trackIndex)
        {
            duplicateTrack(trackIndex);
        };
        mixer.onTrackDeleteRequested = [this](int trackIndex)
        {
            deleteTrack(trackIndex);
        };
        mixer.onTrackMoveUpRequested = [this](int trackIndex)
        {
            if (trackIndex > 0)
                reorderTracks(trackIndex, trackIndex - 1);
        };
        mixer.onTrackMoveDownRequested = [this](int trackIndex)
        {
            if (juce::isPositiveAndBelow(trackIndex, tracks.size() - 1))
                reorderTracks(trackIndex, trackIndex + 1);
        };
        mixer.onTrackOpenChannelRackRequested = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            bottomTabs.setCurrentTabIndex(3);
            refreshChannelRackWindow();
        };
        mixer.onTrackOpenInspectorRequested = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            bottomTabs.setCurrentTabIndex(4);
            refreshChannelRackWindow();
        };
        mixer.onTrackOpenEqRequested = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            openEqWindowForTrack(trackIndex);
        };
        mixer.onAuxClicked = [this](int)
        {
            const bool nextState = !auxEnableButton.getToggleState();
            auxEnableButton.setToggleState(nextState, juce::dontSendNotification);
            auxFxEnabled = nextState;
            auxFxEnabledRt.store(auxFxEnabled, std::memory_order_relaxed);
            mixer.setAuxEnabled(auxFxEnabled);
            refreshStatusText();
        };
        mixer.onAuxContextMenuRequested = [this](juce::Component* target, int busIndex)
        {
            juce::PopupMenu menu;
            menu.addSectionHeader("Aux Bus " + juce::String(busIndex + 1));
            menu.addItem(1, "Enable Aux FX", true, auxEnableButton.getToggleState());
            menu.addItem(2, "Aux Return 0%");
            menu.addItem(3, "Aux Return 25%");
            menu.addItem(4, "Aux Return 50%");
            menu.addItem(5, "Aux Return 75%");
            menu.addItem(6, "Aux Return 100%");
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),
                               [this](int selectedId)
                               {
                                   if (selectedId == 1)
                                   {
                                       const bool nextState = !auxEnableButton.getToggleState();
                                       auxEnableButton.setToggleState(nextState, juce::dontSendNotification);
                                       auxFxEnabled = nextState;
                                       auxFxEnabledRt.store(auxFxEnabled, std::memory_order_relaxed);
                                       mixer.setAuxEnabled(auxFxEnabled);
                                       refreshStatusText();
                                   }
                                   else if (selectedId >= 2 && selectedId <= 6)
                                   {
                                       static constexpr float presetValues[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
                                       const float newGain = presetValues[selectedId - 2];
                                       auxReturnSlider.setValue(newGain, juce::dontSendNotification);
                                       auxReturnGain = newGain;
                                       auxReturnGainRt.store(auxReturnGain, std::memory_order_relaxed);
                                       refreshStatusText();
                                   }
                               });
        };
        timeline.onLoadPluginForTrack = [this](int trackIndex, juce::Component* target, int slotIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            showPluginListMenu(trackIndex, target, slotIndex);
        };
        timeline.onOpenPluginEditorForTrack = [this](int trackIndex, int slotIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            openPluginEditorWindowForTrack(trackIndex, slotIndex);
        };
        mixer.onTrackPluginMenuRequested = [this](int trackIndex, juce::Component* target, int slotIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            showPluginListMenu(trackIndex, target, slotIndex);
        };
        mixer.onTrackPluginEditorRequested = [this](int trackIndex, int slotIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            openPluginEditorWindowForTrack(trackIndex, slotIndex);
        };
        timeline.onCreateMidiTrack = [this](double startBeat)
        {
            createNewTrack();
            if (tracks.isEmpty())
                return;

            const int newTrackIndex = tracks.size() - 1;
            setSelectedTrackIndex(newTrackIndex);
            timeline.selectTrack(newTrackIndex);
            mixer.selectTrack(newTrackIndex);
            createMidiClipAt(newTrackIndex, startBeat, 4.0, true);
        };
        timeline.onCreateMidiClip = [this](int trackIndex, double startBeat, double lengthBeats)
        {
            if (trackIndex < 0 || trackIndex >= tracks.size())
                return;

            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
            createMidiClipAt(trackIndex, startBeat, lengthBeats, true);
        };
        timeline.onDeleteClip = [this](int clipIndex)
        {
            applyArrangementEdit("Delete Clip",
                                 [clipIndex](std::vector<Clip>& state, int& selected)
                                 {
                                     if (clipIndex < 0 || clipIndex >= static_cast<int>(state.size()))
                                         return;

                                     state.erase(state.begin() + clipIndex);
                                     if (selected == clipIndex)
                                         selected = -1;
                                     else if (selected > clipIndex)
                                         --selected;
                                 });
        };
        timeline.onSplitClipAtBeat = [this](int clipIndex, double beat)
        {
            applyArrangementEdit("Split Clip",
                                 [clipIndex, beat](std::vector<Clip>& state, int& selected)
                                 {
                                     if (clipIndex < 0 || clipIndex >= static_cast<int>(state.size()))
                                         return;

                                     auto& left = state[static_cast<size_t>(clipIndex)];
                                     Clip right;
                                     if (!ArrangementEditing::splitClipAtBeat(left, right, beat))
                                         return;

                                     state.insert(state.begin() + clipIndex + 1, std::move(right));
                                     if (selected > clipIndex)
                                         ++selected;
                                 });
        };
        timeline.onDuplicateClip = [this](int clipIndex)
        {
            applyArrangementEdit("Duplicate Clip",
                                 [clipIndex](std::vector<Clip>& state, int& selected)
                                 {
                                     if (clipIndex < 0 || clipIndex >= static_cast<int>(state.size()))
                                         return;

                                     Clip duplicate = state[static_cast<size_t>(clipIndex)];
                                     duplicate.startBeat = state[static_cast<size_t>(clipIndex)].startBeat
                                                         + state[static_cast<size_t>(clipIndex)].lengthBeats;
                                     duplicate.name = state[static_cast<size_t>(clipIndex)].name + " Copy";
                                     state.insert(state.begin() + clipIndex + 1, duplicate);
                                     selected = clipIndex + 1;
                                 });
        };
        timeline.onNudgeClipBy = [this](int clipIndex, double deltaBeats)
        {
            applyArrangementEdit("Nudge Clip",
                                 [clipIndex, deltaBeats](std::vector<Clip>& state, int& selected)
                                 {
                                     juce::ignoreUnused(selected);
                                     if (clipIndex < 0 || clipIndex >= static_cast<int>(state.size()))
                                         return;
                                     auto& clip = state[static_cast<size_t>(clipIndex)];
                                     clip.startBeat = juce::jmax(0.0, clip.startBeat + deltaBeats);
                                 });
        };
        timeline.onMoveClip = [this](int clipIndex, int targetTrackIndex, double targetStartBeat, bool duplicate)
        {
            const int maxTrack = juce::jmax(0, tracks.size() - 1);
            applyArrangementEdit(duplicate ? "Duplicate Clip" : "Move Clip",
                                 [clipIndex, targetTrackIndex, targetStartBeat, duplicate, maxTrack](std::vector<Clip>& state, int& selected)
                                 {
                                     if (!juce::isPositiveAndBelow(clipIndex, static_cast<int>(state.size())))
                                         return;

                                     const int trackIdx = juce::jlimit(0, maxTrack, targetTrackIndex);
                                     const double startBeat = juce::jmax(0.0, targetStartBeat);

                                     if (duplicate)
                                     {
                                         Clip copy = state[static_cast<size_t>(clipIndex)];
                                         copy.trackIndex = trackIdx;
                                         copy.startBeat = startBeat;
                                         copy.name = copy.name + " Copy";
                                         state.push_back(std::move(copy));
                                         selected = static_cast<int>(state.size()) - 1;
                                         return;
                                     }

                                     auto& clip = state[static_cast<size_t>(clipIndex)];
                                     clip.trackIndex = trackIdx;
                                     clip.startBeat = startBeat;
                                     selected = clipIndex;
                                 });
        };
        timeline.onResizeClip = [this](int clipIndex, double newStartBeat, double newLengthBeats)
        {
            applyArrangementEdit("Resize Clip",
                                 [clipIndex, newStartBeat, newLengthBeats](std::vector<Clip>& state, int& selected)
                                 {
                                     if (!juce::isPositiveAndBelow(clipIndex, static_cast<int>(state.size())))
                                         return;

                                     auto& clip = state[static_cast<size_t>(clipIndex)];
                                     const double oldStart = clip.startBeat;
                                     const double trimmedStart = juce::jmax(0.0, newStartBeat);
                                     const double trimmedLength = juce::jmax(0.0625, newLengthBeats);
                                     const double delta = trimmedStart - oldStart;

                                     clip.startBeat = trimmedStart;
                                     clip.lengthBeats = trimmedLength;

                                     if (clip.type == ClipType::Audio)
                                     {
                                         clip.offsetBeats = juce::jmax(0.0, clip.offsetBeats + delta);
                                     }
                                     else if (clip.type == ClipType::MIDI)
                                     {
                                         std::vector<TimelineEvent> keptEvents;
                                         keptEvents.reserve(clip.events.size());
                                         for (auto event : clip.events)
                                         {
                                             event.startBeat -= delta;
                                             double endBeat = event.startBeat + event.durationBeats;

                                             if (endBeat <= 0.0 || event.startBeat >= clip.lengthBeats)
                                                 continue;

                                             if (event.startBeat < 0.0)
                                             {
                                                 event.durationBeats += event.startBeat;
                                                 event.startBeat = 0.0;
                                             }

                                             endBeat = juce::jmin(clip.lengthBeats, event.startBeat + event.durationBeats);
                                             event.durationBeats = juce::jmax(0.001, endBeat - event.startBeat);
                                             keptEvents.push_back(event);
                                         }
                                         clip.events = std::move(keptEvents);

                                         std::vector<MidiCCEvent> keptCC;
                                         keptCC.reserve(clip.ccEvents.size());
                                         for (auto cc : clip.ccEvents)
                                         {
                                             cc.beat -= delta;
                                             if (cc.beat >= 0.0 && cc.beat <= clip.lengthBeats)
                                                 keptCC.push_back(cc);
                                         }
                                         clip.ccEvents = std::move(keptCC);
                                     }

                                     selected = clipIndex;
                                 });
        };
        timeline.onReorderTracks = [this](int sourceTrackIndex, int targetTrackIndex)
        {
            reorderTracks(sourceTrackIndex, targetTrackIndex);
        };
        mixer.onReorderTracks = [this](int sourceTrackIndex, int targetTrackIndex)
        {
            reorderTracks(sourceTrackIndex, targetTrackIndex);
        };

        pianoRoll.onRequestClipEdit = [this](int clipIndex, const juce::String& action, std::function<void(Clip&)> mutator)
        {
            applyClipEdit(clipIndex, action, std::move(mutator));
        };
        pianoRoll.onSwingChanged = [this](int)
        {
            refreshStatusText();
        };
        pianoRoll.onPreviewStepNote = [this](int trackIndex, int noteNumber, int velocity)
        {
            auditionPianoRollNote(trackIndex, noteNumber, velocity);
        };
        stepSequencer.onRequestClipEdit = [this](int clipIndex, const juce::String& action, std::function<void(Clip&)> mutator)
        {
            applyClipEdit(clipIndex, action, std::move(mutator));
        };

        auto* dockedRackContent = new ChannelRackContent();
        dockedChannelRack = dockedRackContent;
        dockedRackContent->onLoadSlot = [this](int targetTrackIndex, int slotIndex)
        {
            showPluginListMenu(targetTrackIndex, dockedChannelRack != nullptr ? dockedChannelRack : static_cast<juce::Component*>(&showEditorButton), slotIndex);
        };
        dockedRackContent->onOpenSlot = [this](int targetTrackIndex, int slotIndex)
        {
            openPluginEditorWindowForTrack(targetTrackIndex, slotIndex);
        };
        dockedRackContent->onBypassChanged = [this](int targetTrackIndex, int slotIndex, bool bypassed)
        {
            if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                return;
            tracks[targetTrackIndex]->setPluginSlotBypassed(slotIndex, bypassed);
            refreshChannelRackWindow();
        };
        dockedRackContent->onClearSlot = [this](int targetTrackIndex, int slotIndex)
        {
            if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                return;
            tracks[targetTrackIndex]->clearPluginSlot(slotIndex);
            if (pluginEditorTrackIndex == targetTrackIndex && pluginEditorSlotIndex == slotIndex)
                closePluginEditorWindow();
            refreshChannelRackWindow();
        };
        dockedRackContent->onMoveSlot = [this](int targetTrackIndex, int fromSlot, int toSlot)
        {
            moveTrackPluginSlot(targetTrackIndex, fromSlot, toSlot);
        };

        auto* inspectorContent = new TrackInspectorContent();
        trackInspectorView = inspectorContent;
        inspectorContent->onRenameTrack = [this](int trackIndex)
        {
            renameTrack(trackIndex);
        };
        inspectorContent->onDuplicateTrack = [this](int trackIndex)
        {
            duplicateTrack(trackIndex);
        };
        inspectorContent->onDeleteTrack = [this](int trackIndex)
        {
            deleteTrack(trackIndex);
        };
        inspectorContent->onOpenFloatingRack = [this](int trackIndex)
        {
            openChannelRackForTrack(trackIndex);
        };
        inspectorContent->onLoadSlot = [this](int targetTrackIndex, int slotIndex)
        {
            showPluginListMenu(targetTrackIndex,
                               trackInspectorView != nullptr ? trackInspectorView
                                                             : static_cast<juce::Component*>(&showEditorButton),
                               slotIndex);
        };
        inspectorContent->onOpenSlot = [this](int targetTrackIndex, int slotIndex)
        {
            openPluginEditorWindowForTrack(targetTrackIndex, slotIndex);
        };
        inspectorContent->onBypassChanged = [this](int targetTrackIndex, int slotIndex, bool bypassed)
        {
            if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                return;
            tracks[targetTrackIndex]->setPluginSlotBypassed(slotIndex, bypassed);
            refreshChannelRackWindow();
        };
        inspectorContent->onClearSlot = [this](int targetTrackIndex, int slotIndex)
        {
            if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                return;
            tracks[targetTrackIndex]->clearPluginSlot(slotIndex);
            if (pluginEditorTrackIndex == targetTrackIndex && pluginEditorSlotIndex == slotIndex)
                closePluginEditorWindow();
            refreshChannelRackWindow();
        };
        inspectorContent->onMoveSlot = [this](int targetTrackIndex, int fromSlot, int toSlot)
        {
            moveTrackPluginSlot(targetTrackIndex, fromSlot, toSlot);
        };
        inspectorContent->onInputMonitoringChanged = [this](int trackIndex, bool enabled)
        {
            return requestTrackInputMonitoringState(trackIndex, enabled, "Inspector monitor toggle");
        };

        auto* recordingContent = new RecordingPanelContent();
        recordingPanelView = recordingContent;
        recordingContent->setMonitorSafeState(monitorSafeMode);
        recordingContent->onCalibrateInput = [this]
        {
            calibrateInputMonitoring();
        };
        recordingContent->onResetHolds = [this]
        {
            clearInputPeakHolds();
        };
        recordingContent->onMonitorSafeChanged = [this](bool enabled)
        {
            setMonitorSafeMode(enabled);
        };
        recordingContent->onCountInBarsChanged = [this](int bars)
        {
            recordCountInBars = juce::jlimit(0, 2, bars);
            refreshStatusText();
        };
        recordingContent->onManualOffsetSamplesChanged = [this](int samples)
        {
            recordingManualOffsetSamples = juce::jlimit(-20000, 20000, samples);
            refreshStatusText();
        };
        recordingContent->onInputMonitoringModeChanged = [this](int mode)
        {
            inputMonitoringMode = static_cast<InputMonitoringMode>(juce::jlimit(0, 2, mode));
            refreshStatusText();
        };
        recordingContent->onOverdubChanged = [this](bool enabled)
        {
            recordOverdubEnabled = enabled;
            refreshStatusText();
        };
        recordingContent->onPunchEnabledChanged = [this](bool enabled)
        {
            punchEnabled = enabled;
            refreshStatusText();
        };
        recordingContent->onPunchInChanged = [this](double beat)
        {
            punchInBeat = juce::jmax(0.0, beat);
            if (punchOutBeat <= punchInBeat + 0.25)
                punchOutBeat = punchInBeat + 0.25;
            refreshStatusText();
        };
        recordingContent->onPunchOutChanged = [this](double beat)
        {
            punchOutBeat = juce::jmax(punchInBeat + 0.25, beat);
            refreshStatusText();
        };
        recordingContent->onPreRollBarsChanged = [this](int bars)
        {
            preRollBars = juce::jlimit(0, 4, bars);
            refreshStatusText();
        };
        recordingContent->onPostRollBarsChanged = [this](int bars)
        {
            postRollBars = juce::jlimit(0, 4, bars);
            refreshStatusText();
        };
        recordingContent->getLatencyText = [this]
        {
            return getLatencySummaryText();
        };
        recordingContent->getCountInBars = [this]
        {
            return recordCountInBars;
        };
        recordingContent->getManualOffsetSamples = [this]
        {
            return recordingManualOffsetSamples;
        };
        recordingContent->getInputMonitoringMode = [this]
        {
            return static_cast<int>(inputMonitoringMode);
        };
        recordingContent->getOverdubEnabled = [this]
        {
            return recordOverdubEnabled;
        };
        recordingContent->getPunchEnabled = [this]
        {
            return punchEnabled;
        };
        recordingContent->getPunchInBeat = [this]
        {
            return punchInBeat;
        };
        recordingContent->getPunchOutBeat = [this]
        {
            return punchOutBeat;
        };
        recordingContent->getPreRollBars = [this]
        {
            return preRollBars;
        };
        recordingContent->getPostRollBars = [this]
        {
            return postRollBars;
        };
        recordingContent->getTrackCount = [this]
        {
            return tracks.size();
        };
        recordingContent->getTrackName = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return juce::String("Track " + juce::String(trackIndex + 1));
            return tracks[trackIndex]->getTrackName();
        };
        recordingContent->getTrackInputPeak = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return 0.0f;
            return tracks[trackIndex]->getInputMeterPeakLevel();
        };
        recordingContent->getTrackInputRms = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return 0.0f;
            return tracks[trackIndex]->getInputMeterRmsLevel();
        };
        recordingContent->getTrackInputHold = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return 0.0f;
            return tracks[trackIndex]->getInputPeakHoldLevel();
        };
        recordingContent->getTrackInputClip = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return false;
            return tracks[trackIndex]->isInputMeterClipping();
        };
        recordingContent->getTrackMonitoringEnabled = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return false;
            return tracks[trackIndex]->isInputMonitoringEnabled();
        };
        recordingContent->getLiveInputPeak = [this]
        {
            return liveInputPeakRt.load(std::memory_order_relaxed);
        };
        recordingContent->getLiveInputRms = [this]
        {
            return liveInputRmsRt.load(std::memory_order_relaxed);
        };
        recordingContent->onTrackRowClicked = [this](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
            setSelectedTrackIndex(trackIndex);
            timeline.selectTrack(trackIndex);
            mixer.selectTrack(trackIndex);
        };

        bottomTabs.addTab("Mixer", theme::Colours::panel(), &mixer, false);
        bottomTabs.addTab("Piano Roll", theme::Colours::background(), &pianoRoll, false);
        bottomTabs.addTab("Step Seq", theme::Colours::background(), &stepSequencer, false);
        bottomTabs.addTab("Channel Rack", theme::Colours::panel(), dockedRackContent, true);
        bottomTabs.addTab("Inspector", theme::Colours::panel(), inspectorContent, true);
        bottomTabs.addTab("Recording", theme::Colours::panel(), recordingContent, true);
        addAndMakeVisible(bottomTabs);

        tempoEvents.clear();
        tempoEvents.push_back({ 0.0, bpm });
        rebuildTempoEventMap();
        globalTransposeRt.store(projectTransposeSemitones, std::memory_order_relaxed);

        setSize(1420, 900);
        applyGridStep(gridStepFromSelectorId(gridSelector.getSelectedId()));
        setTempoBpm(bpm);
        applyProjectScaleToEngines();
        applyUiStyling();
        updateRealtimeFlagsFromUi();
        createNewTrack();
        ensureAutomationLaneIds();
        refreshInputDeviceSafetyState();
        applyLowLatencyMode(lowLatencyMode);
        loadToolbarLayoutSettings();
        rebuildRealtimeSnapshot(false);
        allowDirtyTracking = true;
        projectDirty = false;
        projectMutationSerial = 0;
        lastAutosaveSerial = 0;
        refreshStatusText();
        startTimerHz(30);

        if (!safeModeStartup && autoScanPluginsOnStartup)
        {
            juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
            {
                if (safeThis != nullptr)
                    safeThis->beginPluginScan();
            });
        }
        
        #if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu(this);
        #endif
    }

    ProjectSerializer::ProjectState MainComponent::buildCurrentProjectState() const
    {
        ProjectSerializer::ProjectState project;
        project.bpm = juce::jmax(1.0, bpm);
        project.keyRoot = projectKeyRoot;
        project.scaleMode = projectScaleMode;
        project.transposeSemitones = projectTransposeSemitones;
        project.lcdPositionMode = lcdDisplay != nullptr
            ? static_cast<int>(lcdDisplay->getPositionMode())
            : 1;
        project.loopEnabled = loopButton.getToggleState();
        project.loopStartBeat = transport.getLoopStartBeat();
        project.loopEndBeat = transport.getLoopEndBeat();
        project.arrangement = arrangement;
        project.tempoMap.reserve(tempoEvents.size());
        for (const auto& tempo : tempoEvents)
            project.tempoMap.push_back({ tempo.beat, tempo.bpm });

        project.tracks.reserve(static_cast<size_t>(tracks.size()));
        for (auto* track : tracks)
        {
            if (track == nullptr)
                continue;

            ProjectSerializer::TrackState state;
            state.name = track->getTrackName();
            state.volume = track->getVolume();
            state.pan = track->getPan();
            state.sendLevel = track->getSendLevel();
            state.sendTapMode = static_cast<int>(track->getSendTapMode());
            state.sendTargetBus = track->getSendTargetBus();
            state.mute = track->isMuted();
            state.solo = track->isSolo();
            state.arm = track->isArmed();
            state.inputMonitoring = track->isInputMonitoringEnabled();
            state.inputSourcePair = track->getInputSourcePair();
            state.inputMonitorGain = track->getInputMonitorGain();
            state.monitorTapMode = static_cast<int>(track->getMonitorTapMode());
            state.channelType = static_cast<int>(track->getChannelType());
            state.outputTargetType = static_cast<int>(track->getOutputTargetType());
            state.outputTargetBus = track->getOutputTargetBus();
            state.eqEnabled = track->isEqEnabled();
            state.eqLowGainDb = track->getEqLowGainDb();
            state.eqMidGainDb = track->getEqMidGainDb();
            state.eqHighGainDb = track->getEqHighGainDb();
            state.frozenPlaybackOnly = track->isFrozenPlaybackOnly();
            state.frozenRenderPath = track->getFrozenRenderPath();
            state.builtInInstrumentMode = static_cast<int>(track->getBuiltInInstrumentMode());
            state.samplerSamplePath = track->getSamplerSamplePath();
            state.builtInFxMask = track->getBuiltInEffectsMask();

            const int slotCount = track->getPluginSlotCount();
            for (int slot = Track::instrumentSlotIndex; slot < slotCount; ++slot)
            {
                if (!track->hasPluginInSlot(slot))
                    continue;

                ProjectSerializer::PluginSlotState slotState;
                slotState.slotIndex = slot;
                slotState.bypassed = track->isPluginSlotBypassed(slot);
                slotState.encodedState = track->getPluginStateForSlot(slot);
                slotState.hasDescription = track->getPluginDescriptionForSlot(slot, slotState.description);
                state.pluginSlots.push_back(std::move(slotState));
            }

            project.tracks.push_back(std::move(state));
        }
        project.automationLanes = automationLanes;

        return project;
    }

    bool MainComponent::saveProjectStateToFile(const juce::File& destination, juce::String& errorMessage) const
    {
        return ProjectSerializer::saveProject(destination, buildCurrentProjectState(), errorMessage);
    }

    void MainComponent::markProjectDirty()
    {
        if (!allowDirtyTracking || suppressDirtyTracking)
            return;
        projectDirty = true;
        ++projectMutationSerial;
    }

    void MainComponent::maybeRunAutosave()
    {
        if (!allowDirtyTracking || autosaveProjectFile == juce::File())
            return;
        if (!projectDirty)
            return;
        if (projectMutationSerial == lastAutosaveSerial)
            return;

        juce::String error;
        if (saveProjectStateToFile(autosaveProjectFile, error))
        {
            lastAutosaveSerial = projectMutationSerial;
            return;
        }

        juce::Logger::writeToLog("Autosave failed: " + (error.isNotEmpty() ? error : juce::String("Unknown error")));
    }

    void MainComponent::maybePromptRecoveryLoad()
    {
        recoveryPromptPending = false;
        if (!autosaveProjectFile.existsAsFile())
            return;

        loadProjectFromFile(autosaveProjectFile);
    }

    void MainComponent::ensureDefaultAutomationLanesForTrack(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto ensureLane = [this, trackIndex](AutomationTarget target, float defaultValue)
        {
            if (findAutomationLane(target, trackIndex) != nullptr)
                return;

            AutomationLane lane;
            lane.laneId = nextAutomationLaneId++;
            lane.target = target;
            lane.trackIndex = trackIndex;
            lane.mode = AutomationMode::Read;
            // Default lane should not override live mixer controls until user explicitly arms automation.
            lane.enabled = false;
            lane.points.push_back({ 0.0, defaultValue });
            automationLanes.push_back(std::move(lane));
        };

        auto* track = tracks[trackIndex];
        ensureLane(AutomationTarget::TrackVolume, track != nullptr ? track->getVolume() : 0.8f);
        ensureLane(AutomationTarget::TrackPan, track != nullptr ? track->getPan() : 0.0f);
        ensureLane(AutomationTarget::TrackSend, track != nullptr ? track->getSendLevel() : 0.0f);
    }

    void MainComponent::ensureAutomationLaneIds()
    {
        automationLanes.erase(std::remove_if(automationLanes.begin(),
                                             automationLanes.end(),
                                             [this](const AutomationLane& lane)
                                             {
                                                 switch (lane.target)
                                                 {
                                                     case AutomationTarget::MasterOutput:
                                                         return false;
                                                     case AutomationTarget::TrackVolume:
                                                     case AutomationTarget::TrackPan:
                                                     case AutomationTarget::TrackSend:
                                                         return !juce::isPositiveAndBelow(lane.trackIndex, tracks.size());
                                                     default:
                                                         return true;
                                                 }
                                             }),
                              automationLanes.end());

        for (auto& lane : automationLanes)
        {
            if (lane.target == AutomationTarget::MasterOutput)
                lane.trackIndex = -1;

            lane.points.erase(std::remove_if(lane.points.begin(),
                                             lane.points.end(),
                                             [](const AutomationPoint& point)
                                             {
                                                 return !std::isfinite(point.beat) || !std::isfinite(point.value);
                                             }),
                              lane.points.end());

            std::sort(lane.points.begin(), lane.points.end(),
                      [](const AutomationPoint& a, const AutomationPoint& b)
                      {
                          return a.beat < b.beat;
                      });

            lane.points.erase(std::unique(lane.points.begin(),
                                          lane.points.end(),
                                          [](const AutomationPoint& a, const AutomationPoint& b)
                                          {
                                              return std::abs(a.beat - b.beat) < 1.0e-9;
                                          }),
                              lane.points.end());

            auto clampLaneValue = [&](float value)
            {
                switch (lane.target)
                {
                    case AutomationTarget::TrackPan:
                        return juce::jlimit(-1.0f, 1.0f, value);
                    case AutomationTarget::TrackSend:
                        return juce::jlimit(0.0f, 1.0f, value);
                    case AutomationTarget::TrackVolume:
                    case AutomationTarget::MasterOutput:
                    default:
                        return juce::jlimit(0.0f, 1.2f, value);
                }
            };

            for (auto& point : lane.points)
            {
                point.beat = juce::jmax(0.0, point.beat);
                point.value = clampLaneValue(point.value);
            }

            if (lane.points.empty())
            {
                float defaultValue = 0.8f;
                if (lane.target == AutomationTarget::MasterOutput)
                {
                    defaultValue = masterOutputGainRt.load(std::memory_order_relaxed);
                }
                else if (juce::isPositiveAndBelow(lane.trackIndex, tracks.size()) && tracks[lane.trackIndex] != nullptr)
                {
                    switch (lane.target)
                    {
                        case AutomationTarget::TrackPan:
                            defaultValue = tracks[lane.trackIndex]->getPan();
                            break;
                        case AutomationTarget::TrackSend:
                            defaultValue = tracks[lane.trackIndex]->getSendLevel();
                            break;
                        case AutomationTarget::MasterOutput:
                        case AutomationTarget::TrackVolume:
                        default:
                            defaultValue = tracks[lane.trackIndex]->getVolume();
                            break;
                    }
                }
                lane.points.push_back({ 0.0, clampLaneValue(defaultValue) });
            }
        }

        std::set<int> usedIds;
        int maxId = 0;
        for (auto& lane : automationLanes)
        {
            if (lane.laneId <= 0 || usedIds.contains(lane.laneId))
                lane.laneId = 0;
            else
                usedIds.insert(lane.laneId);
            maxId = juce::jmax(maxId, lane.laneId);
        }

        int nextId = juce::jmax(1, maxId + 1);
        for (auto& lane : automationLanes)
        {
            if (lane.laneId > 0)
                continue;
            while (usedIds.contains(nextId))
                ++nextId;
            lane.laneId = nextId;
            usedIds.insert(nextId);
            ++nextId;
        }

        nextAutomationLaneId = juce::jmax(1, nextId);

        if (findAutomationLane(AutomationTarget::MasterOutput, -1) == nullptr)
        {
            AutomationLane lane;
            lane.laneId = nextAutomationLaneId++;
            lane.target = AutomationTarget::MasterOutput;
            lane.trackIndex = -1;
            lane.mode = AutomationMode::Read;
            lane.enabled = false;
            lane.points.push_back({ 0.0, masterOutputGainRt.load(std::memory_order_relaxed) });
            automationLanes.push_back(std::move(lane));
        }

        // Migration: disable legacy default lanes (single point at beat 0 matching current control value),
        // because older builds created them enabled and they unintentionally forced static values.
        for (auto& lane : automationLanes)
        {
            if (!lane.enabled
                || lane.mode != AutomationMode::Read
                || lane.points.size() != 1
                || std::abs(lane.points.front().beat) > 1.0e-9)
            {
                continue;
            }

            float currentValue = 0.0f;
            bool canCompare = true;
            switch (lane.target)
            {
                case AutomationTarget::TrackVolume:
                case AutomationTarget::TrackPan:
                case AutomationTarget::TrackSend:
                {
                    if (!juce::isPositiveAndBelow(lane.trackIndex, tracks.size())
                        || tracks[lane.trackIndex] == nullptr)
                    {
                        canCompare = false;
                        break;
                    }
                    auto* track = tracks[lane.trackIndex];
                    if (lane.target == AutomationTarget::TrackVolume)
                        currentValue = track->getVolume();
                    else if (lane.target == AutomationTarget::TrackPan)
                        currentValue = track->getPan();
                    else
                        currentValue = track->getSendLevel();
                    break;
                }
                case AutomationTarget::MasterOutput:
                    currentValue = masterOutputGainRt.load(std::memory_order_relaxed);
                    break;
                default:
                    canCompare = false;
                    break;
            }

            if (!canCompare)
                continue;

            if (std::abs(lane.points.front().value - currentValue) <= 1.0e-4f)
                lane.enabled = false;
        }
    }

    AutomationLane* MainComponent::findAutomationLane(AutomationTarget target, int trackIndex)
    {
        for (auto& lane : automationLanes)
        {
            if (lane.target == target && lane.trackIndex == trackIndex)
                return &lane;
        }
        return nullptr;
    }

    const AutomationLane* MainComponent::findAutomationLane(AutomationTarget target, int trackIndex) const
    {
        for (const auto& lane : automationLanes)
        {
            if (lane.target == target && lane.trackIndex == trackIndex)
                return &lane;
        }
        return nullptr;
    }

    AutomationMode MainComponent::getAutomationModeForTrackTarget(int trackIndex, AutomationTarget target) const
    {
        if (target == AutomationTarget::MasterOutput)
            trackIndex = -1;
        if (const auto* lane = findAutomationLane(target, trackIndex))
            return lane->mode;
        return AutomationMode::Read;
    }

    void MainComponent::setAutomationModeForTrackTarget(int trackIndex, AutomationTarget target, AutomationMode mode)
    {
        const bool isMasterTarget = target == AutomationTarget::MasterOutput;
        if (isMasterTarget)
            trackIndex = -1;

        if (!isMasterTarget && !juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto* lane = findAutomationLane(target, trackIndex);
        if (lane == nullptr)
        {
            if (isMasterTarget)
                ensureAutomationLaneIds();
            else
                ensureDefaultAutomationLanesForTrack(trackIndex);
            lane = findAutomationLane(target, trackIndex);
            if (lane == nullptr)
                return;
        }

        if (lane->mode == mode && lane->enabled)
            return;

        lane->mode = mode;
        lane->enabled = true;
        if (!isMasterTarget
            && (target == AutomationTarget::TrackVolume
            || target == AutomationTarget::TrackPan
            || target == AutomationTarget::TrackSend))
        {
            int targetIndex = 0;
            switch (target)
            {
                case AutomationTarget::TrackPan: targetIndex = 1; break;
                case AutomationTarget::TrackSend: targetIndex = 2; break;
                case AutomationTarget::MasterOutput:
                case AutomationTarget::TrackVolume:
                default: targetIndex = 0; break;
            }
            automationLatchStateRt[static_cast<size_t>(trackIndex)][static_cast<size_t>(targetIndex)]
                .store(false, std::memory_order_relaxed);
        }

        rebuildRealtimeSnapshot();
        refreshStatusText();
    }

    float MainComponent::evaluateAutomationLaneValueAtBeat(const AutomationLane& lane, double beat) const
    {
        if (lane.points.empty())
            return 0.0f;

        if (beat <= lane.points.front().beat)
            return lane.points.front().value;
        if (beat >= lane.points.back().beat)
            return lane.points.back().value;

        const auto it = std::lower_bound(lane.points.begin(),
                                         lane.points.end(),
                                         beat,
                                         [](const AutomationPoint& point, double b)
                                         {
                                             return point.beat < b;
                                         });
        if (it == lane.points.begin())
            return it->value;
        if (it == lane.points.end())
            return lane.points.back().value;

        const auto& right = *it;
        const auto& left = *(it - 1);
        const double span = juce::jmax(1.0e-9, right.beat - left.beat);
        const double alpha = juce::jlimit(0.0, 1.0, (beat - left.beat) / span);
        return left.value + static_cast<float>((right.value - left.value) * alpha);
    }

    void MainComponent::enqueueAutomationWriteEvent(int laneId, double beat, float value)
    {
        const int read = automationWriteReadIndex.load(std::memory_order_acquire);
        const int write = automationWriteWriteIndex.load(std::memory_order_relaxed);
        const int nextWrite = (write + 1) % automationWriteQueueCapacity;
        if (nextWrite == read)
        {
            droppedAutomationWriteEvents.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        automationWriteQueue[static_cast<size_t>(write)] = { laneId, beat, value };
        automationWriteWriteIndex.store(nextWrite, std::memory_order_release);
    }

    void MainComponent::drainAutomationWriteEvents()
    {
        bool updated = false;
        int read = automationWriteReadIndex.load(std::memory_order_relaxed);
        const int write = automationWriteWriteIndex.load(std::memory_order_acquire);
        while (read != write)
        {
            const auto& event = automationWriteQueue[static_cast<size_t>(read)];
            read = (read + 1) % automationWriteQueueCapacity;

            auto laneIt = std::find_if(automationLanes.begin(),
                                       automationLanes.end(),
                                       [&event](const AutomationLane& lane)
                                       {
                                           return lane.laneId == event.laneId;
                                       });
            if (laneIt == automationLanes.end())
                continue;

            auto& points = laneIt->points;
            const AutomationPoint point { juce::jmax(0.0, event.beat), event.value };
            if (points.empty())
            {
                points.push_back(point);
                updated = true;
                continue;
            }

            constexpr double minBeatDelta = 1.0 / 128.0;
            if (point.beat >= points.back().beat)
            {
                if (std::abs(point.beat - points.back().beat) <= minBeatDelta)
                    points.back() = point;
                else
                    points.push_back(point);
                updated = true;
                continue;
            }

            const auto insertPos = std::lower_bound(points.begin(),
                                                    points.end(),
                                                    point.beat,
                                                    [](const AutomationPoint& lhs, double b)
                                                    {
                                                        return lhs.beat < b;
                                                    });
            if (insertPos != points.end() && std::abs(insertPos->beat - point.beat) <= minBeatDelta)
            {
                *insertPos = point;
                updated = true;
            }
            else
            {
                points.insert(insertPos, point);
                updated = true;
            }
        }

        automationWriteReadIndex.store(read, std::memory_order_release);
        if (updated)
            rebuildRealtimeSnapshot();
    }

    void MainComponent::resetAutomationLatchStates()
    {
        for (auto& trackTouch : automationLatchStateRt)
            for (auto& state : trackTouch)
                state.store(false, std::memory_order_relaxed);
        masterAutomationLatchRt.store(false, std::memory_order_relaxed);
    }

    void MainComponent::applyAutomationForBlock(const RealtimeStateSnapshot& snapshot,
                                                double beat,
                                                bool isPlaying)
    {
        if (!isPlaying || snapshot.automationLanes.empty())
            return;

        for (const auto& lane : snapshot.automationLanes)
        {
            if (!lane.enabled)
                continue;

            bool shouldRead = false;
            bool shouldWrite = false;
            float currentValue = 0.0f;
            int trackIndex = lane.trackIndex;

            if (lane.target == AutomationTarget::MasterOutput)
            {
                const bool touch = masterAutomationTouchRt.load(std::memory_order_relaxed);
                bool latch = masterAutomationLatchRt.load(std::memory_order_relaxed);
                currentValue = masterOutputGainRt.load(std::memory_order_relaxed);

                switch (lane.mode)
                {
                    case AutomationMode::Read:
                        shouldRead = true;
                        break;
                    case AutomationMode::Touch:
                        shouldWrite = touch;
                        shouldRead = !shouldWrite;
                        break;
                    case AutomationMode::Latch:
                        if (touch)
                        {
                            latch = true;
                            masterAutomationLatchRt.store(true, std::memory_order_relaxed);
                        }
                        shouldWrite = latch;
                        shouldRead = !shouldWrite;
                        break;
                    case AutomationMode::Write:
                        shouldWrite = true;
                        break;
                    default:
                        break;
                }

                if (shouldRead && !lane.points.empty())
                {
                    const float automatedValue = juce::jlimit(0.0f, 1.4f, evaluateAutomationLaneValueAtBeat(lane, beat));
                    masterOutputGainRt.store(automatedValue, std::memory_order_relaxed);
                }
                else if (shouldWrite && lane.laneId > 0)
                {
                    enqueueAutomationWriteEvent(lane.laneId, beat, juce::jlimit(0.0f, 1.4f, currentValue));
                }
                continue;
            }

            if (!juce::isPositiveAndBelow(trackIndex, static_cast<int>(snapshot.trackPointers.size())))
                continue;

            auto* track = snapshot.trackPointers[static_cast<size_t>(trackIndex)];
            if (track == nullptr)
                continue;

            int targetIndex = 0;
            switch (lane.target)
            {
                case AutomationTarget::TrackPan: targetIndex = 1; currentValue = track->getPan(); break;
                case AutomationTarget::TrackSend: targetIndex = 2; currentValue = track->getSendLevel(); break;
                case AutomationTarget::MasterOutput:
                case AutomationTarget::TrackVolume:
                default: targetIndex = 0; currentValue = track->getVolume(); break;
            }

            const bool touch = automationTouchStateRt[static_cast<size_t>(trackIndex)][static_cast<size_t>(targetIndex)]
                .load(std::memory_order_relaxed);
            bool latch = automationLatchStateRt[static_cast<size_t>(trackIndex)][static_cast<size_t>(targetIndex)]
                .load(std::memory_order_relaxed);
            switch (lane.mode)
            {
                case AutomationMode::Read:
                    shouldRead = true;
                    break;
                case AutomationMode::Touch:
                    shouldWrite = touch;
                    shouldRead = !shouldWrite;
                    break;
                case AutomationMode::Latch:
                    if (touch)
                    {
                        latch = true;
                        automationLatchStateRt[static_cast<size_t>(trackIndex)][static_cast<size_t>(targetIndex)]
                            .store(true, std::memory_order_relaxed);
                    }
                    shouldWrite = latch;
                    shouldRead = !shouldWrite;
                    break;
                case AutomationMode::Write:
                    shouldWrite = true;
                    break;
                default:
                    break;
            }

            if (shouldRead && !lane.points.empty())
            {
                const float automatedValue = evaluateAutomationLaneValueAtBeat(lane, beat);
                switch (lane.target)
                {
                    case AutomationTarget::TrackPan:
                        track->setPan(juce::jlimit(-1.0f, 1.0f, automatedValue));
                        break;
                    case AutomationTarget::TrackSend:
                        track->setSendLevel(juce::jlimit(0.0f, 1.0f, automatedValue));
                        break;
                    case AutomationTarget::MasterOutput:
                    case AutomationTarget::TrackVolume:
                    default:
                        track->setVolume(juce::jlimit(0.0f, 1.2f, automatedValue));
                        break;
                }
            }
            else if (shouldWrite && lane.laneId > 0)
            {
                enqueueAutomationWriteEvent(lane.laneId, beat, currentValue);
            }
        }
    }

    void MainComponent::ensureTrackPdcCapacity(int requiredSamples)
    {
        const int clampedRequired = juce::jlimit(2048, 262144, requiredSamples);
        if (clampedRequired <= trackPdcBufferSamples)
            return;

        const int newSize = juce::jlimit(2048, 262144, juce::jmax(clampedRequired, trackPdcBufferSamples * 2));
        for (auto& delayBuffer : trackPdcDelayBuffers)
        {
            delayBuffer.setSize(4, newSize, false, false, true);
            delayBuffer.clear();
        }
        trackPdcBufferSamples = newSize;
        resetTrackPdcState();
    }

    void MainComponent::resetTrackPdcState()
    {
        for (auto& writePos : trackPdcWritePositions)
            writePos = 0;
        for (auto& delayBuffer : trackPdcDelayBuffers)
            delayBuffer.clear();
    }

    void MainComponent::applyTrackDelayCompensation(int trackIndex,
                                                    int mainDelaySamples,
                                                    int sendDelaySamples,
                                                    int blockSamples,
                                                    juce::AudioBuffer<float>& mainBuffer,
                                                    juce::AudioBuffer<float>& sendBuffer)
    {
        if (!juce::isPositiveAndBelow(trackIndex, maxRealtimeTracks))
            return;

        auto& delayBuffer = trackPdcDelayBuffers[static_cast<size_t>(trackIndex)];
        if (delayBuffer.getNumChannels() < 4
            || delayBuffer.getNumSamples() <= 0
            || mainBuffer.getNumSamples() <= 0)
        {
            return;
        }

        const int bufferSamples = juce::jmin(blockSamples,
                                             juce::jmin(mainBuffer.getNumSamples(),
                                                        sendBuffer.getNumSamples()));
        if (bufferSamples <= 0)
            return;

        auto& trackScratch = trackPdcScratchBuffers[static_cast<size_t>(trackIndex)];
        if (trackScratch.getNumChannels() < 4 || trackScratch.getNumSamples() < bufferSamples)
            return;

        const int mainChannels = juce::jmin(2, mainBuffer.getNumChannels());
        const int sendChannels = juce::jmin(2, sendBuffer.getNumChannels());
        trackScratch.clear();
        for (int ch = 0; ch < mainChannels; ++ch)
            trackScratch.copyFrom(ch, 0, mainBuffer, ch, 0, bufferSamples);
        for (int ch = 0; ch < sendChannels; ++ch)
            trackScratch.copyFrom(ch + 2, 0, sendBuffer, ch, 0, bufferSamples);

        int writePos = trackPdcWritePositions[static_cast<size_t>(trackIndex)];
        const int ringSamples = delayBuffer.getNumSamples();
        const int clampedMainDelay = juce::jlimit(0, ringSamples - 1, mainDelaySamples);
        const int clampedSendDelay = juce::jlimit(0, ringSamples - 1, sendDelaySamples);

        int readMainPos = writePos - clampedMainDelay;
        while (readMainPos < 0)
            readMainPos += ringSamples;
        int readSendPos = writePos - clampedSendDelay;
        while (readSendPos < 0)
            readSendPos += ringSamples;

        for (int i = 0; i < bufferSamples; ++i)
        {
            for (int ch = 0; ch < mainChannels; ++ch)
            {
                float delayed = trackScratch.getSample(ch, i);
                if (clampedMainDelay > 0)
                    delayed = delayBuffer.getSample(ch, readMainPos);
                mainBuffer.setSample(ch, i, delayed);
            }

            for (int ch = 0; ch < sendChannels; ++ch)
            {
                const int ringChannel = ch + 2;
                float delayed = trackScratch.getSample(ringChannel, i);
                if (clampedSendDelay > 0)
                    delayed = delayBuffer.getSample(ringChannel, readSendPos);
                sendBuffer.setSample(ch, i, delayed);
            }

            for (int ch = 0; ch < 4; ++ch)
                delayBuffer.setSample(ch, writePos, trackScratch.getSample(ch, i));

            ++writePos;
            if (writePos >= ringSamples)
                writePos = 0;

            ++readMainPos;
            if (readMainPos >= ringSamples)
                readMainPos = 0;
            ++readSendPos;
            if (readSendPos >= ringSamples)
                readSendPos = 0;
        }

        trackPdcWritePositions[static_cast<size_t>(trackIndex)] = writePos;
    }

    void MainComponent::recalculateAuxBusLatencyCache()
    {
        std::array<int, auxBusCount> busLatencySamples {};

        // Convention for active aux insert chains:
        // - Aux channel strips use Track::ChannelType::Aux
        // - Aux input source bus is Track::sendTargetBus
        // - Insert-chain latency comes from insert slots (instrument latency excluded)
        for (int i = 0; i < tracks.size(); ++i)
        {
            auto* track = tracks[i];
            if (track == nullptr || track->getChannelType() != Track::ChannelType::Aux)
                continue;

            const int sourceBus = juce::jlimit(0, auxBusCount - 1, track->getSendTargetBus());
            const int insertLatency = juce::jmax(0, track->getInsertPluginLatencySamples());
            if (insertLatency <= 0)
                continue;
            busLatencySamples[static_cast<size_t>(sourceBus)]
                = juce::jmax(busLatencySamples[static_cast<size_t>(sourceBus)], insertLatency);
        }

        for (int bus = 0; bus < auxBusCount; ++bus)
        {
            auxBusInsertLatencyRt[static_cast<size_t>(bus)]
                .store(busLatencySamples[static_cast<size_t>(bus)], std::memory_order_relaxed);
        }
    }

    int MainComponent::getAuxBusProcessingLatencySamples(int busIndex) const
    {
        if (!juce::isPositiveAndBelow(busIndex, auxBusCount))
            return 0;

        return juce::jmax(0,
                          auxBusInsertLatencyRt[static_cast<size_t>(busIndex)]
                              .load(std::memory_order_relaxed));
    }

    bool MainComponent::sanitizeRoutingConfiguration(bool showAlert)
    {
        juce::StringArray warnings;
        bool hadChanges = false;
        std::array<int, auxBusCount> busInputCounts {};
        for (int i = 0; i < tracks.size(); ++i)
        {
            auto* track = tracks[i];
            if (track == nullptr)
                continue;

            auto outputType = track->getOutputTargetType();
            int outputBus = track->getOutputTargetBus();
            int sendBus = track->getSendTargetBus();
            const auto channelType = track->getChannelType();

            const float clampedSend = juce::jlimit(0.0f, 1.0f, track->getSendLevel());
            if (std::abs(clampedSend - track->getSendLevel()) > 0.0001f)
            {
                track->setSendLevel(clampedSend);
                hadChanges = true;
                warnings.add("Track " + juce::String(i + 1) + " send level was clamped to a valid range.");
            }

            if (!juce::isPositiveAndBelow(sendBus, auxBusCount))
            {
                track->setSendTargetBus(0);
                sendBus = 0;
                hadChanges = true;
                warnings.add("Track " + juce::String(i + 1) + " had invalid send bus and was reset to Bus 1.");
            }

            if (outputType == Track::OutputTargetType::Bus
                && !juce::isPositiveAndBelow(outputBus, auxBusCount))
            {
                track->routeOutputToMaster();
                outputType = Track::OutputTargetType::Master;
                outputBus = 0;
                hadChanges = true;
                warnings.add("Track " + juce::String(i + 1) + " had invalid output bus and was reset to Master.");
            }

            if (channelType == Track::ChannelType::Master
                && outputType != Track::OutputTargetType::Master)
            {
                track->routeOutputToMaster();
                outputType = Track::OutputTargetType::Master;
                outputBus = 0;
                hadChanges = true;
                warnings.add("Master-type track " + juce::String(i + 1) + " output forced to Master.");
            }

            if (channelType == Track::ChannelType::Master && track->getSendLevel() > 0.0001f)
            {
                track->setSendLevel(0.0f);
                hadChanges = true;
                warnings.add("Master-type track " + juce::String(i + 1) + " send was disabled.");
            }

            if (channelType == Track::ChannelType::Master && track->isArmed())
            {
                track->setArm(false);
                hadChanges = true;
                warnings.add("Master-type track " + juce::String(i + 1) + " record arm was disabled.");
            }

            if (channelType == Track::ChannelType::Master && track->isInputMonitoringEnabled())
            {
                track->setInputMonitoring(false);
                hadChanges = true;
                warnings.add("Master-type track " + juce::String(i + 1) + " input monitoring was disabled.");
            }

            if (channelType == Track::ChannelType::Aux && track->isArmed())
            {
                track->setArm(false);
                hadChanges = true;
                warnings.add("Aux track " + juce::String(i + 1) + " record arm was disabled.");
            }

            if (outputType == Track::OutputTargetType::Bus
                && outputBus == sendBus
                && track->getSendLevel() > 0.0001f)
            {
                track->setSendLevel(0.0f);
                hadChanges = true;
                warnings.add("Track " + juce::String(i + 1)
                             + " send to Bus " + juce::String(sendBus + 1)
                             + " was disabled to prevent feedback.");
            }

            if (track->getSendLevel() > 0.0001f)
                ++busInputCounts[static_cast<size_t>(sendBus)];
            if (outputType == Track::OutputTargetType::Bus)
                ++busInputCounts[static_cast<size_t>(outputBus)];
        }

        for (int bus = 0; bus < auxBusCount; ++bus)
        {
            if (busInputCounts[static_cast<size_t>(bus)] <= 0)
                continue;

            if (!auxEnableButton.getToggleState())
            {
                warnings.add("Bus " + juce::String(bus + 1)
                             + " has routed signal while Aux FX is bypassed (signal still routes dry).");
            }
        }

        // Full route graph validation: detect accidental routing cycles before audio callback usage.
        const int trackNodeCount = tracks.size();
        const int busNodeOffset = trackNodeCount;
        const int masterNode = busNodeOffset + auxBusCount;
        std::vector<std::vector<int>> routeGraph(static_cast<size_t>(masterNode + 1));

        for (int i = 0; i < trackNodeCount; ++i)
        {
            auto* track = tracks[i];
            if (track == nullptr)
                continue;

            const auto outputType = track->getOutputTargetType();
            const int outputBus = juce::jlimit(0, auxBusCount - 1, track->getOutputTargetBus());
            if (outputType == Track::OutputTargetType::Bus)
                routeGraph[static_cast<size_t>(i)].push_back(busNodeOffset + outputBus);
            else
                routeGraph[static_cast<size_t>(i)].push_back(masterNode);

            if (track->getSendLevel() > 0.0001f)
            {
                const int sendBus = juce::jlimit(0, auxBusCount - 1, track->getSendTargetBus());
                routeGraph[static_cast<size_t>(i)].push_back(busNodeOffset + sendBus);
            }
        }

        for (int bus = 0; bus < auxBusCount; ++bus)
            routeGraph[static_cast<size_t>(busNodeOffset + bus)].push_back(masterNode);

        std::vector<int> visitState(static_cast<size_t>(routeGraph.size()), 0);
        bool cycleDetected = false;
        std::function<void(int)> visitNode = [&](int nodeIndex)
        {
            if (cycleDetected || !juce::isPositiveAndBelow(nodeIndex, static_cast<int>(visitState.size())))
                return;
            if (visitState[static_cast<size_t>(nodeIndex)] == 1)
            {
                cycleDetected = true;
                return;
            }
            if (visitState[static_cast<size_t>(nodeIndex)] == 2)
                return;

            visitState[static_cast<size_t>(nodeIndex)] = 1;
            for (int nextNode : routeGraph[static_cast<size_t>(nodeIndex)])
                visitNode(nextNode);
            visitState[static_cast<size_t>(nodeIndex)] = 2;
        };

        for (int nodeIndex = 0; nodeIndex < static_cast<int>(routeGraph.size()); ++nodeIndex)
        {
            visitNode(nodeIndex);
            if (cycleDetected)
                break;
        }

        if (cycleDetected)
        {
            for (auto* track : tracks)
            {
                if (track == nullptr)
                    continue;
                track->routeOutputToMaster();
                track->setSendLevel(0.0f);
            }
            hadChanges = true;
            warnings.add("Routing cycle detected. Outputs were reset to Master and sends were muted for safety.");
        }

        if (showAlert && !warnings.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Routing Safety",
                                                   warnings.joinIntoString("\n"));
        }
        return !hadChanges;
    }

    MainComponent::~MainComponent() { 
        setLookAndFeel(nullptr);
        #if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu(nullptr);
        #endif
        stopTimer();
        outputSafetyMuteBlocksRt.store(256, std::memory_order_relaxed);
        transport.stop();
        panicAllNotes();
        renderCancelRequestedRt.store(true, std::memory_order_relaxed);
        deviceManager.removeAudioCallback(this);
        backgroundRenderPool.removeAllJobs(true, 15000);
        backgroundRenderBusyRt.store(false, std::memory_order_relaxed);
        realtimeGraphScheduler.setWorkerCount(0);
        for (const auto& info : juce::MidiInput::getAvailableDevices())
        {
            if (deviceManager.isMidiInputDeviceEnabled(info.identifier))
            {
                deviceManager.removeMidiInputDeviceCallback(info.identifier, this);
                deviceManager.setMidiInputDeviceEnabled(info.identifier, false);
            }
        }
        midiRouter.setVirtualOutputEnabled(false, {});
        midiRouter.setOutputByIndex(-1);
        saveToolbarLayoutSettings();
        saveMidiLearnMappings();
        writePluginSessionGuard(true);
        if (pluginScanProcess != nullptr && pluginScanProcess->isRunning())
            pluginScanProcess->kill();
        pluginScanProcess.reset();
        closePluginEditorWindow();
        closeEqWindow();
        closeChannelRackWindow();
        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            for (auto& take : audioTakeWriters)
            {
                take.writer.reset();
                take.active = false;
            }
        }
        audioRecordDiskThread.stopThread(1500);
        streamingAudioReadThread.stopThread(1500);
        {
            const juce::ScopedLock sl(retiredSnapshotLock);
            retiredRealtimeSnapshots.clear();
        }
        std::atomic_store_explicit(&realtimeSnapshot,
                                   std::shared_ptr<const RealtimeStateSnapshot>{},
                                   std::memory_order_release);
        streamingClipCache.clear();
        if (autosaveProjectFile != juce::File())
            autosaveProjectFile.deleteFile();
        shutdownAudio(); 
    }

    void MainComponent::showToolbarConfigMenu()
    {
        enum MenuIds : int
        {
            profileProducer = 100,
            profileRecording = 101,
            profileMixing = 102,
            sectionBase = 200,
            sectionCount = static_cast<int>(ToolbarSection::Count),
            resetProfileDefaults = 320
        };

        auto sectionLabel = [](ToolbarSection section) -> juce::String
        {
            switch (section)
            {
                case ToolbarSection::Transport: return "Transport";
                case ToolbarSection::Timing: return "Timing";
                case ToolbarSection::AudioMidiIO: return "Audio/MIDI IO";
                case ToolbarSection::Editing: return "Editing";
                case ToolbarSection::Render: return "Render";
                case ToolbarSection::Utility: return "Utility";
                case ToolbarSection::Count:
                default: break;
            }
            return "Unknown";
        };

        juce::PopupMenu profileMenu;
        profileMenu.addItem(profileProducer, "Producer", true, activeToolbarProfile == ToolbarProfile::Producer);
        profileMenu.addItem(profileRecording, "Recording", true, activeToolbarProfile == ToolbarProfile::Recording);
        profileMenu.addItem(profileMixing, "Mixing", true, activeToolbarProfile == ToolbarProfile::Mixing);

        juce::PopupMenu sectionMenu;
        for (int i = 0; i < sectionCount; ++i)
        {
            const auto section = static_cast<ToolbarSection>(i);
            sectionMenu.addItem(sectionBase + i,
                                sectionLabel(section),
                                true,
                                isToolbarSectionVisible(section));
        }

        juce::PopupMenu menu;
        menu.addSubMenu("Profile", profileMenu);
        menu.addSubMenu("Sections", sectionMenu);
        menu.addSeparator();
        menu.addItem(resetProfileDefaults, "Reset Sections to Active Profile");

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&toolbarButton),
                           [this](int selectedId)
                           {
                               if (selectedId == profileProducer)
                               {
                                   applyToolbarProfile(ToolbarProfile::Producer, true);
                                   return;
                               }
                               if (selectedId == profileRecording)
                               {
                                   applyToolbarProfile(ToolbarProfile::Recording, true);
                                   return;
                               }
                               if (selectedId == profileMixing)
                               {
                                   applyToolbarProfile(ToolbarProfile::Mixing, true);
                                   return;
                               }
                               if (selectedId >= sectionBase && selectedId < sectionBase + sectionCount)
                               {
                                   const auto section = static_cast<ToolbarSection>(selectedId - sectionBase);
                                   const bool nextVisible = !isToolbarSectionVisible(section);
                                   setToolbarSectionVisible(section, nextVisible, true);
                                   return;
                               }
                               if (selectedId == resetProfileDefaults)
                                   applyToolbarProfile(activeToolbarProfile, true);
                           });
    }

    void MainComponent::applyToolbarProfile(ToolbarProfile profile, bool persistSettings)
    {
        activeToolbarProfile = profile;
        switch (profile)
        {
            case ToolbarProfile::Producer:
                toolbarSectionVisibility = { true, true, true, true, true, true };
                break;
            case ToolbarProfile::Recording:
                toolbarSectionVisibility = { true, true, true, false, false, true };
                break;
            case ToolbarProfile::Mixing:
                toolbarSectionVisibility = { true, true, false, true, true, true };
                break;
            default:
                toolbarSectionVisibility = { true, true, true, true, true, true };
                break;
        }

        if (persistSettings)
            saveToolbarLayoutSettings();
        resized();
    }

    void MainComponent::setToolbarSectionVisible(ToolbarSection section, bool visible, bool persistSettings)
    {
        const auto idx = static_cast<size_t>(section);
        if (idx >= toolbarSectionVisibility.size())
            return;

        toolbarSectionVisibility[idx] = visible;
        if (persistSettings)
            saveToolbarLayoutSettings();
        resized();
    }

    bool MainComponent::isToolbarSectionVisible(ToolbarSection section) const
    {
        const auto idx = static_cast<size_t>(section);
        if (idx >= toolbarSectionVisibility.size())
            return true;
        return toolbarSectionVisibility[idx];
    }

    MainComponent::ToolbarProfile MainComponent::toolbarProfileFromString(const juce::String& value) const
    {
        const auto lowered = value.trim().toLowerCase();
        if (lowered == "recording")
            return ToolbarProfile::Recording;
        if (lowered == "mixing")
            return ToolbarProfile::Mixing;
        return ToolbarProfile::Producer;
    }

    juce::String MainComponent::toolbarProfileToString(ToolbarProfile profile) const
    {
        switch (profile)
        {
            case ToolbarProfile::Recording: return "recording";
            case ToolbarProfile::Mixing: return "mixing";
            case ToolbarProfile::Producer:
            default: break;
        }
        return "producer";
    }

    void MainComponent::loadToolbarLayoutSettings()
    {
        applyToolbarProfile(activeToolbarProfile, false);

        if (!toolbarLayoutSettingsFile.existsAsFile())
            return;

        juce::String jsonText = toolbarLayoutSettingsFile.loadFileAsString();
        if (jsonText.trim().isEmpty())
            return;

        const juce::var parsed = juce::JSON::parse(jsonText);
        auto* rootObject = parsed.getDynamicObject();
        if (rootObject == nullptr)
            return;

        const auto profile = toolbarProfileFromString(rootObject->getProperty("profile").toString());
        applyToolbarProfile(profile, false);

        const auto sectionsValue = rootObject->getProperty("sections");
        auto* sectionsObject = sectionsValue.getDynamicObject();
        if (sectionsObject == nullptr)
            return;

        auto applyIfPresent = [this, sectionsObject](ToolbarSection section, const juce::String& key)
        {
            if (!sectionsObject->hasProperty(key))
                return;
            const auto idx = static_cast<size_t>(section);
            if (idx < toolbarSectionVisibility.size())
                toolbarSectionVisibility[idx] = static_cast<bool>(sectionsObject->getProperty(key));
        };

        applyIfPresent(ToolbarSection::Transport, "transport");
        applyIfPresent(ToolbarSection::Timing, "timing");
        applyIfPresent(ToolbarSection::AudioMidiIO, "audio_midi_io");
        applyIfPresent(ToolbarSection::Editing, "editing");
        applyIfPresent(ToolbarSection::Render, "render");
        applyIfPresent(ToolbarSection::Utility, "utility");

        const auto timelineZoomValue = rootObject->getProperty("timeline_ppb");
        if (timelineZoomValue.isDouble() || timelineZoomValue.isInt() || timelineZoomValue.isInt64())
        {
            const float ppb = juce::jlimit(30.0f, 420.0f, static_cast<float>(timelineZoomValue));
            timeline.setPixelsPerBeat(ppb);
            timelineZoomSlider.setValue(ppb, juce::dontSendNotification);
        }

        const auto trackHeightValue = rootObject->getProperty("track_height");
        if (trackHeightValue.isDouble() || trackHeightValue.isInt() || trackHeightValue.isInt64())
        {
            const float trackHeight = juce::jlimit(84.0f, 280.0f, static_cast<float>(trackHeightValue));
            timeline.setTrackHeight(trackHeight);
            trackZoomSlider.setValue(trackHeight, juce::dontSendNotification);
        }

        if (rootObject->hasProperty("follow_playhead"))
        {
            const bool followEnabled = static_cast<bool>(rootObject->getProperty("follow_playhead"));
            timeline.setAutoFollowPlayhead(followEnabled);
            followPlayheadButton.setToggleState(followEnabled, juce::dontSendNotification);
        }
    }

    void MainComponent::saveToolbarLayoutSettings() const
    {
        if (toolbarLayoutSettingsFile == juce::File())
            return;

        auto root = juce::DynamicObject::Ptr(new juce::DynamicObject());
        auto sections = juce::DynamicObject::Ptr(new juce::DynamicObject());

        root->setProperty("profile", toolbarProfileToString(activeToolbarProfile));
        sections->setProperty("transport", isToolbarSectionVisible(ToolbarSection::Transport));
        sections->setProperty("timing", isToolbarSectionVisible(ToolbarSection::Timing));
        sections->setProperty("audio_midi_io", isToolbarSectionVisible(ToolbarSection::AudioMidiIO));
        sections->setProperty("editing", isToolbarSectionVisible(ToolbarSection::Editing));
        sections->setProperty("render", isToolbarSectionVisible(ToolbarSection::Render));
        sections->setProperty("utility", isToolbarSectionVisible(ToolbarSection::Utility));
        root->setProperty("sections", juce::var(sections.get()));
        root->setProperty("timeline_ppb", static_cast<double>(timeline.getPixelsPerBeat()));
        root->setProperty("track_height", static_cast<double>(timeline.getTrackHeight()));
        root->setProperty("follow_playhead", timeline.isAutoFollowPlayheadEnabled());

        const juce::String jsonText = juce::JSON::toString(juce::var(root.get()), true);
        toolbarLayoutSettingsFile.replaceWithText(jsonText);
    }

    void MainComponent::paint(juce::Graphics& g)
    {
        g.fillAll(theme::Colours::background());
        auto topBar = getLocalBounds().removeFromTop(72).toFloat();
        juce::ColourGradient topGrad(theme::Colours::panel().brighter(0.08f),
                                     topBar.getTopLeft(),
                                     theme::Colours::darker().withAlpha(0.92f),
                                     topBar.getBottomLeft(),
                                     false);
        g.setGradientFill(topGrad);
        g.fillRect(topBar);
        g.setColour(theme::Colours::gridLine().withAlpha(0.35f));
        g.drawLine(0.0f, topBar.getBottom(), static_cast<float>(getWidth()), topBar.getBottom(), 1.0f);

        if (!bottomSplitterBounds.isEmpty())
        {
            const auto splitterColour = draggingBottomSplitter
                ? theme::Colours::accent().withAlpha(0.9f)
                : theme::Colours::gridLine().withAlpha(0.65f);
            g.setColour(splitterColour);
            g.fillRect(bottomSplitterBounds);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.drawLine(static_cast<float>(bottomSplitterBounds.getX()),
                       static_cast<float>(bottomSplitterBounds.getCentreY()),
                       static_cast<float>(bottomSplitterBounds.getRight()),
                       static_cast<float>(bottomSplitterBounds.getCentreY()),
                       1.0f);
        }

        const auto drawVerticalSplitter = [&g](const juce::Rectangle<int>& bounds, bool active)
        {
            if (bounds.isEmpty())
                return;
            g.setColour(active ? theme::Colours::accent().withAlpha(0.82f)
                               : theme::Colours::gridLine().withAlpha(0.55f));
            g.fillRect(bounds);
        };
        drawVerticalSplitter(browserSplitterBounds, draggingBrowserSplitter);
    }

    bool MainComponent::keyPressed(const juce::KeyPress& key)
    {
        const auto mods = key.getModifiers();
        const bool commandDown = mods.isCommandDown();
        const auto pressedChar = static_cast<juce::juce_wchar>(std::tolower(static_cast<unsigned char>(key.getTextCharacter())));

        if (commandDown && pressedChar == 'z' && !mods.isShiftDown())
        {
            undoManager.undo();
            rebuildRealtimeSnapshot();
            return true;
        }

        if ((commandDown && pressedChar == 'z' && mods.isShiftDown())
            || (commandDown && pressedChar == 'y'))
        {
            undoManager.redo();
            rebuildRealtimeSnapshot();
            return true;
        }

        if (commandDown && mods.isShiftDown() && pressedChar == 's')
        {
            saveProjectAs();
            return true;
        }

        if (commandDown && pressedChar == 's')
        {
            saveProject();
            return true;
        }

        if (commandDown && pressedChar == 'o')
        {
            openProject();
            return true;
        }

        if (commandDown && pressedChar == 'q')
        {
            requestApplicationClose();
            return true;
        }

        if (commandDown && pressedChar == 'l')
        {
            theme::ThemeManager::instance().toggleThemeMode();
            applyUiStyling();
            repaint();
            refreshStatusText();
            return true;
        }

        if (commandDown && pressedChar == 'd' && selectedClipIndex >= 0)
        {
            const int clipIndex = selectedClipIndex;
            applyArrangementEdit("Duplicate Clip",
                                 [clipIndex](std::vector<Clip>& state, int& selected)
                                 {
                                     if (!juce::isPositiveAndBelow(clipIndex, static_cast<int>(state.size())))
                                         return;
                                     Clip duplicate = state[static_cast<size_t>(clipIndex)];
                                     duplicate.startBeat = state[static_cast<size_t>(clipIndex)].startBeat
                                                         + state[static_cast<size_t>(clipIndex)].lengthBeats;
                                     duplicate.name = state[static_cast<size_t>(clipIndex)].name + " Copy";
                                     state.insert(state.begin() + clipIndex + 1, std::move(duplicate));
                                     selected = clipIndex + 1;
                                 });
            return true;
        }

        if (!commandDown && pressedChar == 's' && selectedClipIndex >= 0)
        {
            const int clipIndex = selectedClipIndex;
            const double splitBeat = juce::jmax(0.0, transport.getCurrentBeat());
            applyArrangementEdit("Split Clip",
                                 [clipIndex, splitBeat](std::vector<Clip>& state, int& selected)
                                 {
                                     if (!juce::isPositiveAndBelow(clipIndex, static_cast<int>(state.size())))
                                         return;

                                     auto& left = state[static_cast<size_t>(clipIndex)];
                                     Clip right;
                                     if (!ArrangementEditing::splitClipAtBeat(left, right, splitBeat))
                                         return;

                                     state.insert(state.begin() + clipIndex + 1, std::move(right));
                                     selected = clipIndex;
                                 });
            return true;
        }

        if (!commandDown && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
        {
            if (selectedClipIndex >= 0)
            {
                const int clipIndex = selectedClipIndex;
                applyArrangementEdit("Delete Clip",
                                     [clipIndex](std::vector<Clip>& state, int& selected)
                                     {
                                         if (!juce::isPositiveAndBelow(clipIndex, static_cast<int>(state.size())))
                                             return;
                                         state.erase(state.begin() + clipIndex);
                                         if (selected == clipIndex)
                                             selected = -1;
                                         else if (selected > clipIndex)
                                             --selected;
                                     });
                return true;
            }
        }

        if (commandDown && mods.isShiftDown()
            && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
        {
            if (juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()) && tracks.size() > 1)
            {
                deleteTrack(selectedTrackIndex);
                return true;
            }
        }

        if (commandDown && (pressedChar == '=' || pressedChar == '+'))
        {
            if (mods.isShiftDown())
            {
                timeline.zoomTrackHeightBy(8.0f);
                trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
            }
            else
            {
                timeline.zoomHorizontalBy(1.1f);
                timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
            }
            return true;
        }

        if (commandDown && pressedChar == '-')
        {
            if (mods.isShiftDown())
            {
                timeline.zoomTrackHeightBy(-8.0f);
                trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
            }
            else
            {
                timeline.zoomHorizontalBy(0.9f);
                timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
            }
            return true;
        }

        if (!commandDown && key == juce::KeyPress::spaceKey)
        {
            togglePlayback();
            return true;
        }

        if (!commandDown && key == juce::KeyPress::homeKey)
        {
            transport.setPosition(0.0);
            return true;
        }

        if (!commandDown && (pressedChar == 'r' || pressedChar == 'm' || pressedChar == 'l' || pressedChar == 'f' || pressedChar == 'p'))
        {
            if (pressedChar == 'r')
                recordButton.triggerClick();
            else if (pressedChar == 'm')
                metroButton.triggerClick();
            else if (pressedChar == 'l')
                loopButton.triggerClick();
            else if (pressedChar == 'f')
                timeline.setAutoFollowPlayhead(!timeline.isAutoFollowPlayheadEnabled());
            else if (pressedChar == 'p')
                panicAllNotes();
            refreshStatusText();
            return true;
        }

        if (!commandDown && juce::isPositiveAndBelow(selectedTrackIndex, tracks.size())
            && (pressedChar == '[' || pressedChar == ']' || pressedChar == '{' || pressedChar == '}'))
        {
            const float step = mods.isShiftDown() ? 0.10f : 0.05f;
            const float direction = (pressedChar == '[' || pressedChar == '{') ? -1.0f : 1.0f;
            auto* track = tracks[selectedTrackIndex];
            track->setSendLevel(track->getSendLevel() + (direction * step));
            refreshChannelRackWindow();
            refreshStatusText();
            return true;
        }

        if (commandDown && mods.isAltDown()
            && (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey))
        {
            if (!juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
                return true;

            const int targetTrack = (key == juce::KeyPress::upKey)
                ? selectedTrackIndex - 1
                : selectedTrackIndex + 1;
            if (juce::isPositiveAndBelow(targetTrack, tracks.size()))
                reorderTracks(selectedTrackIndex, targetTrack);
            return true;
        }

        if (commandDown && pressedChar >= '1' && pressedChar <= '4')
        {
            if (!juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
                return true;

            const int slotIndex = static_cast<int>(pressedChar - '1');
            if (mods.isShiftDown() || !tracks[selectedTrackIndex]->hasPluginInSlotNonBlocking(slotIndex))
                showPluginListMenu(selectedTrackIndex, &showEditorButton, slotIndex);
            else
                openPluginEditorWindowForTrack(selectedTrackIndex, slotIndex);
            return true;
        }

        if (!commandDown && mods.isAltDown() && (pressedChar == '1' || pressedChar == '2' || pressedChar == '3'))
        {
            if (lcdDisplay != nullptr)
            {
                if (pressedChar == '1')
                    lcdDisplay->setPositionMode(LcdDisplay::PositionMode::Musical);
                else if (pressedChar == '2')
                    lcdDisplay->setPositionMode(LcdDisplay::PositionMode::Timecode);
                else
                    lcdDisplay->setPositionMode(LcdDisplay::PositionMode::Samples);
            }
            return true;
        }

        if (!commandDown && selectedClipIndex >= 0
            && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey))
        {
            const double nudge = gridStepBeats * (mods.isShiftDown() ? 4.0 : 1.0);
            const double delta = (key == juce::KeyPress::leftKey) ? -nudge : nudge;
            const int clipIndex = selectedClipIndex;
            applyArrangementEdit("Nudge Clip",
                                 [clipIndex, delta](std::vector<Clip>& state, int& selected)
                                 {
                                     if (!juce::isPositiveAndBelow(clipIndex, static_cast<int>(state.size())))
                                         return;
                                     auto& clip = state[static_cast<size_t>(clipIndex)];
                                     clip.startBeat = juce::jmax(0.0, clip.startBeat + delta);
                                     selected = clipIndex;
                                 });
            return true;
        }

        if (!commandDown
            && (pressedChar == '1' || pressedChar == '2' || pressedChar == '3'
                || pressedChar == '4' || pressedChar == '5' || pressedChar == '6'))
        {
            const int tabIndex = static_cast<int>(pressedChar - '1');
            if (juce::isPositiveAndBelow(tabIndex, bottomTabs.getNumTabs()))
                bottomTabs.setCurrentTabIndex(tabIndex);
            return true;
        }

        if (commandDown && (pressedChar == 'k' || pressedChar == 'i' || pressedChar == 'e'))
        {
            if (pressedChar == 'k')
            {
                bottomTabs.setCurrentTabIndex(3);
                refreshChannelRackWindow();
            }
            else if (pressedChar == 'i')
            {
                bottomTabs.setCurrentTabIndex(4);
                refreshChannelRackWindow();
            }
            else if (pressedChar == 'e')
            {
                if (juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
                    openPluginEditorWindowForTrack(selectedTrackIndex, tracks[selectedTrackIndex]->getFirstLoadedPluginSlot());
            }
            return true;
        }

        if (commandDown && pressedChar == 'b')
        {
            if (mods.isShiftDown())
                exportStems();
            else
                exportMixdown();
            return true;
        }

        if (commandDown && pressedChar == 't')
        {
            if (mods.isShiftDown())
                clipToolsButton.triggerClick();
            else
                tempoMenuButton.triggerClick();
            return true;
        }

        if (bottomTabs.getCurrentTabIndex() == 1 && pianoRoll.handleComputerKeyboardPress(key))
            return true;

        return false;
    }

    void MainComponent::mouseDown(const juce::MouseEvent& e)
    {
        if (browserSplitterBounds.expanded(3, 0).contains(e.getPosition()))
        {
            draggingBrowserSplitter = true;
            splitterDragMouseOffset = e.getPosition().x - browserSplitterBounds.getCentreX();
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        if (bottomSplitterBounds.expanded(0, 3).contains(e.getPosition()))
        {
            draggingBottomSplitter = true;
            splitterDragMouseOffset = e.getPosition().y - bottomSplitterBounds.getCentreY();
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        }
    }

    void MainComponent::mouseDrag(const juce::MouseEvent& e)
    {
        if (draggingBrowserSplitter)
        {
            const int contentWidth = getWidth();
            const int splitterCenterX = e.getPosition().x - splitterDragMouseOffset;
            browserPanelRatio = juce::jlimit(0.12f, 0.33f,
                                             static_cast<float>(splitterCenterX) / static_cast<float>(juce::jmax(1, contentWidth)));
            resized();
            repaint(browserSplitterBounds.getUnion(browserSplitterBounds.expanded(12, 4)));
            return;
        }

        if (!draggingBottomSplitter)
            return;

        const int availableHeight = getHeight() - 76;
        if (availableHeight <= 0)
            return;

        const int splitterCenterY = e.getPosition().y - splitterDragMouseOffset;
        const int newBottomHeight = juce::jlimit(220, juce::jmax(220, availableHeight - 170),
                                                 getHeight() - splitterCenterY);
        bottomPanelRatio = juce::jlimit(0.25f, 0.80f, static_cast<float>(newBottomHeight) / static_cast<float>(availableHeight));
        resized();
        repaint();
    }

    void MainComponent::mouseUp(const juce::MouseEvent&)
    {
        if (draggingBrowserSplitter)
        {
            draggingBrowserSplitter = false;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
            return;
        }

        if (!draggingBottomSplitter)
            return;

        draggingBottomSplitter = false;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
    }

    void MainComponent::mouseMove(const juce::MouseEvent& e)
    {
        if (draggingBrowserSplitter)
        {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        if (draggingBottomSplitter)
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        if (browserSplitterBounds.expanded(3, 0).contains(e.getPosition()))
        {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        if (bottomSplitterBounds.expanded(0, 3).contains(e.getPosition()))
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        else
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void MainComponent::mouseExit(const juce::MouseEvent&)
    {
        if (!draggingBottomSplitter)
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
    {
        recalculateAuxBusLatencyCache();
        transport.prepare(sampleRate);
        transport.setTempo(bpmRt.load());
        sampleRateRt.store(sampleRate > 0.0 ? sampleRate : 44100.0, std::memory_order_relaxed);
        const double resolvedSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        const double tauSeconds = 0.005;
        masterGainDezipperCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (resolvedSampleRate * tauSeconds)));
        auxReturnGainRt.store(auxReturnGain, std::memory_order_relaxed);
        auxFxEnabledRt.store(auxFxEnabled, std::memory_order_relaxed);
        outputDcHighPassEnabledRt.store(outputDcHighPassEnabled, std::memory_order_relaxed);
        midiCollector.reset(sampleRate);
        midiScheduler.reset();
        for (auto& auxReverb : auxReverbs)
            auxReverb.setSampleRate(sampleRate);
        
        // Prepare tracks
        for (auto* t : tracks) t->prepareToPlay(sampleRate, samplesPerBlockExpected);

        // PRE-ALLOCATE BUFFERS (Fixes clicks/pops)
        const int reserveSamples = juce::jmax(samplesPerBlockExpected, maxRealtimeBlockSize);
        tempMixingBuffer.setSize(2, reserveSamples);
        for (auto& auxBus : auxBusBuffers)
            auxBus.setSize(2, reserveSamples);
        trackTempAudio.setSize(2, reserveSamples);
        trackSendAudio.setSize(2, reserveSamples);
        trackInputAudio.setSize(2, reserveSamples);
        trackPdcScratchBuffer.setSize(4, reserveSamples);
        audioStreamScratch.setSize(8, reserveSamples * 16, false, false, true);
        for (int trackIndex = 0; trackIndex < maxRealtimeTracks; ++trackIndex)
        {
            trackMainWorkBuffers[static_cast<size_t>(trackIndex)].setSize(2, reserveSamples, false, false, true);
            trackTimelineWorkBuffers[static_cast<size_t>(trackIndex)].setSize(2, reserveSamples, false, false, true);
            trackSendWorkBuffers[static_cast<size_t>(trackIndex)].setSize(2, reserveSamples, false, false, true);
            trackInputWorkBuffers[static_cast<size_t>(trackIndex)].setSize(2, reserveSamples, false, false, true);
            trackPdcScratchBuffers[static_cast<size_t>(trackIndex)].setSize(4, reserveSamples, false, false, true);
        }

        int activeInputChannels = 0;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            activeInputChannels = juce::jmax(0, device->getActiveInputChannels().countNumberOfSetBits());
        activeInputChannelCountRt.store(activeInputChannels, std::memory_order_relaxed);
        liveInputCaptureBuffer.setSize(juce::jmax(2, activeInputChannels), reserveSamples);
        for (auto& buffer : inputTapBuffers)
            buffer.setSize(juce::jmax(2, activeInputChannels), reserveSamples);
        
        for (auto& b : trackMidiBuffers)
            b.ensureSize(4096);
        for (auto& b : previewMidiBuffers)
            b.ensureSize(128);
        
        liveMidiBuffer.ensureSize(2048);
        chordEngineOutputBuffer.ensureSize(2048);
        masterGainSmoothingState = masterOutputGainRt.load(std::memory_order_relaxed);
        outputDcPrevInput = { 0.0f, 0.0f };
        outputDcPrevOutput = { 0.0f, 0.0f };
        masterLimiterPrevInput = { 0.0f, 0.0f };
        masterTruePeakMidpointPrevInput = { 0.0f, 0.0f };
        masterLimiterGainState = 1.0f;
        wasTransportPlayingLastBlock = false;
        masterPhaseCorrelationRt.store(0.0f, std::memory_order_relaxed);
        masterLoudnessLufsRt.store(-120.0f, std::memory_order_relaxed);
        masterAnalyzerReadySnapshot.store(-1, std::memory_order_relaxed);
        masterAnalyzerWriteSnapshot = 0;
        masterAnalyzerBuildPos = 0;
        masterAnalyzerBuildBuffer.fill(0.0f);
        for (auto& snapshot : masterAnalyzerSnapshots)
            snapshot.fill(0.0f);
        int maxTrackLatency = 0;
        for (auto* track : tracks)
            maxTrackLatency = juce::jmax(maxTrackLatency, track != nullptr ? track->getTotalPluginLatencySamples() : 0);
        int maxAuxLatency = 0;
        for (int bus = 0; bus < auxBusCount; ++bus)
            maxAuxLatency = juce::jmax(maxAuxLatency, getAuxBusProcessingLatencySamples(bus));
        const int maxGraphLatency = maxTrackLatency + maxAuxLatency;
        maxPdcLatencySamplesRt.store(maxGraphLatency, std::memory_order_relaxed);
        ensureTrackPdcCapacity(maxGraphLatency + reserveSamples + 128);
        resetTrackPdcState();
        rebuildRealtimeSnapshot();
    }

    // --- THE REAL-TIME AUDIO ENGINE ---
    void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        if (bufferToFill.buffer == nullptr)
            return;

        if (bufferToFill.numSamples <= 0 || bufferToFill.buffer->getNumChannels() <= 0)
            return;

        juce::ScopedNoDenormals noDenormals;

        AudioCallbackPerfScope callbackPerf(sampleRateRt,
                                            bufferToFill.numSamples,
                                            audioCallbackLoadRt,
                                            audioCallbackOverloadCountRt,
                                            audioXrunCountRt,
                                            xrunRecoveryBlocksRt);

        const auto markGuardDrop = [this]
        {
            audioGuardDropCountRt.fetch_add(1, std::memory_order_relaxed);
            audioXrunCountRt.fetch_add(1, std::memory_order_relaxed);
            const int recovery = xrunRecoveryBlocksRt.load(std::memory_order_relaxed);
            xrunRecoveryBlocksRt.store(juce::jmax(recovery, 96), std::memory_order_relaxed);
        };

        if (bufferToFill.startSample < 0
            || bufferToFill.startSample + bufferToFill.numSamples > bufferToFill.buffer->getNumSamples())
        {
            markGuardDrop();
            return;
        }

        const int outputSafetyMuteBlocks = outputSafetyMuteBlocksRt.load(std::memory_order_relaxed);
        if (outputSafetyMuteBlocks > 0)
        {
            bufferToFill.clearActiveBufferRegion();
            const int nextMuteBlocks = outputSafetyMuteBlocks - 1;
            outputSafetyMuteBlocksRt.store(nextMuteBlocks, std::memory_order_relaxed);
            if (nextMuteBlocks == 0)
            {
                const int releaseRampSamples = juce::jmax(128, bufferToFill.numSamples * 3);
                startupOutputRampTotalSamplesRt.store(releaseRampSamples, std::memory_order_relaxed);
                startupOutputRampSamplesRemainingRt.store(releaseRampSamples, std::memory_order_relaxed);
                startupSafetyRampLoggedRt.store(false, std::memory_order_relaxed);
            }
            outputDcPrevInput = { 0.0f, 0.0f };
            outputDcPrevOutput = { 0.0f, 0.0f };
            masterLimiterPrevInput = { 0.0f, 0.0f };
            masterTruePeakMidpointPrevInput = { 0.0f, 0.0f };
            masterLimiterGainState = 1.0f;
            masterPeakMeterRt.store(masterPeakMeterRt.load(std::memory_order_relaxed) * 0.86f, std::memory_order_relaxed);
            masterRmsMeterRt.store(masterRmsMeterRt.load(std::memory_order_relaxed) * 0.82f, std::memory_order_relaxed);
            return;
        }

        auxMeterRt.store(0.0f, std::memory_order_relaxed);
        for (auto& meter : auxBusMeterRt)
            meter.store(0.0f, std::memory_order_relaxed);
        const int requestedInputChannels = juce::jmax(0, activeInputChannelCountRt.load(std::memory_order_relaxed));
        const int readyInputTapIndex = inputTapReadyIndex.load(std::memory_order_acquire);
        const bool tapReady = juce::isPositiveAndBelow(readyInputTapIndex, static_cast<int>(inputTapBuffers.size()));
        const int tapChannels = tapReady
            ? inputTapNumChannels[static_cast<size_t>(readyInputTapIndex)].load(std::memory_order_relaxed)
            : 0;
        const int tapSamples = tapReady
            ? inputTapNumSamples[static_cast<size_t>(readyInputTapIndex)].load(std::memory_order_relaxed)
            : 0;
        const int effectiveRequestedInputChannels = requestedInputChannels > 0 ? requestedInputChannels : tapChannels;
        const int capturedInputChannels = juce::jmax(0, juce::jmin(effectiveRequestedInputChannels, tapChannels));

        if (tapReady
            && capturedInputChannels > 0
            && tapSamples > 0
            && liveInputCaptureBuffer.getNumSamples() >= bufferToFill.numSamples
            && liveInputCaptureBuffer.getNumChannels() >= capturedInputChannels)
        {
            auto& sourceInput = inputTapBuffers[static_cast<size_t>(readyInputTapIndex)];
            const int copySamples = juce::jmin(bufferToFill.numSamples, tapSamples);
            for (int ch = 0; ch < capturedInputChannels; ++ch)
            {
                liveInputCaptureBuffer.copyFrom(ch, 0, sourceInput, ch, 0, copySamples);
                if (copySamples < bufferToFill.numSamples)
                    liveInputCaptureBuffer.clear(ch, copySamples, bufferToFill.numSamples - copySamples);
            }
            for (int ch = capturedInputChannels; ch < liveInputCaptureBuffer.getNumChannels(); ++ch)
                liveInputCaptureBuffer.clear(ch, 0, bufferToFill.numSamples);

            float inputPeak = 0.0f;
            float inputRms = 0.0f;
            for (int ch = 0; ch < capturedInputChannels; ++ch)
            {
                inputPeak = juce::jmax(inputPeak, liveInputCaptureBuffer.getMagnitude(ch, 0, copySamples));
                inputRms = juce::jmax(inputRms, liveInputCaptureBuffer.getRMSLevel(ch, 0, copySamples));
            }

            const float prevPeak = liveInputPeakRt.load(std::memory_order_relaxed);
            const float prevRms = liveInputRmsRt.load(std::memory_order_relaxed);
            liveInputPeakRt.store(inputPeak > prevPeak ? inputPeak : juce::jmax(inputPeak, prevPeak * 0.92f),
                                  std::memory_order_relaxed);
            liveInputRmsRt.store(prevRms + ((inputRms - prevRms) * 0.24f), std::memory_order_relaxed);
        }
        else if (liveInputCaptureBuffer.getNumSamples() >= bufferToFill.numSamples)
        {
            for (int ch = 0; ch < liveInputCaptureBuffer.getNumChannels(); ++ch)
                liveInputCaptureBuffer.clear(ch, 0, bufferToFill.numSamples);

            liveInputPeakRt.store(liveInputPeakRt.load(std::memory_order_relaxed) * 0.90f, std::memory_order_relaxed);
            liveInputRmsRt.store(liveInputRmsRt.load(std::memory_order_relaxed) * 0.88f, std::memory_order_relaxed);
        }

        bufferToFill.clearActiveBufferRegion();
        const auto snapshot = getRealtimeSnapshot();
        if (!snapshot || snapshot->trackPointers.empty())
            return;

        if (bufferToFill.numSamples > tempMixingBuffer.getNumSamples()
            || bufferToFill.numSamples > trackTempAudio.getNumSamples()
            || bufferToFill.numSamples > trackInputAudio.getNumSamples()
            || bufferToFill.numSamples > trackSendAudio.getNumSamples()
            || bufferToFill.numSamples > trackPdcScratchBuffer.getNumSamples())
        {
            // Do not allocate on the audio thread.
            markGuardDrop();
            return;
        }
        for (const auto& auxBus : auxBusBuffers)
        {
            if (bufferToFill.numSamples > auxBus.getNumSamples())
            {
                markGuardDrop();
                return;
            }
        }

        const int activeTrackCount = juce::jmin(static_cast<int>(snapshot->trackPointers.size()),
                                                static_cast<int>(trackMidiBuffers.size()));
        if (activeTrackCount <= 0)
            return;
        for (int i = 0; i < activeTrackCount; ++i)
        {
            if (bufferToFill.numSamples > trackMainWorkBuffers[static_cast<size_t>(i)].getNumSamples()
                || bufferToFill.numSamples > trackTimelineWorkBuffers[static_cast<size_t>(i)].getNumSamples()
                || bufferToFill.numSamples > trackSendWorkBuffers[static_cast<size_t>(i)].getNumSamples()
                || bufferToFill.numSamples > trackInputWorkBuffers[static_cast<size_t>(i)].getNumSamples())
            {
                markGuardDrop();
                return;
            }
        }

        std::array<int, maxRealtimeTracks> trackMainPathLatencySamples {};
        std::array<int, maxRealtimeTracks> trackSendPathLatencySamples {};
        std::array<bool, maxRealtimeTracks> trackSendPathActive {};
        std::array<int, auxBusCount> auxBusLatencySamples {};
        for (int bus = 0; bus < auxBusCount; ++bus)
            auxBusLatencySamples[static_cast<size_t>(bus)] = getAuxBusProcessingLatencySamples(bus);

        const auto resolveTempoAtBeat = [](double beat, const std::vector<MainComponent::TempoEvent>& map, double fallbackBpm)
        {
            double resolved = fallbackBpm;
            for (const auto& event : map)
            {
                if (event.beat > beat + 1.0e-9)
                    break;
                resolved = juce::jmax(1.0, event.bpm);
            }
            return juce::jmax(1.0, resolved);
        };

        double blockTempoBpm = 120.0;

        if (externalMidiClockSyncEnabledRt.load(std::memory_order_relaxed)
            && externalMidiClockActiveRt.load(std::memory_order_relaxed))
        {
            blockTempoBpm = juce::jmax(1.0, externalMidiClockTempoRt.load(std::memory_order_relaxed));
            const bool shouldRun = externalMidiClockTransportRunningRt.load(std::memory_order_relaxed);
            const int64_t generation = externalMidiClockGenerationRt.load(std::memory_order_relaxed);

            if (shouldRun != externalMidiClockWasRunning)
            {
                if (shouldRun)
                    transport.playRt();
                else
                    transport.stopRt();
                externalMidiClockWasRunning = shouldRun;
            }

            if (generation != lastAppliedExternalClockGeneration)
            {
                const double externalBeat = juce::jmax(0.0, externalMidiClockBeatRt.load(std::memory_order_relaxed));
                transport.setPositionBeatsRt(externalBeat);
                lastAppliedExternalClockGeneration = generation;
            }
        }
        else
        {
            externalMidiClockWasRunning = false;
            lastAppliedExternalClockGeneration = -1;
            const double preAdvanceBeat = transport.getCurrentBeat();
            blockTempoBpm = resolveTempoAtBeat(preAdvanceBeat,
                                               snapshot->tempoEvents,
                                               bpmRt.load(std::memory_order_relaxed));
        }

        bpmRt.store(blockTempoBpm, std::memory_order_relaxed);

        // 1. Advance transport and capture exact block range (single transport lock path).
        const auto blockRange = transport.advanceWithTempo(bufferToFill.numSamples, blockTempoBpm);
        const double startBeat = blockRange.startBeat;
        const double endBeat = blockRange.endBeat;
        const bool isPlaying = transport.playing();
        const bool chaseNotesThisBlock = isPlaying && !wasTransportPlayingLastBlock;
        applyAutomationForBlock(*snapshot, startBeat, isPlaying);

        const bool wrappedLoopBlock = blockRange.wrapped && transport.isLooping();
        const double loopStartBeat = transport.getLoopStartBeat();
        const double loopEndBeat = transport.getLoopEndBeat();
        const auto blockCoversBeat = [&](double beat)
        {
            return beatFallsInRange(beat,
                                    startBeat,
                                    endBeat,
                                    wrappedLoopBlock,
                                    loopStartBeat,
                                    loopEndBeat);
        };

        if (isPlaying
            && recordEnabledRt.load(std::memory_order_relaxed)
            && recordStartPendingRt.load(std::memory_order_relaxed))
        {
            const double pendingBeat = recordStartPendingBeatRt.load(std::memory_order_relaxed);
            if (blockCoversBeat(pendingBeat))
            {
                const double beatsPerSample = juce::jmax(1.0e-12, transport.getBeatsPerSample());
                const double beatDelta = juce::jmax(0.0, pendingBeat - startBeat);
                const int offsetSamples = juce::jlimit(0,
                                                       juce::jmax(0, bufferToFill.numSamples - 1),
                                                       static_cast<int>(std::llround(beatDelta / beatsPerSample)));
                const int64_t startSample = blockRange.startSample + static_cast<int64_t>(offsetSamples);
                recordingStartBeatRt.store(pendingBeat, std::memory_order_relaxed);
                recordingStartSampleRt.store(startSample, std::memory_order_relaxed);
                recordingStartOffsetSamplesRt.store(offsetSamples, std::memory_order_relaxed);
                recordStartPendingRt.store(false, std::memory_order_relaxed);
                recordStartRequestRt.store(1, std::memory_order_relaxed);
            }
        }

        const double autoStopBeatRt = autoStopAfterBeatRt.load(std::memory_order_relaxed);
        if (autoStopBeatRt > 0.0
            && transport.recording()
            && blockCoversBeat(autoStopBeatRt))
        {
            autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);
            recordStopRequestRt.store(1, std::memory_order_relaxed);
        }

        int maxGraphLatencySamples = 0;
        for (int i = 0; i < activeTrackCount; ++i)
        {
            auto* track = snapshot->trackPointers[static_cast<size_t>(i)];
            if (track == nullptr)
                continue;

            const int trackLatency = juce::jmax(0, track->getTotalPluginLatencySamples());
            const int sendBusIndex = juce::jlimit(0, auxBusCount - 1, track->getSendTargetBus());
            const bool outputToBus = track->getOutputTargetType() == Track::OutputTargetType::Bus;
            const int outputBusIndex = juce::jlimit(0, auxBusCount - 1, track->getOutputTargetBus());
            const int outputBusLatency = outputToBus ? auxBusLatencySamples[static_cast<size_t>(outputBusIndex)] : 0;
            const int mainPathLatency = trackLatency + outputBusLatency;
            trackMainPathLatencySamples[static_cast<size_t>(i)] = mainPathLatency;
            maxGraphLatencySamples = juce::jmax(maxGraphLatencySamples, mainPathLatency);

            const bool sendActive = track->getSendLevel() > 0.0001f;
            const bool sendFeedbackBlocked = outputToBus
                && sendBusIndex == outputBusIndex
                && sendActive;
            if (sendActive && !sendFeedbackBlocked)
            {
                const int sendPathLatency = trackLatency + auxBusLatencySamples[static_cast<size_t>(sendBusIndex)];
                trackSendPathLatencySamples[static_cast<size_t>(i)] = sendPathLatency;
                trackSendPathActive[static_cast<size_t>(i)] = true;
                maxGraphLatencySamples = juce::jmax(maxGraphLatencySamples, sendPathLatency);
            }
            else
            {
                trackSendPathLatencySamples[static_cast<size_t>(i)] = mainPathLatency;
                trackSendPathActive[static_cast<size_t>(i)] = false;
            }
        }

        maxPdcLatencySamplesRt.store(maxGraphLatencySamples, std::memory_order_relaxed);
        const int requiredPdcSamples = maxGraphLatencySamples + bufferToFill.numSamples + 8;
        const bool pdcReady = trackPdcBufferSamples >= requiredPdcSamples;

        // 2. Clear Temp Buffers
        for (auto& auxBus : auxBusBuffers)
            for (int ch = 0; ch < auxBus.getNumChannels(); ++ch)
                auxBus.clear(ch, 0, bufferToFill.numSamples);
        for (int i = 0; i < activeTrackCount; ++i)
        {
            trackMidiBuffers[static_cast<size_t>(i)].clear();
            auto& trackTimelineBuffer = trackTimelineWorkBuffers[static_cast<size_t>(i)];
            for (int ch = 0; ch < trackTimelineBuffer.getNumChannels(); ++ch)
                trackTimelineBuffer.clear(ch, 0, bufferToFill.numSamples);
        }
        liveMidiBuffer.clear();
        chordEngineOutputBuffer.clear();

        {
            juce::SpinLock::ScopedTryLockType previewLock(previewMidiBuffersLock);
            if (previewLock.isLocked())
            {
                for (int i = 0; i < activeTrackCount; ++i)
                {
                    auto& previewBuffer = previewMidiBuffers[static_cast<size_t>(i)];
                    if (previewBuffer.isEmpty())
                        continue;

                    trackMidiBuffers[static_cast<size_t>(i)].addEvents(previewBuffer,
                                                                       0,
                                                                       bufferToFill.numSamples,
                                                                       0);
                    previewBuffer.clear();
                }

                for (size_t i = static_cast<size_t>(activeTrackCount); i < previewMidiBuffers.size(); ++i)
                    previewMidiBuffers[i].clear();
            }
        }

        if (panicRequestedRt.exchange(false, std::memory_order_relaxed))
        {
            for (int i = 0; i < activeTrackCount; ++i)
            {
                auto& panicBuffer = trackMidiBuffers[static_cast<size_t>(i)];
                for (int ch = 1; ch <= 16; ++ch)
                {
                    panicBuffer.addEvent(juce::MidiMessage::controllerEvent(ch, 64, 0), 0);
                    panicBuffer.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
                    panicBuffer.addEvent(juce::MidiMessage::allSoundOff(ch), 0);
                }
            }
        }

        // 3. Process Live Input -> CHORD ENGINE -> Scheduler
        midiCollector.removeNextBlockOfMessages(liveMidiBuffer, bufferToFill.numSamples);
        
        for (const auto meta : liveMidiBuffer)
        {
            auto m = meta.getMessage();
            // Pass into engine, which queues notes into midiScheduler
            chordEngine.processIncoming(m, midiScheduler);
        }

        // 4. Resolve Chord Engine Events for this block
        double sampleRate = sampleRateRt.load(std::memory_order_relaxed);
        if (sampleRate <= 0.0)
            sampleRate = 44100.0;
        midiScheduler.process(bufferToFill.numSamples, sampleRate, chordEngineOutputBuffer);

        // 5. Route generated input MIDI to armed tracks, then monitored tracks, then selected fallback.
        std::array<int, maxRealtimeTracks> midiInputTargets {};
        int midiInputTargetCount = 0;

        const auto trackCanRenderMidi = [](const Track& track)
        {
            const auto channelType = track.getChannelType();
            if (channelType == Track::ChannelType::Aux || channelType == Track::ChannelType::Master)
                return false;
            if (track.isFrozenPlaybackOnly())
                return false;
            if (track.hasInstrumentPlugin())
                return true;

            const auto builtInMode = track.getBuiltInInstrumentMode();
            if (builtInMode == Track::BuiltInInstrument::BasicSynth)
                return true;
            if (builtInMode == Track::BuiltInInstrument::Sampler && track.hasSamplerSoundLoaded())
                return true;
            return false;
        };

        const auto collectTargets = [&](auto&& predicate)
        {
            for (int i = 0; i < activeTrackCount; ++i)
            {
                auto* candidate = snapshot->trackPointers[static_cast<size_t>(i)];
                if (candidate == nullptr || !predicate(*candidate))
                    continue;

                midiInputTargets[static_cast<size_t>(midiInputTargetCount)] = i;
                ++midiInputTargetCount;
                if (midiInputTargetCount >= activeTrackCount || midiInputTargetCount >= maxRealtimeTracks)
                    break;
            }
        };

        collectTargets([&trackCanRenderMidi](const Track& track) { return track.isArmed() && trackCanRenderMidi(track); });
        if (midiInputTargetCount == 0)
            collectTargets([&trackCanRenderMidi](const Track& track) { return track.isInputMonitoringEnabled() && trackCanRenderMidi(track); });

        if (midiInputTargetCount == 0)
        {
            const int selectedTrackRt = selectedTrackIndexRt.load(std::memory_order_relaxed);
            if (juce::isPositiveAndBelow(selectedTrackRt, activeTrackCount))
            {
                auto* selectedTrack = snapshot->trackPointers[static_cast<size_t>(selectedTrackRt)];
                if (selectedTrack != nullptr && trackCanRenderMidi(*selectedTrack))
                {
                    midiInputTargets[0] = selectedTrackRt;
                    midiInputTargetCount = 1;
                }
            }
        }

        if (midiInputTargetCount == 0)
        {
            for (int i = 0; i < activeTrackCount; ++i)
            {
                auto* candidate = snapshot->trackPointers[static_cast<size_t>(i)];
                if (candidate == nullptr || !trackCanRenderMidi(*candidate))
                    continue;

                midiInputTargets[0] = i;
                midiInputTargetCount = 1;
                break;
            }
        }

        // Last-resort fallback keeps MIDI usable for external routing workflows when no render-capable track is selected.
        if (midiInputTargetCount == 0)
        {
            const int selectedTrackRt = selectedTrackIndexRt.load(std::memory_order_relaxed);
            if (juce::isPositiveAndBelow(selectedTrackRt, activeTrackCount))
            {
                midiInputTargets[0] = selectedTrackRt;
                midiInputTargetCount = 1;
            }
        }

        for (int targetIdx = 0; targetIdx < midiInputTargetCount; ++targetIdx)
        {
            const auto bufferIndex = static_cast<size_t>(midiInputTargets[static_cast<size_t>(targetIdx)]);
            trackMidiBuffers[bufferIndex].addEvents(chordEngineOutputBuffer, 0, bufferToFill.numSamples, 0);
        }

        // Record generated input output with sample-accurate beat timestamps.
        if (isPlaying && recordEnabledRt.load(std::memory_order_relaxed) && midiInputTargetCount > 0)
        {
            const double beatsPerSample = transport.getBeatsPerSample();
            for (const auto meta : chordEngineOutputBuffer)
            {
                auto m = meta.getMessage();
                if (!(m.isNoteOn() || m.isNoteOff()))
                    continue;

                const double eventBeat = startBeat + (static_cast<double>(meta.samplePosition) * beatsPerSample);
                for (int targetIdx = 0; targetIdx < midiInputTargetCount; ++targetIdx)
                {
                    const auto trackIndex = static_cast<size_t>(midiInputTargets[static_cast<size_t>(targetIdx)]);
                    snapshot->trackPointers[trackIndex]->addMidiToRecord(m, eventBeat);
                }
            }
        }

        bool anySolo = false;
        for (int i = 0; i < activeTrackCount; ++i)
        {
            if (snapshot->trackPointers[static_cast<size_t>(i)]->isSolo())
            {
                anySolo = true;
                break;
            }
        }

        const auto trackIsAudible = [&](int trackIndex)
        {
            if (!juce::isPositiveAndBelow(trackIndex, activeTrackCount))
                return false;
            auto* track = snapshot->trackPointers[static_cast<size_t>(trackIndex)];
            if (track == nullptr || track->isMuted())
                return false;
            if (anySolo && !track->isSolo())
                return false;
            return true;
        };

        // 6. Gather Sequencer MIDI (From Clips)
        if (isPlaying)
        {
            const bool wrappedThisBlock = blockRange.wrapped && transport.isLooping();
            const double loopStart = transport.getLoopStartBeat();
            const double loopEnd = transport.getLoopEndBeat();
            const bool extClockActive = externalMidiClockSyncEnabledRt.load(std::memory_order_relaxed)
                                     && externalMidiClockActiveRt.load(std::memory_order_relaxed);
            const double bpmValue = extClockActive
                ? juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed))
                : resolveTempoAtBeat(startBeat, snapshot->tempoEvents, bpmRt.load(std::memory_order_relaxed));
            const int globalTranspose = juce::jlimit(-48, 48, snapshot->globalTransposeSemitones);

            for (size_t clipIdx = 0; clipIdx < snapshot->arrangement.size(); ++clipIdx)
            {
                const auto& clip = snapshot->arrangement[clipIdx];
                if (!juce::isPositiveAndBelow(clip.trackIndex, activeTrackCount))
                    continue;

                if (clip.type == ClipType::MIDI)
                {
                    if (wrappedThisBlock)
                    {
                        const auto clipTrackBufferIndex = static_cast<size_t>(clip.trackIndex);
                        clip.getEventsInRange(startBeat,
                                              loopEnd,
                                              trackMidiBuffers[clipTrackBufferIndex],
                                              bpmValue,
                                              sampleRate,
                                              bufferToFill.numSamples,
                                              chaseNotesThisBlock,
                                              1,
                                              globalTranspose);
                        clip.getEventsInRange(loopStart,
                                              endBeat,
                                              trackMidiBuffers[clipTrackBufferIndex],
                                              bpmValue,
                                              sampleRate,
                                              bufferToFill.numSamples,
                                              true,
                                              1,
                                              globalTranspose);
                    }
                    else
                    {
                        const auto clipTrackBufferIndex = static_cast<size_t>(clip.trackIndex);
                        clip.getEventsInRange(startBeat,
                                              endBeat,
                                              trackMidiBuffers[clipTrackBufferIndex],
                                              bpmValue,
                                              sampleRate,
                                              bufferToFill.numSamples,
                                              chaseNotesThisBlock,
                                              1,
                                              globalTranspose);
                    }
                }
                else if (clip.type == ClipType::Audio)
                {
                    const auto clipStream = clipIdx < snapshot->audioClipStreams.size()
                        ? snapshot->audioClipStreams[clipIdx]
                        : std::shared_ptr<StreamingClipSource>();
                    const bool hasInMemoryAudio = (clip.audioData != nullptr);
                    const bool hasDiskStream = (clipStream != nullptr && clipStream->isReady());
                    if (!hasInMemoryAudio && !hasDiskStream)
                        continue;

                    if (!trackIsAudible(clip.trackIndex))
                        continue;

                    const double clipStart = clip.startBeat;
                    const double clipEnd = clip.startBeat + juce::jmax(0.0001, clip.lengthBeats);
                    if (clipEnd <= startBeat || clipStart >= endBeat)
                        continue;

                    const double segmentStartBeat = juce::jmax(startBeat, clipStart);
                    const double segmentEndBeat = juce::jmin(endBeat, clipEnd);
                    if (segmentEndBeat <= segmentStartBeat)
                        continue;

                    const double secondsPerBeat = 60.0 / bpmValue;
                    const double outputSeconds = (segmentEndBeat - segmentStartBeat) * secondsPerBeat;
                    int targetNumSamples = static_cast<int>(std::floor(outputSeconds * sampleRate));
                    if (targetNumSamples <= 0)
                        continue;

                    const int targetStartSample = static_cast<int>(std::round((segmentStartBeat - startBeat) * secondsPerBeat * sampleRate));
                    if (targetStartSample < 0 || targetStartSample >= bufferToFill.numSamples)
                        continue;
                    targetNumSamples = juce::jmin(targetNumSamples, bufferToFill.numSamples - targetStartSample);
                    if (targetNumSamples <= 0)
                        continue;

                    const double sourceStartBeat = (segmentStartBeat - clip.startBeat) + clip.offsetBeats;
                    const double sourceSampleRate = hasDiskStream
                        ? juce::jmax(1.0, clipStream->getSampleRate())
                        : juce::jmax(1.0, clip.audioSampleRate);
                    double sourcePosition = juce::jmax(0.0, sourceStartBeat * secondsPerBeat * sourceSampleRate);
                    const double sourceIncrement = sourceSampleRate / juce::jmax(1.0, sampleRate);
                    const int clipNumSamples = hasDiskStream
                        ? static_cast<int>(juce::jmin<int64>(std::numeric_limits<int>::max(), clipStream->getNumSamples()))
                        : clip.audioData->getNumSamples();
                    const int clipNumChannels = hasDiskStream
                        ? clipStream->getNumChannels()
                        : clip.audioData->getNumChannels();
                    if (clipNumSamples <= 1 || clipNumChannels <= 0 || sourceIncrement <= 0.0)
                        continue;

                    int64 readWindowStart = 0;
                    int readWindowLength = 0;
                    if (hasDiskStream)
                    {
                        const int64 clipTotalSamples = clipStream->getNumSamples();
                        readWindowStart = juce::jlimit<int64>(0,
                                                              juce::jmax<int64>(0, clipTotalSamples - 1),
                                                              static_cast<int64>(std::floor(sourcePosition)) - clipResamplerTaps);
                        const double readWindowEndPos = sourcePosition
                                                      + (sourceIncrement * static_cast<double>(targetNumSamples + 2))
                                                      + static_cast<double>(clipResamplerTaps);
                        const int64 windowEnd = juce::jlimit<int64>(0,
                                                                     clipTotalSamples,
                                                                     static_cast<int64>(std::ceil(readWindowEndPos)) + 2);
                        readWindowLength = static_cast<int>(juce::jmax<int64>(0, windowEnd - readWindowStart));
                        if (readWindowLength <= 1
                            || audioStreamScratch.getNumChannels() < clipNumChannels
                            || audioStreamScratch.getNumSamples() < readWindowLength)
                        {
                            continue;
                        }

                        if (!clipStream->readSamples(audioStreamScratch, readWindowStart, readWindowLength))
                            continue;
                    }

                    const float baseGain = juce::jlimit(0.0f, 8.0f, clip.gainLinear);
                    const double fadeInBeats = juce::jmax(0.0, juce::jmax(clip.fadeInBeats, clip.crossfadeInBeats));
                    const double fadeOutBeats = juce::jmax(0.0, juce::jmax(clip.fadeOutBeats, clip.crossfadeOutBeats));
                    const double beatStep = bpmValue / (60.0 * juce::jmax(1.0, sampleRate));
                    double beatInClip = segmentStartBeat - clip.startBeat;
                    auto& clipTrackBuffer = trackTimelineWorkBuffers[static_cast<size_t>(clip.trackIndex)];
                    const int clipOutputChannels = clipTrackBuffer.getNumChannels();
                    if (clipOutputChannels <= 0 || clipTrackBuffer.getNumSamples() < bufferToFill.numSamples)
                        continue;
                    for (int sampleIdx = 0; sampleIdx < targetNumSamples; ++sampleIdx)
                    {
                        if (sourcePosition >= static_cast<double>(clipNumSamples - 1))
                            break;
                        const float fadeGain = computeClipFadeGain(beatInClip,
                                                                    clip.lengthBeats,
                                                                    fadeInBeats,
                                                                    fadeOutBeats);
                        const float sampleGain = baseGain * fadeGain;
                        const double beatsPerSample = beatStep;
                        const int writeIndex = targetStartSample + sampleIdx;
                        if (clipNumChannels == 1)
                        {
                            const auto* src = hasDiskStream ? audioStreamScratch.getReadPointer(0)
                                                            : clip.audioData->getReadPointer(0);
                            const double localSourcePosition = hasDiskStream
                                ? sourcePosition - static_cast<double>(readWindowStart)
                                : sourcePosition;
                            float interpolated = sampleBandlimited(src,
                                                                    hasDiskStream ? readWindowLength : clipNumSamples,
                                                                    localSourcePosition);
                            interpolated = applyMicroFadeWindow(beatInClip, clip.lengthBeats, beatsPerSample, interpolated);
                            if (clipOutputChannels > 0)
                            {
                                auto* dstL = clipTrackBuffer.getWritePointer(0);
                                dstL[writeIndex] += interpolated * sampleGain;
                            }
                            if (clipOutputChannels > 1)
                            {
                                auto* dstR = clipTrackBuffer.getWritePointer(1);
                                dstR[writeIndex] += interpolated * sampleGain;
                            }
                        }
                        else
                        {
                            const int mixChannels = juce::jmin(clipNumChannels, clipOutputChannels);
                            for (int ch = 0; ch < mixChannels; ++ch)
                            {
                                const auto* src = hasDiskStream ? audioStreamScratch.getReadPointer(ch)
                                                                : clip.audioData->getReadPointer(ch);
                                auto* dst = clipTrackBuffer.getWritePointer(ch);
                                const double localSourcePosition = hasDiskStream
                                    ? sourcePosition - static_cast<double>(readWindowStart)
                                    : sourcePosition;
                                float interpolated = sampleBandlimited(src,
                                                                        hasDiskStream ? readWindowLength : clipNumSamples,
                                                                        localSourcePosition);
                                interpolated = applyMicroFadeWindow(beatInClip, clip.lengthBeats, beatsPerSample, interpolated);
                                dst[writeIndex] += interpolated * sampleGain;
                            }
                        }

                        sourcePosition += sourceIncrement;
                        beatInClip += beatStep;
                    }
                }
            }
        }

        // 7. Process Audio Tracks
        tempMixingBuffer.clear(0, 0, bufferToFill.numSamples);
        tempMixingBuffer.clear(1, 0, bufferToFill.numSamples);
        const int bestAutoInputPair = chooseBestInputPairForCurrentBlock(capturedInputChannels, bufferToFill.numSamples);
        const bool recordingCaptureActive = transport.recording();
        const bool builtInFailSafe = builtInMicHardFailSafeRt.load(std::memory_order_relaxed);
        const bool startupGuardActive = startupSafetyBlocksRemainingRt.load(std::memory_order_relaxed) > 0;
        const bool lowLatencyProcessing = lowLatencyModeRt.load(std::memory_order_relaxed)
                                       || xrunRecoveryBlocksRt.load(std::memory_order_relaxed) > 0;
        const bool offlineRenderActive = offlineRenderActiveRt.load(std::memory_order_relaxed);
        const bool monitorSafeForTrackProcessing = monitorSafeModeRt.load(std::memory_order_relaxed);
        bool monitoredTrackInputActive = false;
        std::array<RealtimeTrackGraphJob, static_cast<size_t>(maxRealtimeTracks)> trackGraphJobs {};
        std::array<bool, static_cast<size_t>(maxRealtimeTracks)> trackGraphAudible {};
        std::array<bool, static_cast<size_t>(maxRealtimeTracks)> trackMonitorInputUsed {};
        std::array<bool, static_cast<size_t>(maxRealtimeTracks)> trackSendFeedbackBlocked {};
        std::array<bool, static_cast<size_t>(maxRealtimeTracks)> trackOutputToBus {};
        std::array<int, static_cast<size_t>(maxRealtimeTracks)> trackSendBusIndex {};
        std::array<int, static_cast<size_t>(maxRealtimeTracks)> trackOutputBusIndex {};

        for (int i = 0; i < activeTrackCount; ++i)
        {
            auto* track = snapshot->trackPointers[static_cast<size_t>(i)];
            auto& job = trackGraphJobs[static_cast<size_t>(i)];
            job.track = track;
            job.mainBuffer = &trackMainWorkBuffers[static_cast<size_t>(i)];
            job.sourceAudio = &trackTimelineWorkBuffers[static_cast<size_t>(i)];
            job.sendBuffer = &trackSendWorkBuffers[static_cast<size_t>(i)];
            job.midi = &trackMidiBuffers[static_cast<size_t>(i)];
            job.monitorInput = nullptr;
            job.blockSamples = bufferToFill.numSamples;
            job.monitorSafeInput = monitorSafeForTrackProcessing;
            job.processTrack = false;

            if (track == nullptr)
                continue;

            const bool audible = trackIsAudible(i);
            trackGraphAudible[static_cast<size_t>(i)] = audible;
            if (!audible)
                continue;

            const int sendBusIndex = juce::jlimit(0, auxBusCount - 1, track->getSendTargetBus());
            const bool outputToBus = track->getOutputTargetType() == Track::OutputTargetType::Bus;
            const int outputBusIndex = juce::jlimit(0, auxBusCount - 1, track->getOutputTargetBus());
            const bool sendFeedbackBlocked = outputToBus
                && sendBusIndex == outputBusIndex
                && track->getSendLevel() > 0.0001f;

            trackSendBusIndex[static_cast<size_t>(i)] = sendBusIndex;
            trackOutputToBus[static_cast<size_t>(i)] = outputToBus;
            trackOutputBusIndex[static_cast<size_t>(i)] = outputBusIndex;
            trackSendFeedbackBlocked[static_cast<size_t>(i)] = sendFeedbackBlocked;

            const bool allowBuiltInMonitor = !builtInFailSafe
                                             || recordingCaptureActive
                                             || track->isArmed();

            const juce::AudioBuffer<float>* monitorInput = nullptr;
            const juce::AudioBuffer<float>* recordInput = nullptr;
            auto& trackInputBuffer = trackInputWorkBuffers[static_cast<size_t>(i)];
            if (!offlineRenderActive
                && capturedInputChannels > 0
                && trackInputBuffer.getNumChannels() >= 2
                && trackInputBuffer.getNumSamples() >= bufferToFill.numSamples
                && liveInputCaptureBuffer.getNumSamples() >= bufferToFill.numSamples)
            {
                int pairIndex = track->getInputSourcePair();
                if (pairIndex < 0)
                    pairIndex = bestAutoInputPair;
                const int sourceLeft = juce::jlimit(0, capturedInputChannels - 1, pairIndex * 2);
                int sourceRight = sourceLeft + 1;
                if (sourceRight >= capturedInputChannels)
                    sourceRight = sourceLeft;

                trackInputBuffer.clear(0, 0, bufferToFill.numSamples);
                trackInputBuffer.clear(1, 0, bufferToFill.numSamples);
                const float monitorTrim = inputMonitorSafetyTrimRt.load(std::memory_order_relaxed);
                trackInputBuffer.copyFrom(0, 0, liveInputCaptureBuffer, sourceLeft, 0, bufferToFill.numSamples);
                trackInputBuffer.copyFrom(1, 0, liveInputCaptureBuffer, sourceRight, 0, bufferToFill.numSamples);
                if (std::abs(monitorTrim - 1.0f) > 0.0001f)
                {
                    trackInputBuffer.applyGain(0, 0, bufferToFill.numSamples, monitorTrim);
                    trackInputBuffer.applyGain(1, 0, bufferToFill.numSamples, monitorTrim);
                }

                recordInput = &trackInputBuffer;

                bool allowMonitorForMode = false;
                switch (inputMonitoringMode)
                {
                    case InputMonitoringMode::ArmOnly:
                        allowMonitorForMode = false;
                        break;
                    case InputMonitoringMode::MonitorOnly:
                        allowMonitorForMode = track->isInputMonitoringEnabled();
                        break;
                    case InputMonitoringMode::AutoMonitor:
                    default:
                        allowMonitorForMode = track->isArmed() && recordingCaptureActive;
                        break;
                }

                if (allowMonitorForMode
                    && !((builtInFailSafe && !allowBuiltInMonitor) || startupGuardActive))
                {
                    monitorInput = &trackInputBuffer;
                }
            }

            if (monitorInput != nullptr)
            {
                monitoredTrackInputActive = true;
                trackMonitorInputUsed[static_cast<size_t>(i)] = true;
            }
            job.monitorInput = monitorInput;
            job.processTrack = true;

            if (recordingCaptureActive
                && track->isArmed()
                && recordInput != nullptr
                && juce::isPositiveAndBelow(i, maxRealtimeTracks))
            {
                auto& take = audioTakeWriters[static_cast<size_t>(i)];
                if (take.active && take.ringFifo != nullptr
                    && recordInput->getNumChannels() >= 2
                    && recordInput->getNumSamples() >= bufferToFill.numSamples)
                {
                    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                    take.ringFifo->prepareToWrite(bufferToFill.numSamples, start1, size1, start2, size2);
                    const float* left = recordInput->getReadPointer(0);
                    const float* right = recordInput->getReadPointer(1);
                    if (left != nullptr && right != nullptr)
                    {
                        int copied = 0;
                        if (size1 > 0)
                        {
                            std::memcpy(take.ringLeft.data() + start1,
                                        left,
                                        static_cast<size_t>(size1) * sizeof(float));
                            std::memcpy(take.ringRight.data() + start1,
                                        right,
                                        static_cast<size_t>(size1) * sizeof(float));
                            copied += size1;
                        }
                        if (size2 > 0)
                        {
                            std::memcpy(take.ringLeft.data() + start2,
                                        left + copied,
                                        static_cast<size_t>(size2) * sizeof(float));
                            std::memcpy(take.ringRight.data() + start2,
                                        right + copied,
                                        static_cast<size_t>(size2) * sizeof(float));
                            copied += size2;
                        }
                        take.ringFifo->finishedWrite(copied);
                        if (copied < bufferToFill.numSamples)
                            take.droppedSamples.fetch_add(static_cast<int64>(bufferToFill.numSamples - copied), std::memory_order_relaxed);
                    }
                }
            }
        }

        const bool useParallelGraph = !offlineRenderActive
                                   && !lowLatencyProcessing
                                   && bufferToFill.numSamples >= 256
                                   && realtimeGraphScheduler.getWorkerCount() > 0
                                   && activeTrackCount >= 4;
        if (useParallelGraph)
            realtimeGraphScheduler.run(activeTrackCount, trackGraphJobs.data(), &runRealtimeTrackGraphJob);
        else
            for (int i = 0; i < activeTrackCount; ++i)
                runRealtimeTrackGraphJob(trackGraphJobs.data(), i);

        for (int i = 0; i < activeTrackCount; ++i)
        {
            const auto& job = trackGraphJobs[static_cast<size_t>(i)];
            if (!job.processTrack
                || !trackGraphAudible[static_cast<size_t>(i)]
                || job.mainBuffer == nullptr
                || job.sendBuffer == nullptr)
            {
                continue;
            }

            auto& processedTrackAudio = *job.mainBuffer;
            auto& processedTrackSend = *job.sendBuffer;

            if (builtInFailSafe && trackMonitorInputUsed[static_cast<size_t>(i)])
            {
                // Prevent live-monitor feedback loops from entering aux returns on built-in mic paths.
                processedTrackSend.clear();
            }

            if (pdcReady)
            {
                const int mainDelaySamples = juce::jmax(0,
                                                        maxGraphLatencySamples
                                                            - trackMainPathLatencySamples[static_cast<size_t>(i)]);
                const int sendDelaySamples = trackSendPathActive[static_cast<size_t>(i)]
                    ? juce::jmax(0,
                                 maxGraphLatencySamples
                                     - trackSendPathLatencySamples[static_cast<size_t>(i)])
                    : 0;
                applyTrackDelayCompensation(i,
                                            mainDelaySamples,
                                            sendDelaySamples,
                                            bufferToFill.numSamples,
                                            processedTrackAudio,
                                            processedTrackSend);
            }

            if (!trackSendFeedbackBlocked[static_cast<size_t>(i)])
            {
                auto& targetAuxBus = auxBusBuffers[static_cast<size_t>(trackSendBusIndex[static_cast<size_t>(i)])];
                const int sendChannels = juce::jmin(targetAuxBus.getNumChannels(), processedTrackSend.getNumChannels());
                for (int ch = 0; ch < sendChannels; ++ch)
                    targetAuxBus.addFrom(ch, 0, processedTrackSend, ch, 0, bufferToFill.numSamples);
            }

            if (trackOutputToBus[static_cast<size_t>(i)])
            {
                auto& outputBus = auxBusBuffers[static_cast<size_t>(trackOutputBusIndex[static_cast<size_t>(i)])];
                const int mixChannels = juce::jmin(outputBus.getNumChannels(), processedTrackAudio.getNumChannels());
                for (int ch = 0; ch < mixChannels; ++ch)
                    outputBus.addFrom(ch, 0, processedTrackAudio, ch, 0, bufferToFill.numSamples);
            }
            else
            {
                const int mixChannels = juce::jmin(tempMixingBuffer.getNumChannels(), processedTrackAudio.getNumChannels());
                for (int ch = 0; ch < mixChannels; ++ch)
                    tempMixingBuffer.addFrom(ch, 0, processedTrackAudio, ch, 0, bufferToFill.numSamples);
            }
        }
        
        // 8. Aux Effects
        const bool auxEnabled = auxFxEnabledRt.load(std::memory_order_relaxed);
        const float auxReturnLevel = auxReturnGainRt.load(std::memory_order_relaxed);
        float auxMeterMax = 0.0f;
        for (int bus = 0; bus < auxBusCount; ++bus)
        {
            auto& auxBus = auxBusBuffers[static_cast<size_t>(bus)];
            const int auxChannels = auxBus.getNumChannels();
            const bool auxCanProcess = bufferToFill.numSamples > 0
                                       && auxBus.getNumSamples() >= bufferToFill.numSamples
                                       && auxChannels > 0;
            float auxMeter = 0.0f;
            if (auxCanProcess && auxReturnLevel > 0.0001f)
            {
                sanitizeAudioBuffer(auxBus, bufferToFill.numSamples);
                bool auxProcessed = !auxEnabled;
                if (auxEnabled && lowLatencyProcessing)
                {
                    auxProcessed = true;
                }
                else if (auxEnabled && auxChannels >= 2)
                {
                    auto* left = auxBus.getWritePointer(0);
                    auto* right = auxBus.getWritePointer(1);
                    if (left != nullptr && right != nullptr)
                    {
                        auxReverbs[static_cast<size_t>(bus)].processStereo(left, right, bufferToFill.numSamples);
                        auxProcessed = true;
                    }

                    // Any channels above stereo are treated as mono for safety.
                    for (int ch = 2; ch < auxChannels; ++ch)
                    {
                        auto* mono = auxBus.getWritePointer(ch);
                        if (mono == nullptr)
                            continue;
                        auxReverbs[static_cast<size_t>(bus)].processMono(mono, bufferToFill.numSamples);
                        auxProcessed = true;
                    }
                }
                else if (auxEnabled)
                {
                    for (int ch = 0; ch < auxChannels; ++ch)
                    {
                        auto* mono = auxBus.getWritePointer(ch);
                        if (mono == nullptr)
                            continue;
                        auxReverbs[static_cast<size_t>(bus)].processMono(mono, bufferToFill.numSamples);
                        auxProcessed = true;
                    }
                }

                if (auxProcessed)
                {
                    auxBus.applyGain(auxReturnLevel);
                    sanitizeAudioBuffer(auxBus, bufferToFill.numSamples);

                    if (auxEnabled && monitorSafeModeRt.load(std::memory_order_relaxed))
                    {
                        constexpr float drive = 1.18f;
                        const float normalise = 1.0f / std::tanh(drive);
                        for (int ch = 0; ch < auxChannels; ++ch)
                        {
                            auto* write = auxBus.getWritePointer(ch);
                            if (write == nullptr)
                                continue;
                            for (int sampleIdx = 0; sampleIdx < bufferToFill.numSamples; ++sampleIdx)
                                write[sampleIdx] = std::tanh(write[sampleIdx] * drive) * normalise;
                        }
                    }

                    for (int ch = 0; ch < auxChannels; ++ch)
                        auxMeter = juce::jmax(auxMeter, auxBus.getMagnitude(ch, 0, bufferToFill.numSamples));
                }
                else
                {
                    auxBus.clear();
                }
            }
            else
            {
                auxBus.clear();
            }

            auxBusMeterRt[static_cast<size_t>(bus)].store(auxMeter, std::memory_order_relaxed);
            auxMeterMax = juce::jmax(auxMeterMax, auxMeter);
        }
        auxMeterRt.store(auxMeterMax, std::memory_order_relaxed);
        
        // 9. Metronome (short decaying click with bar accent)
        if (metronomeEnabledRt.load(std::memory_order_relaxed) && isPlaying)
        {
            const double beatsPerSample = transport.getBeatsPerSample();
            if (beatsPerSample > 0.0 && sampleRate > 0.0)
            {
                const auto addClick = [&](double beatInSegment, double segmentStartBeat)
                {
                    const int sampleOffset = static_cast<int>(std::round((beatInSegment - segmentStartBeat) / beatsPerSample));
                    if (!juce::isPositiveAndBelow(sampleOffset, bufferToFill.numSamples))
                        return;

                    const int64_t beatIndex = static_cast<int64_t>(std::llround(std::floor(beatInSegment + 1.0e-6)));
                    const bool accent = (beatIndex % 4) == 0;
                    const float gain = accent ? 0.34f : 0.23f;
                    const double frequency = accent ? 1850.0 : 1320.0;
                    const int clickSamples = juce::jmin(bufferToFill.numSamples - sampleOffset,
                                                        juce::jmax(12, static_cast<int>(sampleRate * 0.014)));

                    for (int i = 0; i < clickSamples; ++i)
                    {
                        const double phase = (static_cast<double>(i) * frequency * juce::MathConstants<double>::twoPi) / sampleRate;
                        const float env = std::exp(-6.5f * static_cast<float>(i) / static_cast<float>(clickSamples));
                        const float sampleValue = static_cast<float>(std::sin(phase)) * env * gain;
                        for (int ch = 0; ch < tempMixingBuffer.getNumChannels(); ++ch)
                            tempMixingBuffer.addSample(ch, sampleOffset + i, sampleValue);
                    }
                };

                const auto addClicksForSegment = [&](double segmentStartBeat, double segmentEndBeat)
                {
                    if (segmentEndBeat <= segmentStartBeat + 1.0e-9)
                        return;

                    double beat = std::ceil(segmentStartBeat - 1.0e-9);
                    if (std::abs(segmentStartBeat - std::round(segmentStartBeat)) < 1.0e-6)
                        beat = std::round(segmentStartBeat);

                    for (; beat < segmentEndBeat - 1.0e-9; beat += 1.0)
                        addClick(beat, segmentStartBeat);
                };

                if (blockRange.wrapped && transport.isLooping())
                {
                    addClicksForSegment(startBeat, transport.getLoopEndBeat());
                    addClicksForSegment(transport.getLoopStartBeat(), endBeat);
                }
                else
                {
                    addClicksForSegment(startBeat, endBeat);
                }
            }
        }

        // 10. Final Sum
        const int outputChannels = juce::jmin(tempMixingBuffer.getNumChannels(), bufferToFill.buffer->getNumChannels());
        sanitizeAudioBuffer(tempMixingBuffer, bufferToFill.numSamples);
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            bufferToFill.buffer->addFrom(ch, bufferToFill.startSample, tempMixingBuffer, ch, 0, bufferToFill.numSamples);
            for (int bus = 0; bus < auxBusCount; ++bus)
            {
                const auto& auxBus = auxBusBuffers[static_cast<size_t>(bus)];
                if (ch < auxBus.getNumChannels())
                    bufferToFill.buffer->addFrom(ch, bufferToFill.startSample, auxBus, ch, 0, bufferToFill.numSamples);
            }
        }
        const int startupRampRemaining = startupOutputRampSamplesRemainingRt.load(std::memory_order_relaxed);
        if (startupRampRemaining > 0)
        {
            if (!startupSafetyRampLoggedRt.exchange(true, std::memory_order_relaxed))
            {
                juce::Logger::writeToLog("Startup safety: output ramp active (samples remaining="
                                         + juce::String(startupRampRemaining) + ")");
            }
            const int startupRampTotal = juce::jmax(1, startupOutputRampTotalSamplesRt.load(std::memory_order_relaxed));
            const int nextRampRemaining = juce::jmax(0, startupRampRemaining - bufferToFill.numSamples);
            const float startGain = juce::jlimit(0.0f,
                                                 1.0f,
                                                 1.0f - (static_cast<float>(startupRampRemaining) / static_cast<float>(startupRampTotal)));
            const float endGain = juce::jlimit(0.0f,
                                               1.0f,
                                               1.0f - (static_cast<float>(nextRampRemaining) / static_cast<float>(startupRampTotal)));
            for (int ch = 0; ch < outputChannels; ++ch)
            {
                bufferToFill.buffer->applyGainRamp(ch,
                                                   bufferToFill.startSample,
                                                   bufferToFill.numSamples,
                                                   startGain,
                                                   endGain);
            }
            startupOutputRampSamplesRemainingRt.store(nextRampRemaining, std::memory_order_relaxed);
        }
        if (startupSafetyBlocksRemainingRt.load(std::memory_order_relaxed) > 0)
            startupSafetyBlocksRemainingRt.fetch_sub(1, std::memory_order_relaxed);

        const float targetMasterGain = masterOutputGainRt.load(std::memory_order_relaxed);
        const bool useSoftClip = masterSoftClipEnabledRt.load(std::memory_order_relaxed);
        const bool limiterEnabled = masterLimiterEnabledRt.load(std::memory_order_relaxed);
        constexpr float softClipDrive = 1.34f;
        const float softClipNormaliser = std::tanh(softClipDrive);
        constexpr float limiterCeiling = 0.972f;
        constexpr float limiterAttack = 0.45f;
        constexpr float limiterRelease = 0.0015f;
        const float dezipperCoeff = masterGainDezipperCoeff > 0.0f
            ? masterGainDezipperCoeff
            : 0.0015f;

        for (int i = 0; i < bufferToFill.numSamples; ++i)
        {
            masterGainSmoothingState += (targetMasterGain - masterGainSmoothingState) * dezipperCoeff;

            float overPeak = 0.0f;
            for (int ch = 0; ch < outputChannels; ++ch)
            {
                auto* write = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                if (write == nullptr)
                    continue;

                float sample = write[i] * masterGainSmoothingState;
                if (useSoftClip)
                {
                    auto& prevIn = masterTruePeakMidpointPrevInput[static_cast<size_t>(juce::jmin(ch, 1))];
                    sample = processSoftClipOversampled2x(sample, softClipDrive, softClipNormaliser, prevIn);
                }

                write[i] = sample;
                const float midpoint = 0.5f * (masterLimiterPrevInput[static_cast<size_t>(juce::jmin(ch, 1))] + sample);
                overPeak = juce::jmax(overPeak, std::abs(sample));
                overPeak = juce::jmax(overPeak, std::abs(midpoint));
            }

            if (limiterEnabled)
            {
                const float targetGain = overPeak > limiterCeiling ? (limiterCeiling / overPeak) : 1.0f;
                if (targetGain < masterLimiterGainState)
                    masterLimiterGainState += (targetGain - masterLimiterGainState) * limiterAttack;
                else
                    masterLimiterGainState += (targetGain - masterLimiterGainState) * limiterRelease;
            }
            else
            {
                masterLimiterGainState += (1.0f - masterLimiterGainState) * 0.01f;
            }

            for (int ch = 0; ch < outputChannels; ++ch)
            {
                auto* write = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                if (write == nullptr)
                    continue;

                float limited = write[i] * masterLimiterGainState;
                limited = juce::jlimit(-limiterCeiling, limiterCeiling, limited);
                if (std::abs(limited) < 1.0e-24f)
                    limited = 0.0f;
                write[i] = limited;
                masterLimiterPrevInput[static_cast<size_t>(juce::jmin(ch, 1))] = limited;
            }
        }

        if (!limiterEnabled)
        {
            masterLimiterPrevInput = { 0.0f, 0.0f };
            masterLimiterGainState = 1.0f;
        }

        if (!useSoftClip)
            masterTruePeakMidpointPrevInput = { 0.0f, 0.0f };

        if (outputDcHighPassEnabledRt.load(std::memory_order_relaxed))
        {
            constexpr float dcBlockCoeff = 0.995f;
            for (int ch = 0; ch < juce::jmin(outputChannels, 2); ++ch)
            {
                auto* write = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
                if (write == nullptr)
                    continue;

                float prevIn = outputDcPrevInput[static_cast<size_t>(ch)];
                float prevOut = outputDcPrevOutput[static_cast<size_t>(ch)];
                for (int i = 0; i < bufferToFill.numSamples; ++i)
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
        else
        {
            outputDcPrevInput = { 0.0f, 0.0f };
            outputDcPrevOutput = { 0.0f, 0.0f };
        }

        if (builtInFailSafe && monitoredTrackInputActive && !isPlaying && !recordingCaptureActive)
        {
            for (int ch = 0; ch < outputChannels; ++ch)
                bufferToFill.buffer->applyGain(ch, bufferToFill.startSample, bufferToFill.numSamples, 0.0f);
        }

        bool severeOutputFault = false;
        constexpr float hardOutputClamp = 1.25f;
        constexpr float faultThreshold = 24.0f;
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            auto* write = bufferToFill.buffer->getWritePointer(ch, bufferToFill.startSample);
            if (write == nullptr)
                continue;

            for (int i = 0; i < bufferToFill.numSamples; ++i)
            {
                float sample = write[i];
                if (!std::isfinite(sample) || std::abs(sample) > faultThreshold)
                {
                    severeOutputFault = true;
                    sample = 0.0f;
                }
                else
                {
                    sample = juce::jlimit(-hardOutputClamp, hardOutputClamp, sample);
                }
                write[i] = sample;
            }
        }

        if (severeOutputFault)
        {
            if (!startupSafetyFaultLoggedRt.exchange(true, std::memory_order_relaxed))
            {
                juce::Logger::writeToLog("Startup/output safety: severe output fault detected; safety mute + guard extension applied.");
            }
            audioGuardDropCountRt.fetch_add(1, std::memory_order_relaxed);
            feedbackAutoMuteRequestedRt.store(true, std::memory_order_relaxed);
            const int currentMuteBlocks = outputSafetyMuteBlocksRt.load(std::memory_order_relaxed);
            outputSafetyMuteBlocksRt.store(juce::jmax(currentMuteBlocks, 96), std::memory_order_relaxed);
            const int currentGuard = startupSafetyBlocksRemainingRt.load(std::memory_order_relaxed);
            startupSafetyBlocksRemainingRt.store(juce::jmax(currentGuard, 24), std::memory_order_relaxed);
            const int recoveryRampSamples = juce::jmax(256, bufferToFill.numSamples * 4);
            startupOutputRampTotalSamplesRt.store(recoveryRampSamples, std::memory_order_relaxed);
            startupOutputRampSamplesRemainingRt.store(recoveryRampSamples, std::memory_order_relaxed);
            startupSafetyRampLoggedRt.store(false, std::memory_order_relaxed);
        }

        float masterPeak = 0.0f;
        float masterRms = 0.0f;
        for (int ch = 0; ch < outputChannels; ++ch)
        {
            masterPeak = juce::jmax(masterPeak,
                                    bufferToFill.buffer->getMagnitude(ch, bufferToFill.startSample, bufferToFill.numSamples));
            masterRms = juce::jmax(masterRms,
                                   bufferToFill.buffer->getRMSLevel(ch, bufferToFill.startSample, bufferToFill.numSamples));
        }

        const float* leftOut = outputChannels > 0
            ? bufferToFill.buffer->getReadPointer(0, bufferToFill.startSample)
            : nullptr;
        const float* rightOut = outputChannels > 1
            ? bufferToFill.buffer->getReadPointer(1, bufferToFill.startSample)
            : leftOut;

        double lrDot = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        double monoSquareSum = 0.0;
        int analyzerWritePos = masterAnalyzerBuildPos;
        int analyzerWriteSnapshot = masterAnalyzerWriteSnapshot;
        if (leftOut != nullptr)
        {
            for (int i = 0; i < bufferToFill.numSamples; ++i)
            {
                const float leftSample = leftOut[i];
                const float rightSample = rightOut != nullptr ? rightOut[i] : leftSample;
                lrDot += static_cast<double>(leftSample) * static_cast<double>(rightSample);
                leftEnergy += static_cast<double>(leftSample) * static_cast<double>(leftSample);
                rightEnergy += static_cast<double>(rightSample) * static_cast<double>(rightSample);

                const float mono = outputChannels > 1
                    ? (0.5f * (leftSample + rightSample))
                    : leftSample;
                monoSquareSum += static_cast<double>(mono) * static_cast<double>(mono);

                if (!lowLatencyProcessing)
                {
                    masterAnalyzerBuildBuffer[static_cast<size_t>(analyzerWritePos)] = mono;
                    ++analyzerWritePos;
                    if (analyzerWritePos >= monitorAnalyzerFftSize)
                    {
                        std::memcpy(masterAnalyzerSnapshots[static_cast<size_t>(analyzerWriteSnapshot)].data(),
                                    masterAnalyzerBuildBuffer.data(),
                                    sizeof(float) * static_cast<size_t>(monitorAnalyzerFftSize));
                        masterAnalyzerReadySnapshot.store(analyzerWriteSnapshot, std::memory_order_release);
                        analyzerWriteSnapshot = analyzerWriteSnapshot == 0 ? 1 : 0;
                        analyzerWritePos = 0;
                    }
                }
            }
        }
        masterAnalyzerBuildPos = analyzerWritePos;
        masterAnalyzerWriteSnapshot = analyzerWriteSnapshot;

        float phaseCorrelation = 1.0f;
        if (outputChannels >= 2)
        {
            const double denom = std::sqrt((leftEnergy * rightEnergy) + 1.0e-12);
            phaseCorrelation = denom > 1.0e-9
                ? static_cast<float>(juce::jlimit(-1.0, 1.0, lrDot / denom))
                : 0.0f;
        }
        const float previousPhase = masterPhaseCorrelationRt.load(std::memory_order_relaxed);
        masterPhaseCorrelationRt.store(previousPhase + ((phaseCorrelation - previousPhase) * 0.22f),
                                       std::memory_order_relaxed);

        float loudnessLufs = -120.0f;
        if (monoSquareSum > 0.0 && bufferToFill.numSamples > 0)
        {
            const float monoRms = static_cast<float>(std::sqrt(monoSquareSum / static_cast<double>(bufferToFill.numSamples)));
            const float dbFs = juce::Decibels::gainToDecibels(juce::jmax(1.0e-6f, monoRms), -120.0f);
            loudnessLufs = juce::jlimit(-120.0f, 6.0f, dbFs - 0.69f); // Approximate LUFS from output RMS.
        }
        const float previousLoudness = masterLoudnessLufsRt.load(std::memory_order_relaxed);
        masterLoudnessLufsRt.store(previousLoudness + ((loudnessLufs - previousLoudness) * 0.08f),
                                   std::memory_order_relaxed);

        const float previousPeak = masterPeakMeterRt.load(std::memory_order_relaxed);
        const float displayPeak = (masterPeak > previousPeak)
            ? masterPeak
            : juce::jmax(masterPeak, previousPeak * 0.94f);
        masterPeakMeterRt.store(displayPeak, std::memory_order_relaxed);

        const float previousRms = masterRmsMeterRt.load(std::memory_order_relaxed);
        masterRmsMeterRt.store(previousRms + ((masterRms - previousRms) * 0.20f), std::memory_order_relaxed);

        if (builtInFailSafe && monitoredTrackInputActive && !isPlaying && !recordingCaptureActive)
        {
            const float liveInputPeak = liveInputPeakRt.load(std::memory_order_relaxed);
            const bool hazardous = masterPeak > 0.96f
                                && masterRms > 0.32f
                                && liveInputPeak > 0.06f;
            int hazardBlocks = feedbackHazardBlocksRt.load(std::memory_order_relaxed);
            hazardBlocks = hazardous
                ? juce::jmin(512, hazardBlocks + 1)
                : juce::jmax(0, hazardBlocks - 2);
            feedbackHazardBlocksRt.store(hazardBlocks, std::memory_order_relaxed);
            if (hazardBlocks >= 8)
            {
                feedbackHazardBlocksRt.store(0, std::memory_order_relaxed);
                feedbackAutoMuteRequestedRt.store(true, std::memory_order_relaxed);
                const int currentMuteBlocks = outputSafetyMuteBlocksRt.load(std::memory_order_relaxed);
                outputSafetyMuteBlocksRt.store(juce::jmax(currentMuteBlocks, 96), std::memory_order_relaxed);
            }
        }
        else
        {
            const int hazardBlocks = feedbackHazardBlocksRt.load(std::memory_order_relaxed);
            if (hazardBlocks > 0)
                feedbackHazardBlocksRt.store(hazardBlocks - 1, std::memory_order_relaxed);
        }

        if (masterPeak >= 0.995f)
            masterClipHoldRt.store(45, std::memory_order_relaxed);
        else
        {
            const int clipHold = masterClipHoldRt.load(std::memory_order_relaxed);
            if (clipHold > 0)
                masterClipHoldRt.store(clipHold - 1, std::memory_order_relaxed);
        }

        wasTransportPlayingLastBlock = isPlaying;
    }

    void MainComponent::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                          int numInputChannels,
                                                          float* const* outputChannelData,
                                                          int numOutputChannels,
                                                          int numSamples,
                                                          const juce::AudioIODeviceCallbackContext&)
    {
        juce::ignoreUnused(outputChannelData, numOutputChannels);
        if (numSamples <= 0)
            return;

        const int writeIndex = inputTapReadyIndex.load(std::memory_order_relaxed) == 0 ? 1 : 0;
        if (!juce::isPositiveAndBelow(writeIndex, static_cast<int>(inputTapBuffers.size())))
        {
            audioGuardDropCountRt.fetch_add(1, std::memory_order_relaxed);
            audioXrunCountRt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto& tapBuffer = inputTapBuffers[static_cast<size_t>(writeIndex)];
        if (tapBuffer.getNumChannels() <= 0 || tapBuffer.getNumSamples() < numSamples)
        {
            audioGuardDropCountRt.fetch_add(1, std::memory_order_relaxed);
            audioXrunCountRt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const int channelsToCopy = juce::jmin(numInputChannels, tapBuffer.getNumChannels());
        for (int ch = 0; ch < channelsToCopy; ++ch)
        {
            auto* dst = tapBuffer.getWritePointer(ch);
            const float* src = (inputChannelData != nullptr) ? inputChannelData[ch] : nullptr;
            if (dst == nullptr)
                continue;
            if (src != nullptr)
                std::memcpy(dst, src, static_cast<size_t>(numSamples) * sizeof(float));
            else
                juce::FloatVectorOperations::clear(dst, numSamples);
        }
        for (int ch = channelsToCopy; ch < tapBuffer.getNumChannels(); ++ch)
        {
            if (auto* dst = tapBuffer.getWritePointer(ch))
                juce::FloatVectorOperations::clear(dst, numSamples);
        }

        inputTapNumChannels[static_cast<size_t>(writeIndex)].store(channelsToCopy, std::memory_order_relaxed);
        inputTapNumSamples[static_cast<size_t>(writeIndex)].store(numSamples, std::memory_order_relaxed);
        inputTapReadyIndex.store(writeIndex, std::memory_order_release);
        activeInputChannelCountRt.store(channelsToCopy, std::memory_order_relaxed);
    }

    void MainComponent::audioDeviceAboutToStart(juce::AudioIODevice* device)
    {
        if (device == nullptr)
            return;

        const int activeInputChannels = juce::jmax(0, device->getActiveInputChannels().countNumberOfSetBits());
        const int channels = juce::jmax(1, activeInputChannels);
        const int samples = juce::jmax(maxRealtimeBlockSize, device->getCurrentBufferSizeSamples());
        const double sr = juce::jmax(1.0, device->getCurrentSampleRate());
        const int startupGuardBlocks = juce::jmax(6, static_cast<int>(std::ceil((sr * 0.45) / static_cast<double>(samples))));
        const int startupRampSamples = juce::jmax(512, static_cast<int>(std::round(sr * 0.35)));
        for (auto& tapBuffer : inputTapBuffers)
            tapBuffer.setSize(channels, samples, false, false, true);
        inputTapReadyIndex.store(-1, std::memory_order_relaxed);
        activeInputChannelCountRt.store(activeInputChannels, std::memory_order_relaxed);
        startupSafetyBlocksRemainingRt.store(startupGuardBlocks, std::memory_order_relaxed);
        startupOutputRampTotalSamplesRt.store(startupRampSamples, std::memory_order_relaxed);
        startupOutputRampSamplesRemainingRt.store(startupRampSamples, std::memory_order_relaxed);
        startupSafetyRampLoggedRt.store(false, std::memory_order_relaxed);
        startupSafetyFaultLoggedRt.store(false, std::memory_order_relaxed);
        feedbackHazardBlocksRt.store(0, std::memory_order_relaxed);
        feedbackAutoMuteRequestedRt.store(false, std::memory_order_relaxed);
        outputSafetyMuteBlocksRt.store(0, std::memory_order_relaxed);
        outputDcPrevInput = { 0.0f, 0.0f };
        outputDcPrevOutput = { 0.0f, 0.0f };
        masterLimiterPrevInput = { 0.0f, 0.0f };
        masterTruePeakMidpointPrevInput = { 0.0f, 0.0f };
        masterLimiterGainState = 1.0f;
        lastAudioDeviceNameSeen = device->getName();
        refreshInputDeviceSafetyState();
    }

    void MainComponent::audioDeviceStopped()
    {
        inputTapReadyIndex.store(-1, std::memory_order_relaxed);
        activeInputChannelCountRt.store(0, std::memory_order_relaxed);
        inputMonitorSafetyTrimRt.store(1.0f, std::memory_order_relaxed);
        usingLikelyBuiltInAudioRt.store(false, std::memory_order_relaxed);
        builtInMicHardFailSafeRt.store(false, std::memory_order_relaxed);
        startupSafetyBlocksRemainingRt.store(0, std::memory_order_relaxed);
        startupOutputRampTotalSamplesRt.store(1, std::memory_order_relaxed);
        startupOutputRampSamplesRemainingRt.store(0, std::memory_order_relaxed);
        startupSafetyRampLoggedRt.store(false, std::memory_order_relaxed);
        startupSafetyFaultLoggedRt.store(false, std::memory_order_relaxed);
        feedbackHazardBlocksRt.store(0, std::memory_order_relaxed);
        feedbackAutoMuteRequestedRt.store(false, std::memory_order_relaxed);
        outputSafetyMuteBlocksRt.store(0, std::memory_order_relaxed);
        outputDcPrevInput = { 0.0f, 0.0f };
        outputDcPrevOutput = { 0.0f, 0.0f };
        masterLimiterPrevInput = { 0.0f, 0.0f };
        masterTruePeakMidpointPrevInput = { 0.0f, 0.0f };
        masterLimiterGainState = 1.0f;
        for (auto& tapInfo : inputTapNumChannels)
            tapInfo.store(0, std::memory_order_relaxed);
        for (auto& tapInfo : inputTapNumSamples)
            tapInfo.store(0, std::memory_order_relaxed);
    }

    void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
    {
        const auto sourceIdentifier = getMidiSourceIdentifier(source);
        juce::String controlSurfaceIdentifier;
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            controlSurfaceIdentifier = activeControlSurfaceInputIdentifier;
        }
        const bool fromControlSurface = controlSurfaceIdentifier.isNotEmpty()
                                     && sourceIdentifier == controlSurfaceIdentifier;

        const bool isSyncRelatedMessage = message.isMidiClock()
                                       || message.isMidiStart()
                                       || message.isMidiContinue()
                                       || message.isMidiStop()
                                       || message.isSongPositionPointer()
                                       || message.isQuarterFrame()
                                       || message.isFullFrame();

        if (message.isController())
        {
            if (midiLearnArmed)
            {
                auto messageCopy = message;
                auto sourceCopy = sourceIdentifier;
                juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this),
                                                 sourceCopy,
                                                 messageCopy]
                {
                    if (safeThis != nullptr)
                        safeThis->captureMidiLearnMapping(sourceCopy, messageCopy);
                });
                if (fromControlSurface)
                    return;
            }
            else
            {
                applyMappedMidiLearn(sourceIdentifier, message);
            }
        }

        if (fromControlSurface)
        {
            handleControlSurfaceMidi(message);
            if (!isSyncRelatedMessage)
                return;
        }
        else
        {
            midiCollector.addMessageToQueue(message);

            const bool isMusicalMessage = message.isNoteOn()
                                       || message.isNoteOff()
                                       || message.isController()
                                       || message.isAftertouch()
                                       || message.isChannelPressure()
                                       || message.isPitchWheel()
                                       || message.isProgramChange();
            if (isMusicalMessage
                && midiThruEnabledRt.load(std::memory_order_relaxed)
                && midiOutputActiveRt.load(std::memory_order_relaxed))
                midiRouter.sendNow(message);
        }

        const double nowMs = juce::Time::getMillisecondCounterHiRes();

        if (message.isMidiClock() || message.isMidiStart() || message.isMidiContinue() || message.isMidiStop())
        {
            lastMidiClockMessageMs.store(nowMs, std::memory_order_relaxed);
            transport.setSyncSource(TransportEngine::SyncSource::MidiClock);
        }

        if (message.isQuarterFrame() || message.isFullFrame())
        {
            lastMtcMessageMs.store(nowMs, std::memory_order_relaxed);
            transport.setSyncSource(TransportEngine::SyncSource::MidiTimecode);
        }

        if (!externalMidiClockSyncEnabledRt.load(std::memory_order_relaxed))
            return;

        if (message.isSongPositionPointer())
        {
            const double songBeat = static_cast<double>(message.getSongPositionPointerMidiBeat()) * 0.25;
            const int tickCounter = juce::jmax(0, static_cast<int>(std::llround(songBeat * 24.0)));
            externalMidiClockTickCounterRt.store(tickCounter, std::memory_order_relaxed);
            externalMidiClockBeatOffsetRt.store(songBeat - (static_cast<double>(tickCounter) / 24.0),
                                                std::memory_order_relaxed);
            externalMidiClockBeatRt.store(juce::jmax(0.0, songBeat), std::memory_order_relaxed);
            externalMidiClockActiveRt.store(true, std::memory_order_relaxed);
            externalMidiClockGenerationRt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (message.isMidiStart())
        {
            externalMidiClockTickCounterRt.store(0, std::memory_order_relaxed);
            externalMidiClockBeatOffsetRt.store(0.0, std::memory_order_relaxed);
            externalMidiClockBeatRt.store(0.0, std::memory_order_relaxed);
            externalMidiClockTransportRunningRt.store(true, std::memory_order_relaxed);
            externalMidiClockActiveRt.store(true, std::memory_order_relaxed);
            externalMidiClockLastTickMsRt.store(-1.0, std::memory_order_relaxed);
            externalMidiClockGenerationRt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (message.isMidiContinue())
        {
            const int ticks = juce::jmax(0, externalMidiClockTickCounterRt.load(std::memory_order_relaxed));
            const double currentBeat = externalMidiClockBeatRt.load(std::memory_order_relaxed);
            externalMidiClockBeatOffsetRt.store(currentBeat - (static_cast<double>(ticks) / 24.0),
                                                std::memory_order_relaxed);
            externalMidiClockTransportRunningRt.store(true, std::memory_order_relaxed);
            externalMidiClockActiveRt.store(true, std::memory_order_relaxed);
            externalMidiClockGenerationRt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (message.isMidiStop())
        {
            externalMidiClockTransportRunningRt.store(false, std::memory_order_relaxed);
            externalMidiClockActiveRt.store(true, std::memory_order_relaxed);
            externalMidiClockGenerationRt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (message.isMidiClock())
        {
            const double previousTickMs = externalMidiClockLastTickMsRt.exchange(nowMs, std::memory_order_relaxed);
            double tempoBpm = externalMidiClockTempoRt.load(std::memory_order_relaxed);
            if (previousTickMs > 0.0 && nowMs > previousTickMs)
            {
                const double tickDeltaMs = nowMs - previousTickMs;
                if (tickDeltaMs > 0.2 && tickDeltaMs < 500.0)
                {
                    const double instantBpm = 60000.0 / (tickDeltaMs * 24.0);
                    tempoBpm = juce::jmax(1.0, (tempoBpm * 0.82) + (instantBpm * 0.18));
                    externalMidiClockTempoRt.store(tempoBpm, std::memory_order_relaxed);
                }
            }

            const int nextTicks = juce::jmax(0, externalMidiClockTickCounterRt.load(std::memory_order_relaxed) + 1);
            externalMidiClockTickCounterRt.store(nextTicks, std::memory_order_relaxed);
            const double beatOffset = externalMidiClockBeatOffsetRt.load(std::memory_order_relaxed);
            const double beat = juce::jmax(0.0, beatOffset + (static_cast<double>(nextTicks) / 24.0));
            externalMidiClockBeatRt.store(beat, std::memory_order_relaxed);
            externalMidiClockActiveRt.store(true, std::memory_order_relaxed);
            externalMidiClockGenerationRt.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void MainComponent::releaseResources()
    {
        for (auto* t : tracks)
            t->releaseResources();

        masterPhaseCorrelationRt.store(0.0f, std::memory_order_relaxed);
        masterLoudnessLufsRt.store(-120.0f, std::memory_order_relaxed);
        masterAnalyzerReadySnapshot.store(-1, std::memory_order_relaxed);
        masterAnalyzerWriteSnapshot = 0;
        masterAnalyzerBuildPos = 0;
        masterAnalyzerBuildBuffer.fill(0.0f);
        for (auto& snapshot : masterAnalyzerSnapshots)
            snapshot.fill(0.0f);
    }
    bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
    {
        for (const auto& path : files)
        {
            const juce::File candidate(path);
            if (candidate.isDirectory())
                return true;
            if (candidate.hasFileExtension("sampledex"))
                return true;
            if (isSupportedAudioFile(candidate) || isSupportedMidiFile(candidate))
                return true;
        }
        return false;
    }

    void MainComponent::filesDropped(const juce::StringArray& files, int, int)
    {
        if (files.isEmpty() || tracks.isEmpty())
            return;

        if (files.size() == 1)
        {
            const juce::File maybeProject(files[0]);
            if (maybeProject.existsAsFile() && maybeProject.hasFileExtension("sampledex"))
            {
                loadProjectFromFile(maybeProject);
                return;
            }
        }

        juce::Array<juce::File> flattenedFiles;
        for (const auto& path : files)
            collectDroppedFilesRecursively(juce::File(path), flattenedFiles);

        if (flattenedFiles.isEmpty())
            return;

        std::sort(flattenedFiles.begin(), flattenedFiles.end(),
                  [](const juce::File& a, const juce::File& b)
                  {
                      return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0;
                  });

        bool arrangementChanged = false;
        bool importedMidiClip = false;
        int firstImportedClipIndex = -1;
        double importedTempo = 0.0;
        double importBeatCursor = transport.getCurrentBeat();
        const int selectedTargetTrack = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), selectedTrackIndex);
        const int audioTargetTrack = selectedTargetTrack;
        const bool hasMidiFiles = std::any_of(flattenedFiles.begin(),
                                              flattenedFiles.end(),
                                              [this](const juce::File& candidate)
                                              {
                                                  return isSupportedMidiFile(candidate);
                                              });
        const int midiTargetTrack = hasMidiFiles ? resolveMidiImportTargetTrack(selectedTargetTrack, true) : -1;

        for (const auto& file : flattenedFiles)
        {
            int clipIndex = -1;
            if (isSupportedMidiFile(file))
            {
                if (!juce::isPositiveAndBelow(midiTargetTrack, tracks.size()))
                    continue;
                double midiTempo = 0.0;
                if (importMidiFileToClip(file, midiTargetTrack, importBeatCursor, clipIndex, midiTempo))
                {
                    arrangementChanged = true;
                    importedMidiClip = true;
                    if (importedTempo <= 0.0 && midiTempo > 0.0)
                        importedTempo = midiTempo;
                }
            }
            else if (isSupportedAudioFile(file))
            {
                double detectedTempo = 0.0;
                if (importAudioFileToClip(file, audioTargetTrack, importBeatCursor, clipIndex, detectedTempo))
                {
                    arrangementChanged = true;
                    if (importedTempo <= 0.0 && detectedTempo > 0.0)
                        importedTempo = detectedTempo;
                }
            }

            if (clipIndex >= 0 && juce::isPositiveAndBelow(clipIndex, static_cast<int>(arrangement.size())))
            {
                if (firstImportedClipIndex < 0)
                    firstImportedClipIndex = clipIndex;
                importBeatCursor += juce::jmax(0.25, arrangement[static_cast<size_t>(clipIndex)].lengthBeats);
            }
        }

        if (!arrangementChanged)
            return;

        if (importedTempo > 1.0)
            setTempoBpm(importedTempo);

        rebuildRealtimeSnapshot();
        setSelectedClipIndex(firstImportedClipIndex, importedMidiClip);
        resized();
        refreshStatusText();
    }

    bool MainComponent::isSupportedAudioFile(const juce::File& file) const
    {
        return file.hasFileExtension("wav;aiff;aif;flac;mp3;ogg;m4a;caf");
    }

    bool MainComponent::isSupportedMidiFile(const juce::File& file) const
    {
        return file.hasFileExtension("mid;midi");
    }

    void MainComponent::collectDroppedFilesRecursively(const juce::File& candidate, juce::Array<juce::File>& output) const
    {
        if (!candidate.exists())
            return;

        if (candidate.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator(candidate, true, "*", juce::File::findFiles))
                output.addIfNotAlreadyThere(entry.getFile());
            return;
        }

        if (candidate.existsAsFile())
            output.addIfNotAlreadyThere(candidate);
    }

    double MainComponent::tryExtractTempoFromFilename(const juce::String& fileName) const
    {
        const auto lower = fileName.toLowerCase();
        const int bpmPos = lower.indexOf("bpm");
        if (bpmPos > 0)
        {
            int start = bpmPos - 1;
            while (start >= 0)
            {
                const juce::juce_wchar c = lower[start];
                if (!juce::CharacterFunctions::isDigit(c) && c != '.')
                    break;
                --start;
            }

            const auto bpmText = lower.substring(start + 1, bpmPos).retainCharacters("0123456789.");
            const double parsed = bpmText.getDoubleValue();
            if (parsed >= 40.0 && parsed <= 260.0)
                return parsed;
        }

        juce::StringArray tokens;
        tokens.addTokens(lower, " _-()[]{}", "");
        tokens.removeEmptyStrings();
        for (const auto& token : tokens)
        {
            if (!token.containsOnly("0123456789."))
                continue;
            const double parsed = token.getDoubleValue();
            if (parsed >= 40.0 && parsed <= 260.0)
                return parsed;
        }

        return 0.0;
    }

    int MainComponent::resolveMidiImportTargetTrack(int preferredTrackIndex, bool createTrackIfNeeded)
    {
        if (tracks.isEmpty())
            return -1;

        const int preferred = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), preferredTrackIndex);
        const auto isInstrumentCapable = [](Track* track)
        {
            if (track == nullptr)
                return false;

            const auto channelType = track->getChannelType();
            return channelType != Track::ChannelType::Aux
                && channelType != Track::ChannelType::Master;
        };

        const auto hasPlayableMidiSource = [&isInstrumentCapable](Track* track)
        {
            if (!isInstrumentCapable(track))
                return false;

            if (track->hasInstrumentPlugin())
                return true;

            const auto builtInMode = track->getBuiltInInstrumentMode();
            if (builtInMode == Track::BuiltInInstrument::BasicSynth)
                return true;
            if (builtInMode == Track::BuiltInInstrument::Sampler && track->hasSamplerSoundLoaded())
                return true;
            return false;
        };

        int resolved = -1;
        if (juce::isPositiveAndBelow(preferred, tracks.size()) && hasPlayableMidiSource(tracks[preferred]))
            resolved = preferred;

        if (resolved < 0
            && juce::isPositiveAndBelow(preferred, tracks.size())
            && isInstrumentCapable(tracks[preferred])
            && tracks[preferred]->getChannelType() == Track::ChannelType::Instrument)
        {
            resolved = preferred;
        }

        if (resolved < 0)
        {
            for (int i = 0; i < tracks.size(); ++i)
            {
                if (hasPlayableMidiSource(tracks[i]))
                {
                    resolved = i;
                    break;
                }
            }
        }

        if (resolved < 0)
        {
            for (int i = 0; i < tracks.size(); ++i)
            {
                if (tracks[i] != nullptr && tracks[i]->getChannelType() == Track::ChannelType::Instrument)
                {
                    resolved = i;
                    break;
                }
            }
        }

        if (resolved < 0 && createTrackIfNeeded)
        {
            createNewTrack(Track::ChannelType::Instrument);
            if (juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
                resolved = selectedTrackIndex;
        }

        if (!juce::isPositiveAndBelow(resolved, tracks.size()))
            return -1;

        ensureTrackHasPlayableInstrument(resolved);
        return resolved;
    }

    void MainComponent::ensureTrackHasPlayableInstrument(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto* track = tracks[trackIndex];
        if (track == nullptr)
            return;

        const auto channelType = track->getChannelType();
        if (channelType == Track::ChannelType::Aux || channelType == Track::ChannelType::Master)
            return;

        if (channelType == Track::ChannelType::Audio)
            track->setChannelType(Track::ChannelType::Instrument);

        if (track->isFrozenPlaybackOnly())
            track->setFrozenPlaybackOnly(false);

        if (track->hasInstrumentPlugin())
            return;

        const auto builtInMode = track->getBuiltInInstrumentMode();
        if (builtInMode == Track::BuiltInInstrument::BasicSynth)
            return;
        if (builtInMode == Track::BuiltInInstrument::Sampler && track->hasSamplerSoundLoaded())
            return;

        track->useBuiltInSynthInstrument();
    }

    void MainComponent::enqueuePreviewMidiEvent(int trackIndex, const juce::MidiMessage& message)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size())
            || !juce::isPositiveAndBelow(trackIndex, static_cast<int>(previewMidiBuffers.size())))
            return;

        juce::SpinLock::ScopedLockType lock(previewMidiBuffersLock);
        auto& previewBuffer = previewMidiBuffers[static_cast<size_t>(trackIndex)];
        previewBuffer.addEvent(message, 0);
    }

    void MainComponent::auditionPianoRollNote(int trackIndex, int noteNumber, int velocity)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        ensureTrackHasPlayableInstrument(trackIndex);
        setSelectedTrackIndex(trackIndex);
        timeline.selectTrack(trackIndex);
        mixer.selectTrack(trackIndex);

        const int note = juce::jlimit(0, 127, noteNumber);
        const auto vel = static_cast<juce::uint8>(juce::jlimit(1, 127, velocity));
        enqueuePreviewMidiEvent(trackIndex, juce::MidiMessage::noteOn(1, note, vel));

        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::Timer::callAfterDelay(180,
                                    [safeThis, trackIndex, note]
                                    {
                                        if (safeThis == nullptr)
                                            return;
                                        safeThis->enqueuePreviewMidiEvent(trackIndex,
                                                                          juce::MidiMessage::noteOff(1, note));
                                    });
    }

    bool MainComponent::importAudioFileToClip(const juce::File& file,
                                              int targetTrack,
                                              double startBeat,
                                              int& outClipIndex,
                                              double& outDetectedTempoBpm)
    {
        outClipIndex = -1;
        outDetectedTempoBpm = 0.0;
        if (!file.existsAsFile() || !isSupportedAudioFile(file))
            return false;

        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
            return false;

        Clip newClip;
        newClip.type = ClipType::Audio;
        newClip.name = file.getFileNameWithoutExtension();
        newClip.audioData.reset();
        newClip.audioFilePath = file.getFullPathName();
        newClip.audioSampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        newClip.startBeat = juce::jmax(0.0, startBeat);
        newClip.trackIndex = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), targetTrack);
        newClip.gainLinear = 1.0f;
        newClip.fadeInBeats = 0.0;
        newClip.fadeOutBeats = 0.0;
        newClip.crossfadeInBeats = 0.0;
        newClip.crossfadeOutBeats = 0.0;

        const double bpmNow = juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed));
        const double lengthSeconds = static_cast<double>(reader->lengthInSamples) / newClip.audioSampleRate;
        newClip.lengthBeats = juce::jmax(0.25, lengthSeconds * (bpmNow / 60.0));

        AcidLoopMetadata acidMeta;
        const bool hasAcidMeta = parseAcidMetadataFromWav(file, acidMeta);
        if (hasAcidMeta && !acidMeta.oneShot)
        {
            if (acidMeta.beatCount > 0.0)
                newClip.lengthBeats = juce::jmax(0.25, acidMeta.beatCount);
            else if (acidMeta.tempoBpm > 1.0)
                newClip.lengthBeats = juce::jmax(0.25, lengthSeconds * (acidMeta.tempoBpm / 60.0));
        }

        if (hasAcidMeta && acidMeta.tempoBpm > 1.0)
            newClip.detectedTempoBpm = acidMeta.tempoBpm;
        else
            newClip.detectedTempoBpm = tryExtractTempoFromFilename(file.getFileNameWithoutExtension());
        outDetectedTempoBpm = newClip.detectedTempoBpm;

        arrangement.push_back(std::move(newClip));
        outClipIndex = static_cast<int>(arrangement.size()) - 1;
        return true;
    }

    bool MainComponent::importMidiFileToClip(const juce::File& file,
                                             int targetTrack,
                                             double startBeat,
                                             int& outClipIndex,
                                             double& outTempoBpm)
    {
        outClipIndex = -1;
        outTempoBpm = 0.0;
        if (!file.existsAsFile() || !isSupportedMidiFile(file) || tracks.isEmpty())
            return false;
        const int resolvedTargetTrack = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), targetTrack);

        juce::FileInputStream input(file);
        if (!input.openedOk())
            return false;

        juce::MidiFile midiFile;
        if (!midiFile.readFrom(input))
            return false;

        midiFile.convertTimestampTicksToSeconds();

        // First tempo meta event becomes imported file tempo hint.
        for (int trackIdx = 0; trackIdx < midiFile.getNumTracks() && outTempoBpm <= 0.0; ++trackIdx)
        {
            if (const auto* seq = midiFile.getTrack(trackIdx))
            {
                for (int i = 0; i < seq->getNumEvents(); ++i)
                {
                    const auto& msg = seq->getEventPointer(i)->message;
                    if (!msg.isTempoMetaEvent())
                        continue;
                    const double secPerQuarter = msg.getTempoSecondsPerQuarterNote();
                    if (secPerQuarter > 0.0)
                    {
                        outTempoBpm = 60.0 / secPerQuarter;
                        break;
                    }
                }
            }
        }

        const double bpmForBeatConversion = juce::jmax(1.0,
                                                       outTempoBpm > 1.0 ? outTempoBpm
                                                                         : bpmRt.load(std::memory_order_relaxed));
        const double secondsPerBeat = 60.0 / bpmForBeatConversion;

        std::vector<TimelineEvent> events;
        std::vector<MidiCCEvent> ccEvents;
        events.reserve(1024);
        ccEvents.reserve(1024);

        for (int trackIdx = 0; trackIdx < midiFile.getNumTracks(); ++trackIdx)
        {
            const auto* seq = midiFile.getTrack(trackIdx);
            if (seq == nullptr)
                continue;
            const double trackEndSeconds = juce::jmax(0.0, seq->getEndTime());

            std::array<std::array<bool, 128>, 16> active {};
            std::array<std::array<double, 128>, 16> activeStartSeconds {};
            std::array<std::array<uint8_t, 128>, 16> activeVelocity {};

            for (int i = 0; i < seq->getNumEvents(); ++i)
            {
                const auto& msg = seq->getEventPointer(i)->message;
                const int channel = juce::jlimit(1, 16, msg.getChannel()) - 1;
                const int note = juce::jlimit(0, 127, msg.getNoteNumber());
                const double eventSeconds = juce::jmax(0.0, msg.getTimeStamp());

                if (msg.isNoteOn())
                {
                    active[static_cast<size_t>(channel)][static_cast<size_t>(note)] = true;
                    activeStartSeconds[static_cast<size_t>(channel)][static_cast<size_t>(note)] = eventSeconds;
                    const float rawVelocity = static_cast<float>(msg.getVelocity());
                    const int velocity = rawVelocity <= 1.0f
                        ? static_cast<int>(std::lround(rawVelocity * 127.0f))
                        : static_cast<int>(std::lround(rawVelocity));
                    activeVelocity[static_cast<size_t>(channel)][static_cast<size_t>(note)]
                        = static_cast<uint8_t>(juce::jlimit(1, 127, velocity));
                }
                else if (msg.isNoteOff())
                {
                    if (!active[static_cast<size_t>(channel)][static_cast<size_t>(note)])
                        continue;

                    const double startSeconds = activeStartSeconds[static_cast<size_t>(channel)][static_cast<size_t>(note)];
                    const double endSeconds = juce::jmax(startSeconds + 0.01, eventSeconds);
                    TimelineEvent ev;
                    ev.startBeat = juce::jmax(0.0, startSeconds / secondsPerBeat);
                    ev.durationBeats = juce::jmax(0.0625, (endSeconds - startSeconds) / secondsPerBeat);
                    ev.noteNumber = note;
                    ev.velocity = activeVelocity[static_cast<size_t>(channel)][static_cast<size_t>(note)];
                    events.push_back(ev);
                    active[static_cast<size_t>(channel)][static_cast<size_t>(note)] = false;
                }
                else if (msg.isController())
                {
                    MidiCCEvent cc;
                    cc.beat = juce::jmax(0.0, eventSeconds / secondsPerBeat);
                    cc.controller = juce::jlimit(0, 127, msg.getControllerNumber());
                    cc.value = static_cast<uint8_t>(juce::jlimit(0, 127, msg.getControllerValue()));
                    ccEvents.push_back(cc);
                }
            }

            for (int ch = 0; ch < 16; ++ch)
            {
                for (int note = 0; note < 128; ++note)
                {
                    if (!active[static_cast<size_t>(ch)][static_cast<size_t>(note)])
                        continue;
                    const double startSeconds = activeStartSeconds[static_cast<size_t>(ch)][static_cast<size_t>(note)];
                    const double endSeconds = juce::jmax(startSeconds + 0.01, trackEndSeconds);
                    TimelineEvent ev;
                    ev.startBeat = juce::jmax(0.0, startSeconds / secondsPerBeat);
                    ev.durationBeats = juce::jmax(0.0625, (endSeconds - startSeconds) / secondsPerBeat);
                    ev.noteNumber = note;
                    ev.velocity = activeVelocity[static_cast<size_t>(ch)][static_cast<size_t>(note)];
                    events.push_back(ev);
                }
            }
        }

        if (events.empty() && ccEvents.empty())
            return false;

        std::sort(events.begin(), events.end(),
                  [](const TimelineEvent& a, const TimelineEvent& b)
                  {
                      if (std::abs(a.startBeat - b.startBeat) > 1.0e-6)
                          return a.startBeat < b.startBeat;
                      return a.noteNumber < b.noteNumber;
                  });
        std::sort(ccEvents.begin(), ccEvents.end(),
                  [](const MidiCCEvent& a, const MidiCCEvent& b)
                  {
                      if (std::abs(a.beat - b.beat) > 1.0e-6)
                          return a.beat < b.beat;
                      return a.controller < b.controller;
                  });

        double endBeat = 0.0;
        for (const auto& ev : events)
            endBeat = juce::jmax(endBeat, ev.startBeat + juce::jmax(0.0625, ev.durationBeats));
        if (!ccEvents.empty())
            endBeat = juce::jmax(endBeat, ccEvents.back().beat + 0.25);

        Clip clip;
        clip.type = ClipType::MIDI;
        clip.name = file.getFileNameWithoutExtension();
        clip.startBeat = juce::jmax(0.0, startBeat);
        clip.lengthBeats = juce::jmax(0.25, endBeat);
        clip.trackIndex = resolvedTargetTrack;
        clip.events = std::move(events);
        clip.ccEvents = std::move(ccEvents);

        ensureTrackHasPlayableInstrument(resolvedTargetTrack);
        arrangement.push_back(std::move(clip));
        outClipIndex = static_cast<int>(arrangement.size()) - 1;
        return true;
    }

    void MainComponent::resized()
    {
        auto r = getLocalBounds();
        const int topBarHeight = juce::jlimit(100, 136, juce::roundToInt(static_cast<float>(getHeight()) * 0.145f));
        auto topArea = r.removeFromTop(topBarHeight);
        transportBar.setBounds(topArea.reduced(2));
        auto topRow = topArea.removeFromTop(topBarHeight / 2);
        auto secondRow = topArea;

        const float uiScale = theme::ThemeManager::instance().uiScaleFor(getWidth());
        const auto scaled = [uiScale](int baseWidth)
        {
            return juce::jmax(30, juce::roundToInt(static_cast<float>(baseWidth) * uiScale));
        };
        const auto place = [&](juce::Component& component, juce::Rectangle<int>& row, int baseWidth)
        {
            const int targetWidth = juce::jmin(row.getWidth(), scaled(baseWidth));
            component.setBounds(row.removeFromLeft(targetWidth).reduced(2));
        };

        place(playButton, topRow, 64);
        place(stopButton, topRow, 64);
        place(recordButton, topRow, 64);
        place(loopButton, topRow, 64);
        place(metroButton, topRow, 64);
        place(panicButton, topRow, 70);
        place(tempoMenuButton, topRow, 78);
        place(clipToolsButton, topRow, 96);

        if (lcdDisplay)
        {
            const int lcdBaseWidth = getWidth() >= 1750 ? 360
                                  : (getWidth() >= 1500 ? 320
                                                        : 270);
            place(*lcdDisplay, topRow, lcdBaseWidth);
        }

        place(undoButton, topRow, 70);
        place(redoButton, topRow, 70);
        place(gridSelector, topRow, 124);
        place(keySelector, topRow, 70);
        place(scaleSelector, topRow, 96);
        place(transposeSelector, topRow, 74);
        place(masterOutLabel, topRow, 60);
        place(masterOutSlider, topRow, 140);
        if (masterMeterWidget != nullptr)
            place(*masterMeterWidget, topRow, 230);
        place(softClipButton, topRow, 88);
        place(limiterButton, topRow, 86);
        place(auxEnableButton, topRow, 78);
        place(auxReturnLabel, topRow, 78);
        place(auxReturnSlider, topRow, 140);

        toolbarOverflowItems.clear();

        auto hideControl = [](juce::Component& component)
        {
            component.setVisible(false);
            component.setBounds({});
        };

        juce::Rectangle<int> statusArea;
        const int statusReserve = scaled(260);
        if (secondRow.getWidth() > statusReserve + scaled(340))
            statusArea = secondRow.removeFromRight(juce::jmin(secondRow.getWidth(), statusReserve));

        const auto layoutControl = [&](juce::Component& component,
                                       int baseWidth,
                                       ToolbarSection section,
                                       const juce::String& overflowLabel,
                                       std::function<void()> overflowAction,
                                       int insetX = 2,
                                       int insetY = 2,
                                       bool alwaysShow = false)
        {
            if (!alwaysShow && !isToolbarSectionVisible(section))
            {
                hideControl(component);
                return;
            }

            const int targetWidth = scaled(baseWidth);
            if (secondRow.getWidth() >= targetWidth)
            {
                component.setVisible(true);
                component.setBounds(secondRow.removeFromLeft(targetWidth).reduced(insetX, insetY));
                return;
            }

            hideControl(component);
            if (overflowAction)
                toolbarOverflowItems.push_back({ overflowLabel, std::move(overflowAction) });
        };

        layoutControl(transportStartButton, 44, ToolbarSection::Transport, "Jump to Start",
                      [this] { transportStartButton.triggerClick(); });
        layoutControl(transportPrevBarButton, 74, ToolbarSection::Transport, "Previous Bar",
                      [this] { transportPrevBarButton.triggerClick(); });
        layoutControl(transportNextBarButton, 74, ToolbarSection::Transport, "Next Bar",
                      [this] { transportNextBarButton.triggerClick(); });
        layoutControl(followPlayheadButton, 76, ToolbarSection::Transport, "Toggle Follow Playhead",
                      [this] { followPlayheadButton.triggerClick(); });

        layoutControl(timelineZoomOutButton, 34, ToolbarSection::Timing, "Timeline Zoom Out",
                      [this] { timelineZoomOutButton.triggerClick(); });
        layoutControl(timelineZoomInButton, 34, ToolbarSection::Timing, "Timeline Zoom In",
                      [this] { timelineZoomInButton.triggerClick(); });
        layoutControl(timelineZoomSlider, 120, ToolbarSection::Timing, {},
                      nullptr, 4, 8);
        layoutControl(trackZoomOutButton, 54, ToolbarSection::Timing, "Track Height -",
                      [this] { trackZoomOutButton.triggerClick(); });
        layoutControl(trackZoomInButton, 54, ToolbarSection::Timing, "Track Height +",
                      [this] { trackZoomInButton.triggerClick(); });
        layoutControl(trackZoomSlider, 110, ToolbarSection::Timing, {},
                      nullptr, 4, 8);
        layoutControl(resetZoomButton, 94, ToolbarSection::Timing, "Reset Zoom",
                      [this] { resetZoomButton.triggerClick(); });
        layoutControl(gridSelector, 124, ToolbarSection::Timing, "Grid",
                      [this] { gridSelector.showPopup(); });

        layoutControl(sampleRateSelector, 84, ToolbarSection::AudioMidiIO, "Sample Rate",
                      [this] { sampleRateSelector.showPopup(); });
        layoutControl(bufferSizeSelector, 76, ToolbarSection::AudioMidiIO, "Buffer Size",
                      [this] { bufferSizeSelector.showPopup(); });
        layoutControl(lowLatencyToggle, 84, ToolbarSection::AudioMidiIO, "Toggle Low Latency",
                      [this] { lowLatencyToggle.triggerClick(); });
        layoutControl(midiInputSelector, 210, ToolbarSection::AudioMidiIO, "MIDI Input",
                      [this] { midiInputSelector.showPopup(); });
        layoutControl(midiOutputSelector, 210, ToolbarSection::AudioMidiIO, "MIDI Output",
                      [this] { midiOutputSelector.showPopup(); });
        layoutControl(controlSurfaceInputSelector, 220, ToolbarSection::AudioMidiIO, "Control Surface Input",
                      [this] { controlSurfaceInputSelector.showPopup(); });
        layoutControl(midiLearnTargetSelector, 220, ToolbarSection::AudioMidiIO, "MIDI Learn Target",
                      [this] { midiLearnTargetSelector.showPopup(); });
        layoutControl(midiLearnArmToggle, 104, ToolbarSection::AudioMidiIO, "Toggle MIDI Learn Arm",
                      [this] { midiLearnArmToggle.triggerClick(); });
        layoutControl(midiThruToggle, 98, ToolbarSection::AudioMidiIO, "Toggle MIDI Thru",
                      [this] { midiThruToggle.triggerClick(); });
        layoutControl(externalClockToggle, 92, ToolbarSection::AudioMidiIO, "Toggle External Clock",
                      [this] { externalClockToggle.triggerClick(); });

        layoutControl(scanButton, 98, ToolbarSection::Editing, "Scan Plugins",
                      [this] { scanButton.triggerClick(); });
        layoutControl(addTrackButton, 92, ToolbarSection::Editing, "Add Track",
                      [this] { addTrackButton.triggerClick(); });
        layoutControl(showEditorButton, 92, ToolbarSection::Editing, "Show Plugin UI",
                      [this] { showEditorButton.triggerClick(); });
        layoutControl(rackButton, 74, ToolbarSection::Editing, "Open Channel Rack",
                      [this] { rackButton.triggerClick(); });
        layoutControl(inspectorButton, 80, ToolbarSection::Editing, "Open Inspector",
                      [this] { inspectorButton.triggerClick(); });

        layoutControl(freezeButton, 86, ToolbarSection::Render, "Freeze Menu",
                      [this] { freezeButton.triggerClick(); });
        layoutControl(backgroundRenderToggle, 106, ToolbarSection::Render, "Toggle Background Render",
                      [this] { backgroundRenderToggle.triggerClick(); });
        layoutControl(exportButton, 84, ToolbarSection::Render, "Export Menu",
                      [this] { exportButton.triggerClick(); });

        layoutControl(recordSetupButton, 96, ToolbarSection::Utility, "Recording Setup",
                      [this] { recordSetupButton.triggerClick(); });
        layoutControl(projectButton, 96, ToolbarSection::Utility, "Project Settings",
                      [this] { projectButton.triggerClick(); });
        layoutControl(settingsButton, 84, ToolbarSection::Utility, "Audio Settings",
                      [this] { settingsButton.triggerClick(); });
        layoutControl(helpButton, 72, ToolbarSection::Utility, "Help",
                      [this] { helpButton.triggerClick(); });
        layoutControl(saveButton, 72, ToolbarSection::Utility, "Save Project",
                      [this] { saveButton.triggerClick(); });
        layoutControl(toolbarButton, 88, ToolbarSection::Utility, "Toolbar Options",
                      [this] { toolbarButton.triggerClick(); },
                      2, 2, true);

        const int moreButtonWidth = scaled(78);
        if (!toolbarOverflowItems.empty())
        {
            if (secondRow.getWidth() >= moreButtonWidth)
            {
                toolbarMoreButton.setVisible(true);
                toolbarMoreButton.setBounds(secondRow.removeFromLeft(moreButtonWidth).reduced(2));
            }
            else if (statusArea.getWidth() >= moreButtonWidth + scaled(120))
            {
                toolbarMoreButton.setVisible(true);
                toolbarMoreButton.setBounds(statusArea.removeFromLeft(moreButtonWidth).reduced(2));
            }
            else
            {
                hideControl(toolbarMoreButton);
            }
        }
        else
        {
            hideControl(toolbarMoreButton);
        }

        const bool showPluginScanStatus = pluginScanProcess != nullptr;
        if (showPluginScanStatus && statusArea.getWidth() >= scaled(340))
        {
            auto scanArea = statusArea.removeFromLeft(scaled(320)).reduced(4, 3);
            auto barArea = scanArea.removeFromLeft(scaled(120)).reduced(2, 2);
            pluginScanStatusBar.setVisible(true);
            pluginScanStatusBar.setBounds(barArea);
            pluginScanStatusLabel.setVisible(true);
            pluginScanStatusLabel.setBounds(scanArea.reduced(4, 2));
        }
        else
        {
            pluginScanStatusBar.setVisible(false);
            pluginScanStatusBar.setBounds({});
            pluginScanStatusLabel.setVisible(false);
            pluginScanStatusLabel.setBounds({});
        }

        if (statusArea.getWidth() >= scaled(150))
        {
            statusLabel.setVisible(true);
            statusLabel.setBounds(statusArea.reduced(6, 4));
        }
        else
        {
            statusLabel.setVisible(false);
            statusLabel.setBounds({});
        }

        auto content = r;
        const int splitterWidth = 8;
        const int minBrowserWidth = 180;
        const int maxBrowserWidth = juce::jmax(minBrowserWidth, content.getWidth() / 3);
        const int desiredBrowserWidth = juce::roundToInt(browserPanelRatio * static_cast<float>(content.getWidth()));
        const int browserWidth = juce::jlimit(minBrowserWidth, maxBrowserWidth, desiredBrowserWidth);

        auto browserArea = content.removeFromLeft(browserWidth);
        browserPanel.setBounds(browserArea);
        browserSplitterBounds = content.removeFromLeft(splitterWidth);

        auto centerArea = content;

        const int trackListWidth = juce::jlimit(140, 260, juce::roundToInt(static_cast<float>(centerArea.getWidth()) * 0.18f));
        auto trackListArea = centerArea.removeFromLeft(trackListWidth);
        trackListView.setBounds(trackListArea.reduced(2));
        auto arrangeArea = centerArea.reduced(2);
        timelineView.setBounds(arrangeArea);

        const int totalRemaining = arrangeArea.getHeight();
        const int minTopHeight = 170;
        const int minBottomHeight = 220;
        const int splitterHeight = 8;
        const int maxBottomHeight = juce::jmax(minBottomHeight, totalRemaining - minTopHeight - splitterHeight);
        const int desiredBottomHeight = juce::roundToInt(bottomPanelRatio * static_cast<float>(totalRemaining));
        const int bottomHeight = juce::jlimit(minBottomHeight, maxBottomHeight, desiredBottomHeight);
        const int topHeight = juce::jmax(minTopHeight, totalRemaining - bottomHeight - splitterHeight);

        timeline.setBounds(arrangeArea.removeFromTop(topHeight).reduced(2));
        bottomSplitterBounds = arrangeArea.removeFromTop(splitterHeight);
        bottomTabs.setBounds(arrangeArea);
    }

    void MainComponent::togglePlayback()
    {
        if (transport.playing())
        {
            transport.stop();
            transport.setRecording(false);
        }
        else
        {
            transport.play();
            transport.setRecording(recordEnabledRt.load(std::memory_order_relaxed));
        }
        refreshStatusText();
    }

    void MainComponent::createMidiClipAt(int trackIndex, double startBeat, double lengthBeats, bool selectClip)
    {
        if (trackIndex < 0 || trackIndex >= tracks.size())
            return;

        ensureTrackHasPlayableInstrument(trackIndex);
        applyArrangementEdit("Create MIDI Clip",
                             [trackIndex, startBeat, lengthBeats, selectClip](std::vector<Clip>& state, int& selected)
                             {
                                 Clip newClip;
                                 newClip.type = ClipType::MIDI;
                                 newClip.name = "MIDI " + juce::String(state.size() + 1);
                                 newClip.startBeat = juce::jmax(0.0, startBeat);
                                 newClip.lengthBeats = juce::jmax(0.25, lengthBeats);
                                 newClip.trackIndex = trackIndex;
                                 state.push_back(newClip);
                                 if (selectClip)
                                     selected = static_cast<int>(state.size()) - 1;
                             });

        if (selectClip)
            bottomTabs.setCurrentTabIndex(1);
    }

    int MainComponent::findClipIndex(const Clip* clip) const
    {
        if (clip == nullptr)
            return -1;

        for (int i = 0; i < static_cast<int>(arrangement.size()); ++i)
            if (&arrangement[static_cast<size_t>(i)] == clip)
                return i;
        return -1;
    }

    void MainComponent::setSelectedClipIndex(int clipIndex, bool showMidiEditorsTab)
    {
        if (clipIndex < 0 || clipIndex >= static_cast<int>(arrangement.size()))
            selectedClipIndex = -1;
        else
            selectedClipIndex = clipIndex;

        Clip* selectedClip = nullptr;
        if (selectedClipIndex >= 0)
            selectedClip = &arrangement[static_cast<size_t>(selectedClipIndex)];

        if (selectedClip != nullptr
            && juce::isPositiveAndBelow(selectedClip->trackIndex, tracks.size()))
        {
            setSelectedTrackIndex(selectedClip->trackIndex);
            timeline.selectTrack(selectedClip->trackIndex);
            mixer.selectTrack(selectedClip->trackIndex);
        }

        pianoRoll.setClip(selectedClip, selectedClipIndex);
        stepSequencer.setClip(selectedClip, selectedClipIndex);
        timeline.selectClipIndex(selectedClipIndex);

        if (showMidiEditorsTab && selectedClip != nullptr && selectedClip->type == ClipType::MIDI)
            bottomTabs.setCurrentTabIndex(1);

        refreshStatusText();
    }

    void MainComponent::applyArrangementEdit(const juce::String& actionName,
                                             std::function<void(std::vector<Clip>&, int&)> mutator)
    {
        auto before = arrangement;
        auto after = before;
        const int beforeSelection = selectedClipIndex;
        int afterSelection = beforeSelection;

        mutator(after, afterSelection);
        applyAutomaticAudioCrossfades(after);

        if (before == after && beforeSelection == afterSelection)
            return;

        undoManager.perform(new ArrangementStateAction(
                                arrangement,
                                std::move(before),
                                std::move(after),
                                beforeSelection,
                                afterSelection,
                                [this](int restoredSelection)
                                {
                                    setSelectedClipIndex(restoredSelection, false);
                                    repaint();
                                }),
                            actionName);
        rebuildRealtimeSnapshot();
    }

    void MainComponent::applyClipEdit(int clipIndex,
                                      const juce::String& actionName,
                                      std::function<void(Clip&)> mutator)
    {
        if (clipIndex < 0 || clipIndex >= static_cast<int>(arrangement.size()))
            return;

        applyArrangementEdit(actionName,
                             [clipIndex, edit = std::move(mutator)](std::vector<Clip>& state, int& selected) mutable
                             {
                                 if (clipIndex < 0 || clipIndex >= static_cast<int>(state.size()))
                                     return;
                                 edit(state[static_cast<size_t>(clipIndex)]);
                                 selected = clipIndex;
                             });
    }

    double MainComponent::gridStepFromSelectorId(int selectorId) const
    {
        switch (selectorId)
        {
            case 1: return 1.0;
            case 2: return 0.5;
            case 3: return 0.25;
            case 4: return 0.125;
            case 5: return 1.0 / 3.0;
            case 6: return 1.0 / 6.0;
            default: return gridStepBeats;
        }
    }

    int MainComponent::selectorIdFromGridStep(double beats) const
    {
        if (std::abs(beats - 1.0) < 1.0e-6) return 1;
        if (std::abs(beats - 0.5) < 1.0e-6) return 2;
        if (std::abs(beats - 0.25) < 1.0e-6) return 3;
        if (std::abs(beats - 0.125) < 1.0e-6) return 4;
        if (std::abs(beats - (1.0 / 3.0)) < 1.0e-6) return 5;
        if (std::abs(beats - (1.0 / 6.0)) < 1.0e-6) return 6;
        return 3;
    }

    void MainComponent::applyGridStep(double beats)
    {
        gridStepBeats = juce::jlimit(1.0 / 64.0, 4.0, beats);
        timeline.setGridStepBeats(gridStepBeats);
        pianoRoll.setSnapBeat(gridStepBeats);
        const int selectorId = selectorIdFromGridStep(gridStepBeats);
        if (gridSelector.getSelectedId() != selectorId)
            gridSelector.setSelectedId(selectorId, juce::dontSendNotification);
    }

    int MainComponent::chooseBestInputPairForCurrentBlock(int capturedInputChannels, int blockSamples) const
    {
        if (capturedInputChannels <= 0 || blockSamples <= 0
            || liveInputCaptureBuffer.getNumChannels() < capturedInputChannels
            || liveInputCaptureBuffer.getNumSamples() < blockSamples)
        {
            return 0;
        }

        int bestChannel = 0;
        float bestRms = 0.0f;
        for (int ch = 0; ch < capturedInputChannels; ++ch)
        {
            const float rms = liveInputCaptureBuffer.getRMSLevel(ch, 0, blockSamples);
            if (rms > bestRms)
            {
                bestRms = rms;
                bestChannel = ch;
            }
        }
        return juce::jmax(0, bestChannel / 2);
    }

    void MainComponent::startAudioTakeWriters(double startBeat)
    {
        const double sampleRate = juce::jmax(1.0, sampleRateRt.load(std::memory_order_relaxed));

        auto recordingsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("Sampledex")
            .getChildFile("ChordLab")
            .getChildFile("Recordings");
        recordingsDir.createDirectory();
        const auto timestampToken = juce::String(juce::Time::getCurrentTime().toMilliseconds());

        const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
        const int ringCapacitySamples = juce::jmax(maxRealtimeBlockSize * 32, 65536);
        for (auto& take : audioTakeWriters)
        {
            take.writer.reset();
            take.file = juce::File();
            take.active = false;
            take.inputPair = -1;
            take.samplesWritten = 0;
            take.startBeat = startBeat;
            take.sampleRate = sampleRate;
            take.trackIndex = -1;
            take.ringLeft.clear();
            take.ringRight.clear();
            take.ringFifo.reset();
            take.droppedSamples.store(0, std::memory_order_relaxed);
            take.hadWriteError.store(false, std::memory_order_relaxed);
            take.writeErrorMessage.clear();
        }

        for (int trackIndex = 0; trackIndex < tracks.size() && trackIndex < maxRealtimeTracks; ++trackIndex)
        {
            auto* track = tracks[trackIndex];
            if (track == nullptr || !track->isArmed() || !track->isInputMonitoringEnabled())
                continue;

            auto& take = audioTakeWriters[static_cast<size_t>(trackIndex)];
            const auto fileName = "AudioTake_" + timestampToken
                                + "_T" + juce::String(trackIndex + 1)
                                + "_" + juce::String(recordingTakeCounter);
            take.file = recordingsDir.getNonexistentChildFile(fileName, ".wav", true);
            take.inputPair = track->getInputSourcePair();
            take.startBeat = startBeat;
            take.sampleRate = sampleRate;
            take.trackIndex = trackIndex;

            auto fileOutput = take.file.createOutputStream();
            if (fileOutput == nullptr || !fileOutput->openedOk())
            {
                take.writeErrorMessage = "Unable to open recording file for writing.";
                take.file = juce::File();
                continue;
            }

            juce::WavAudioFormat wavFormat;
            juce::AudioFormatWriterOptions writerOptions;
            writerOptions = writerOptions.withSampleRate(sampleRate)
                                         .withNumChannels(2)
                                         .withBitsPerSample(24);
            std::unique_ptr<juce::OutputStream> outputStream(std::move(fileOutput));
            auto writer = wavFormat.createWriterFor(outputStream, writerOptions);
            if (writer == nullptr)
            {
                take.file.deleteFile();
                take.writeErrorMessage = "Unable to create WAV writer for recording take.";
                take.file = juce::File();
                continue;
            }

            take.writer = std::move(writer);
            take.ringLeft.assign(static_cast<size_t>(ringCapacitySamples), 0.0f);
            take.ringRight.assign(static_cast<size_t>(ringCapacitySamples), 0.0f);
            take.ringFifo = std::make_unique<juce::AbstractFifo>(ringCapacitySamples);
            take.active = (take.writer != nullptr);
            take.samplesWritten = 0;
        }
    }

    void MainComponent::flushAudioTakeRingBuffers(bool flushAllPending)
    {
        for (auto& take : audioTakeWriters)
        {
            if (take.writer == nullptr || take.ringFifo == nullptr)
                continue;

            while (take.ringFifo->getNumReady() > 0)
            {
                const int numReady = take.ringFifo->getNumReady();
                if (numReady <= 0)
                    break;

                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                take.ringFifo->prepareToRead(numReady, start1, size1, start2, size2);
                if (size1 > 0)
                {
                    juce::AudioBuffer<float> writeBuffer(2, size1);
                    std::memcpy(writeBuffer.getWritePointer(0), take.ringLeft.data() + start1, static_cast<size_t>(size1) * sizeof(float));
                    std::memcpy(writeBuffer.getWritePointer(1), take.ringRight.data() + start1, static_cast<size_t>(size1) * sizeof(float));
                    if (!take.writer->writeFromAudioSampleBuffer(writeBuffer, 0, size1))
                    {
                        take.hadWriteError.store(true, std::memory_order_relaxed);
                        if (take.writeErrorMessage.isEmpty())
                            take.writeErrorMessage = "Disk write failed (disk full or permission denied).";
                        take.active = false;
                        break;
                    }
                    take.samplesWritten += static_cast<int64>(size1);
                }
                if (size2 > 0)
                {
                    juce::AudioBuffer<float> writeBuffer(2, size2);
                    std::memcpy(writeBuffer.getWritePointer(0), take.ringLeft.data() + start2, static_cast<size_t>(size2) * sizeof(float));
                    std::memcpy(writeBuffer.getWritePointer(1), take.ringRight.data() + start2, static_cast<size_t>(size2) * sizeof(float));
                    if (!take.writer->writeFromAudioSampleBuffer(writeBuffer, 0, size2))
                    {
                        take.hadWriteError.store(true, std::memory_order_relaxed);
                        if (take.writeErrorMessage.isEmpty())
                            take.writeErrorMessage = "Disk write failed (disk full or permission denied).";
                        take.active = false;
                        break;
                    }
                    take.samplesWritten += static_cast<int64>(size2);
                }
                take.ringFifo->finishedRead(size1 + size2);

                if (!flushAllPending)
                    break;
            }
        }
    }

    void MainComponent::stopAudioTakeWriters(double endBeat, std::vector<Clip>& destinationClips)
    {
        struct ClosedTake
        {
            juce::File file;
            int trackIndex = -1;
            int64 samplesWritten = 0;
            double startBeat = 0.0;
            double endBeat = 0.0;
            double sampleRate = 44100.0;
            bool hadError = false;
        };

        std::vector<ClosedTake> closedTakes;
        closedTakes.reserve(static_cast<size_t>(tracks.size()));

        flushAudioTakeRingBuffers(true);
        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            for (auto& take : audioTakeWriters)
            {
                if (!take.active && take.writer == nullptr)
                    continue;

                ClosedTake closed;
                closed.file = take.file;
                closed.trackIndex = take.trackIndex;
                closed.samplesWritten = take.samplesWritten;
                closed.startBeat = take.startBeat;
                closed.endBeat = endBeat;
                closed.sampleRate = take.sampleRate;
                closed.hadError = take.hadWriteError.load(std::memory_order_relaxed)
                                  || take.droppedSamples.load(std::memory_order_relaxed) > 0;
                closedTakes.push_back(std::move(closed));

                take.writer.reset();
                take.file = juce::File();
                take.active = false;
                take.inputPair = -1;
                take.samplesWritten = 0;
                take.startBeat = 0.0;
                take.sampleRate = 44100.0;
                take.trackIndex = -1;
                take.ringLeft.clear();
                take.ringRight.clear();
                take.ringFifo.reset();
                take.droppedSamples.store(0, std::memory_order_relaxed);
                take.hadWriteError.store(false, std::memory_order_relaxed);
                take.writeErrorMessage.clear();
            }
        }

        for (const auto& take : closedTakes)
        {
            if (!take.file.existsAsFile()
                || !juce::isPositiveAndBelow(take.trackIndex, tracks.size())
                || take.samplesWritten < 64
                || take.hadError)
            {
                continue;
            }

            const double durationSeconds = static_cast<double>(take.samplesWritten) / juce::jmax(1.0, take.sampleRate);
            const double durationBeatsFromSamples = juce::jmax(0.0, durationSeconds * (bpmRt.load(std::memory_order_relaxed) / 60.0));
            const double durationBeatsFromTimeline = juce::jmax(0.0, take.endBeat - take.startBeat);
            const double preRollSafetyBeats = (static_cast<double>(recordingSafetyPreRollSamplesRt.load(std::memory_order_relaxed))
                                               / juce::jmax(1.0, take.sampleRate))
                                              * (bpmRt.load(std::memory_order_relaxed) / 60.0);
            const double clipDurationBeats = juce::jmax(0.25,
                                                        (durationBeatsFromTimeline > 0.0001
                                                            ? durationBeatsFromTimeline
                                                            : durationBeatsFromSamples) - preRollSafetyBeats);

            Clip clip;
            clip.type = ClipType::Audio;
            clip.name = "Audio Take " + juce::String(recordingTakeCounter++);
            clip.startBeat = juce::jmax(0.0, take.startBeat + preRollSafetyBeats - recordingLatencyCompensationBeats);
            clip.lengthBeats = clipDurationBeats;
            clip.trackIndex = take.trackIndex;
            clip.audioData.reset();
            clip.audioFilePath = take.file.getFullPathName();
            clip.audioSampleRate = take.sampleRate;
            clip.gainLinear = 1.0f;
            clip.fadeInBeats = 0.0;
            clip.fadeOutBeats = 0.0;
            clip.crossfadeInBeats = 0.0;
            clip.crossfadeOutBeats = 0.0;
            destinationClips.push_back(std::move(clip));
        }
    }

    void MainComponent::startCaptureRecordingNow()
    {
        startCaptureRecordingNow(transport.getCurrentBeat(), transport.getCurrentSample());
    }

    void MainComponent::startCaptureRecordingNow(double requestedStartBeat, int64_t requestedStartSample)
    {
        recordingStartBeat = juce::jmax(0.0, requestedStartBeat);
        recordingStartSample = juce::jmax<int64_t>(0, requestedStartSample);
        recordStartPending = false;
        recordStartPendingRt.store(false, std::memory_order_relaxed);
        recordStartPendingBeatRt.store(recordingStartBeat, std::memory_order_relaxed);
        recordStartRequestRt.store(0, std::memory_order_relaxed);

        int inputLatencySamples = 0;
        int blockSizeSamples = 0;
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            inputLatencySamples = juce::jmax(0, device->getInputLatencyInSamples());
            blockSizeSamples = juce::jmax(0, device->getCurrentBufferSizeSamples());
        }

        const int manualOffsetSamples = recordingManualOffsetSamples;
        const int latencySamples = juce::jmax(0, inputLatencySamples + manualOffsetSamples);
        recordingSafetyPreRollSamples = juce::jmax(0, blockSizeSamples * juce::jmax(1, recordingSafetyPreRollBlocks));
        recordingSafetyPreRollSamplesRt.store(recordingSafetyPreRollSamples, std::memory_order_relaxed);
        recordingLatencyCompensationSamplesRt.store(latencySamples, std::memory_order_relaxed);

        const double currentTempo = juce::jmax(1.0, transport.getTempo());
        const double sampleRate = juce::jmax(1.0, sampleRateRt.load(std::memory_order_relaxed));
        recordingLatencyCompensationBeats = (static_cast<double>(latencySamples) / sampleRate) * (currentTempo / 60.0);

        const double preRollSafetyBeats = (static_cast<double>(recordingSafetyPreRollSamples) / sampleRate) * (currentTempo / 60.0);
        const double writerStartBeat = juce::jmax(0.0, recordingStartBeat - preRollSafetyBeats);

        transport.setRecording(true);
        for (auto* track : tracks)
            track->startRecording();
        startAudioTakeWriters(writerStartBeat);
    }

    void MainComponent::stopCaptureRecordingAndCommit(bool stopTransportAfterCommit)
    {
        const double recordStopBeat = transport.getCurrentBeat();
        const bool recordingWasActive = transport.getCurrentPositionInfo().isRecording;
        transport.setRecording(false);
        for (auto* track : tracks)
            track->stopRecording();

        if (forcedMetronomeForCountIn)
        {
            forcedMetronomeForCountIn = false;
            metronomeEnabled = metronomeStateBeforeCountIn;
            metronomeEnabledRt.store(metronomeEnabled, std::memory_order_relaxed);
            metroButton.setToggleState(metronomeEnabled, juce::dontSendNotification);
        }

        std::vector<Clip> committedClips;
        committedClips.reserve(static_cast<size_t>(tracks.size() * 2));

        if (recordingWasActive)
        {
            const double clipBaseBeat = juce::jmax(0.0, juce::jmin(recordingStartBeat, recordStopBeat) - recordingLatencyCompensationBeats);
            const double nominalTakeLength = juce::jmax(1.0, std::abs(recordStopBeat - recordingStartBeat));

            for (int i = 0; i < tracks.size(); ++i)
            {
                if (!tracks[i]->isArmed())
                    continue;

                std::vector<TimelineEvent> recordedEvents;
                recordedEvents.reserve(512);
                tracks[i]->flushRecordingToClip(recordedEvents, recordingLatencyCompensationBeats);
                if (recordedEvents.empty())
                    continue;

                std::sort(recordedEvents.begin(), recordedEvents.end(),
                          [](const TimelineEvent& a, const TimelineEvent& b)
                          {
                              if (std::abs(a.startBeat - b.startBeat) > 1.0e-6)
                                  return a.startBeat < b.startBeat;
                              return a.noteNumber < b.noteNumber;
                          });

                double maxEndBeat = clipBaseBeat + nominalTakeLength;
                for (const auto& ev : recordedEvents)
                    maxEndBeat = juce::jmax(maxEndBeat, ev.startBeat + juce::jmax(0.0625, ev.durationBeats));

                for (auto& ev : recordedEvents)
                {
                    ev.startBeat = juce::jmax(0.0, ev.startBeat - clipBaseBeat);
                    ev.durationBeats = juce::jmax(0.0625, ev.durationBeats);
                }

                Clip midiClip;
                midiClip.type = ClipType::MIDI;
                midiClip.name = "Take " + juce::String(recordingTakeCounter++);
                midiClip.startBeat = clipBaseBeat;
                midiClip.lengthBeats = juce::jmax(1.0, maxEndBeat - clipBaseBeat);
                midiClip.trackIndex = i;
                midiClip.events = std::move(recordedEvents);
                committedClips.push_back(std::move(midiClip));
            }
        }

        stopAudioTakeWriters(recordStopBeat, committedClips);

        recordStartPending = false;
        autoStopAfterBeat = -1.0;
        stopTransportAfterAutoPunch = false;
        recordStartPendingRt.store(false, std::memory_order_relaxed);
        autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);
        recordStartRequestRt.store(0, std::memory_order_relaxed);
        recordStopRequestRt.store(0, std::memory_order_relaxed);

        if (!committedClips.empty())
        {
            const bool overdubEnabled = recordOverdubEnabled;
            applyArrangementEdit("Commit Recording",
                                 [clipsToInsert = std::move(committedClips), overdubEnabled](std::vector<Clip>& state, int& selected) mutable
                                 {
                                     if (!overdubEnabled)
                                     {
                                         state.erase(std::remove_if(state.begin(), state.end(),
                                                                    [&clipsToInsert](const Clip& existing)
                                                                    {
                                                                        for (const auto& incoming : clipsToInsert)
                                                                        {
                                                                            if (existing.trackIndex != incoming.trackIndex || existing.type != incoming.type)
                                                                                continue;
                                                                            const double e0 = existing.startBeat;
                                                                            const double e1 = existing.startBeat + juce::jmax(0.0, existing.lengthBeats);
                                                                            const double i0 = incoming.startBeat;
                                                                            const double i1 = incoming.startBeat + juce::jmax(0.0, incoming.lengthBeats);
                                                                            if (i1 > e0 && e1 > i0)
                                                                                return true;
                                                                        }
                                                                        return false;
                                                                    }),
                                                   state.end());
                                     }

                                     for (auto& clip : clipsToInsert)
                                         state.push_back(std::move(clip));
                                     selected = static_cast<int>(state.size()) - 1;
                                 });
            setSelectedClipIndex(selectedClipIndex, false);
            resized();
        }

        if (stopTransportAfterCommit)
            transport.stop();

        refreshStatusText();
    }

    void MainComponent::applyAutomaticAudioCrossfades(std::vector<Clip>& state) const
    {
        std::vector<int> audioIndexes;
        audioIndexes.reserve(state.size());
        for (int i = 0; i < static_cast<int>(state.size()); ++i)
        {
            if (state[static_cast<size_t>(i)].type == ClipType::Audio)
            {
                state[static_cast<size_t>(i)].crossfadeInBeats = 0.0;
                state[static_cast<size_t>(i)].crossfadeOutBeats = 0.0;
                audioIndexes.push_back(i);
            }
        }

        for (int i = 0; i < static_cast<int>(audioIndexes.size()); ++i)
        {
            for (int j = i + 1; j < static_cast<int>(audioIndexes.size()); ++j)
            {
                auto& a = state[static_cast<size_t>(audioIndexes[static_cast<size_t>(i)])];
                auto& b = state[static_cast<size_t>(audioIndexes[static_cast<size_t>(j)])];
                if (a.trackIndex != b.trackIndex)
                    continue;

                Clip* left = &a;
                Clip* right = &b;
                if (left->startBeat > right->startBeat)
                    std::swap(left, right);

                const double leftEnd = left->startBeat + juce::jmax(0.0, left->lengthBeats);
                const double overlap = leftEnd - right->startBeat;
                if (overlap <= 0.0)
                    continue;

                const double fade = juce::jlimit(0.0, 0.25, overlap * 0.5);
                ArrangementEditing::applySymmetricCrossfade(*left, *right, fade);
            }
        }
    }

    void MainComponent::toggleRecord() {
        const bool rec = recordButton.getToggleState();
        recordEnabledRt.store(rec, std::memory_order_relaxed);
        recordStartRequestRt.store(0, std::memory_order_relaxed);
        recordStopRequestRt.store(0, std::memory_order_relaxed);

        bool anyArmedTrack = false;
        bool armedAudioTrack = false;
        for (auto* t : tracks)
        {
            if (t == nullptr || !t->isArmed())
                continue;
            anyArmedTrack = true;
            armedAudioTrack = armedAudioTrack || (t->getChannelType() == Track::ChannelType::Audio);
        }

        if (rec && armedAudioTrack && !ensureAudioInputReadyForMonitoring("Record enable"))
        {
            recordButton.setToggleState(false, juce::dontSendNotification);
            recordEnabledRt.store(false, std::memory_order_relaxed);
            refreshStatusText();
            return;
        }

        if (rec && !anyArmedTrack && juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
        {
            tracks[selectedTrackIndex]->setArm(true);
            const bool likelyBuiltIn = usingLikelyBuiltInAudioRt.load(std::memory_order_relaxed);
            requestTrackInputMonitoringState(selectedTrackIndex,
                                             !likelyBuiltIn,
                                             "Record auto-arm");
            if (likelyBuiltIn)
            {
                tracks[selectedTrackIndex]->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
                tracks[selectedTrackIndex]->setInputMonitorGain(
                    juce::jmin(tracks[selectedTrackIndex]->getInputMonitorGain(), 0.60f));
            }
        }

        const auto position = transport.getCurrentPositionInfo();
        const double numerator = static_cast<double>(juce::jmax(1, position.timeSigNumerator));
        const double denominator = static_cast<double>(juce::jmax(1, position.timeSigDenominator));
        const double beatsPerBar = juce::jmax(1.0, numerator * (4.0 / denominator));

        if (rec)
        {
            autoStopAfterBeat = -1.0;
            stopTransportAfterAutoPunch = false;
            autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);

            const bool usePunch = punchEnabled && punchOutBeat > punchInBeat + 0.125;
            if (usePunch)
            {
                const double preRollBeats = beatsPerBar * static_cast<double>(juce::jmax(0, preRollBars));
                const double postRollBeats = beatsPerBar * static_cast<double>(juce::jmax(0, postRollBars));
                const double desiredStartBeat = juce::jmax(0.0, punchInBeat - preRollBeats);
                autoStopAfterBeat = juce::jmax(punchInBeat + 0.125, punchOutBeat + postRollBeats);
                stopTransportAfterAutoPunch = true;
                autoStopAfterBeatRt.store(autoStopAfterBeat, std::memory_order_relaxed);

                if (!transport.playing())
                    transport.setPositionBeats(desiredStartBeat);
                else if (transport.getCurrentBeat() >= punchOutBeat)
                    transport.setPositionBeats(desiredStartBeat);

                if (transport.getCurrentBeat() < punchInBeat - 1.0e-6)
                {
                    recordStartPending = true;
                    recordStartPendingBeat = punchInBeat;
                    recordStartPendingRt.store(true, std::memory_order_relaxed);
                    recordStartPendingBeatRt.store(recordStartPendingBeat, std::memory_order_relaxed);
                    transport.play();
                    transport.setRecording(false);
                    for (auto* track : tracks)
                        track->stopRecording();
                }
                else
                {
                    recordStartPending = false;
                    recordStartPendingRt.store(false, std::memory_order_relaxed);
                    startCaptureRecordingNow();
                }

                metronomeStateBeforeCountIn = metronomeEnabled;
                forcedMetronomeForCountIn = !metronomeEnabled;
                if (forcedMetronomeForCountIn)
                {
                    metronomeEnabled = true;
                    metronomeEnabledRt.store(true, std::memory_order_relaxed);
                    metroButton.setToggleState(true, juce::dontSendNotification);
                }

                refreshStatusText();
                return;
            }

            if (!transport.playing() && recordCountInBars > 0)
            {
                const double currentBeat = transport.getCurrentBeat();
                double nextBar = std::ceil(currentBeat / beatsPerBar) * beatsPerBar;
                if (nextBar < currentBeat + 1.0e-6)
                    nextBar += beatsPerBar;
                recordStartPendingBeat = nextBar + static_cast<double>(juce::jmax(0, recordCountInBars - 1)) * beatsPerBar;
                recordStartPending = true;
                recordStartPendingRt.store(true, std::memory_order_relaxed);
                recordStartPendingBeatRt.store(recordStartPendingBeat, std::memory_order_relaxed);

                metronomeStateBeforeCountIn = metronomeEnabled;
                forcedMetronomeForCountIn = !metronomeEnabled;
                if (forcedMetronomeForCountIn)
                {
                    metronomeEnabled = true;
                    metronomeEnabledRt.store(true, std::memory_order_relaxed);
                    metroButton.setToggleState(true, juce::dontSendNotification);
                }

                transport.play();
                transport.setRecording(false);
                for (auto* t : tracks)
                    t->stopRecording();
                refreshStatusText();
                return;
            }

            recordStartPending = false;
            forcedMetronomeForCountIn = false;
            recordStartPendingRt.store(false, std::memory_order_relaxed);
            if (!transport.playing())
                transport.play();
            startCaptureRecordingNow();
            refreshStatusText();
            return;
        }

        const bool wasPendingStart = recordStartPending;
        recordStartPending = false;
        autoStopAfterBeat = -1.0;
        stopTransportAfterAutoPunch = false;
        recordStartPendingRt.store(false, std::memory_order_relaxed);
        autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);

        if (wasPendingStart)
        {
            if (forcedMetronomeForCountIn)
            {
                forcedMetronomeForCountIn = false;
                metronomeEnabled = metronomeStateBeforeCountIn;
                metronomeEnabledRt.store(metronomeEnabled, std::memory_order_relaxed);
                metroButton.setToggleState(metronomeEnabled, juce::dontSendNotification);
            }
            transport.setRecording(false);
            for (auto* track : tracks)
                track->stopRecording();
            std::vector<Clip> discardedClips;
            stopAudioTakeWriters(transport.getCurrentBeat(), discardedClips);
            refreshStatusText();
            return;
        }

        stopCaptureRecordingAndCommit(false);
    }
    void MainComponent::toggleMetronome()
    {
        metronomeEnabled = metroButton.getToggleState();
        metronomeEnabledRt.store(metronomeEnabled, std::memory_order_relaxed);
        refreshStatusText();
    }

    void MainComponent::toggleLoop()
    {
        syncTransportLoopFromUi();
        refreshStatusText();
    }

    void MainComponent::showAddTrackMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Instrument Track");
        menu.addItem(2, "Audio Input Track");
        menu.addItem(3, "MIDI Track + 4-Bar Clip");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addTrackButton),
                           [this](int selectedId)
                           {
                               if (selectedId == 0)
                                   return;

                               const auto channelType = (selectedId == 2)
                                   ? Track::ChannelType::Audio
                                   : Track::ChannelType::Instrument;
                               createNewTrack(channelType);
                               if (tracks.isEmpty())
                                   return;

                               const int newTrackIndex = tracks.size() - 1;
                               if (!juce::isPositiveAndBelow(newTrackIndex, tracks.size()))
                                   return;

                               if (selectedId == 2)
                               {
                                   const bool likelyBuiltIn = usingLikelyBuiltInAudioRt.load(std::memory_order_relaxed);
                                   requestTrackInputMonitoringState(newTrackIndex,
                                                                    !likelyBuiltIn,
                                                                    "Add audio-input track");
                                   if (likelyBuiltIn)
                                   {
                                       tracks[newTrackIndex]->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
                                       tracks[newTrackIndex]->setInputMonitorGain(
                                           juce::jmin(tracks[newTrackIndex]->getInputMonitorGain(), 0.60f));
                                   }
                                   tracks[newTrackIndex]->setArm(true);
                                   refreshStatusText();
                               }
                               else if (selectedId == 3)
                               {
                                   createMidiClipAt(newTrackIndex, transport.getCurrentBeat(), 4.0, true);
                               }
                           });
    }

    void MainComponent::reorderTracks(int sourceTrackIndex, int targetTrackIndex)
    {
        if (!juce::isPositiveAndBelow(sourceTrackIndex, tracks.size())
            || !juce::isPositiveAndBelow(targetTrackIndex, tracks.size())
            || sourceTrackIndex == targetTrackIndex)
            return;

        const auto remapTrackIndex = [sourceTrackIndex, targetTrackIndex](int oldIndex)
        {
            if (oldIndex == sourceTrackIndex)
                return targetTrackIndex;

            if (sourceTrackIndex < targetTrackIndex)
            {
                if (oldIndex > sourceTrackIndex && oldIndex <= targetTrackIndex)
                    return oldIndex - 1;
            }
            else
            {
                if (oldIndex >= targetTrackIndex && oldIndex < sourceTrackIndex)
                    return oldIndex + 1;
            }

            return oldIndex;
        };

        applyArrangementEdit("Reorder Track",
                             [remapTrackIndex](std::vector<Clip>& state, int& selected)
                             {
                                 juce::ignoreUnused(selected);
                                 for (auto& clip : state)
                                     clip.trackIndex = juce::jmax(0, remapTrackIndex(clip.trackIndex));
                             });

        for (auto& lane : automationLanes)
        {
            if (lane.trackIndex >= 0)
                lane.trackIndex = juce::jmax(0, remapTrackIndex(lane.trackIndex));
        }

        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            tracks.move(sourceTrackIndex, targetTrackIndex);
        }
        mixer.rebuildFromTracks(tracks);
        timeline.refreshHeaders();

        if (pluginEditorTrackIndex >= 0)
            pluginEditorTrackIndex = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), remapTrackIndex(pluginEditorTrackIndex));
        if (channelRackTrackIndex >= 0)
            channelRackTrackIndex = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), remapTrackIndex(channelRackTrackIndex));
        if (eqTrackIndex >= 0)
            eqTrackIndex = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), remapTrackIndex(eqTrackIndex));

        const int remappedSelectedTrack = juce::jlimit(0, juce::jmax(0, tracks.size() - 1),
                                                       remapTrackIndex(selectedTrackIndex));
        setSelectedTrackIndex(remappedSelectedTrack);
        timeline.selectTrack(remappedSelectedTrack);
        mixer.selectTrack(remappedSelectedTrack);
        refreshChannelRackWindow();
        rebuildRealtimeSnapshot();
        resized();
    }

    void MainComponent::createNewTrack(Track::ChannelType type)
    {
        const int idx = tracks.size();
        if (idx >= maxRealtimeTracks)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Track Limit",
                                                   "Maximum realtime track count reached.");
            return;
        }

        auto* t = new Track("Track " + juce::String(idx + 1), formatManager);
        t->setTransportPlayHead(&transport);
        t->setChannelType(type);
        if (type == Track::ChannelType::Audio)
            t->disableBuiltInInstrument();
        else
            t->useBuiltInSynthInstrument();
        if (monitorSafeMode)
        {
            t->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
            t->setInputMonitorGain(juce::jmin(t->getInputMonitorGain(), 0.72f));
        }
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        if (setup.sampleRate > 0)
            t->prepareToPlay(setup.sampleRate, setup.bufferSize > 0 ? setup.bufferSize : 512);

        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            tracks.add(t);
        }
        trackMidiBuffers[static_cast<size_t>(idx)].ensureSize(4096);
        ensureDefaultAutomationLanesForTrack(idx);
        mixer.addTrack(t, idx);
        timeline.refreshHeaders(); 
        setSelectedTrackIndex(idx);
        timeline.selectTrack(idx);
        mixer.selectTrack(idx);
        sanitizeRoutingConfiguration(false);
        resized();
        rebuildRealtimeSnapshot();
    }

    void MainComponent::showAudioSettings()
    {
        refreshMidiInputSelector();
        refreshMidiOutputSelector();
        refreshControlSurfaceInputSelector();
        refreshAudioEngineSelectors();

        juce::DialogWindow::LaunchOptions opt;
        opt.content.setOwned(new juce::AudioDeviceSelectorComponent(deviceManager, 0, 256, 0, 256, true, true, true, false));
        opt.content->setSize(500, 450);
        opt.dialogTitle = "Audio Settings";
        opt.componentToCentreAround = this;
        opt.launchAsync();
    }

    void MainComponent::showProjectSettingsMenu()
    {
        juce::PopupMenu keyMenu;
        for (int i = 0; i < keyNames.size(); ++i)
            keyMenu.addItem(100 + i, keyNames[i], true, projectKeyRoot == i);

        juce::PopupMenu scaleMenu;
        for (int i = 0; i < scaleNames.size(); ++i)
            scaleMenu.addItem(200 + i, scaleNames[i], true, projectScaleMode == i);

        juce::PopupMenu transposeMenu;
        for (int semis = -24; semis <= 24; ++semis)
        {
            const juce::String label = juce::String(semis > 0 ? "+" : "") + juce::String(semis) + " st";
            transposeMenu.addItem(300 + (semis + 24), label, true, projectTransposeSemitones == semis);
        }

        juce::PopupMenu menu;
        menu.addSectionHeader("Project Settings");
        menu.addItem(1, "Audio / MIDI Device Setup...");
        menu.addItem(2, "Tempo / Meter...");
        menu.addItem(3, "Recording Setup Panel");
        menu.addItem(4, "Export...");
        menu.addSubMenu("Key", keyMenu);
        menu.addSubMenu("Scale", scaleMenu);
        menu.addSubMenu("Transpose", transposeMenu);
        menu.addSeparator();
        menu.addItem(5, "Help Guide");

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&projectButton),
                           [this](int selectedId)
                           {
                               if (selectedId == 0)
                                   return;

                               if (selectedId == 1)
                               {
                                   showAudioSettings();
                                   return;
                               }

                               if (selectedId == 2)
                               {
                                   showTempoMenu();
                                   return;
                               }

                               if (selectedId == 3)
                               {
                                   if (bottomTabs.getNumTabs() > 0)
                                       bottomTabs.setCurrentTabIndex(bottomTabs.getNumTabs() - 1);
                                   return;
                               }

                               if (selectedId == 4)
                               {
                                   showExportMenu();
                                   return;
                               }

                               if (selectedId == 5)
                               {
                                   showHelpGuide();
                                   return;
                               }

                               if (selectedId >= 100 && selectedId < 100 + keyNames.size())
                               {
                                   projectKeyRoot = juce::jlimit(0, 11, selectedId - 100);
                                   keySelector.setSelectedId(projectKeyRoot + 1, juce::dontSendNotification);
                                   applyProjectScaleToEngines();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedId >= 200 && selectedId < 200 + scaleNames.size())
                               {
                                   projectScaleMode = juce::jlimit(0, scaleNames.size() - 1, selectedId - 200);
                                   scaleSelector.setSelectedId(projectScaleMode + 1, juce::dontSendNotification);
                                   applyProjectScaleToEngines();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedId >= 300 && selectedId <= 348)
                               {
                                   const int semitones = (selectedId - 300) - 24;
                                   applyGlobalTransposeToSelection(semitones);
                                   return;
                               }
                           });
    }

    void MainComponent::refreshAudioEngineSelectors()
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);

        const auto currentRate = juce::jmax(1.0, setup.sampleRate > 0.0
                                                      ? setup.sampleRate
                                                      : sampleRateRt.load(std::memory_order_relaxed));
        const int currentBuffer = juce::jmax(0, setup.bufferSize);

        const std::array<double, 6> sampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        int sampleRateId = 0;
        for (int i = 0; i < static_cast<int>(sampleRates.size()); ++i)
        {
            if (std::abs(sampleRates[static_cast<size_t>(i)] - currentRate) <= 2.0)
            {
                sampleRateId = i + 1;
                break;
            }
        }
        if (sampleRateId > 0)
            sampleRateSelector.setSelectedId(sampleRateId, juce::dontSendNotification);
        else
            sampleRateSelector.setText("SR " + juce::String(currentRate, 0), juce::dontSendNotification);

        const std::array<int, 7> bufferSizes { 32, 64, 128, 256, 512, 1024, 2048 };
        int bufferSizeId = 0;
        for (int i = 0; i < static_cast<int>(bufferSizes.size()); ++i)
        {
            if (bufferSizes[static_cast<size_t>(i)] == currentBuffer)
            {
                bufferSizeId = i + 1;
                break;
            }
        }
        if (bufferSizeId > 0)
            bufferSizeSelector.setSelectedId(bufferSizeId, juce::dontSendNotification);
        else
            bufferSizeSelector.setText("Buf " + juce::String(currentBuffer), juce::dontSendNotification);

        lowLatencyToggle.setToggleState(lowLatencyMode, juce::dontSendNotification);
    }

    void MainComponent::applySampleRateSelection(int selectedId)
    {
        const std::array<double, 6> sampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
        if (selectedId <= 0 || selectedId > static_cast<int>(sampleRates.size()))
            return;

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);

        const double requestedRate = sampleRates[static_cast<size_t>(selectedId - 1)];
        if (std::abs(setup.sampleRate - requestedRate) <= 0.5)
            return;

        setup.sampleRate = requestedRate;
        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Engine",
                                                   "Sample-rate change failed:\n" + error);
        }

        refreshAudioEngineSelectors();
        refreshStatusText();
    }

    void MainComponent::applyBufferSizeSelection(int selectedId)
    {
        const std::array<int, 7> bufferSizes { 32, 64, 128, 256, 512, 1024, 2048 };
        if (selectedId <= 0 || selectedId > static_cast<int>(bufferSizes.size()))
            return;

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);

        const int requestedBuffer = bufferSizes[static_cast<size_t>(selectedId - 1)];
        if (setup.bufferSize == requestedBuffer)
            return;

        setup.bufferSize = requestedBuffer;
        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Engine",
                                                   "Buffer-size change failed:\n" + error);
        }

        refreshAudioEngineSelectors();
        refreshStatusText();
    }

    void MainComponent::applyLowLatencyMode(bool enabled)
    {
        lowLatencyMode = enabled;
        lowLatencyModeRt.store(enabled, std::memory_order_relaxed);
        lowLatencyToggle.setToggleState(enabled, juce::dontSendNotification);

        if (enabled)
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            deviceManager.getAudioDeviceSetup(setup);
            if (setup.bufferSize > 256)
            {
                setup.bufferSize = setup.bufferSize >= 1024 ? 256 : 128;
                const auto error = deviceManager.setAudioDeviceSetup(setup, true);
                if (error.isNotEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Low Latency",
                                                           "Could not lower buffer size automatically:\n" + error);
                }
            }
        }

        refreshAudioEngineSelectors();
        refreshStatusText();
    }

    void MainComponent::setMonitorSafeMode(bool enabled)
    {
        monitorSafeMode = enabled;
        monitorSafeModeRt.store(enabled, std::memory_order_relaxed);
        refreshInputDeviceSafetyState();

        for (auto* track : tracks)
        {
            if (track == nullptr || (!track->isInputMonitoringEnabled() && !track->isArmed()))
                continue;

            if (monitorSafeMode)
            {
                track->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
                track->setInputMonitorGain(juce::jmin(track->getInputMonitorGain(), 0.72f));
            }
        }

        if (auto* recording = dynamic_cast<RecordingPanelContent*>(recordingPanelView))
            recording->setMonitorSafeState(monitorSafeMode);

        refreshStatusText();
    }

    void MainComponent::calibrateInputMonitoring()
    {
        float sourcePeak = liveInputPeakRt.load(std::memory_order_relaxed);
        if (!std::isfinite(sourcePeak))
            sourcePeak = 0.0f;

        const float targetPeak = monitorSafeMode ? 0.48f : 0.62f;
        float gain = monitorSafeMode ? 0.62f : 0.82f;
        if (sourcePeak > 0.0025f)
            gain = juce::jlimit(0.14f, 1.6f, targetPeak / sourcePeak);

        int adjustedTracks = 0;
        for (auto* track : tracks)
        {
            if (track == nullptr || (!track->isInputMonitoringEnabled() && !track->isArmed()))
                continue;
            track->setInputMonitorGain(gain);
            if (monitorSafeMode)
                track->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
            ++adjustedTracks;
        }

        if (adjustedTracks == 0 && juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
        {
            tracks[selectedTrackIndex]->setInputMonitorGain(gain);
            if (monitorSafeMode)
                tracks[selectedTrackIndex]->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
        }

        refreshStatusText();
    }

    void MainComponent::clearInputPeakHolds()
    {
        for (auto* track : tracks)
            if (track != nullptr)
                track->clearInputPeakHold();
    }

    void MainComponent::refreshInputDeviceSafetyState()
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr)
        {
            usingLikelyBuiltInAudioRt.store(false, std::memory_order_relaxed);
            builtInMicHardFailSafeRt.store(false, std::memory_order_relaxed);
            inputMonitorSafetyTrimRt.store(1.0f, std::memory_order_relaxed);
            builtInMonitorSafetyNoticeRequestedRt.store(false, std::memory_order_relaxed);
            return;
        }

        const auto deviceName = device->getName();
        const bool likelyBuiltIn = deviceName.containsIgnoreCase("MacBook")
                                || deviceName.containsIgnoreCase("Built-In")
                                || deviceName.containsIgnoreCase("Internal")
                                || deviceName.containsIgnoreCase("Apple");
        usingLikelyBuiltInAudioRt.store(likelyBuiltIn, std::memory_order_relaxed);
        const bool hardFailSafe = likelyBuiltIn && monitorSafeMode;
        builtInMicHardFailSafeRt.store(hardFailSafe, std::memory_order_relaxed);
        if (!hardFailSafe)
        {
            feedbackHazardBlocksRt.store(0, std::memory_order_relaxed);
            feedbackAutoMuteRequestedRt.store(false, std::memory_order_relaxed);
        }

        const float safetyTrim = hardFailSafe
            ? 0.38f
            : 1.0f;
        inputMonitorSafetyTrimRt.store(safetyTrim, std::memory_order_relaxed);

        const int activeInputChannels = juce::jmax(1, device->getActiveInputChannels().countNumberOfSetBits());
        const int maxValidPair = juce::jmax(0, (activeInputChannels - 1) / 2);
        bool monitoringDisabledBySafety = false;
        for (auto* track : tracks)
        {
            if (track == nullptr)
                continue;

            const int configuredPair = track->getInputSourcePair();
            if (configuredPair > maxValidPair)
                track->setInputSourcePair(-1);

            if (!hardFailSafe)
                continue;

            track->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
            track->setInputMonitorGain(juce::jmin(track->getInputMonitorGain(), 0.42f));
            if (track->isInputMonitoringEnabled())
                track->setSendLevel(0.0f);

            if (track->isInputMonitoringEnabled()
                && !track->isArmed()
                && !recordButton.getToggleState())
            {
                track->setInputMonitoring(false);
                monitoringDisabledBySafety = true;
            }
        }

        if (hardFailSafe && (monitoringDisabledBySafety || !builtInMonitorSafetyNoticeShown))
            builtInMonitorSafetyNoticeRequestedRt.store(true, std::memory_order_relaxed);
    }

    juce::String MainComponent::getPluginIdentity(const juce::PluginDescription& desc) const
    {
        auto identifier = desc.fileOrIdentifier.trim();
        if (identifier.isEmpty())
            identifier = desc.name.trim() + "|" + desc.manufacturerName.trim();
        return (desc.pluginFormatName.trim() + "|" + identifier).toLowerCase();
    }

    int MainComponent::getPluginFormatRank(const juce::String& formatName) const
    {
       #if JUCE_MAC
        const bool preferAu = !preferredMacPluginFormat.equalsIgnoreCase("VST3");
        if (formatNameLooksLikeAudioUnit(formatName))
            return preferAu ? 0 : 1;
        if (formatNameLooksLikeVST3(formatName))
            return preferAu ? 1 : 0;
       #else
        if (formatNameLooksLikeVST3(formatName))
            return 0;
        if (formatNameLooksLikeAudioUnit(formatName))
            return 1;
       #endif
        return 2;
    }

    bool MainComponent::pluginDescriptionsShareIdentity(const juce::PluginDescription& a,
                                                        const juce::PluginDescription& b) const
    {
        if (a.isInstrument != b.isInstrument)
            return false;

        const auto aName = a.name.trim();
        const auto bName = b.name.trim();
        if (aName.isEmpty() || bName.isEmpty())
            return false;
        if (!aName.equalsIgnoreCase(bName))
            return false;

        const auto aMfr = a.manufacturerName.trim();
        const auto bMfr = b.manufacturerName.trim();
        if (!aMfr.equalsIgnoreCase(bMfr))
            return false;

        return true;
    }

    juce::Array<juce::PluginDescription> MainComponent::getPluginLoadCandidates(const juce::PluginDescription& requested,
                                                                                 bool preferRequestedFormatFirst) const
    {
        juce::Array<juce::PluginDescription> directMatches;
        juce::Array<juce::PluginDescription> identityMatches;
        juce::StringArray seenKeys;

        const auto addUnique = [&seenKeys](juce::Array<juce::PluginDescription>& destination,
                                           const juce::PluginDescription& candidate)
        {
            const auto key = (candidate.pluginFormatName.trim()
                              + "|"
                              + candidate.fileOrIdentifier.trim()
                              + "|"
                              + candidate.name.trim()
                              + "|"
                              + candidate.manufacturerName.trim()
                              + "|"
                              + juce::String(candidate.isInstrument ? 1 : 0)).toLowerCase();
            if (seenKeys.contains(key))
                return;
            seenKeys.add(key);
            destination.add(candidate);
        };

        const auto knownTypes = knownPluginList.getTypes();
        for (const auto& candidate : knownTypes)
        {
            if (requested.fileOrIdentifier.isNotEmpty()
                && candidate.fileOrIdentifier.equalsIgnoreCase(requested.fileOrIdentifier))
            {
                if (requested.pluginFormatName.isEmpty()
                    || candidate.pluginFormatName.equalsIgnoreCase(requested.pluginFormatName))
                {
                    addUnique(directMatches, candidate);
                }
                else
                {
                    addUnique(identityMatches, candidate);
                }
                continue;
            }

            if (pluginDescriptionsShareIdentity(candidate, requested))
                addUnique(identityMatches, candidate);
        }

        const auto sortByPreference = [this, &requested, preferRequestedFormatFirst](const juce::PluginDescription& a,
                                                                                     const juce::PluginDescription& b)
        {
            if (preferRequestedFormatFirst && requested.pluginFormatName.isNotEmpty())
            {
                const bool aRequestedFormat = a.pluginFormatName.equalsIgnoreCase(requested.pluginFormatName);
                const bool bRequestedFormat = b.pluginFormatName.equalsIgnoreCase(requested.pluginFormatName);
                if (aRequestedFormat != bRequestedFormat)
                    return aRequestedFormat;
            }

            const int rankA = getPluginFormatRank(a.pluginFormatName);
            const int rankB = getPluginFormatRank(b.pluginFormatName);
            if (rankA != rankB)
                return rankA < rankB;
            return a.pluginFormatName.compareIgnoreCase(b.pluginFormatName) < 0;
        };

        std::sort(directMatches.begin(), directMatches.end(), sortByPreference);
        std::sort(identityMatches.begin(), identityMatches.end(), sortByPreference);

        juce::Array<juce::PluginDescription> ordered;
        for (const auto& candidate : directMatches)
            ordered.add(candidate);
        for (const auto& candidate : identityMatches)
            ordered.add(candidate);

        if (ordered.isEmpty() && requested.fileOrIdentifier.isNotEmpty())
            ordered.add(requested);

        return ordered;
    }

    juce::StringArray MainComponent::getPluginScanFormatsInPreferredOrder() const
    {
        juce::StringArray formats;
        for (int i = 0; i < formatManager.getNumFormats(); ++i)
        {
            const auto* format = formatManager.getFormat(i);
            if (format == nullptr)
                continue;
            const auto formatName = format->getName().trim();
            if (formatName.isNotEmpty())
                formats.addIfNotAlreadyThere(formatName);
        }

        std::vector<juce::String> sortedFormats;
        sortedFormats.reserve(static_cast<size_t>(formats.size()));
        for (const auto& formatName : formats)
            sortedFormats.push_back(formatName);

        std::sort(sortedFormats.begin(),
                  sortedFormats.end(),
                  [this](const juce::String& a, const juce::String& b)
                  {
                      const int rankA = getPluginFormatRank(a);
                      const int rankB = getPluginFormatRank(b);
                      if (rankA != rankB)
                          return rankA < rankB;
                      return a.compareIgnoreCase(b) < 0;
                  });

        juce::StringArray ordered;
        for (const auto& formatName : sortedFormats)
            ordered.add(formatName);
        return ordered;
    }

    void MainComponent::applyDeadMansPedalBlacklist(const juce::String& scanFormatName)
    {
        if (!pluginScanDeadMansPedalFile.existsAsFile())
            return;

        juce::StringArray deadmanEntries;
        deadmanEntries.addLines(pluginScanDeadMansPedalFile.loadFileAsString());
        for (auto entry : deadmanEntries)
        {
            entry = entry.trim();
            if (entry.isEmpty())
                continue;
            pluginScanBlacklistedItems.addIfNotAlreadyThere(scanFormatDisplayName(scanFormatName)
                                                            + ": " + entry);
        }

        if (knownPluginListFile.existsAsFile())
        {
            if (std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(knownPluginListFile)); xml != nullptr)
                knownPluginList.recreateFromXml(*xml);
        }

        juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal(knownPluginList,
                                                                           pluginScanDeadMansPedalFile);
        pluginScanDeadMansPedalFile.deleteFile();

        if (std::unique_ptr<juce::XmlElement> xml(knownPluginList.createXml()); xml != nullptr)
            xml->writeTo(knownPluginListFile);
    }

    void MainComponent::handlePluginScanPassResult(int exitCode, const juce::String& output, bool timedOut)
    {
        const auto currentFormat = activeScanFormat;
        const auto displayFormat = scanFormatDisplayName(currentFormat);
        juce::StringArray lines;
        lines.addLines(output);

        for (auto line : lines)
        {
            line = line.trim();
            if (line.startsWithIgnoreCase("FAILED:"))
            {
                const auto failedPath = line.fromFirstOccurrenceOf("FAILED:", false, false).trim();
                if (failedPath.isNotEmpty())
                    pluginScanFailedItems.addIfNotAlreadyThere(displayFormat + ": " + failedPath);
                continue;
            }

            if (line.startsWithIgnoreCase("BLACKLISTED:"))
            {
                auto blacklistedItem = line.fromFirstOccurrenceOf("BLACKLISTED:", false, false).trim();
                if (blacklistedItem.contains(":"))
                    blacklistedItem = blacklistedItem.fromFirstOccurrenceOf(":", false, false).trim();
                if (blacklistedItem.isNotEmpty())
                    pluginScanBlacklistedItems.addIfNotAlreadyThere(displayFormat + ": " + blacklistedItem);
            }
        }

        if (timedOut)
        {
            pluginScanFailedItems.addIfNotAlreadyThere(displayFormat
                + ": scan pass timed out after "
                + juce::String(pluginScanPassTimeoutMs) + " ms");
        }
        else if (exitCode != 0)
        {
            pluginScanFailedItems.addIfNotAlreadyThere(displayFormat
                + ": scan pass exited with code "
                + juce::String(exitCode));
        }

        if (timedOut || exitCode != 0)
            applyDeadMansPedalBlacklist(currentFormat);
        else
            pluginScanDeadMansPedalFile.deleteFile();
    }

    void MainComponent::recordLastLoadedPlugin(const juce::PluginDescription& desc)
    {
        if (desc.fileOrIdentifier.trim().isEmpty() && desc.name.trim().isEmpty())
            return;
        lastLoadedPluginDescription = desc;
        hasLastLoadedPluginDescription = true;
        writePluginSessionGuard(false);
    }

    bool MainComponent::readPluginSessionGuard(bool& cleanState, juce::PluginDescription& lastPlugin) const
    {
        cleanState = true;
        lastPlugin = {};
        if (!pluginSessionGuardFile.existsAsFile())
            return false;

        juce::var parsed = juce::JSON::parse(pluginSessionGuardFile.loadFileAsString());
        auto* root = parsed.getDynamicObject();
        if (root == nullptr)
            return false;

        cleanState = static_cast<bool>(root->getProperty("clean"));
        const juce::var lastPluginVar = root->getProperty("lastPlugin");
        auto* pluginObj = lastPluginVar.getDynamicObject();
        if (pluginObj != nullptr)
        {
            lastPlugin.pluginFormatName = pluginObj->getProperty("pluginFormatName").toString();
            lastPlugin.fileOrIdentifier = pluginObj->getProperty("fileOrIdentifier").toString();
            lastPlugin.name = pluginObj->getProperty("name").toString();
            lastPlugin.manufacturerName = pluginObj->getProperty("manufacturerName").toString();
            lastPlugin.isInstrument = static_cast<bool>(pluginObj->getProperty("isInstrument"));
            lastPlugin.uniqueId = static_cast<int>(pluginObj->getProperty("uniqueId"));
            lastPlugin.deprecatedUid = static_cast<int>(pluginObj->getProperty("deprecatedUid"));
        }

        return true;
    }

    void MainComponent::writePluginSessionGuard(bool cleanState) const
    {
        if (pluginSessionGuardFile == juce::File())
            return;

        auto root = std::make_unique<juce::DynamicObject>();
        root->setProperty("clean", cleanState);
        root->setProperty("timestampMs", juce::Time::getMillisecondCounterHiRes());

        if (hasLastLoadedPluginDescription)
        {
            auto pluginObj = std::make_unique<juce::DynamicObject>();
            pluginObj->setProperty("pluginFormatName", lastLoadedPluginDescription.pluginFormatName);
            pluginObj->setProperty("fileOrIdentifier", lastLoadedPluginDescription.fileOrIdentifier);
            pluginObj->setProperty("name", lastLoadedPluginDescription.name);
            pluginObj->setProperty("manufacturerName", lastLoadedPluginDescription.manufacturerName);
            pluginObj->setProperty("isInstrument", lastLoadedPluginDescription.isInstrument);
            pluginObj->setProperty("uniqueId", lastLoadedPluginDescription.uniqueId);
            pluginObj->setProperty("deprecatedUid", lastLoadedPluginDescription.deprecatedUid);
            root->setProperty("lastPlugin", juce::var(pluginObj.release()));
        }

        const juce::var serialised(root.release());
        juce::ignoreUnused(pluginSessionGuardFile.replaceWithText(juce::JSON::toString(serialised, true)));
    }

    void MainComponent::handleUncleanPluginSessionRecovery()
    {
        bool previousSessionClean = true;
        juce::PluginDescription previousLastPlugin;
        if (!readPluginSessionGuard(previousSessionClean, previousLastPlugin))
            return;

        if (previousLastPlugin.fileOrIdentifier.isNotEmpty() || previousLastPlugin.name.isNotEmpty())
        {
            lastLoadedPluginDescription = previousLastPlugin;
            hasLastLoadedPluginDescription = true;
        }

        if (previousSessionClean || !autoQuarantineOnUncleanExit)
            return;

        if (previousLastPlugin.fileOrIdentifier.isEmpty() && previousLastPlugin.name.isEmpty())
            return;

        quarantinePlugin(previousLastPlugin,
                         "Automatically quarantined after abnormal app termination.");
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Plugin Recovery",
            "The previous session ended abnormally.\n\n"
            "Auto-quarantined plugin:\n"
                + (previousLastPlugin.name.isNotEmpty()
                       ? previousLastPlugin.name
                       : previousLastPlugin.fileOrIdentifier)
                + " (" + previousLastPlugin.pluginFormatName + ")\n\n"
            "Reason: it was the last plugin loaded before the abnormal termination.");
    }

    bool MainComponent::isPluginQuarantined(const juce::PluginDescription& desc) const
    {
        if (!pluginSafetyGuardsEnabled)
            return false;
        return quarantinedPluginIds.contains(getPluginIdentity(desc));
    }

    void MainComponent::loadQuarantinedPlugins()
    {
        quarantinedPluginIds.clear();
        if (!pluginSafetyGuardsEnabled)
        {
            quarantinedPluginsFile.deleteFile();
            return;
        }
        if (!quarantinedPluginsFile.existsAsFile())
            return;

        juce::StringArray entries;
        entries.addLines(quarantinedPluginsFile.loadFileAsString());
        for (auto entry : entries)
        {
            entry = entry.trim();
            if (entry.isNotEmpty())
                quarantinedPluginIds.addIfNotAlreadyThere(entry);
        }
    }

    void MainComponent::saveQuarantinedPlugins() const
    {
        if (!pluginSafetyGuardsEnabled)
            return;
        auto sorted = quarantinedPluginIds;
        sorted.sortNatural();
        juce::ignoreUnused(quarantinedPluginsFile.replaceWithText(sorted.joinIntoString("\n")));
    }

    void MainComponent::quarantinePlugin(const juce::PluginDescription& desc, const juce::String& reason)
    {
        if (!pluginSafetyGuardsEnabled)
            return;
        const auto identity = getPluginIdentity(desc);
        if (identity.isEmpty() || quarantinedPluginIds.contains(identity))
            return;

        quarantinedPluginIds.add(identity);
        saveQuarantinedPlugins();
        juce::Logger::writeToLog("Plugin quarantined: " + desc.name
                                 + " [" + desc.pluginFormatName + "] reason=" + reason);
    }

    void MainComponent::unquarantinePlugin(const juce::PluginDescription& desc)
    {
        if (!pluginSafetyGuardsEnabled)
            return;
        const auto identity = getPluginIdentity(desc);
        if (identity.isEmpty())
            return;
        quarantinedPluginIds.removeString(identity);
        saveQuarantinedPlugins();
    }

    void MainComponent::applyFeedbackSafetyIfRequested()
    {
        if (!feedbackAutoMuteRequestedRt.exchange(false, std::memory_order_relaxed))
            return;

        const int currentMuteBlocks = outputSafetyMuteBlocksRt.load(std::memory_order_relaxed);
        outputSafetyMuteBlocksRt.store(juce::jmax(currentMuteBlocks, 96), std::memory_order_relaxed);

        bool disabledAnyMonitoring = false;
        for (auto* track : tracks)
        {
            if (track == nullptr || !track->isInputMonitoringEnabled())
                continue;
            track->setInputMonitoring(false);
            track->setSendLevel(0.0f);
            disabledAnyMonitoring = true;
        }

        if (!monitorSafeMode)
            setMonitorSafeMode(true);

        if (disabledAnyMonitoring)
        {
            feedbackWarningPending = true;
            refreshChannelRackWindow();
            refreshStatusText();
        }
    }

    bool MainComponent::ensureMicrophonePermissionForInputUse(const juce::String& trigger)
    {
       #if JUCE_MAC || JUCE_IOS
        if (juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio))
            return true;

        if (micPermissionPromptedOnce)
        {
            juce::Logger::writeToLog("Mic permission unavailable (previously requested), trigger=" + trigger);
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Microphone Permission",
                                                   "Microphone access is required for input monitoring/recording.\n\n"
                                                   "Enable it in System Settings, then try again.");
            return false;
        }

        micPermissionPromptedOnce = true;
        saveStartupPreferences();
        juce::Logger::writeToLog("Requesting microphone permission, trigger=" + trigger);

        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                          [safeThis = juce::Component::SafePointer<MainComponent>(this),
                                           trigger](bool granted)
                                          {
                                              if (safeThis == nullptr)
                                                  return;
                                              juce::Logger::writeToLog("Microphone permission result ("
                                                                       + trigger + "): "
                                                                       + (granted ? juce::String("granted")
                                                                                  : juce::String("denied")));
                                              if (!granted)
                                              {
                                                  juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                         "Microphone Permission",
                                                                                         "Microphone access was denied.\n\n"
                                                                                         "Recording/input monitoring will stay disabled.");
                                              }
                                          });

        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                               "Microphone Permission",
                                               "A one-time microphone permission request was sent.\n\n"
                                               "After choosing Allow or Don't Allow, trigger monitoring/record again.");
        return false;
       #else
        juce::ignoreUnused(trigger);
        return true;
       #endif
    }

    bool MainComponent::ensureAudioInputChannelsActive(const juce::String& trigger)
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        if (setup.useDefaultInputChannels || setup.inputChannels.countNumberOfSetBits() > 0)
            return true;

        auto* device = deviceManager.getCurrentAudioDevice();
        const int availableInputChannels = device != nullptr
            ? device->getInputChannelNames().size()
            : 0;
        if (availableInputChannels <= 0)
        {
            juce::Logger::writeToLog("No audio input channels available, trigger=" + trigger);
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Input",
                                                   "No audio input channels are available on the current device.");
            return false;
        }

        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setBit(0);
        if (availableInputChannels > 1)
            setup.inputChannels.setBit(1);

        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            juce::Logger::writeToLog("Failed to enable audio input channels, trigger="
                                     + trigger + ", error=" + error);
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Input",
                                                   "Unable to enable audio input channels:\n" + error);
            return false;
        }

        juce::Logger::writeToLog("Enabled audio input channels, trigger=" + trigger);
        refreshInputDeviceSafetyState();
        refreshAudioEngineSelectors();
        return true;
    }

    bool MainComponent::ensureAudioInputReadyForMonitoring(const juce::String& trigger)
    {
        if (!ensureMicrophonePermissionForInputUse(trigger))
            return false;
        return ensureAudioInputChannelsActive(trigger);
    }

    bool MainComponent::requestTrackInputMonitoringState(int trackIndex, bool enable, const juce::String& trigger)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return false;

        auto* track = tracks[trackIndex];
        if (track == nullptr)
            return false;

        if (!enable)
        {
            track->setInputMonitoring(false);
            refreshStatusText();
            return true;
        }

        if (!ensureAudioInputReadyForMonitoring(trigger))
        {
            track->setInputMonitoring(false);
            refreshStatusText();
            return false;
        }

        track->setInputMonitoring(true);
        if (usingLikelyBuiltInAudioRt.load(std::memory_order_relaxed))
        {
            track->setMonitorTapMode(Track::MonitorTapMode::PreInserts);
            track->setInputMonitorGain(juce::jmin(track->getInputMonitorGain(), 0.60f));
        }

        refreshInputDeviceSafetyState();
        refreshStatusText();
        return true;
    }

    void MainComponent::enforceStartupOutputOnlyMode()
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        if (!setup.useDefaultInputChannels && setup.inputChannels.countNumberOfSetBits() == 0)
            return;

        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        const auto error = deviceManager.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty())
        {
            juce::Logger::writeToLog("Startup output-only enforcement failed: " + error);
            return;
        }

        juce::Logger::writeToLog("Startup output-only mode active: audio input channels disabled.");
        refreshInputDeviceSafetyState();
    }

    bool MainComponent::runPluginIsolationProbe(const juce::PluginDescription& desc,
                                                bool instrumentPlugin,
                                                juce::String& errorMessage) const
    {
        errorMessage.clear();
        if (!pluginSafetyGuardsEnabled)
            return true;

        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        if (!executable.existsAsFile())
        {
            errorMessage = "Plugin probe failed: host executable path not found.";
            return false;
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        const double sr = juce::jmax(44100.0,
                                     device != nullptr ? device->getCurrentSampleRate() : 44100.0);
        const int bs = juce::jlimit(64, 4096,
                                    device != nullptr ? device->getCurrentBufferSizeSamples() : 512);

        const juce::String command = executable.getFullPathName().quoted()
                                   + " --plugin-probe"
                                   + " --format=" + desc.pluginFormatName.quoted()
                                   + " --id=" + desc.fileOrIdentifier.quoted()
                                   + " --name=" + desc.name.quoted()
                                   + " --mfr=" + desc.manufacturerName.quoted()
                                   + " --uid2=" + juce::String::toHexString(desc.uniqueId)
                                   + " --uid=" + juce::String::toHexString(desc.deprecatedUid)
                                   + " --instrument=" + juce::String(instrumentPlugin ? 1 : 0)
                                   + " --sr=" + juce::String(sr, 5)
                                   + " --bs=" + juce::String(bs);

        juce::ChildProcess probe;
        if (!probe.start(command))
        {
            errorMessage = "Plugin probe failed: unable to launch isolation process.";
            return false;
        }

        if (!probe.waitForProcessToFinish(12000))
        {
            probe.kill();
            errorMessage = "Plugin probe timed out.";
            return false;
        }

        const auto exitCode = probe.getExitCode();
        const juce::String output = probe.readAllProcessOutput().trim();
        if (exitCode != 0)
        {
            errorMessage = "Plugin isolation probe failed."
                         + (output.isNotEmpty() ? ("\n" + output) : juce::String());
            return false;
        }

        if (!output.startsWithIgnoreCase("OK"))
        {
            errorMessage = "Plugin isolation probe returned an unexpected result."
                         + (output.isNotEmpty() ? ("\n" + output) : juce::String());
            return false;
        }

        return true;
    }

    juce::String MainComponent::getLatencySummaryText() const
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr)
            return "No audio device selected";

        const double sampleRate = juce::jmax(1.0, device->getCurrentSampleRate());
        const int bufferSamples = juce::jmax(0, device->getCurrentBufferSizeSamples());
        const int inputLatencySamples = juce::jmax(0, device->getInputLatencyInSamples());
        const int outputLatencySamples = juce::jmax(0, device->getOutputLatencyInSamples());
        const int roundTripSamples = inputLatencySamples + outputLatencySamples + bufferSamples;

        const auto toMs = [sampleRate](int samples)
        {
            return (1000.0 * static_cast<double>(samples)) / sampleRate;
        };

        int maxPluginLatencySamples = 0;
        int maxAuxInsertLatencySamples = 0;
        int selectedTrackLatencySamples = 0;
        if (juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
            selectedTrackLatencySamples = juce::jmax(0, tracks[selectedTrackIndex]->getTotalPluginLatencySamples());

        for (auto* track : tracks)
        {
            if (track == nullptr)
                continue;
            maxPluginLatencySamples = juce::jmax(maxPluginLatencySamples, track->getTotalPluginLatencySamples());
        }
        for (int bus = 0; bus < auxBusCount; ++bus)
            maxAuxInsertLatencySamples = juce::jmax(maxAuxInsertLatencySamples, getAuxBusProcessingLatencySamples(bus));
        const int graphPdcSamples = juce::jmax(maxPluginLatencySamples,
                                               maxPdcLatencySamplesRt.load(std::memory_order_relaxed));

        const int captureCompSamples = juce::jmax(0, inputLatencySamples + recordingManualOffsetSamples);
        return "Latency: In " + juce::String(inputLatencySamples) + " (" + juce::String(toMs(inputLatencySamples), 2) + "ms)"
             + "  Out " + juce::String(outputLatencySamples) + " (" + juce::String(toMs(outputLatencySamples), 2) + "ms)"
             + "  RTL " + juce::String(roundTripSamples) + " (" + juce::String(toMs(roundTripSamples), 2) + "ms)"
             + "  |  RecComp " + juce::String(captureCompSamples) + " (" + juce::String(toMs(captureCompSamples), 2) + "ms)"
             + "  Manual " + juce::String(recordingManualOffsetSamples) + " smp"
             + "  |  SR " + juce::String(sampleRate, 0) + "  Buf " + juce::String(bufferSamples)
             + "  |  PDC Sel " + juce::String(selectedTrackLatencySamples) + " (" + juce::String(toMs(selectedTrackLatencySamples), 2) + "ms)"
             + "  AuxMax " + juce::String(maxAuxInsertLatencySamples) + " (" + juce::String(toMs(maxAuxInsertLatencySamples), 2) + "ms)"
             + "  Graph " + juce::String(graphPdcSamples) + " (" + juce::String(toMs(graphPdcSamples), 2) + "ms)"
             + "  |  Safe " + (monitorSafeMode ? juce::String("ON") : juce::String("OFF"));
    }

    void MainComponent::showExportMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Export Mixdown...");
        menu.addItem(2, "Export Stems...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportButton),
                           [this](int selectedId)
                           {
                               if (selectedId == 1)
                                   exportMixdown();
                               else if (selectedId == 2)
                                   exportStems();
                           });
    }

    void MainComponent::showTempoMenu()
    {
        juce::PopupMenu menu;
        const double playheadBeat = transport.getCurrentBeat();
        const double currentTempo = getTempoAtBeat(playheadBeat);

        menu.addItem(1, "Add Tempo Event @ Beat " + juce::String(playheadBeat, 2)
                        + " (" + juce::String(currentTempo, 1) + " BPM)");
        menu.addItem(2, "Remove Nearest Tempo Event");
        menu.addItem(3, "Clear Tempo Events (Keep Beat 0)");
        menu.addSeparator();
        menu.addItem(4, "Set Project BPM...");

        juce::PopupMenu quickTempoMenu;
        const std::array<double, 8> quickTempos { 70.0, 80.0, 90.0, 100.0, 110.0, 120.0, 130.0, 140.0 };
        for (int i = 0; i < static_cast<int>(quickTempos.size()); ++i)
            quickTempoMenu.addItem(100 + i, juce::String(quickTempos[static_cast<size_t>(i)], 0) + " BPM");
        menu.addSubMenu("Quick BPM", quickTempoMenu);

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&tempoMenuButton),
                           [this, playheadBeat, currentTempo](int selectedId)
                           {
                               if (selectedId == 1)
                               {
                                   addTempoEvent(playheadBeat, currentTempo);
                               }
                               else if (selectedId == 2)
                               {
                                   removeTempoEventNear(playheadBeat, 2.0);
                               }
                               else if (selectedId == 3)
                               {
                                   clearTempoEvents();
                               }
                               else if (selectedId == 4)
                               {
                                   auto* prompt = new juce::AlertWindow("Project BPM",
                                                                        "Set project BPM (also updates beat-0 tempo event):",
                                                                        juce::AlertWindow::NoIcon);
                                   prompt->addTextEditor("bpm", juce::String(bpm, 2), "BPM");
                                   prompt->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
                                   prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                                   prompt->enterModalState(true, juce::ModalCallbackFunction::create(
                                       [this, prompt](int result)
                                       {
                                           std::unique_ptr<juce::AlertWindow> owner(prompt);
                                           if (result != 1)
                                               return;
                                           setTempoBpm(juce::jmax(1.0, prompt->getTextEditorContents("bpm").getDoubleValue()));
                                           refreshStatusText();
                                       }));
                               }
                               else if (selectedId >= 100 && selectedId < 108)
                               {
                                   const double preset = 70.0 + (selectedId - 100) * 10.0;
                                   setTempoBpm(preset);
                                   refreshStatusText();
                               }
                           });
    }

    void MainComponent::showClipToolsMenu()
    {
        const bool hasSelectedClip = juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size()));
        const bool selectedAudio = hasSelectedClip
            && arrangement[static_cast<size_t>(selectedClipIndex)].type == ClipType::Audio;
        const bool selectedMidi = hasSelectedClip
            && arrangement[static_cast<size_t>(selectedClipIndex)].type == ClipType::MIDI;

        juce::PopupMenu menu;
        menu.addItem(1, "Normalize Audio Clip...", selectedAudio);
        menu.addItem(2, "Audio Gain +3 dB", selectedAudio);
        menu.addItem(3, "Audio Gain -3 dB", selectedAudio);
        menu.addSeparator();
        menu.addItem(4, "Fade In 1/8", selectedAudio);
        menu.addItem(5, "Fade In 1/4", selectedAudio);
        menu.addItem(6, "Fade In 1", selectedAudio);
        menu.addItem(7, "Fade Out 1/8", selectedAudio);
        menu.addItem(8, "Fade Out 1/4", selectedAudio);
        menu.addItem(9, "Fade Out 1", selectedAudio);
        menu.addItem(10, "Reset Fades", selectedAudio);
        menu.addItem(12, "Apply 1/8 Crossfade To Next Clip", selectedAudio);
        menu.addItem(13, "Apply 1/4 Crossfade To Next Clip", selectedAudio);
        menu.addSeparator();
        menu.addItem(11, "Bake Global Transpose into Selected MIDI Clip", selectedMidi && projectTransposeSemitones != 0);

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&clipToolsButton),
                           [this](int selectedId)
                           {
                               if (selectedId == 1) normalizeSelectedAudioClip();
                               else if (selectedId == 2) adjustSelectedAudioClipGain(juce::Decibels::decibelsToGain(3.0f));
                               else if (selectedId == 3) adjustSelectedAudioClipGain(juce::Decibels::decibelsToGain(-3.0f));
                               else if (selectedId == 4) setSelectedAudioClipFades(0.5, -1.0);
                               else if (selectedId == 5) setSelectedAudioClipFades(1.0, -1.0);
                               else if (selectedId == 6) setSelectedAudioClipFades(4.0, -1.0);
                               else if (selectedId == 7) setSelectedAudioClipFades(-1.0, 0.5);
                               else if (selectedId == 8) setSelectedAudioClipFades(-1.0, 1.0);
                               else if (selectedId == 9) setSelectedAudioClipFades(-1.0, 4.0);
                               else if (selectedId == 10) setSelectedAudioClipFades(0.0, 0.0);
                               else if (selectedId == 12 || selectedId == 13)
                               {
                                   if (!juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
                                       return;

                                   const int clipIdx = selectedClipIndex;
                                   const double fadeBeats = selectedId == 12 ? 0.5 : 1.0;
                                   applyArrangementEdit("Apply Crossfade",
                                                        [clipIdx, fadeBeats](std::vector<Clip>& state, int&)
                                                        {
                                                            if (!juce::isPositiveAndBelow(clipIdx, static_cast<int>(state.size())))
                                                                return;
                                                            auto& left = state[static_cast<size_t>(clipIdx)];
                                                            int rightIndex = -1;
                                                            double bestStart = std::numeric_limits<double>::max();
                                                            for (int i = 0; i < static_cast<int>(state.size()); ++i)
                                                            {
                                                                if (i == clipIdx)
                                                                    continue;
                                                                const auto& candidate = state[static_cast<size_t>(i)];
                                                                if (candidate.trackIndex != left.trackIndex)
                                                                    continue;
                                                                if (candidate.startBeat < left.startBeat + left.lengthBeats - 0.0001)
                                                                    continue;
                                                                if (candidate.startBeat < bestStart)
                                                                {
                                                                    bestStart = candidate.startBeat;
                                                                    rightIndex = i;
                                                                }
                                                            }
                                                            if (rightIndex < 0)
                                                                return;
                                                            ArrangementEditing::applySymmetricCrossfade(left,
                                                                                                        state[static_cast<size_t>(rightIndex)],
                                                                                                        fadeBeats);
                                                        });
                               }
                               else if (selectedId == 11)
                               {
                                   if (!juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
                                       return;
                                   if (arrangement[static_cast<size_t>(selectedClipIndex)].type != ClipType::MIDI)
                                       return;
                                   const int semis = projectTransposeSemitones;
                                   applyClipEdit(selectedClipIndex, "Bake Global Transpose",
                                                 [semis](Clip& clip)
                                                 {
                                                     for (auto& ev : clip.events)
                                                         ev.noteNumber = juce::jlimit(0, 127, ev.noteNumber + semis);
                                                 });
                               }
                           });
    }

    void MainComponent::normalizeSelectedAudioClip()
    {
        if (!juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
            return;
        const auto& selected = arrangement[static_cast<size_t>(selectedClipIndex)];
        if (selected.type != ClipType::Audio)
            return;

        const float peak = analyseClipPeak(selected, audioFormatManager, false);
        if (peak <= 0.00001f)
            return;

        const double peakDb = juce::Decibels::gainToDecibels(peak, -120.0f);
        auto safeThis = juce::Component::SafePointer<MainComponent>(this);
        NormalizeDialog::showAsync(peakDb,
                                   [safeThis](std::optional<NormalizeDialog::Result> options)
                                   {
                                       if (safeThis == nullptr || !options.has_value())
                                           return;

                                       safeThis->normalizeSelectedAudioClipWithOptions(options->targetPeakDb,
                                                                                       options->removeDc,
                                                                                       options->preserveDynamics);
                                   });
    }

    void MainComponent::normalizeSelectedAudioClipWithOptions(float targetPeakDb,
                                                              bool removeDc,
                                                              bool preserveDynamics)
    {
        if (!juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
            return;
        const auto& selected = arrangement[static_cast<size_t>(selectedClipIndex)];
        if (selected.type != ClipType::Audio)
            return;

        const float peak = analyseClipPeak(selected, audioFormatManager, removeDc);
        if (peak <= 0.00001f)
            return;

        const float targetPeak = juce::Decibels::decibelsToGain(targetPeakDb);
        float multiplier = juce::jlimit(0.01f, 12.0f, targetPeak / peak);

        // Current normalization path is intentionally gain-only; preserveDynamics keeps this explicit.
        if (!preserveDynamics)
            multiplier = juce::jlimit(0.01f, 12.0f, multiplier);

        adjustSelectedAudioClipGain(multiplier);
    }

    void MainComponent::setSelectedAudioClipFades(double fadeInBeats, double fadeOutBeats)
    {
        if (!juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
            return;

        applyClipEdit(selectedClipIndex, "Set Clip Fades",
                      [fadeInBeats, fadeOutBeats](Clip& clip)
                      {
                          if (clip.type != ClipType::Audio)
                              return;
                          if (fadeInBeats >= 0.0)
                              clip.fadeInBeats = juce::jmax(0.0, fadeInBeats);
                          if (fadeOutBeats >= 0.0)
                              clip.fadeOutBeats = juce::jmax(0.0, fadeOutBeats);
                      });
    }

    void MainComponent::adjustSelectedAudioClipGain(float multiplier)
    {
        if (!juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
            return;

        applyClipEdit(selectedClipIndex, "Adjust Clip Gain",
                      [multiplier](Clip& clip)
                      {
                          if (clip.type != ClipType::Audio)
                              return;
                          clip.gainLinear = juce::jlimit(0.0f, 12.0f, clip.gainLinear * multiplier);
                      });
    }

    void MainComponent::exportMixdown()
    {
        struct FormatChoice { juce::String label; juce::String extension; };
        const std::array<FormatChoice, 5> majorFormats {{
            { "WAV", "wav" },
            { "AIFF", "aiff" },
            { "FLAC", "flac" },
            { "OGG", "ogg" },
            { "MP3", "mp3" }
        }};

        std::vector<FormatChoice> availableFormats;
        juce::PopupMenu menu;
        int menuId = 1;
        for (const auto& format : majorFormats)
        {
            if (findWritableExportFormatForExtension(format.extension) == nullptr)
                continue;
            availableFormats.push_back(format);
            menu.addItem(menuId++, format.label + " (." + format.extension + ")");
        }

        if (availableFormats.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Export Mixdown",
                                                   "No writable export formats are available in this build.");
            return;
        }

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportButton),
                           [this, availableFormats](int selectedId)
                           {
                               if (selectedId <= 0 || selectedId > static_cast<int>(availableFormats.size()))
                                   return;
                               beginMixdownExportForFormat(availableFormats[static_cast<size_t>(selectedId - 1)].extension);
                           });
    }

    void MainComponent::exportStems()
    {
        struct FormatChoice { juce::String label; juce::String extension; };
        const std::array<FormatChoice, 5> majorFormats {{
            { "WAV", "wav" },
            { "AIFF", "aiff" },
            { "FLAC", "flac" },
            { "OGG", "ogg" },
            { "MP3", "mp3" }
        }};

        std::vector<FormatChoice> availableFormats;
        juce::PopupMenu menu;
        int menuId = 1;
        for (const auto& format : majorFormats)
        {
            if (findWritableExportFormatForExtension(format.extension) == nullptr)
                continue;
            availableFormats.push_back(format);
            menu.addItem(menuId++, format.label + " (." + format.extension + ")");
        }

        if (availableFormats.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Export Stems",
                                                   "No writable export formats are available in this build.");
            return;
        }

        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportButton),
                           [this, availableFormats](int selectedId)
                           {
                               if (selectedId <= 0 || selectedId > static_cast<int>(availableFormats.size()))
                                   return;
                               beginStemExportForFormat(availableFormats[static_cast<size_t>(selectedId - 1)].extension);
                           });
    }

    bool MainComponent::promptForExportSettings(const juce::String& formatExtension,
                                                bool exportingStems,
                                                ExportSettings& outSettings)
    {
        juce::ignoreUnused(formatExtension);
        outSettings.sampleRate = 48000.0;
        outSettings.bitDepth = 24;
        outSettings.loopRangeOnly = transport.isLooping();
        outSettings.includeMasterProcessing = !exportingStems;
        outSettings.enableDither = outSettings.bitDepth < 24;
        return true;
    }

    void MainComponent::beginMixdownExportForFormat(const juce::String& formatExtension)
    {
        ExportSettings settings;
        if (!promptForExportSettings(formatExtension, false, settings))
            return;

        const auto documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

        exportFileChooser = std::make_unique<juce::FileChooser>(
            "Export Mixdown",
            documentsDir.getChildFile("Sampledex Mixdown." + formatExtension),
            "*." + formatExtension,
            true);

        auto flags = juce::FileBrowserComponent::saveMode
                   | juce::FileBrowserComponent::canSelectFiles
                   | juce::FileBrowserComponent::warnAboutOverwriting;
        exportFileChooser->launchAsync(flags,
                                       [this, formatExtension, settings]
                                       (const juce::FileChooser& chooser)
                                       {
                                           const juce::File selectedFile = chooser.getResult().withFileExtension("." + formatExtension);
                                           exportFileChooser.reset();
                                           if (selectedFile == juce::File{})
                                               return;

                                           if (!runOfflineExport(selectedFile,
                                                                 false,
                                                                 formatExtension,
                                                                 settings.sampleRate,
                                                                 settings.bitDepth,
                                                                 settings.loopRangeOnly,
                                                                 settings.includeMasterProcessing,
                                                                 settings.enableDither))
                                               return;

                                           juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                                                  "Export Complete",
                                                                                  "Mixdown exported to:\n" + selectedFile.getFullPathName());
                                       });
    }

    void MainComponent::beginStemExportForFormat(const juce::String& formatExtension)
    {
        ExportSettings settings;
        if (!promptForExportSettings(formatExtension, true, settings))
            return;

        const auto documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

        exportFileChooser = std::make_unique<juce::FileChooser>(
            "Choose Stems Export Folder",
            documentsDir.getChildFile("Sampledex Stems"),
            "*",
            true);

        auto flags = juce::FileBrowserComponent::openMode
                   | juce::FileBrowserComponent::canSelectDirectories;
        exportFileChooser->launchAsync(flags,
                                       [this, formatExtension, settings]
                                       (const juce::FileChooser& chooser)
                                       {
                                           const juce::File selectedFolder = chooser.getResult();
                                           exportFileChooser.reset();
                                           if (selectedFolder == juce::File{})
                                               return;

                                           if (!runOfflineExport(selectedFolder,
                                                                 true,
                                                                 formatExtension,
                                                                 settings.sampleRate,
                                                                 settings.bitDepth,
                                                                 settings.loopRangeOnly,
                                                                 settings.includeMasterProcessing,
                                                                 settings.enableDither))
                                               return;

                                           juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                                                  "Export Complete",
                                                                                  "Stems exported to:\n" + selectedFolder.getFullPathName());
                                       });
    }

    juce::AudioFormat* MainComponent::findWritableExportFormatForExtension(const juce::String& extension) const
    {
        auto* format = audioFormatManager.findFormatForFileExtension(extension);
        if (format == nullptr && extension.equalsIgnoreCase("aiff"))
            format = audioFormatManager.findFormatForFileExtension("aif");
        if (format == nullptr)
            return nullptr;

        auto* probeStream = new juce::MemoryOutputStream();
        juce::AudioFormatWriterOptions probeOptions;
        probeOptions = probeOptions.withSampleRate(44100.0)
                                   .withNumChannels(2)
                                   .withBitsPerSample(16)
                                   .withQualityOptionIndex(0);
        std::unique_ptr<juce::OutputStream> probeOutputStream(probeStream);
        std::unique_ptr<juce::AudioFormatWriter> probeWriter(
            format->createWriterFor(probeOutputStream, probeOptions));
        if (probeWriter == nullptr)
        {
            return nullptr;
        }
        return format;
    }

    bool MainComponent::renderOfflinePassToWriter(juce::AudioFormatWriter& writer,
                                                   double startBeat,
                                                   double endBeat,
                                                   double exportSampleRate,
                                                   int targetBitDepth,
                                                   bool enableDither,
                                                   std::atomic<bool>* cancelFlag,
                                                   std::function<void(float)> progressCallback)
    {
        if (endBeat <= startBeat)
            return false;

        const double bpmValue = juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed));
        const double secondsPerBeat = 60.0 / bpmValue;
        const double contentSeconds = (endBeat - startBeat) * secondsPerBeat;
        const double tailSeconds = 2.0;
        const int64_t totalSamples = static_cast<int64_t>(std::ceil((contentSeconds + tailSeconds) * exportSampleRate));
        if (totalSamples <= 0)
            return false;

        static constexpr int renderBlockSize = 1024;
        juce::AudioBuffer<float> renderBlock(2, renderBlockSize);

        midiCollector.reset(exportSampleRate);
        midiScheduler.reset();
        offlineRenderActiveRt.store(true, std::memory_order_relaxed);
        for (auto* track : tracks)
        {
            if (track != nullptr)
            {
                track->setPluginsNonRealtime(true);
                track->panic();
            }
        }
        panicRequestedRt.store(true, std::memory_order_relaxed);

        struct OfflineRenderScope
        {
            MainComponent& owner;
            ~OfflineRenderScope()
            {
                owner.offlineRenderActiveRt.store(false, std::memory_order_relaxed);
                for (auto* track : owner.tracks)
                    if (track != nullptr)
                        track->setPluginsNonRealtime(false);
            }
        } offlineRenderScope { *this };

        transport.stop();
        transport.setPosition(startBeat);
        transport.play();

        std::uint32_t ditherState = 0x51ed270bu;
        int64_t samplesRendered = 0;
        while (samplesRendered < totalSamples)
        {
            if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_relaxed))
            {
                transport.stop();
                return false;
            }

            const int numSamples = static_cast<int>(juce::jmin<int64_t>(renderBlockSize, totalSamples - samplesRendered));
            renderBlock.clear();
            juce::AudioSourceChannelInfo blockInfo(&renderBlock, 0, numSamples);
            getNextAudioBlock(blockInfo);
            if (enableDither)
                applyTpdfDither(renderBlock, 0, numSamples, targetBitDepth, ditherState);
            if (!writer.writeFromAudioSampleBuffer(renderBlock, 0, numSamples))
            {
                transport.stop();
                return false;
            }
            samplesRendered += numSamples;

            if (progressCallback)
            {
                const float progress = static_cast<float>(static_cast<double>(samplesRendered)
                                                          / static_cast<double>(juce::jmax<int64_t>(1, totalSamples)));
                progressCallback(juce::jlimit(0.0f, 1.0f, progress));
            }
        }

        transport.stop();
        if (progressCallback)
            progressCallback(1.0f);
        return true;
    }

    double MainComponent::getProjectEndBeat() const
    {
        double endBeat = 4.0;
        for (const auto& clip : arrangement)
            endBeat = juce::jmax(endBeat, clip.startBeat + juce::jmax(0.0625, clip.lengthBeats));
        return endBeat;
    }

    bool MainComponent::runOfflineExport(const juce::File& destination,
                                         bool exportStems,
                                         const juce::String& formatExtension,
                                         double exportSampleRate,
                                         int bitDepth,
                                         bool loopRangeOnly,
                                         bool includeMasterProcessing,
                                         bool enableDither)
    {
        if (tracks.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Export",
                                                   "No tracks available to export.");
            return false;
        }

        auto* format = findWritableExportFormatForExtension(formatExtension);
        if (format == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Export",
                                                   "Selected format is not writable in this build.");
            return false;
        }

        auto possibleBitDepths = format->getPossibleBitDepths();
        int resolvedBitDepth = bitDepth;
        if (!possibleBitDepths.isEmpty() && !possibleBitDepths.contains(resolvedBitDepth))
            resolvedBitDepth = possibleBitDepths.getFirst();

        double startBeat = 0.0;
        double endBeat = getProjectEndBeat();
        if (loopRangeOnly && transport.isLooping())
        {
            startBeat = transport.getLoopStartBeat();
            endBeat = transport.getLoopEndBeat();
        }
        if (endBeat <= startBeat + 0.0001)
            endBeat = startBeat + 4.0;

        juce::String failureReason;
        bool success = true;

        const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        const double restoreSampleRate = setup.sampleRate > 0.0
                                         ? setup.sampleRate
                                         : sampleRateRt.load(std::memory_order_relaxed);
        const int restoreBlockSize = setup.bufferSize > 0 ? setup.bufferSize : 512;

        const bool wasPlaying = transport.playing();
        const double previousBeat = transport.getCurrentBeat();
        const bool previousLooping = transport.isLooping();
        const double previousLoopStart = transport.getLoopStartBeat();
        const double previousLoopEnd = transport.getLoopEndBeat();
        const bool previousRecordEnabled = recordEnabledRt.load(std::memory_order_relaxed);
        const bool previousMetronomeEnabled = metronomeEnabledRt.load(std::memory_order_relaxed);
        const float previousMasterGain = masterOutputGainRt.load(std::memory_order_relaxed);
        const bool previousSoftClipEnabled = masterSoftClipEnabledRt.load(std::memory_order_relaxed);
        const bool previousLimiterEnabled = masterLimiterEnabledRt.load(std::memory_order_relaxed);

        std::vector<bool> previousSoloStates;
        previousSoloStates.reserve(static_cast<size_t>(tracks.size()));
        for (auto* track : tracks)
            previousSoloStates.push_back(track->isSolo());

        auto restoreRenderState = [&]
        {
            sampleRateRt.store(restoreSampleRate, std::memory_order_relaxed);
            transport.prepare(restoreSampleRate);
            for (auto* track : tracks)
                track->prepareToPlay(restoreSampleRate, restoreBlockSize);

            for (int i = 0; i < tracks.size() && i < static_cast<int>(previousSoloStates.size()); ++i)
                tracks[i]->setSolo(previousSoloStates[static_cast<size_t>(i)]);

            recordEnabledRt.store(previousRecordEnabled, std::memory_order_relaxed);
            metronomeEnabledRt.store(previousMetronomeEnabled, std::memory_order_relaxed);
            masterOutputGainRt.store(previousMasterGain, std::memory_order_relaxed);
            masterSoftClipEnabledRt.store(previousSoftClipEnabled, std::memory_order_relaxed);
            masterLimiterEnabledRt.store(previousLimiterEnabled, std::memory_order_relaxed);

            transport.setLoop(previousLooping, previousLoopStart, previousLoopEnd);
            transport.setPosition(previousBeat);
            if (wasPlaying)
                transport.play();
            else
                transport.stop();

            masterGainSmoothingState = masterOutputGainRt.load(std::memory_order_relaxed);
        };

        auto createWriterForFile = [&](const juce::File& outputFile)
            -> std::unique_ptr<juce::AudioFormatWriter>
        {
            auto parentDir = outputFile.getParentDirectory();
            if (!parentDir.exists() && !parentDir.createDirectory())
            {
                failureReason = "Unable to create output directory:\n" + parentDir.getFullPathName();
                return {};
            }

            auto outputFileWithExt = outputFile.withFileExtension("." + formatExtension);
            if (outputFileWithExt.existsAsFile() && !outputFileWithExt.deleteFile())
            {
                failureReason = "Unable to overwrite output file:\n" + outputFileWithExt.getFullPathName();
                return {};
            }

            std::unique_ptr<juce::FileOutputStream> stream(outputFileWithExt.createOutputStream());
            if (stream == nullptr)
            {
                failureReason = "Unable to create output stream:\n" + outputFileWithExt.getFullPathName();
                return {};
            }

            juce::AudioFormatWriterOptions writerOptions;
            writerOptions = writerOptions.withSampleRate(exportSampleRate)
                                         .withNumChannels(2)
                                         .withBitsPerSample(resolvedBitDepth)
                                         .withQualityOptionIndex(0);
            std::unique_ptr<juce::OutputStream> outputStream(std::move(stream));
            std::unique_ptr<juce::AudioFormatWriter> writer(
                format->createWriterFor(outputStream, writerOptions));
            if (writer == nullptr)
                failureReason = "Unable to create " + formatExtension.toUpperCase() + " writer.";
            return writer;
        };

        const auto configureRenderState = [&]
        {
            recordEnabledRt.store(false, std::memory_order_relaxed);
            metronomeEnabledRt.store(false, std::memory_order_relaxed);
            transport.stop();
            transport.setRecording(false);
            transport.setLoop(false, startBeat, endBeat);
            sampleRateRt.store(exportSampleRate, std::memory_order_relaxed);
            transport.prepare(exportSampleRate);
            for (auto* track : tracks)
                track->prepareToPlay(exportSampleRate, 1024);

            if (includeMasterProcessing)
            {
                masterOutputGainRt.store(previousMasterGain, std::memory_order_relaxed);
                masterSoftClipEnabledRt.store(previousSoftClipEnabled, std::memory_order_relaxed);
                masterLimiterEnabledRt.store(previousLimiterEnabled, std::memory_order_relaxed);
                masterGainSmoothingState = previousMasterGain;
            }
            else
            {
                masterOutputGainRt.store(1.0f, std::memory_order_relaxed);
                masterSoftClipEnabledRt.store(false, std::memory_order_relaxed);
                masterLimiterEnabledRt.store(false, std::memory_order_relaxed);
                masterGainSmoothingState = 1.0f;
            }
        };

        if (!exportStems)
        {
            configureRenderState();
            auto writer = createWriterForFile(destination);
            if (writer == nullptr
                || !renderOfflinePassToWriter(*writer,
                                              startBeat,
                                              endBeat,
                                              exportSampleRate,
                                              resolvedBitDepth,
                                              enableDither && resolvedBitDepth < 24))
            {
                success = false;
                if (failureReason.isEmpty())
                    failureReason = "Mix export render failed.";
            }
        }
        else
        {
            if ((!destination.exists() && !destination.createDirectory()) || !destination.isDirectory())
            {
                restoreRenderState();
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                       "Export Stems",
                                                       "Unable to access stems output folder.");
                return false;
            }

            for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
            {
                for (int i = 0; i < tracks.size(); ++i)
                    tracks[i]->setSolo(i == trackIndex);

                configureRenderState();

                juce::String stemName = juce::String(trackIndex + 1).paddedLeft('0', 2)
                                      + " - "
                                      + juce::File::createLegalFileName(tracks[trackIndex]->getTrackName());
                if (stemName.trim().isEmpty())
                    stemName = "Track " + juce::String(trackIndex + 1);

                auto writer = createWriterForFile(destination.getChildFile(stemName + "." + formatExtension));
                if (writer == nullptr
                    || !renderOfflinePassToWriter(*writer,
                                                  startBeat,
                                                  endBeat,
                                                  exportSampleRate,
                                                  resolvedBitDepth,
                                                  enableDither && resolvedBitDepth < 24))
                {
                    success = false;
                    if (failureReason.isEmpty())
                        failureReason = "Stem export failed on track: " + tracks[trackIndex]->getTrackName();
                    break;
                }
            }
        }

        restoreRenderState();
        refreshChannelRackWindow();
        refreshStatusText();

        if (!success)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Export",
                                                   failureReason.isNotEmpty() ? failureReason
                                                                              : juce::String("Export failed."));
        }
        return success;
    }

    void MainComponent::runRenderTask(const juce::String& taskName,
                                      std::function<void()> task,
                                      int renderTrackIndex,
                                      Track::RenderTaskType taskType)
    {
        if (!task)
            return;

        bool expectedIdle = false;
        if (!backgroundRenderBusyRt.compare_exchange_strong(expectedIdle, true, std::memory_order_acq_rel))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "Background Render",
                                                   "A render task is already running.");
            return;
        }

        renderCancelRequestedRt.store(false, std::memory_order_relaxed);
        renderProgressRt.store(0.0f, std::memory_order_relaxed);
        renderTrackIndexRt.store(renderTrackIndex, std::memory_order_relaxed);
        renderTaskTypeRt.store(static_cast<int>(taskType), std::memory_order_relaxed);
        if (juce::isPositiveAndBelow(renderTrackIndex, tracks.size()) && tracks[renderTrackIndex] != nullptr)
            tracks[renderTrackIndex]->setRenderTaskState(taskType, true, 0.0f);
        refreshStatusText();

        auto wrappedTask = [this, taskFn = std::move(task)]() mutable
        {
            try
            {
                taskFn();
            }
            catch (const std::exception& e)
            {
                juce::MessageManager::callAsync([message = juce::String(e.what())]
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Render Error",
                                                           "Render task failed: " + message);
                });
            }
            catch (...)
            {
                juce::MessageManager::callAsync([]()
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Render Error",
                                                           "Render task failed with an unknown error.");
                });
            }

            const int trackIndex = renderTrackIndexRt.exchange(-1, std::memory_order_relaxed);
            const bool wasCancelled = renderCancelRequestedRt.exchange(false, std::memory_order_relaxed);
            renderTaskTypeRt.store(static_cast<int>(Track::RenderTaskType::None), std::memory_order_relaxed);
            renderProgressRt.store(wasCancelled ? 0.0f : 1.0f, std::memory_order_relaxed);
            if (juce::isPositiveAndBelow(trackIndex, tracks.size()) && tracks[trackIndex] != nullptr)
                tracks[trackIndex]->setRenderTaskState(Track::RenderTaskType::None, false, 0.0f);

            backgroundRenderBusyRt.store(false, std::memory_order_release);
            juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
            {
                if (safeThis != nullptr)
                    safeThis->refreshStatusText();
            });
        };

        if (backgroundRenderingEnabledRt.load(std::memory_order_relaxed))
        {
            backgroundRenderPool.addJob(new LambdaRenderJob(taskName, std::move(wrappedTask)), true);
            return;
        }

        wrappedTask();
    }

    void MainComponent::cancelActiveRenderTask()
    {
        if (!backgroundRenderBusyRt.load(std::memory_order_relaxed))
            return;

        renderCancelRequestedRt.store(true, std::memory_order_relaxed);
        const int trackIndex = renderTrackIndexRt.load(std::memory_order_relaxed);
        if (juce::isPositiveAndBelow(trackIndex, tracks.size()) && tracks[trackIndex] != nullptr)
            tracks[trackIndex]->setRenderTaskState(tracks[trackIndex]->getRenderTaskType(),
                                                   true,
                                                   tracks[trackIndex]->getRenderTaskProgress());
        refreshStatusText();
    }

    bool MainComponent::renderTrackToAudioFile(int trackIndex,
                                               const juce::File& outputFile,
                                               double startBeat,
                                               double endBeat,
                                               double renderSampleRate,
                                               bool includeMasterProcessing,
                                               juce::String& errorMessage)
    {
        errorMessage.clear();
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
        {
            errorMessage = "Invalid track selected for rendering.";
            return false;
        }

        if (endBeat <= startBeat + 1.0e-9)
        {
            errorMessage = "Invalid render range.";
            return false;
        }

        if (outputFile == juce::File{})
        {
            errorMessage = "Render output path is empty.";
            return false;
        }

        const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        const double restoreSampleRate = setup.sampleRate > 0.0
                                         ? setup.sampleRate
                                         : juce::jmax(1.0, sampleRateRt.load(std::memory_order_relaxed));
        const int restoreBlockSize = setup.bufferSize > 0 ? setup.bufferSize : 512;

        const bool wasPlaying = transport.playing();
        const bool wasRecording = transport.recording();
        const double previousBeat = transport.getCurrentBeat();
        const bool previousLooping = transport.isLooping();
        const double previousLoopStart = transport.getLoopStartBeat();
        const double previousLoopEnd = transport.getLoopEndBeat();
        const bool previousRecordEnabled = recordEnabledRt.load(std::memory_order_relaxed);
        const bool previousMetronomeEnabled = metronomeEnabledRt.load(std::memory_order_relaxed);
        const float previousMasterGain = masterOutputGainRt.load(std::memory_order_relaxed);
        const bool previousSoftClipEnabled = masterSoftClipEnabledRt.load(std::memory_order_relaxed);
        const bool previousLimiterEnabled = masterLimiterEnabledRt.load(std::memory_order_relaxed);
        const bool previousExternalClockSync = externalMidiClockSyncEnabledRt.load(std::memory_order_relaxed);

        std::vector<bool> previousSoloStates;
        previousSoloStates.reserve(static_cast<size_t>(tracks.size()));
        for (auto* track : tracks)
            previousSoloStates.push_back(track != nullptr ? track->isSolo() : false);

        auto* targetTrack = tracks[trackIndex];
        const bool targetWasFrozen = targetTrack != nullptr && targetTrack->isFrozenPlaybackOnly();
        const juce::String targetFrozenPath = targetTrack != nullptr ? targetTrack->getFrozenRenderPath()
                                                                     : juce::String();

        auto restoreRenderState = [&]
        {
            sampleRateRt.store(restoreSampleRate, std::memory_order_relaxed);
            transport.prepare(restoreSampleRate);
            for (auto* track : tracks)
            {
                if (track != nullptr)
                    track->prepareToPlay(restoreSampleRate, restoreBlockSize);
            }

            for (int i = 0; i < tracks.size() && i < static_cast<int>(previousSoloStates.size()); ++i)
            {
                if (tracks[i] != nullptr)
                    tracks[i]->setSolo(previousSoloStates[static_cast<size_t>(i)]);
            }

            if (targetTrack != nullptr)
            {
                targetTrack->setFrozenPlaybackOnly(targetWasFrozen);
                targetTrack->setFrozenRenderPath(targetFrozenPath);
            }

            recordEnabledRt.store(previousRecordEnabled, std::memory_order_relaxed);
            metronomeEnabledRt.store(previousMetronomeEnabled, std::memory_order_relaxed);
            masterOutputGainRt.store(previousMasterGain, std::memory_order_relaxed);
            masterSoftClipEnabledRt.store(previousSoftClipEnabled, std::memory_order_relaxed);
            masterLimiterEnabledRt.store(previousLimiterEnabled, std::memory_order_relaxed);
            externalMidiClockSyncEnabledRt.store(previousExternalClockSync, std::memory_order_relaxed);

            transport.setLoop(previousLooping, previousLoopStart, previousLoopEnd);
            transport.setPosition(previousBeat);
            transport.setRecording(false);
            if (wasRecording)
                transport.setRecording(true);
            else if (wasPlaying)
                transport.play();
            else
                transport.stop();

            masterGainSmoothingState = masterOutputGainRt.load(std::memory_order_relaxed);
            externalMidiClockWasRunning = false;
            lastAppliedExternalClockGeneration = -1;
        };

        for (int i = 0; i < tracks.size(); ++i)
        {
            if (tracks[i] == nullptr)
                continue;
            tracks[i]->setSolo(i == trackIndex);
        }

        if (targetTrack != nullptr)
            targetTrack->setFrozenPlaybackOnly(false);

        recordEnabledRt.store(false, std::memory_order_relaxed);
        metronomeEnabledRt.store(false, std::memory_order_relaxed);
        externalMidiClockSyncEnabledRt.store(false, std::memory_order_relaxed);
        transport.stop();
        transport.setRecording(false);
        transport.setLoop(false, startBeat, endBeat);
        sampleRateRt.store(renderSampleRate, std::memory_order_relaxed);
        transport.prepare(renderSampleRate);
        for (auto* track : tracks)
        {
            if (track != nullptr)
                track->prepareToPlay(renderSampleRate, 1024);
        }

        if (includeMasterProcessing)
        {
            masterOutputGainRt.store(previousMasterGain, std::memory_order_relaxed);
            masterSoftClipEnabledRt.store(previousSoftClipEnabled, std::memory_order_relaxed);
            masterLimiterEnabledRt.store(previousLimiterEnabled, std::memory_order_relaxed);
            masterGainSmoothingState = previousMasterGain;
        }
        else
        {
            masterOutputGainRt.store(1.0f, std::memory_order_relaxed);
            masterSoftClipEnabledRt.store(false, std::memory_order_relaxed);
            masterLimiterEnabledRt.store(false, std::memory_order_relaxed);
            masterGainSmoothingState = 1.0f;
        }

        auto parentDir = outputFile.getParentDirectory();
        if (!parentDir.exists() && !parentDir.createDirectory())
        {
            errorMessage = "Unable to create render folder:\n" + parentDir.getFullPathName();
            restoreRenderState();
            return false;
        }

        const juce::File outputWithExt = outputFile.withFileExtension(".wav");
        if (outputWithExt.existsAsFile() && !outputWithExt.deleteFile())
        {
            errorMessage = "Unable to overwrite rendered file:\n" + outputWithExt.getFullPathName();
            restoreRenderState();
            return false;
        }

        std::unique_ptr<juce::FileOutputStream> stream(outputWithExt.createOutputStream());
        if (stream == nullptr || !stream->openedOk())
        {
            errorMessage = "Unable to open render output stream:\n" + outputWithExt.getFullPathName();
            restoreRenderState();
            return false;
        }

        juce::WavAudioFormat wavFormat;
        juce::AudioFormatWriterOptions writerOptions;
        writerOptions = writerOptions.withSampleRate(renderSampleRate)
                                     .withNumChannels(2)
                                     .withBitsPerSample(24);
        std::unique_ptr<juce::OutputStream> outputStream(std::move(stream));
        std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(outputStream, writerOptions));
        if (writer == nullptr)
        {
            errorMessage = "Unable to create WAV writer for track render.";
            restoreRenderState();
            return false;
        }

        const bool renderOk = renderOfflinePassToWriter(
            *writer,
            startBeat,
            endBeat,
            renderSampleRate,
            24,
            false,
            &renderCancelRequestedRt,
            [this, trackIndex](float progress)
            {
                const float clamped = juce::jlimit(0.0f, 1.0f, progress);
                renderProgressRt.store(clamped, std::memory_order_relaxed);
                if (juce::isPositiveAndBelow(trackIndex, tracks.size()) && tracks[trackIndex] != nullptr)
                {
                    tracks[trackIndex]->setRenderTaskState(
                        tracks[trackIndex]->getRenderTaskType(),
                        true,
                        clamped);
                }
            });
        writer.reset();
        restoreRenderState();
        if (!renderOk)
        {
            if (renderCancelRequestedRt.load(std::memory_order_relaxed))
                errorMessage = "Render cancelled.";
            else
                errorMessage = "Track render failed during offline pass.";
            return false;
        }

        return true;
    }

    void MainComponent::finishFreezeTrack(int trackIndex,
                                          const juce::File& renderedFile,
                                          double startBeat,
                                          double endBeat,
                                          double renderSampleRate)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()) || !renderedFile.existsAsFile())
            return;

        auto* track = tracks[trackIndex];
        if (track == nullptr)
            return;

        const juce::String previousFrozenPath = track->getFrozenRenderPath();
        const juce::String renderedPath = renderedFile.getFullPathName();

        arrangement.erase(std::remove_if(arrangement.begin(),
                                         arrangement.end(),
                                         [trackIndex, previousFrozenPath, renderedPath](const Clip& clip)
                                         {
                                             if (clip.trackIndex != trackIndex || clip.type != ClipType::Audio)
                                                 return false;
                                             if (!previousFrozenPath.isEmpty() && clip.audioFilePath == previousFrozenPath)
                                                 return true;
                                             if (clip.audioFilePath == renderedPath)
                                                 return true;
                                             return clip.name.startsWithIgnoreCase("Freeze: ");
                                         }),
                        arrangement.end());

        double clipLengthBeats = juce::jmax(0.25, endBeat - startBeat);
        if (auto reader = std::unique_ptr<juce::AudioFormatReader>(audioFormatManager.createReaderFor(renderedFile)))
        {
            const double fileSampleRate = juce::jmax(1.0, reader->sampleRate);
            const double fileLengthSeconds = static_cast<double>(reader->lengthInSamples) / fileSampleRate;
            const double beatsPerSecond = juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed)) / 60.0;
            clipLengthBeats = juce::jmax(0.25, fileLengthSeconds * beatsPerSecond);
            renderSampleRate = fileSampleRate;
        }

        Clip frozenClip;
        frozenClip.type = ClipType::Audio;
        frozenClip.name = "Freeze: " + track->getTrackName();
        frozenClip.startBeat = juce::jmax(0.0, startBeat);
        frozenClip.lengthBeats = clipLengthBeats;
        frozenClip.trackIndex = trackIndex;
        frozenClip.audioData.reset();
        frozenClip.audioFilePath = renderedPath;
        frozenClip.audioSampleRate = juce::jmax(1.0, renderSampleRate);
        frozenClip.gainLinear = 1.0f;
        frozenClip.fadeInBeats = 0.0;
        frozenClip.fadeOutBeats = 0.0;
        frozenClip.crossfadeInBeats = 0.0;
        frozenClip.crossfadeOutBeats = 0.0;
        arrangement.push_back(std::move(frozenClip));

        track->setFrozenRenderPath(renderedPath);
        track->setFrozenPlaybackOnly(true);

        setSelectedTrackIndex(trackIndex);
        timeline.selectTrack(trackIndex);
        mixer.selectTrack(trackIndex);
        refreshChannelRackWindow();
        rebuildRealtimeSnapshot();
        refreshStatusText();
    }

    void MainComponent::finishCommitTrack(int trackIndex,
                                          const juce::File& renderedFile,
                                          double startBeat,
                                          double endBeat,
                                          double renderSampleRate)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()) || !renderedFile.existsAsFile())
            return;

        auto* track = tracks[trackIndex];
        if (track == nullptr)
            return;

        arrangement.erase(std::remove_if(arrangement.begin(),
                                         arrangement.end(),
                                         [trackIndex](const Clip& clip)
                                         {
                                             return clip.trackIndex == trackIndex;
                                         }),
                        arrangement.end());

        double clipLengthBeats = juce::jmax(0.25, endBeat - startBeat);
        if (auto reader = std::unique_ptr<juce::AudioFormatReader>(audioFormatManager.createReaderFor(renderedFile)))
        {
            const double fileSampleRate = juce::jmax(1.0, reader->sampleRate);
            const double fileLengthSeconds = static_cast<double>(reader->lengthInSamples) / fileSampleRate;
            const double beatsPerSecond = juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed)) / 60.0;
            clipLengthBeats = juce::jmax(0.25, fileLengthSeconds * beatsPerSecond);
            renderSampleRate = fileSampleRate;
        }

        Clip committedClip;
        committedClip.type = ClipType::Audio;
        committedClip.name = "Commit: " + track->getTrackName();
        committedClip.startBeat = juce::jmax(0.0, startBeat);
        committedClip.lengthBeats = clipLengthBeats;
        committedClip.trackIndex = trackIndex;
        committedClip.audioData.reset();
        committedClip.audioFilePath = renderedFile.getFullPathName();
        committedClip.audioSampleRate = juce::jmax(1.0, renderSampleRate);
        committedClip.gainLinear = 1.0f;
        committedClip.fadeInBeats = 0.0;
        committedClip.fadeOutBeats = 0.0;
        committedClip.crossfadeInBeats = 0.0;
        committedClip.crossfadeOutBeats = 0.0;
        arrangement.push_back(std::move(committedClip));

        track->setFrozenPlaybackOnly(false);
        track->setFrozenRenderPath({});
        track->setChannelType(Track::ChannelType::Audio);
        track->disableBuiltInInstrument();
        track->clearPluginSlot(Track::instrumentSlotIndex);
        for (int slot = 0; slot < track->getPluginSlotCount(); ++slot)
            track->clearPluginSlot(slot);

        setSelectedTrackIndex(trackIndex);
        timeline.selectTrack(trackIndex);
        mixer.selectTrack(trackIndex);
        refreshChannelRackWindow();
        rebuildRealtimeSnapshot();
        refreshStatusText();
    }

    void MainComponent::freezeTrackToAudio(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        const double startBeat = 0.0;
        const double endBeat = juce::jmax(startBeat + 0.25, getProjectEndBeat());
        const double renderSampleRate = juce::jmax(22050.0, sampleRateRt.load(std::memory_order_relaxed));
        auto freezeDir = appDataDir.getChildFile("FreezeCache");
        freezeDir.createDirectory();
        const juce::String stem = juce::File::createLegalFileName("Freeze_" + juce::String(trackIndex + 1) + "_" + tracks[trackIndex]->getTrackName());
        const juce::File outputFile = freezeDir.getNonexistentChildFile(stem, ".wav", false);
        juce::Component::SafePointer<MainComponent> safeThis(this);

        runRenderTask("Freeze Track", [safeThis, trackIndex, outputFile, startBeat, endBeat, renderSampleRate]() mutable
        {
            if (safeThis == nullptr)
                return;

            juce::String error;
            const bool ok = safeThis->renderTrackToAudioFile(trackIndex,
                                                             outputFile,
                                                             startBeat,
                                                             endBeat,
                                                             renderSampleRate,
                                                             false,
                                                             error);
            if (!ok)
            {
                if (error.startsWithIgnoreCase("Render cancelled"))
                    return;
                juce::MessageManager::callAsync([errorMessage = error.isNotEmpty() ? error
                                                                                   : juce::String("Freeze render failed.")]
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Freeze Track",
                                                           errorMessage);
                });
                return;
            }

            juce::MessageManager::callAsync([safeThis, trackIndex, outputFile, startBeat, endBeat, renderSampleRate]
            {
                if (safeThis == nullptr)
                    return;
                safeThis->finishFreezeTrack(trackIndex, outputFile, startBeat, endBeat, renderSampleRate);
            });
        },
        trackIndex,
        Track::RenderTaskType::Freeze);
    }

    void MainComponent::unfreezeTrack(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto* track = tracks[trackIndex];
        if (track == nullptr)
            return;

        const juce::String frozenPath = track->getFrozenRenderPath();
        arrangement.erase(std::remove_if(arrangement.begin(),
                                         arrangement.end(),
                                         [trackIndex, frozenPath](const Clip& clip)
                                         {
                                             if (clip.trackIndex != trackIndex || clip.type != ClipType::Audio)
                                                 return false;
                                             if (!frozenPath.isEmpty() && clip.audioFilePath == frozenPath)
                                                 return true;
                                             return clip.name.startsWithIgnoreCase("Freeze: ");
                                         }),
                        arrangement.end());

        track->setFrozenPlaybackOnly(false);
        track->setFrozenRenderPath({});
        rebuildRealtimeSnapshot();
        refreshChannelRackWindow();
        refreshStatusText();
    }

    void MainComponent::commitTrackToAudio(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        const double startBeat = 0.0;
        const double endBeat = juce::jmax(startBeat + 0.25, getProjectEndBeat());
        const double renderSampleRate = juce::jmax(22050.0, sampleRateRt.load(std::memory_order_relaxed));
        auto commitDir = appDataDir.getChildFile("CommitCache");
        commitDir.createDirectory();
        const juce::String stem = juce::File::createLegalFileName("Commit_" + juce::String(trackIndex + 1) + "_" + tracks[trackIndex]->getTrackName());
        const juce::File outputFile = commitDir.getNonexistentChildFile(stem, ".wav", false);
        juce::Component::SafePointer<MainComponent> safeThis(this);

        runRenderTask("Commit Track", [safeThis, trackIndex, outputFile, startBeat, endBeat, renderSampleRate]() mutable
        {
            if (safeThis == nullptr)
                return;

            juce::String error;
            const bool ok = safeThis->renderTrackToAudioFile(trackIndex,
                                                             outputFile,
                                                             startBeat,
                                                             endBeat,
                                                             renderSampleRate,
                                                             false,
                                                             error);
            if (!ok)
            {
                if (error.startsWithIgnoreCase("Render cancelled"))
                    return;
                juce::MessageManager::callAsync([errorMessage = error.isNotEmpty() ? error
                                                                                   : juce::String("Commit render failed.")]
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Commit Track",
                                                           errorMessage);
                });
                return;
            }

            juce::MessageManager::callAsync([safeThis, trackIndex, outputFile, startBeat, endBeat, renderSampleRate]
            {
                if (safeThis == nullptr)
                    return;
                safeThis->finishCommitTrack(trackIndex, outputFile, startBeat, endBeat, renderSampleRate);
            });
        },
        trackIndex,
        Track::RenderTaskType::Commit);
    }

    void MainComponent::showHelpGuide()
    {
        const juce::String guide =
            "Sampledex Quick Guide\n\n"
            "Timeline:\n"
            "- Right-click track header/lane: create MIDI clips, load plugins, open plugin UI.\n"
            "- Right-click track header/lane: rename, duplicate, or delete tracks.\n"
            "- Double-click a track name in timeline or mixer to rename quickly.\n"
            "- Drag track headers vertically to reorder tracks.\n"
            "- In timeline lanes: drag mostly vertical on empty lane space to reorder tracks.\n"
            "- Drag mixer strips horizontally to reorder tracks.\n"
            "- Right-click headers/strips for Move Track Up/Down or Left/Right.\n"
            "- Right-click headers/strips to jump directly to Channel Rack or Inspector.\n"
            "- Drag clips to move between beats/tracks (hold Shift for fine non-snapped drag).\n"
            "- Drag clip left/right edge to resize clip length.\n"
            "- Option/Alt-drag clips to duplicate by drag.\n"
            "- Double-click empty lane space to create a 4-beat MIDI clip.\n"
            "- Click clips to select track + clip.\n\n"
            "Track Controls:\n"
            "- M = Mute, S = Solo, R = Arm, I = Input monitor.\n"
            "- Click plugin names in timeline/mixer/channel rack to open UI.\n"
            "- Right-click plugin names to load/change plugins.\n"
            "- Click EQ buttons on track headers/strips to open built-in track EQ.\n"
            "- Show UI menu can switch Target Insert, open loaded inserts, bypass/clear current insert.\n"
            "- Use I1-I4 insert chips in timeline/mixer for slot-specific open/load.\n"
            "- Right-click timeline headers / mixer strips to set send level, tap mode, and target bus quickly.\n"
            "- Inspector + mixer menus let you choose per-track audio input source and monitor tap mode (Pre/Post inserts).\n"
            "- Input source can be set to Auto to follow the strongest incoming channel pair.\n"
            "- Monitor Gain controls how loud live input is in the software monitor path.\n"
            "- Panic button or P key sends all-notes-off to all tracks.\n"
            "- Input MIDI routes to armed tracks first, then monitored tracks, then selected track.\n"
            "- MIDI Out selector routes live MIDI-thru to hardware/virtual destinations.\n"
            "- MIDI Thru toggle controls whether incoming MIDI is echoed to MIDI Out.\n"
            "- Surface selector enables control-surface mappings (fader/pan/send + transport).\n"
            "- MIDI Learn: choose target, arm Learn, then move a hardware CC to bind.\n\n"
            "Track Creation:\n"
            "- Add Track opens templates: Instrument, Audio Input, or MIDI+Clip.\n\n"

            "Audio Recording Workflow:\n"
            "- 1) Open Audio settings and choose your interface/input device.\n"
            "- 2) Create/select an Audio Input track and enable R (arm) and I (monitor).\n"
            "- 3) In Inspector set Input Source (or Auto), Monitor Tap, and Monitor Gain.\n"
            "- 4) Open Rec Setup and set Count-In, Punch In/Out, and Pre/Post roll.\n"
            "- 5) Press Rec, then Play (or press Rec with transport stopped to auto-start).\n"
            "- 6) Audio takes are captured per armed monitored track and committed as timeline audio clips.\n"
            "- 7) Press Rec again (or hit auto post-roll stop) to commit MIDI + audio takes.\n"
            "- Built-in laptop mic + speakers can feedback; for stable monitoring, use headphones.\n\n"

            "Input Troubleshooting:\n"
            "- If no input appears, verify macOS mic permission and Audio settings input device.\n"
            "- If a third-party mic is silent, set Input Source to Auto or choose the correct channel pair.\n"
            "- If levels jump/feedback, lower Monitor Gain and/or use Pre Inserts monitor mode.\n"
            "- Keep interface sample rate/buffer stable when loading heavy plugins.\n\n"
            "Audio Engine Controls:\n"
            "- SR and Buf dropdowns in the top bar change interface sample rate and buffer size directly.\n"
            "- Low Lat toggle prioritizes monitoring stability and auto-reduces heavy processing on overload.\n"
            "- LCD + status line show callback load, XRUN count, and guard-drop count for troubleshooting.\n\n"

            "Plugin Workflow + Compatibility:\n"
            "- Instrument slot is for synth/sampler plugins, inserts are for effect plugins.\n"
            "- Right-click track/mixer/plugin names to load plugins by manufacturer groups.\n"
            "- Use Open Loaded Plugins to re-open existing plugin windows quickly.\n"
            "- If a plugin window is hidden, use Show UI or slot right-click -> Open Plugin UI.\n"
            "- For heavy plugins, raise buffer size in Audio settings before recording.\n\n"
            "Global Musical Context:\n"
            "- Top-bar Key/Scale controls drive chord engine and piano-roll highlighting.\n"
            "- Transpose applies non-destructive global MIDI transpose on playback.\n"
            "- Tempo menu manages BPM and tempo events for arrangement changes.\n\n"
            "Clip Tools:\n"
            "- Clip Tools normalizes audio clips, adjusts clip gain, and sets fades.\n"
            "- Shift+Cmd+T opens clip tools quickly.\n\n"
            "Navigation:\n"
            "- Rack / Inspect buttons jump to Channel Rack and Inspector for selected track.\n\n"
            "- Project button opens one menu for Audio setup, Tempo, Key/Scale/Transpose, Export, and Help.\n\n"
            "- Rec Setup opens the dedicated recording panel for calibration/latency/peak holds.\n\n"
            "LCD Transport Dashboard:\n"
            "- LCD shows transport state + dual time readouts + tempo/meter + grid + engine health.\n"
            "- Click main readout to jump by Bars|Beats, Timecode, or Samples (mode dropdown).\n"
            "- Click BPM or meter fields directly to edit.\n"
            "- Drag main readout up/down to scrub (Shift=fine, Cmd/Ctrl=coarse).\n"
            "- Right-click LCD for quick mode switch and edit menu.\n\n"
            "Inspector:\n"
            "- Inspector tab shows selected-track controls and insert slots.\n"
            "- Use it for fast track edits without opening extra windows.\n"
            "- Use Up/Dn on insert rows to reorder plugin chain slots.\n\n"
            "Mixer Routing:\n"
            "- Send knob drives AUX wire intensity to the routed AUX bus strip.\n"
            "- Set send tap point per track (Pre/Post/Post-Pan) and send target bus from track or mixer right-click menus.\n"
            "- Use Aux FX toggle + Aux Return slider in top bar to control return processing.\n"
            "- Mixer: Cmd/Ctrl + wheel resizes strips. Double-click background auto-fits strip width.\n"
            "- Master slider + Soft Clip + Limiter control final output loudness/protection.\n"
            "- Dedicated master analyzer shows spectrum + phase + LUFS (approx) + RMS/Peak with clip latch (click CLIP to reset).\n"
            "- Export button opens Mixdown and Stems render workflow.\n\n"
            "Export:\n"
            "- Mixdown: full song render in major formats available on this build.\n"
            "- Stems: per-track solo renders with the chosen output format.\n"
            "- Exports include a short render tail for delays/reverbs.\n\n"
            "Global Shortcuts:\n"
            "- Cmd+S = save project, Cmd+Shift+S = Save As, Cmd+O = open project\n"
            "- Cmd/Ctrl+D = duplicate selected clip\n"
            "- S = split selected clip at playhead\n"
            "- Delete/Backspace = delete selected clip\n"
            "- Cmd+Shift+Delete = delete selected track\n"
            "- Cmd +/- = timeline horizontal zoom\n"
            "- Cmd+Shift +/- = track row height zoom\n"
            "- Cmd+1..4 = open insert slot UI (Shift+Cmd+1..4 = load plugin in that slot)\n"
            "- Alt+1/2/3 = LCD mode (Bars|Beats / Timecode / Samples)\n"
            "- Cmd+E = open selected-track plugin UI\n"
            "- Cmd+T = tempo menu, Cmd+Shift+T = clip tools\n"
            "- Cmd+B = export mixdown, Cmd+Shift+B = export stems\n"
            "- Cmd+K / Cmd+I = Channel Rack / Inspector tab\n"
            "- 1 / 2 / 3 / 4 / 5 / 6 = Mixer / Piano Roll / Step Seq / Channel Rack / Inspector / Recording tabs\n\n"
            "- [ / ] = send down/up on selected track (Shift = larger step)\n"
            "- Space = play/stop transport\n"
            "- R / M / L = toggle Record / Metronome / Loop\n"
            "- Cmd/Ctrl+L = toggle dark/light theme\n"
            "- F = toggle timeline follow playhead\n"
            "- Home = move playhead to start\n"
            "- Left/Right = nudge selected clip (Shift for larger)\n\n"
            "Piano Roll Tools:\n"
            "- Select: click notes to select, drag empty area for marquee select.\n"
            "- Draw: click/drag to brush-paint notes.\n"
            "- Erase: click/drag to erase notes.\n"
            "- Alt-drag selected notes to duplicate and move.\n"
            "- Drag note body to move, drag left/right edge to resize.\n"
            "- Velocity lane: drag note handles to edit velocity per-note.\n"
            "- Keys 1/2/3 = Select/Draw/Erase, Q = Quantize in piano roll.\n\n"
            "Timeline Navigation:\n"
            "- Mouse wheel: horizontal scroll timeline.\n"
            "- Cmd/Ctrl + wheel: timeline horizontal zoom.\n"
            "- Shift + wheel: track lane height zoom.\n\n"
            "Piano Roll Navigation:\n"
            "- Mouse wheel: pan horizontally.\n"
            "- Cmd/Ctrl + wheel: horizontal zoom.\n"
            "- Shift + wheel: vertical note-row zoom.\n\n"
            "Keyboard Input (Piano Roll tab):\n"
            "- A W S E D F T G Y H U J K O L P ; = step-note input\n"
            "- Z/X = octave down/up\n"
            "- Left/Right = move step cursor\n"
            "- Space = play/stop";

        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Help", guide);
    }

    bool MainComponent::saveProjectToFile(const juce::File& destination, juce::String& errorMessage)
    {
        errorMessage.clear();
        juce::File selectedFile = destination;
        if (selectedFile == juce::File{})
        {
            errorMessage = "No destination file selected.";
            return false;
        }

        if (selectedFile.getFileExtension().isEmpty())
            selectedFile = selectedFile.withFileExtension(".sampledex");

        const juce::File parentDir = selectedFile.getParentDirectory();
        if (parentDir == juce::File())
        {
            errorMessage = "Invalid save location:\n" + selectedFile.getFullPathName();
            return false;
        }

        if (!parentDir.exists() && !parentDir.createDirectory())
        {
            errorMessage = "Unable to create destination folder:\n" + parentDir.getFullPathName();
            return false;
        }

        if (!saveProjectStateToFile(selectedFile, errorMessage))
            return false;

        currentProjectFile = selectedFile;
        projectDirty = false;
        lastAutosaveSerial = projectMutationSerial;
        autosaveProjectFile.deleteFile();
        refreshStatusText();
        return true;
    }

    void MainComponent::saveProject()
    {
        if (currentProjectFile != juce::File{})
        {
            juce::String error;
            if (saveProjectToFile(currentProjectFile, error))
                return;

            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Save Project",
                                                   error.isNotEmpty() ? error
                                                                      : juce::String("Project save failed."));
            return;
        }

        saveProjectAs();
    }

    void MainComponent::saveProjectAs()
    {
        const auto documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        const auto defaultName = juce::File::createLegalFileName(
            tracks.isEmpty() ? juce::String("Sampledex Project")
                             : (tracks[juce::jlimit(0, tracks.size() - 1, selectedTrackIndex)]->getTrackName() + " Project"));
        const juce::File initialTarget = currentProjectFile != juce::File{}
            ? currentProjectFile
            : documentsDir.getChildFile(defaultName + ".sampledex");

        projectFileChooser = std::make_unique<juce::FileChooser>(
            "Save Project As",
            initialTarget,
            "*.sampledex;*.xml",
            true);

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::warnAboutOverwriting;
        projectFileChooser->launchAsync(flags,
                                        [this](const juce::FileChooser& chooser)
                                        {
                                            const juce::File selectedFile = chooser.getResult();
                                            projectFileChooser.reset();
                                            if (selectedFile == juce::File{})
                                                return;

                                            juce::String error;
                                            if (!saveProjectToFile(selectedFile, error))
                                            {
                                                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                       "Save Project",
                                                                                       error.isNotEmpty() ? error
                                                                                                          : juce::String("Project save failed."));
                                            }
                                        });
    }

    bool MainComponent::saveProjectSynchronously()
    {
        juce::File target = currentProjectFile;
        if (target == juce::File{})
        {
            saveProjectAs();
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "Save Project",
                                                   "Choose a project file location, then close the app again.");
            return false;
        }

        if (target.getParentDirectory() == juce::File())
        {
            saveProjectAs();
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "Save Project",
                                                   "Choose a valid project save location, then close the app again.");
            return false;
        }

        if (!target.getParentDirectory().exists())
        {
            if (!target.getParentDirectory().createDirectory())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                       "Save Project",
                                                       "Unable to create destination folder:\n"
                                                           + target.getParentDirectory().getFullPathName());
                return false;
            }
        }

        juce::String error;
        if (!saveProjectToFile(target, error))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Save Project",
                                                   error.isNotEmpty() ? error
                                                                      : juce::String("Project save failed."));
            return false;
        }

        return true;
    }

    void MainComponent::logCloseDecision(const juce::String& decision) const
    {
        juce::Logger::writeToLog("Close decision: " + decision);
    }

    void MainComponent::requestApplicationClose()
    {
        closePluginEditorWindow();
        outputSafetyMuteBlocksRt.store(256, std::memory_order_relaxed);
        transport.stop();
        panicAllNotes();
        renderCancelRequestedRt.store(true, std::memory_order_relaxed);

        if (closeRequestInProgress.exchange(true, std::memory_order_acq_rel))
        {
            logCloseDecision("ignored-duplicate-request");
            return;
        }

        const auto clearCloseRequest = [this]
        {
            closeRequestInProgress.store(false, std::memory_order_release);
        };

        const auto quitNow = [this, clearCloseRequest]
        {
            logCloseDecision("quit");
            // If the platform routes quit() back through systemRequestedQuit(),
            // mark the project clean so the second pass can terminate immediately
            // instead of repeatedly prompting with the unsaved-changes dialog.
            projectDirty = false;
            clearCloseRequest();
            if (auto* app = juce::JUCEApplication::getInstance())
                app->quit();
        };

        const auto cancelClose = [this, clearCloseRequest](const juce::String& reason)
        {
            logCloseDecision(reason);
            clearCloseRequest();
        };

        if (!projectDirty)
        {
            logCloseDecision("clean-project");
            quitNow();
            return;
        }

        const int result = juce::AlertWindow::showYesNoCancelBox(juce::AlertWindow::WarningIcon,
                                                                  "Unsaved Project",
                                                                  "Save changes before closing?",
                                                                  "Save",
                                                                  "Don't Save",
                                                                  "Cancel",
                                                                  this,
                                                                  nullptr);
        if (result == 0)
        {
            cancelClose("cancel");
            return;
        }
        if (result == 2)
        {
            logCloseDecision("dont-save");
            quitNow();
            return;
        }

        if (currentProjectFile != juce::File{})
        {
            juce::String error;
            if (!saveProjectToFile(currentProjectFile, error))
            {
                logCloseDecision("save-failed");
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                       "Save Project",
                                                       error.isNotEmpty() ? error
                                                                          : juce::String("Project save failed."));
                cancelClose("save-failed");
                return;
            }
            logCloseDecision("save-success");
            quitNow();
            return;
        }

        const auto documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        const auto defaultName = juce::File::createLegalFileName(
            tracks.isEmpty() ? juce::String("Sampledex Project")
                             : (tracks[juce::jlimit(0, tracks.size() - 1, selectedTrackIndex)]->getTrackName() + " Project"));
        const juce::File initialTarget = documentsDir.getChildFile(defaultName + ".sampledex");

        juce::FileChooser chooser("Save Project As",
                                  initialTarget,
                                  "*.sampledex;*.xml",
                                  true);

        if (!chooser.browseForFileToSave(true))
        {
            cancelClose("save-as-cancel");
            return;
        }

        juce::String error;
        if (!saveProjectToFile(chooser.getResult(), error))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Save Project",
                                                   error.isNotEmpty() ? error
                                                                      : juce::String("Project save failed."));
            cancelClose("save-as-failed");
            return;
        }

        logCloseDecision("save-as-success");
        quitNow();
    }

    bool MainComponent::handleApplicationCloseRequest()
    {
        if (!projectDirty)
        {
            logCloseDecision("close-check-clean");
            return true;
        }

        const int result = juce::AlertWindow::showYesNoCancelBox(juce::AlertWindow::WarningIcon,
                                                                  "Unsaved Project",
                                                                  "Save changes before closing?",
                                                                  "Save",
                                                                  "Don't Save",
                                                                  "Cancel",
                                                                  this,
                                                                  nullptr);
        if (result == 0)
        {
            logCloseDecision("close-check-cancel");
            return false;
        }
        if (result == 2)
        {
            logCloseDecision("close-check-dont-save");
            return true;
        }

        const bool saved = saveProjectSynchronously();
        logCloseDecision(saved ? juce::String("close-check-save-success")
                               : juce::String("close-check-save-failed"));
        return saved;
    }

    void MainComponent::loadStartupPreferences()
    {
        autoScanPluginsOnStartup = true;
        autoQuarantineOnUncleanExit = true;
        micPermissionPromptedOnce = false;
        pluginScanPassTimeoutMs = 45000;
        preferredMacPluginFormat = "AudioUnit";
        if (canonicalBuildPath.trim().isEmpty())
            canonicalBuildPath = "/Users/robertclemons/Downloads/sampledex_daw-main/build/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app";
        if (startupSettingsFile == juce::File() || !startupSettingsFile.existsAsFile())
            return;

        juce::StringArray lines;
        lines.addLines(startupSettingsFile.loadFileAsString());
        for (auto line : lines)
        {
            line = line.trim();
            if (line.isEmpty() || line.startsWithChar('#'))
                continue;
            if (line.startsWithIgnoreCase("auto_scan_plugins_on_startup="))
            {
                const auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
                autoScanPluginsOnStartup = value.getIntValue() != 0;
                continue;
            }

            if (line.startsWithIgnoreCase("auto_quarantine_on_unclean_exit="))
            {
                const auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
                autoQuarantineOnUncleanExit = value.getIntValue() != 0;
                continue;
            }

            if (line.startsWithIgnoreCase("mic_permission_prompted_once="))
            {
                const auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
                micPermissionPromptedOnce = value.getIntValue() != 0;
                continue;
            }

            if (line.startsWithIgnoreCase("plugin_scan_pass_timeout_ms="))
            {
                const int parsed = line.fromFirstOccurrenceOf("=", false, false).trim().getIntValue();
                pluginScanPassTimeoutMs = juce::jlimit(10000, 120000, parsed > 0 ? parsed : 45000);
                continue;
            }

            if (line.startsWithIgnoreCase("mac_plugin_preferred_format="))
            {
                const auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
                if (value.equalsIgnoreCase("VST3"))
                    preferredMacPluginFormat = "VST3";
                else if (value.equalsIgnoreCase("AudioUnit") || value.equalsIgnoreCase("AU"))
                    preferredMacPluginFormat = "AudioUnit";
                continue;
            }

            if (line.startsWithIgnoreCase("canonical_build_path="))
            {
                const auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
                if (value.isNotEmpty())
                    canonicalBuildPath = value;
            }
        }
    }

    void MainComponent::saveStartupPreferences() const
    {
        if (startupSettingsFile == juce::File())
            return;

        juce::StringArray lines;
        lines.add("auto_scan_plugins_on_startup=" + juce::String(autoScanPluginsOnStartup ? 1 : 0));
        lines.add("auto_quarantine_on_unclean_exit=" + juce::String(autoQuarantineOnUncleanExit ? 1 : 0));
        lines.add("mic_permission_prompted_once=" + juce::String(micPermissionPromptedOnce ? 1 : 0));
        lines.add("plugin_scan_pass_timeout_ms=" + juce::String(pluginScanPassTimeoutMs));
        lines.add("mac_plugin_preferred_format="
                  + (preferredMacPluginFormat.equalsIgnoreCase("VST3")
                         ? juce::String("VST3")
                         : juce::String("AudioUnit")));
        lines.add("canonical_build_path="
                  + (canonicalBuildPath.isNotEmpty()
                         ? canonicalBuildPath
                         : juce::String("/Users/robertclemons/Downloads/sampledex_daw-main/build/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app")));
        juce::ignoreUnused(startupSettingsFile.replaceWithText(lines.joinIntoString("\n") + "\n"));
    }

    void MainComponent::openProject()
    {
        if (!handleApplicationCloseRequest())
            return;

        const auto documentsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        const juce::File initial = currentProjectFile.existsAsFile()
            ? currentProjectFile
            : documentsDir.getChildFile("Sampledex Project.sampledex");

        projectFileChooser = std::make_unique<juce::FileChooser>(
            "Open Project",
            initial,
            "*.sampledex;*.xml",
            true);

        const auto flags = juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles;
        projectFileChooser->launchAsync(flags,
                                        [this](const juce::FileChooser& chooser)
                                        {
                                            const juce::File selectedFile = chooser.getResult();
                                            projectFileChooser.reset();
                                            if (selectedFile == juce::File{})
                                                return;
                                            loadProjectFromFile(selectedFile);
                                        });
    }

    void MainComponent::resetStreamingStateForProjectSwitch()
    {
        panicAllNotes();
        transport.stop();
        transport.setRecording(false);
        recordStartPending = false;
        autoStopAfterBeat = -1.0;
        stopTransportAfterAutoPunch = false;
        recordStartPendingRt.store(false, std::memory_order_relaxed);
        autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);
        recordStartRequestRt.store(0, std::memory_order_relaxed);
        recordStopRequestRt.store(0, std::memory_order_relaxed);
        recordEnabledRt.store(false, std::memory_order_relaxed);
        recordButton.setToggleState(false, juce::dontSendNotification);

        const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
        tracks.clear();
        arrangement.clear();
        automationLanes.clear();
        resetAutomationLatchStates();
        automationWriteReadIndex.store(0, std::memory_order_relaxed);
        automationWriteWriteIndex.store(0, std::memory_order_relaxed);
        {
            const juce::ScopedLock sl(retiredSnapshotLock);
            retiredRealtimeSnapshots.clear();
        }
        std::atomic_store_explicit(&realtimeSnapshot,
                                   std::shared_ptr<const RealtimeStateSnapshot>{},
                                   std::memory_order_release);
        streamingClipCache.clear();
        rebuildRealtimeSnapshot(false);
    }

    bool MainComponent::loadProjectFromFile(const juce::File& fileToLoad)
    {
        ProjectSerializer::ProjectState loadedProject;
        juce::String error;
        if (!ProjectSerializer::loadProject(fileToLoad, loadedProject, audioFormatManager, error))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Open Project",
                                                   error.isNotEmpty() ? error
                                                                      : juce::String("Unable to load selected project."));
            return false;
        }

        const juce::ScopedValueSetter<bool> suspendDirtyTracking(suppressDirtyTracking, true);

        closePluginEditorWindow();
        closeChannelRackWindow();
        closeEqWindow();
        resetStreamingStateForProjectSwitch();

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        const double sampleRate = setup.sampleRate > 0.0 ? setup.sampleRate
                                                          : juce::jmax(1.0, sampleRateRt.load(std::memory_order_relaxed));
        const int blockSize = setup.bufferSize > 0 ? setup.bufferSize : 512;

        juce::StringArray loadWarnings;
        auto* activeDevice = deviceManager.getCurrentAudioDevice();
        const juce::String activeDeviceName = activeDevice != nullptr ? activeDevice->getName() : juce::String();
        const bool likelyBuiltInOnLoad = (activeDevice == nullptr)
                                      || activeDeviceName.containsIgnoreCase("MacBook")
                                      || activeDeviceName.containsIgnoreCase("Built-In")
                                      || activeDeviceName.containsIgnoreCase("Internal")
                                      || activeDeviceName.containsIgnoreCase("Apple");

        const int tracksToLoad = juce::jmin(static_cast<int>(loadedProject.tracks.size()), maxRealtimeTracks);
        for (int i = 0; i < tracksToLoad; ++i)
        {
            const auto& sourceTrack = loadedProject.tracks[static_cast<size_t>(i)];
            auto* track = new Track(sourceTrack.name.isNotEmpty()
                                        ? sourceTrack.name
                                        : juce::String("Track " + juce::String(i + 1)),
                                    formatManager);
            track->setTransportPlayHead(&transport);
            track->setVolume(sourceTrack.volume);
            track->setPan(sourceTrack.pan);
            track->setSendLevel(sourceTrack.sendLevel);
            track->setSendTapMode(static_cast<Track::SendTapMode>(juce::jlimit(0, 2, sourceTrack.sendTapMode)));
            track->setSendTargetBus(sourceTrack.sendTargetBus);
            track->setMute(sourceTrack.mute);
            track->setSolo(sourceTrack.solo);
            track->setArm(sourceTrack.arm);
            const bool enableMonitoringOnLoad = sourceTrack.inputMonitoring && !likelyBuiltInOnLoad;
            track->setInputMonitoring(enableMonitoringOnLoad);
            track->setInputSourcePair(sourceTrack.inputSourcePair);
            track->setInputMonitorGain(sourceTrack.inputMonitorGain);
            track->setMonitorTapMode(static_cast<Track::MonitorTapMode>(juce::jlimit(0, 1, sourceTrack.monitorTapMode)));
            track->setChannelType(static_cast<Track::ChannelType>(juce::jlimit(0, 3, sourceTrack.channelType)));
            track->setOutputTargetType(static_cast<Track::OutputTargetType>(juce::jlimit(0, 1, sourceTrack.outputTargetType)));
            track->setOutputTargetBus(sourceTrack.outputTargetBus);
            track->setEqEnabled(sourceTrack.eqEnabled);
            track->setEqBandGains(sourceTrack.eqLowGainDb, sourceTrack.eqMidGainDb, sourceTrack.eqHighGainDb);
            track->setBuiltInEffectsMask(sourceTrack.builtInFxMask);
            track->setFrozenPlaybackOnly(false);
            track->setFrozenRenderPath({});
            track->prepareToPlay(sampleRate, juce::jmax(128, blockSize));

            if (sourceTrack.inputMonitoring && !enableMonitoringOnLoad)
            {
                loadWarnings.add("Track " + juce::String(i + 1)
                                 + " input monitoring was disabled on load to prevent built-in mic feedback.");
            }

            if (track->getChannelType() == Track::ChannelType::Audio)
                track->disableBuiltInInstrument();

            switch (track->getChannelType() == Track::ChannelType::Audio
                        ? Track::BuiltInInstrument::None
                        : static_cast<Track::BuiltInInstrument>(juce::jlimit(0, 2, sourceTrack.builtInInstrumentMode)))
            {
                case Track::BuiltInInstrument::None:
                    track->disableBuiltInInstrument();
                    break;
                case Track::BuiltInInstrument::Sampler:
                {
                    juce::String samplerError;
                    if (sourceTrack.samplerSamplePath.isNotEmpty()
                        && !track->loadSamplerSoundFromFile(juce::File(sourceTrack.samplerSamplePath), samplerError))
                    {
                        loadWarnings.add("Track " + juce::String(i + 1)
                                         + " sampler load failed: "
                                         + (samplerError.isNotEmpty() ? samplerError : juce::String("Unknown error")));
                    }
                    break;
                }
                case Track::BuiltInInstrument::BasicSynth:
                default:
                    track->useBuiltInSynthInstrument();
                    break;
            }

            if (!safeModeStartup)
            {
                for (const auto& slot : sourceTrack.pluginSlots)
                {
                    if (!slot.hasDescription)
                    {
                        continue;
                    }

                    auto candidates = getPluginLoadCandidates(slot.description, true);
                    if (candidates.isEmpty())
                    {
                        loadWarnings.add("Track " + juce::String(i + 1)
                                         + " missing plugin: "
                                         + (slot.description.name.isNotEmpty() ? slot.description.name
                                                                               : slot.description.fileOrIdentifier));
                        continue;
                    }

                    juce::String lastFailure;
                    bool loaded = false;
                    juce::PluginDescription loadedDescription;
                    const bool isInstrumentSlot = slot.slotIndex == Track::instrumentSlotIndex;

                    for (const auto& candidate : candidates)
                    {
                        if (isPluginQuarantined(candidate))
                            continue;

                        juce::String probeError;
                        if (!runPluginIsolationProbe(candidate, isInstrumentSlot, probeError))
                        {
                            quarantinePlugin(candidate, probeError);
                            lastFailure = probeError.isNotEmpty() ? probeError
                                                                  : juce::String("Plugin probe failed.");
                            continue;
                        }

                        juce::String pluginError;
                        loaded = isInstrumentSlot
                            ? track->loadInstrumentPlugin(candidate, pluginError)
                            : track->loadPluginInSlot(slot.slotIndex, candidate, pluginError);
                        if (!loaded || pluginError.isNotEmpty())
                        {
                            if (shouldQuarantinePluginLoadError(pluginError))
                                quarantinePlugin(candidate, pluginError);
                            lastFailure = pluginError.isNotEmpty() ? pluginError
                                                                   : juce::String("Plugin load failed.");
                            loaded = false;
                            continue;
                        }

                        loadedDescription = candidate;
                        break;
                    }

                    if (!loaded)
                    {
                        loadWarnings.add("Track " + juce::String(i + 1)
                                         + " plugin load failed ("
                                         + (slot.description.name.isNotEmpty() ? slot.description.name
                                                                               : slot.description.fileOrIdentifier)
                                         + "): "
                                         + (lastFailure.isNotEmpty() ? lastFailure : juce::String("Unknown error")));
                        continue;
                    }

                    if (slot.encodedState.isNotEmpty()
                        && !track->setPluginStateForSlot(slot.slotIndex, slot.encodedState))
                    {
                        loadWarnings.add("Track " + juce::String(i + 1)
                                         + " plugin state restore failed ("
                                         + (slot.description.name.isNotEmpty() ? slot.description.name
                                                                               : slot.description.fileOrIdentifier)
                                         + ").");
                    }
                    track->setPluginSlotBypassed(slot.slotIndex, slot.bypassed);
                    recordLastLoadedPlugin(loadedDescription);

                    if (slot.description.pluginFormatName.isNotEmpty()
                        && loadedDescription.pluginFormatName.isNotEmpty()
                        && !loadedDescription.pluginFormatName.equalsIgnoreCase(slot.description.pluginFormatName))
                    {
                        loadWarnings.add("Track " + juce::String(i + 1)
                                         + " plugin format fallback: "
                                         + (loadedDescription.name.isNotEmpty() ? loadedDescription.name
                                                                                : loadedDescription.fileOrIdentifier)
                                         + " loaded as "
                                         + loadedDescription.pluginFormatName
                                         + " (requested "
                                         + slot.description.pluginFormatName + ").");
                    }
                }
            }
            else if (!sourceTrack.pluginSlots.empty())
            {
                loadWarnings.add("Track " + juce::String(i + 1)
                                 + " plugin chain skipped (Safe Mode startup).");
            }

            track->setFrozenRenderPath(sourceTrack.frozenRenderPath);
            const bool validFrozenClip = sourceTrack.frozenPlaybackOnly
                                      && sourceTrack.frozenRenderPath.isNotEmpty()
                                      && juce::File(sourceTrack.frozenRenderPath).existsAsFile();
            track->setFrozenPlaybackOnly(validFrozenClip);
            if (sourceTrack.frozenPlaybackOnly && !validFrozenClip)
            {
                loadWarnings.add("Track " + juce::String(i + 1)
                                 + " frozen state disabled because frozen render file is missing.");
            }

            tracks.add(track);
            trackMidiBuffers[static_cast<size_t>(i)].ensureSize(4096);
        }

        if (loadedProject.tracks.size() > static_cast<size_t>(maxRealtimeTracks))
        {
            loadWarnings.add("Project had more tracks than supported. Loaded first "
                             + juce::String(maxRealtimeTracks) + " tracks.");
        }

        bpm = juce::jmax(1.0, loadedProject.bpm);
        bpmRt.store(bpm, std::memory_order_relaxed);
        transport.setTempo(bpm);

        projectKeyRoot = juce::jlimit(0, 11, loadedProject.keyRoot);
        projectScaleMode = juce::jmax(0, loadedProject.scaleMode);
        projectTransposeSemitones = juce::jlimit(-24, 24, loadedProject.transposeSemitones);
        globalTransposeRt.store(projectTransposeSemitones, std::memory_order_relaxed);

        keySelector.setSelectedId(projectKeyRoot + 1, juce::dontSendNotification);
        scaleSelector.setSelectedId(projectScaleMode + 1, juce::dontSendNotification);
        transposeSelector.setSelectedId(projectTransposeSemitones + 25, juce::dontSendNotification);
        if (lcdDisplay != nullptr)
        {
            const auto requestedMode = static_cast<LcdDisplay::PositionMode>(juce::jlimit(1, 3, loadedProject.lcdPositionMode));
            lcdDisplay->setPositionMode(requestedMode);
        }
        applyProjectScaleToEngines();

        tempoEvents.clear();
        for (const auto& tempo : loadedProject.tempoMap)
            tempoEvents.push_back({ tempo.beat, tempo.bpm });
        rebuildTempoEventMap();
        setTempoBpm(bpm);

        std::vector<Clip> loadedArrangement = loadedProject.arrangement;
        relinkMissingAudioFiles(loadedArrangement, fileToLoad, loadWarnings);
        for (auto& clip : loadedArrangement)
            clip.trackIndex = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), clip.trackIndex);
        arrangement = std::move(loadedArrangement);
        automationLanes = loadedProject.automationLanes;
        ensureAutomationLaneIds();
        for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
            ensureDefaultAutomationLanesForTrack(trackIndex);

        loopButton.setToggleState(loadedProject.loopEnabled, juce::dontSendNotification);
        transport.setLoop(loadedProject.loopEnabled,
                          loadedProject.loopStartBeat,
                          loadedProject.loopEndBeat);
        loopEnabledRt.store(loadedProject.loopEnabled, std::memory_order_relaxed);

        sanitizeRoutingConfiguration(false);

        if (tracks.isEmpty())
            createNewTrack();

        mixer.rebuildFromTracks(tracks);
        timeline.refreshHeaders();
        selectedClipIndex = -1;
        setSelectedTrackIndex(juce::jlimit(0, juce::jmax(0, tracks.size() - 1), selectedTrackIndex));
        timeline.selectTrack(selectedTrackIndex);
        mixer.selectTrack(selectedTrackIndex);
        undoManager.clearUndoHistory();
        rebuildRealtimeSnapshot();
        refreshChannelRackWindow();
        resized();
        refreshStatusText();
        currentProjectFile = fileToLoad;
        projectDirty = false;
        projectMutationSerial = 0;
        lastAutosaveSerial = 0;
        autosaveProjectFile.deleteFile();

        if (!loadWarnings.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Project Loaded With Warnings",
                                                   loadWarnings.joinIntoString("\n"));
        }

        return true;
    }

    void MainComponent::relinkMissingAudioFiles(std::vector<Clip>& clips,
                                                const juce::File& projectFile,
                                                juce::StringArray& warnings)
    {
        std::vector<int> missingIndices;
        missingIndices.reserve(clips.size());
        for (int i = 0; i < static_cast<int>(clips.size()); ++i)
        {
            const auto& clip = clips[static_cast<size_t>(i)];
            if (clip.type != ClipType::Audio)
                continue;

            const juce::File sourceFile(clip.audioFilePath);
            if (!sourceFile.existsAsFile())
                missingIndices.push_back(i);
        }

        if (missingIndices.empty())
            return;
        const juce::File root = projectFile.getParentDirectory();
        int relinkedCount = 0;
        for (const auto index : missingIndices)
        {
            auto& clip = clips[static_cast<size_t>(index)];
            const juce::File originalPath(clip.audioFilePath);
            juce::String targetName = originalPath.getFileName();
            if (targetName.isEmpty())
                targetName = clip.name;

            juce::File foundFile;
            for (const auto& entry : juce::RangedDirectoryIterator(root, true, targetName, juce::File::findFiles))
            {
                const auto candidate = entry.getFile();
                if (candidate.existsAsFile())
                {
                    foundFile = candidate;
                    break;
                }
            }

            if (!foundFile.existsAsFile())
                continue;

            clip.audioFilePath = foundFile.getFullPathName();
            auto reader = std::unique_ptr<juce::AudioFormatReader>(audioFormatManager.createReaderFor(foundFile));
            if (reader != nullptr)
                clip.audioSampleRate = juce::jmax(1.0, reader->sampleRate);
            ++relinkedCount;
        }

        if (relinkedCount == 0)
            warnings.add("Unable to relink missing audio files in: " + root.getFullPathName());
        else if (relinkedCount < static_cast<int>(missingIndices.size()))
            warnings.add("Relinked " + juce::String(relinkedCount)
                         + " of " + juce::String(static_cast<int>(missingIndices.size())) + " missing audio files.");
    }

    void MainComponent::scanForPlugins()
    {
        beginPluginScan();
    }

    void MainComponent::beginPluginScan()
    {
        if (pluginScanProcess != nullptr && pluginScanProcess->isRunning())
            return;

        if (formatManager.getNumFormats() == 0)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Plugin Scan",
                                                   "No plugin formats are enabled in this build.");
            return;
        }

        pendingScanFormats = getPluginScanFormatsInPreferredOrder();
        if (pendingScanFormats.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Plugin Scan",
                                                   "No plugin formats are currently available for scanning.");
            return;
        }

        scanButton.setEnabled(false);
        scanButton.setButtonText("Scanning...");
        pluginScanPassCount = 0;
        pluginScanTotalPassCount = pendingScanFormats.size();
        pluginScanProgress = -1.0;
        scanPassStartTimeMs = 0.0;
        activeScanFormat.clear();
        pluginScanFailedItems.clear();
        pluginScanBlacklistedItems.clear();
        pluginScanStatusLabel.setText("Plugin scan: preparing...", juce::dontSendNotification);
        pluginScanDeadMansPedalFile.deleteFile();
        resized();
        refreshStatusText();

        if (!startPluginScanPass())
            finishPluginScan(false, "Unable to launch isolated plugin scan process.");
    }

    bool MainComponent::startPluginScanPass()
    {
        if (pendingScanFormats.isEmpty())
            return false;

        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        if (!executable.existsAsFile())
            return false;

        activeScanFormat = pendingScanFormats[0];
        pendingScanFormats.remove(0);
        pluginScanDeadMansPedalFile.deleteFile();

        const juce::String command = executable.getFullPathName().quoted()
                                   + " --plugin-scan-pass"
                                   + " --known=" + knownPluginListFile.getFullPathName().quoted()
                                   + " --deadman=" + pluginScanDeadMansPedalFile.getFullPathName().quoted()
                                   + " --plugin-scan-format=" + activeScanFormat.quoted()
                                   + " --plugin-scan-timeout-ms=" + juce::String(pluginScanPassTimeoutMs);

        pluginScanProcess = std::make_unique<juce::ChildProcess>();
        if (!pluginScanProcess->start(command))
        {
            pluginScanProcess.reset();
            return false;
        }

        ++pluginScanPassCount;
        pluginScanProgress = -1.0;
        scanPassStartTimeMs = juce::Time::getMillisecondCounterHiRes();
        const auto passLabel = juce::String(pluginScanPassCount)
                             + "/" + juce::String(juce::jmax(1, pluginScanTotalPassCount));
        const auto displayFormat = scanFormatDisplayName(activeScanFormat);
        scanButton.setButtonText("Scanning " + displayFormat + "... (" + passLabel + ")");
        pluginScanStatusLabel.setText("Scanning " + displayFormat
                                          + " (pass " + passLabel + ")",
                                      juce::dontSendNotification);
        resized();
        refreshStatusText();
        return true;
    }

    void MainComponent::finishPluginScan(bool success, const juce::String& detailMessage)
    {
        if (pluginScanProcess != nullptr && pluginScanProcess->isRunning())
            pluginScanProcess->kill();
        pluginScanProcess.reset();

        scanButton.setEnabled(true);
        scanButton.setButtonText("Scan Plugins");
        pluginScanPassCount = 0;
        pluginScanTotalPassCount = 0;
        pluginScanProgress = 0.0;
        scanPassStartTimeMs = 0.0;
        activeScanFormat.clear();
        pendingScanFormats.clear();
        pluginScanStatusLabel.setText(success ? "Plugin scan complete" : "Plugin scan failed",
                                      juce::dontSendNotification);
        resized();
        refreshStatusText();

        if (success)
        {
            if (knownPluginListFile.existsAsFile())
            {
                std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(knownPluginListFile));
                if (xml != nullptr)
                    knownPluginList.recreateFromXml(*xml);
            }

            juce::String completionMessage = "Plugin scan complete.";
            if (!pluginScanFailedItems.isEmpty())
            {
                completionMessage
                    << "\n\nSome plugins failed or were skipped:\n"
                    << pluginScanFailedItems.joinIntoString("\n");
            }
            if (!pluginScanBlacklistedItems.isEmpty())
            {
                completionMessage
                    << "\n\nAuto-blacklisted from dead-man recovery:\n"
                    << pluginScanBlacklistedItems.joinIntoString("\n");
            }
            if (detailMessage.isNotEmpty() && pluginScanFailedItems.isEmpty())
                completionMessage << "\n\n" << detailMessage;

            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "Plugin Scan",
                                                   completionMessage);
            pluginScanFailedItems.clear();
            pluginScanBlacklistedItems.clear();
            return;
        }

        const juce::String message = detailMessage.isNotEmpty()
            ? detailMessage
            : juce::String("Plugin scan failed.");
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Plugin Scan",
                                               message);
        pluginScanFailedItems.clear();
        pluginScanBlacklistedItems.clear();
    }

    void MainComponent::timerCallback()
    {
        drainRetiredRealtimeSnapshots();

        if (pluginScanProcess != nullptr)
        {
            if (pluginScanProcess->isRunning())
            {
                pluginScanProgress = -1.0;
                pluginScanStatusBar.repaint();

                if (scanPassStartTimeMs > 0.0)
                {
                    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - scanPassStartTimeMs;
                    if (elapsedMs > static_cast<double>(juce::jmax(1000, pluginScanPassTimeoutMs)))
                    {
                        pluginScanProcess->kill();
                        const auto output = pluginScanProcess->readAllProcessOutput().trim();
                        pluginScanProcess.reset();
                        handlePluginScanPassResult(-1, output, true);

                        if (!pendingScanFormats.isEmpty())
                        {
                            if (startPluginScanPass())
                                return;
                            finishPluginScan(false, "Unable to launch isolated plugin scan process.");
                            return;
                        }

                        finishPluginScan(true, output);
                        return;
                    }
                }
                return;
            }

            const int exitCode = static_cast<int>(pluginScanProcess->getExitCode());
            const auto output = pluginScanProcess->readAllProcessOutput().trim();
            pluginScanProcess.reset();
            scanPassStartTimeMs = 0.0;

            handlePluginScanPassResult(exitCode, output, false);
            if (!pendingScanFormats.isEmpty())
            {
                if (startPluginScanPass())
                    return;
                finishPluginScan(false, "Unable to launch isolated plugin scan process.");
                return;
            }

            finishPluginScan(true, output);
            return;
        }

        static int midiDeviceRefreshTicks = 0;
        if (++midiDeviceRefreshTicks >= 30)
        {
            midiDeviceRefreshTicks = 0;

            const auto inputDevices = juce::MidiInput::getAvailableDevices();
            const int inputDeviceCount = static_cast<int>(inputDevices.size());
            bool inputListChanged = inputDeviceCount != midiInputDeviceIds.size();
            bool controlListChanged = inputDeviceCount != controlSurfaceInputDeviceIds.size();
            if (!inputListChanged || !controlListChanged)
            {
                for (int i = 0; i < inputDeviceCount; ++i)
                {
                    if (!inputListChanged
                        && (!juce::isPositiveAndBelow(i, midiInputDeviceIds.size())
                            || inputDevices[i].identifier != midiInputDeviceIds[i]))
                    {
                        inputListChanged = true;
                    }
                    if (!controlListChanged
                        && (!juce::isPositiveAndBelow(i, controlSurfaceInputDeviceIds.size())
                            || inputDevices[i].identifier != controlSurfaceInputDeviceIds[i]))
                    {
                        controlListChanged = true;
                    }
                }
            }
            if (inputListChanged)
                refreshMidiInputSelector();
            if (controlListChanged)
                refreshControlSurfaceInputSelector();

            const auto outputDevices = midiRouter.getOutputs();
            bool outputListChanged = outputDevices.size() != static_cast<size_t>(midiOutputDeviceIds.size());
            if (!outputListChanged)
            {
                for (int i = 0; i < static_cast<int>(outputDevices.size()); ++i)
                {
                    if (!juce::isPositiveAndBelow(i, midiOutputDeviceIds.size())
                        || outputDevices[static_cast<size_t>(i)].identifier != midiOutputDeviceIds[i])
                    {
                        outputListChanged = true;
                        break;
                    }
                }
            }
            if (outputListChanged)
                refreshMidiOutputSelector();

            refreshAudioEngineSelectors();
        }

        updateDetectedSyncSource();
        applyFeedbackSafetyIfRequested();
        if (builtInMonitorSafetyNoticeRequestedRt.exchange(false, std::memory_order_relaxed)
            && !builtInMonitorSafetyNoticeShown)
        {
            builtInMonitorSafetyNoticeShown = true;
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Monitor Safety",
                "Built-in mic/speaker monitoring was auto-safed on startup.\n"
                "Track input monitoring defaults to OFF to prevent feedback/buzz. Use headphones or an external interface, then enable monitoring as needed.");
        }
        if (feedbackWarningPending)
        {
            feedbackWarningPending = false;
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Feedback Protection",
                "Input monitoring was automatically disabled to prevent a built-in mic/speaker feedback loop.\n"
                "Use headphones or an external interface, then re-enable monitoring on the required track.");
        }

        drainAutomationWriteEvents();
        if (!transport.playing())
            resetAutomationLatchStates();

        recalculateAuxBusLatencyCache();
        int maxTrackLatency = 0;
        for (auto* track : tracks)
            maxTrackLatency = juce::jmax(maxTrackLatency, track != nullptr ? track->getTotalPluginLatencySamples() : 0);
        int maxAuxLatency = 0;
        for (int bus = 0; bus < auxBusCount; ++bus)
            maxAuxLatency = juce::jmax(maxAuxLatency, getAuxBusProcessingLatencySamples(bus));
        const int maxGraphLatency = maxTrackLatency + maxAuxLatency;
        maxPdcLatencySamplesRt.store(maxGraphLatency, std::memory_order_relaxed);
        const int requiredPdcCapacity = maxGraphLatency + maxRealtimeBlockSize + 128;
        if (requiredPdcCapacity > trackPdcBufferSamples)
        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            ensureTrackPdcCapacity(requiredPdcCapacity);
        }

        if (recordStartRequestRt.exchange(0, std::memory_order_relaxed) > 0
            && recordButton.getToggleState())
        {
            recordStartPending = false;
            recordStartPendingRt.store(false, std::memory_order_relaxed);
            const double startBeat = recordingStartBeatRt.load(std::memory_order_relaxed);
            const int64_t startSample = recordingStartSampleRt.load(std::memory_order_relaxed);
            startCaptureRecordingNow(startBeat, startSample);
            if (forcedMetronomeForCountIn)
            {
                forcedMetronomeForCountIn = false;
                metronomeEnabled = metronomeStateBeforeCountIn;
                metronomeEnabledRt.store(metronomeEnabled, std::memory_order_relaxed);
                metroButton.setToggleState(metronomeEnabled, juce::dontSendNotification);
            }
            refreshStatusText();
        }

        if (recordStopRequestRt.exchange(0, std::memory_order_relaxed) > 0
            && recordButton.getToggleState()
            && transport.getCurrentPositionInfo().isRecording)
        {
            recordButton.setToggleState(false, juce::dontSendNotification);
            recordEnabledRt.store(false, std::memory_order_relaxed);
            autoStopAfterBeat = -1.0;
            autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);
            stopCaptureRecordingAndCommit(stopTransportAfterAutoPunch);
        }

        if (recordStartPending && recordButton.getToggleState())
        {
            const double currentBeat = transport.getCurrentBeat();
            if (currentBeat + 1.0e-6 >= recordStartPendingBeat)
            {
                recordStartPending = false;
                recordStartPendingRt.store(false, std::memory_order_relaxed);
                startCaptureRecordingNow(recordStartPendingBeat, transport.getCurrentSample());

                if (forcedMetronomeForCountIn)
                {
                    forcedMetronomeForCountIn = false;
                    metronomeEnabled = metronomeStateBeforeCountIn;
                    metronomeEnabledRt.store(metronomeEnabled, std::memory_order_relaxed);
                    metroButton.setToggleState(metronomeEnabled, juce::dontSendNotification);
                }

                refreshStatusText();
            }
        }

        if (recordButton.getToggleState()
            && transport.getCurrentPositionInfo().isRecording
            && autoStopAfterBeat > 0.0
            && transport.getCurrentBeat() + 1.0e-6 >= autoStopAfterBeat)
        {
            recordButton.setToggleState(false, juce::dontSendNotification);
            recordEnabledRt.store(false, std::memory_order_relaxed);
            autoStopAfterBeatRt.store(-1.0, std::memory_order_relaxed);
            stopCaptureRecordingAndCommit(stopTransportAfterAutoPunch);
        }

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            activeInputChannelCountRt.store(juce::jmax(0, device->getActiveInputChannels().countNumberOfSetBits()),
                                            std::memory_order_relaxed);
            const auto currentDeviceName = device->getName();
            if (currentDeviceName != lastAudioDeviceNameSeen)
            {
                const bool wasLikelyBuiltIn = usingLikelyBuiltInAudioRt.load(std::memory_order_relaxed);
                lastAudioDeviceNameSeen = currentDeviceName;
                refreshInputDeviceSafetyState();
                const bool nowLikelyBuiltIn = usingLikelyBuiltInAudioRt.load(std::memory_order_relaxed);
                if (nowLikelyBuiltIn && !wasLikelyBuiltIn && !recordButton.getToggleState())
                {
                    bool disabledMonitoring = false;
                    for (auto* track : tracks)
                    {
                        if (track == nullptr || !track->isInputMonitoringEnabled())
                            continue;

                        track->setInputMonitoring(false);
                        disabledMonitoring = true;
                    }
                    if (disabledMonitoring)
                        refreshStatusText();
                }
            }
        }
        else if (lastAudioDeviceNameSeen.isNotEmpty())
        {
            lastAudioDeviceNameSeen.clear();
            refreshInputDeviceSafetyState();
        }
        if (auto* inspector = dynamic_cast<TrackInspectorContent*>(trackInspectorView))
            inspector->setAvailableInputChannels(activeInputChannelCountRt.load(std::memory_order_relaxed));

        std::array<float, auxBusCount> auxMeterLevels {};
        for (int bus = 0; bus < auxBusCount; ++bus)
            auxMeterLevels[static_cast<size_t>(bus)] = auxBusMeterRt[static_cast<size_t>(bus)].load(std::memory_order_relaxed);
        mixer.setAuxMeterLevels(auxMeterLevels);
        mixer.setAuxEnabled(auxFxEnabledRt.load(std::memory_order_relaxed));

        timelineZoomSlider.setValue(timeline.getPixelsPerBeat(), juce::dontSendNotification);
        trackZoomSlider.setValue(timeline.getTrackHeight(), juce::dontSendNotification);
        followPlayheadButton.setToggleState(timeline.isAutoFollowPlayheadEnabled(), juce::dontSendNotification);

        const double cpu = juce::jmax(0.0, deviceManager.getCpuUsage());
        cpuUsageRt.store(cpu, std::memory_order_relaxed);
        const float callbackLoad = juce::jmax(0.0f, audioCallbackLoadRt.load(std::memory_order_relaxed));
        if (cpu >= 0.95)
            ++highCpuFrameCount;
        else
            highCpuFrameCount = juce::jmax(0, highCpuFrameCount - 2);

        static int previousXrunCount = 0;
        const int xrunCountNow = audioXrunCountRt.load(std::memory_order_relaxed);
        if (xrunCountNow > previousXrunCount
            && !lowLatencyMode
            && callbackLoad >= 0.95f)
        {
            applyLowLatencyMode(true);
        }
        previousXrunCount = xrunCountNow;

        if (highCpuFrameCount == 90 && auxEnableButton.getToggleState())
        {
            auxEnableButton.setToggleState(false, juce::dontSendNotification);
            auxFxEnabled = false;
            auxFxEnabledRt.store(false, std::memory_order_relaxed);
            mixer.setAuxEnabled(false);
        }

        if (highCpuFrameCount == 130 && metroButton.getToggleState())
        {
            metroButton.setToggleState(false, juce::dontSendNotification);
            metronomeEnabled = false;
            metronomeEnabledRt.store(false, std::memory_order_relaxed);
        }

        if (backgroundRenderBusyRt.load(std::memory_order_relaxed))
        {
            const int renderTrackIndex = renderTrackIndexRt.load(std::memory_order_relaxed);
            const float progress = juce::jlimit(0.0f, 1.0f, renderProgressRt.load(std::memory_order_relaxed));
            juce::String taskLabel = "Render";
            if (juce::isPositiveAndBelow(renderTrackIndex, tracks.size()) && tracks[renderTrackIndex] != nullptr)
                taskLabel = tracks[renderTrackIndex]->getRenderTaskLabel();
            const juce::String pct = juce::String(juce::roundToInt(progress * 100.0f)) + "%";
            freezeButton.setButtonText(taskLabel + " " + pct);
            freezeButton.setTooltip("Background render in progress. Open menu to cancel.");
        }
        else
        {
            freezeButton.setButtonText("Freeze");
            freezeButton.setTooltip("Freeze/unfreeze/commit selected track.");
        }

        juce::StringArray names;
        for (auto* track : tracks)
            if (track != nullptr)
                names.add(track->getTrackName());
        trackListView.setTrackNames(names);

        refreshStatusText();
    }

    void MainComponent::rebuildRealtimeSnapshot(bool markDirty)
    {
        std::array<bool, static_cast<size_t>(maxRealtimeTracks)> midiTrackNeedsInstrument {};
        for (const auto& clip : arrangement)
        {
            if (clip.type != ClipType::MIDI)
                continue;
            if (!juce::isPositiveAndBelow(clip.trackIndex, tracks.size()))
                continue;
            if (!juce::isPositiveAndBelow(clip.trackIndex, maxRealtimeTracks))
                continue;
            midiTrackNeedsInstrument[static_cast<size_t>(clip.trackIndex)] = true;
        }

        for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
        {
            if (!midiTrackNeedsInstrument[static_cast<size_t>(trackIndex)])
                continue;
            ensureTrackHasPlayableInstrument(trackIndex);
        }

        recalculateAuxBusLatencyCache();
        auto snapshot = std::make_shared<RealtimeStateSnapshot>();
        snapshot->arrangement = arrangement;
        snapshot->trackPointers.reserve(static_cast<size_t>(tracks.size()));
        for (auto* track : tracks)
            snapshot->trackPointers.push_back(track);
        snapshot->tempoEvents = tempoEvents;
        snapshot->automationLanes = automationLanes;
        snapshot->globalTransposeSemitones = globalTransposeRt.load(std::memory_order_relaxed);
        snapshot->audioClipStreams.resize(snapshot->arrangement.size());

        for (size_t clipIndex = 0; clipIndex < snapshot->arrangement.size(); ++clipIndex)
        {
            auto& clip = snapshot->arrangement[clipIndex];
            if (clip.type != ClipType::Audio || clip.audioFilePath.isEmpty())
                continue;

            const juce::File sourceFile(clip.audioFilePath);
            if (!sourceFile.existsAsFile())
                continue;

            const auto key = sourceFile.getFullPathName();

            auto it = streamingClipCache.find(key);
            if (it == streamingClipCache.end() || it->second == nullptr || !it->second->isReady())
            {
                auto stream = std::make_shared<StreamingClipSource>(sourceFile,
                                                                    audioFormatManager,
                                                                    streamingAudioReadThread);
                if (!stream->isReady())
                    continue;

                it = streamingClipCache.insert_or_assign(key, stream).first;
            }

            snapshot->audioClipStreams[clipIndex] = it->second;
            if (clip.audioSampleRate <= 1.0 && it->second != nullptr)
                clip.audioSampleRate = it->second->getSampleRate();
        }

        auto newSnapshot = std::static_pointer_cast<const RealtimeStateSnapshot>(snapshot);
        auto previousSnapshot = std::atomic_exchange_explicit(&realtimeSnapshot,
                                                              std::move(newSnapshot),
                                                              std::memory_order_acq_rel);
        if (previousSnapshot != nullptr)
            retireRealtimeSnapshot(std::move(previousSnapshot));

        if (markDirty)
            markProjectDirty();
    }

    std::shared_ptr<const MainComponent::RealtimeStateSnapshot> MainComponent::getRealtimeSnapshot() const
    {
        return std::atomic_load_explicit(&realtimeSnapshot, std::memory_order_acquire);
    }

    void MainComponent::retireRealtimeSnapshot(std::shared_ptr<const RealtimeStateSnapshot> snapshot)
    {
        if (snapshot == nullptr)
            return;

        const juce::ScopedLock sl(retiredSnapshotLock);
        retiredRealtimeSnapshots.push_back(std::move(snapshot));
    }

    void MainComponent::drainRetiredRealtimeSnapshots()
    {
        std::vector<std::shared_ptr<const RealtimeStateSnapshot>> releasableSnapshots;
        {
            const juce::ScopedLock sl(retiredSnapshotLock);
            if (retiredRealtimeSnapshots.empty())
                return;

            std::vector<std::shared_ptr<const RealtimeStateSnapshot>> survivors;
            survivors.reserve(retiredRealtimeSnapshots.size());
            releasableSnapshots.reserve(retiredRealtimeSnapshots.size());
            for (auto& snapshot : retiredRealtimeSnapshots)
            {
                if (snapshot != nullptr && snapshot.use_count() > 1)
                    survivors.push_back(std::move(snapshot));
                else
                    releasableSnapshots.push_back(std::move(snapshot));
            }

            retiredRealtimeSnapshots.swap(survivors);
        }

        // Release outside the lock so expensive teardown never blocks UI state updates.
        for (auto& snapshot : releasableSnapshots)
        {
            snapshot.reset();
        }
    }

    void MainComponent::setSelectedTrackIndex(int idx)
    {
        if (tracks.isEmpty())
        {
            selectedTrackIndex = 0;
            selectedTrackIndexRt.store(0, std::memory_order_relaxed);
            channelRackTrackIndex = -1;
            refreshChannelRackWindow();
            refreshStatusText();
            return;
        }

        selectedTrackIndex = juce::jlimit(0, juce::jmax(0, tracks.size() - 1), idx);

        selectedTrackIndexRt.store(selectedTrackIndex, std::memory_order_relaxed);
        channelRackTrackIndex = selectedTrackIndex;
        refreshChannelRackWindow();
        refreshStatusText();
    }

    void MainComponent::setTempoBpm(double newBpm)
    {
        bpm = juce::jmax(1.0, newBpm);
        bpmRt.store(bpm, std::memory_order_relaxed);
        transport.setTempo(bpm);

        bool updatedBaseEvent = false;
        for (auto& event : tempoEvents)
        {
            if (std::abs(event.beat) <= 1.0e-6)
            {
                event.bpm = bpm;
                updatedBaseEvent = true;
                break;
            }
        }
        if (!updatedBaseEvent)
            tempoEvents.push_back({ 0.0, bpm });

        rebuildTempoEventMap();
    }

    void MainComponent::rebuildTempoEventMap()
    {
        for (auto& event : tempoEvents)
        {
            event.beat = juce::jmax(0.0, event.beat);
            event.bpm = juce::jmax(1.0, event.bpm);
        }

        std::sort(tempoEvents.begin(), tempoEvents.end(),
                  [](const TempoEvent& a, const TempoEvent& b)
                  {
                      if (std::abs(a.beat - b.beat) > 1.0e-9)
                          return a.beat < b.beat;
                      return a.bpm < b.bpm;
                  });

        if (tempoEvents.empty())
            tempoEvents.push_back({ 0.0, bpm });
        else if (tempoEvents.front().beat > 1.0e-6)
            tempoEvents.insert(tempoEvents.begin(), TempoEvent { 0.0, bpm });

        rebuildRealtimeSnapshot();
    }

    double MainComponent::getTempoAtBeat(double beat) const
    {
        const double clampedBeat = juce::jmax(0.0, beat);
        double result = juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed));
        for (const auto& event : tempoEvents)
        {
            if (event.beat > clampedBeat + 1.0e-9)
                break;
            result = juce::jmax(1.0, event.bpm);
        }
        return result;
    }

    void MainComponent::addTempoEvent(double beat, double tempoBpm)
    {
        tempoEvents.push_back({ juce::jmax(0.0, beat), juce::jmax(1.0, tempoBpm) });
        rebuildTempoEventMap();
    }

    void MainComponent::removeTempoEventNear(double beat, double maxDistanceBeats)
    {
        if (tempoEvents.size() <= 1)
            return;

        int nearestIndex = -1;
        double nearestDistance = std::numeric_limits<double>::max();
        for (int i = 0; i < static_cast<int>(tempoEvents.size()); ++i)
        {
            const auto& event = tempoEvents[static_cast<size_t>(i)];
            if (event.beat <= 1.0e-6)
                continue;
            const double d = std::abs(event.beat - beat);
            if (d < nearestDistance)
            {
                nearestDistance = d;
                nearestIndex = i;
            }
        }

        if (nearestIndex >= 0 && nearestDistance <= maxDistanceBeats)
        {
            tempoEvents.erase(tempoEvents.begin() + nearestIndex);
            rebuildTempoEventMap();
        }
    }

    void MainComponent::clearTempoEvents()
    {
        tempoEvents.clear();
        tempoEvents.push_back({ 0.0, juce::jmax(1.0, bpmRt.load(std::memory_order_relaxed)) });
        rebuildTempoEventMap();
    }

    void MainComponent::jumpToPreviousTempoEvent()
    {
        if (tempoEvents.empty())
            return;

        const double currentBeat = transport.getCurrentBeat();
        double previousBeat = 0.0;
        bool found = false;

        for (const auto& event : tempoEvents)
        {
            if (event.beat < currentBeat - 1.0e-6)
            {
                previousBeat = event.beat;
                found = true;
            }
            else
            {
                break;
            }
        }

        if (!found)
            return;

        transport.setPositionBeats(previousBeat);
        refreshStatusText();
    }

    void MainComponent::jumpToNextTempoEvent()
    {
        if (tempoEvents.empty())
            return;

        const double currentBeat = transport.getCurrentBeat();
        for (const auto& event : tempoEvents)
        {
            if (event.beat > currentBeat + 1.0e-6)
            {
                transport.setPositionBeats(event.beat);
                refreshStatusText();
                return;
            }
        }
    }

    void MainComponent::updateDetectedSyncSource()
    {
        constexpr double midiClockTimeoutMs = 380.0;
        constexpr double mtcTimeoutMs = 1600.0;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double lastMtcMs = lastMtcMessageMs.load(std::memory_order_relaxed);
        const double lastClockMs = lastMidiClockMessageMs.load(std::memory_order_relaxed);

        TransportEngine::SyncSource detected = TransportEngine::SyncSource::Internal;
        if (lastMtcMs >= 0.0 && (nowMs - lastMtcMs) <= mtcTimeoutMs)
            detected = TransportEngine::SyncSource::MidiTimecode;
        else if (lastClockMs >= 0.0 && (nowMs - lastClockMs) <= midiClockTimeoutMs)
            detected = TransportEngine::SyncSource::MidiClock;

        if (externalMidiClockSyncEnabledRt.load(std::memory_order_relaxed)
            && detected != TransportEngine::SyncSource::MidiClock)
        {
            externalMidiClockActiveRt.store(false, std::memory_order_relaxed);
            externalMidiClockTransportRunningRt.store(false, std::memory_order_relaxed);
            externalMidiClockLastTickMsRt.store(-1.0, std::memory_order_relaxed);
        }

        transport.setSyncSource(detected);
    }

    void MainComponent::applyProjectScaleToEngines()
    {
        auto settings = chordEngine.getSettings();
        settings.keyRootPc = juce::jlimit(0, 11, projectKeyRoot);
        switch (projectScaleMode)
        {
            case 1: settings.scale = ChordEngine::Scale::NaturalMinor; break;
            case 2: settings.scale = ChordEngine::Scale::Dorian; break;
            case 3: settings.scale = ChordEngine::Scale::Mixolydian; break;
            default: settings.scale = ChordEngine::Scale::Major; break;
        }
        chordEngine.setSettings(settings);

        pianoRoll.setScaleContext(projectKeyRoot, projectScaleMode, pianoRoll.isScaleSnapEnabled());
    }

    void MainComponent::applyGlobalTransposeToSelection(int semitones)
    {
        projectTransposeSemitones = juce::jlimit(-24, 24, semitones);
        globalTransposeRt.store(projectTransposeSemitones, std::memory_order_relaxed);
        if (transposeSelector.getSelectedId() != projectTransposeSemitones + 25)
            transposeSelector.setSelectedId(projectTransposeSemitones + 25, juce::dontSendNotification);

        // Optional "commit transpose" gesture: hold Shift while changing transpose.
        if (juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown()
            && juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size()))
            && arrangement[static_cast<size_t>(selectedClipIndex)].type == ClipType::MIDI)
        {
            const int semis = projectTransposeSemitones;
            applyClipEdit(selectedClipIndex, "Transpose MIDI Clip",
                          [semis](Clip& clip)
                          {
                              for (auto& ev : clip.events)
                                  ev.noteNumber = juce::jlimit(0, 127, ev.noteNumber + semis);
                          });
            projectTransposeSemitones = 0;
            globalTransposeRt.store(0, std::memory_order_relaxed);
            transposeSelector.setSelectedId(25, juce::dontSendNotification);
        }

        rebuildRealtimeSnapshot();
        refreshStatusText();
    }

    void MainComponent::syncTransportLoopFromUi()
    {
        const bool loopState = loopButton.getToggleState();
        loopEnabledRt.store(loopState, std::memory_order_relaxed);
        transport.setLoop(loopState, 0.0, 8.0);
    }

    void MainComponent::updateRealtimeFlagsFromUi()
    {
        recordEnabledRt.store(recordButton.getToggleState(), std::memory_order_relaxed);
        metronomeEnabledRt.store(metroButton.getToggleState(), std::memory_order_relaxed);
        masterOutputGainRt.store(masterOutputGain, std::memory_order_relaxed);
        masterSoftClipEnabledRt.store(masterSoftClipEnabled, std::memory_order_relaxed);
        masterLimiterEnabledRt.store(masterLimiterEnabled, std::memory_order_relaxed);
        auxFxEnabledRt.store(auxEnableButton.getToggleState(), std::memory_order_relaxed);
        auxReturnGainRt.store(static_cast<float>(auxReturnSlider.getValue()), std::memory_order_relaxed);
        outputDcHighPassEnabledRt.store(outputDcHighPassEnabled, std::memory_order_relaxed);
        globalTransposeRt.store(projectTransposeSemitones, std::memory_order_relaxed);
        monitorSafeModeRt.store(monitorSafeMode, std::memory_order_relaxed);
        lowLatencyMode = lowLatencyToggle.getToggleState();
        lowLatencyModeRt.store(lowLatencyMode, std::memory_order_relaxed);
        mixer.setAuxEnabled(auxEnableButton.getToggleState());
        syncTransportLoopFromUi();
        setSelectedTrackIndex(selectedTrackIndex);
        rebuildRealtimeSnapshot();
    }

    void MainComponent::refreshStatusText()
    {
        juce::String trackText = "Track: None";
        juce::String sendText = "Send: --";
        juce::String outputRouteText = "Out: MSTR";
        juce::String monitorRouteText = "Input: --";
        if (juce::isPositiveAndBelow(selectedTrackIndex, tracks.size()))
        {
            auto* selectedTrack = tracks[selectedTrackIndex];
            trackText = "Track: " + selectedTrack->getTrackName();
            juce::String sendModeLabel = "Post";
            switch (selectedTrack->getSendTapMode())
            {
                case Track::SendTapMode::PreFader: sendModeLabel = "Pre"; break;
                case Track::SendTapMode::PostPan: sendModeLabel = "Post-Pan"; break;
                case Track::SendTapMode::PostFader: sendModeLabel = "Post"; break;
                default: break;
            }
            sendText = "Send: "
                     + juce::String(std::round(selectedTrack->getSendLevel() * 100.0f))
                     + "%"
                     + " B" + juce::String(selectedTrack->getSendTargetBus() + 1)
                     + " " + sendModeLabel;
            outputRouteText = "Out: "
                            + juce::String(selectedTrack->getOutputTargetType() == Track::OutputTargetType::Bus
                                               ? ("B" + juce::String(selectedTrack->getOutputTargetBus() + 1))
                                               : juce::String("MSTR"));

            const int pairIndex = selectedTrack->getInputSourcePair();
            const juce::String monitorTapLabel = (selectedTrack->getMonitorTapMode() == Track::MonitorTapMode::PreInserts)
                ? juce::String("PreIns")
                : juce::String("PostIns");
            if (pairIndex < 0)
            {
                monitorRouteText = "Input: Auto " + monitorTapLabel
                                 + " G" + juce::String(selectedTrack->getInputMonitorGain(), 2);
            }
            else
            {
                const int leftInput = pairIndex * 2 + 1;
                const int rightInput = leftInput + 1;
                monitorRouteText = "Input: "
                                 + juce::String(leftInput) + "/" + juce::String(rightInput)
                                 + " " + monitorTapLabel
                                 + " G" + juce::String(selectedTrack->getInputMonitorGain(), 2);
            }
        }

        juce::String clipText = "Clip: None";
        if (juce::isPositiveAndBelow(selectedClipIndex, static_cast<int>(arrangement.size())))
            clipText = "Clip: " + arrangement[static_cast<size_t>(selectedClipIndex)].name;

        juce::String midiText = "MIDI In: Internal (DAW MIDI+WAV)";
        if (midiInputSelector.getSelectedId() > 1)
            midiText = midiInputSelector.getText();
        juce::String midiOutText = "MIDI Out: None";
        if (midiOutputSelector.getSelectedId() > 1)
            midiOutText = midiOutputSelector.getText();
        juce::String controlSurfaceText = "Surface: None";
        if (controlSurfaceInputSelector.getSelectedId() > 1)
            controlSurfaceText = controlSurfaceInputSelector.getText();
        const juce::String midiThruText = midiThruToggle.getToggleState() ? "Thru ON" : "Thru OFF";
        const juce::String midiLearnText = midiLearnArmed
            ? ("Learn ARMED (" + midiLearnTargetToLabel(pendingMidiLearnTarget) + ")")
            : "Learn Off";

        int armedCount = 0;
        int monitorCount = 0;
        for (auto* track : tracks)
        {
            if (track->isArmed())
                ++armedCount;
            if (track->isInputMonitoringEnabled())
                ++monitorCount;
        }

        const juce::String transportState = transport.playing() ? "Playing" : "Stopped";
        const juce::String recState = recordButton.getToggleState()
            ? (recordStartPending ? "Count-In" : "Rec Armed")
            : "Rec Off";
        const juce::String followState = timeline.isAutoFollowPlayheadEnabled() ? "Follow ON" : "Follow OFF";
        const juce::String auxState = (auxEnableButton.getToggleState() ? "Aux ON " : "Aux OFF ")
                                      + juce::String(auxReturnSlider.getValue(), 2);
        const juce::String outputState = "Out "
                                       + juce::String(masterOutSlider.getValue(), 2)
                                       + " "
                                       + (softClipButton.getToggleState() ? "Clip" : "Clean")
                                       + " "
                                       + (limiterButton.getToggleState() ? "Limit" : "NoLimit");
        const float masterPeak = juce::jmax(0.000001f, masterPeakMeterRt.load(std::memory_order_relaxed));
        const float masterRms = juce::jmax(0.000001f, masterRmsMeterRt.load(std::memory_order_relaxed));
        const float masterPhase = juce::jlimit(-1.0f, 1.0f, masterPhaseCorrelationRt.load(std::memory_order_relaxed));
        const float masterLufs = masterLoudnessLufsRt.load(std::memory_order_relaxed);
        const float peakDb = juce::Decibels::gainToDecibels(masterPeak, -60.0f);
        const float rmsDb = juce::Decibels::gainToDecibels(masterRms, -60.0f);
        const juce::String meterState = "Master "
                                      + juce::String(peakDb, 1) + "pk "
                                      + juce::String(rmsDb, 1) + "rms"
                                      + "  LUFS " + juce::String(masterLufs, 1)
                                      + "  Phase " + juce::String(masterPhase, 2)
                                      + (masterClipHoldRt.load(std::memory_order_relaxed) > 0 ? " CLIP" : "");
        const juce::String ioState = "Arm " + juce::String(armedCount) + " / Mon " + juce::String(monitorCount);
        const juce::String musicalState = "Key "
                                        + keyNames[projectKeyRoot]
                                        + " "
                                        + scaleNames[juce::jlimit(0, scaleNames.size() - 1, projectScaleMode)]
                                        + "  Tr " + juce::String(projectTransposeSemitones > 0 ? "+" : "")
                                        + juce::String(projectTransposeSemitones);
        const double cpuPercent = cpuUsageRt.load(std::memory_order_relaxed) * 100.0;
        const double callbackLoadPercent = static_cast<double>(audioCallbackLoadRt.load(std::memory_order_relaxed)) * 100.0;
        const juce::String cpuState = "CPU " + juce::String(cpuPercent, 1) + "%";
        const juce::String tempoState = "TempoEv " + juce::String(static_cast<int>(tempoEvents.size()));
        const int guardDrops = audioGuardDropCountRt.load(std::memory_order_relaxed);
        const int xrunCount = audioXrunCountRt.load(std::memory_order_relaxed);
        const int overloadCount = audioCallbackOverloadCountRt.load(std::memory_order_relaxed);
        const juce::String guardState = "GuardDrop " + juce::String(guardDrops);
        const juce::String perfState = "CB "
                                     + juce::String(callbackLoadPercent, 1) + "%"
                                     + " XR " + juce::String(xrunCount)
                                     + " OL " + juce::String(overloadCount)
                                     + " LL " + (lowLatencyMode ? juce::String("ON") : juce::String("OFF"));
        const juce::String liveInState = "InCh " + juce::String(activeInputChannelCountRt.load(std::memory_order_relaxed));
        const juce::String monitorSafeState = monitorSafeMode ? "MonSafe ON" : "MonSafe OFF";
        const float inputTrim = inputMonitorSafetyTrimRt.load(std::memory_order_relaxed);
        const juce::String inputSafetyState
            = juce::String(usingLikelyBuiltInAudioRt.load(std::memory_order_relaxed)
                               ? "BuiltinIO "
                               : "IfaceIO ")
            + "Trim " + juce::String(inputTrim, 2);
        const juce::String countInState = "CntIn " + juce::String(recordCountInBars) + "b";
        const juce::String punchState = punchEnabled
            ? ("Punch " + juce::String(punchInBeat, 2) + "->" + juce::String(punchOutBeat, 2)
               + " Pre " + juce::String(preRollBars) + "b"
               + " Post " + juce::String(postRollBars) + "b")
            : "Punch OFF";
        const juce::String pendingRecordState = recordStartPending
            ? ("Rec@" + juce::String(recordStartPendingBeat, 2))
            : "Rec@Now";
        const bool renderBusy = backgroundRenderBusyRt.load(std::memory_order_relaxed);
        const juce::String renderState = renderBusy
            ? (juce::String("Render ")
               + juce::String(juce::roundToInt(juce::jlimit(0.0f, 1.0f, renderProgressRt.load(std::memory_order_relaxed)) * 100.0f))
               + "%")
            : juce::String("Render Idle");
        const int startupGuardBlocksRemaining = startupSafetyBlocksRemainingRt.load(std::memory_order_relaxed);
        const int startupMuteBlocksRemaining = outputSafetyMuteBlocksRt.load(std::memory_order_relaxed);
        const juce::String startupSafetyState
            = (startupGuardBlocksRemaining > 0 || startupMuteBlocksRemaining > 0)
                ? ("StartSafe G" + juce::String(startupGuardBlocksRemaining)
                   + " M" + juce::String(startupMuteBlocksRemaining))
                : juce::String("StartSafe Idle");
        const juce::String scanState = (pluginScanProcess != nullptr)
            ? ("Scan " + scanFormatDisplayName(activeScanFormat)
               + " "
               + juce::String(juce::jmax(1, pluginScanPassCount))
               + "/"
               + juce::String(juce::jmax(1, pluginScanTotalPassCount)))
            : juce::String("Scan Idle");

        statusLabel.setText(trackText + "  |  " + sendText + "  |  " + clipText + "  |  " + midiText
                            + "  |  " + midiOutText
                            + "  |  " + controlSurfaceText
                            + "  |  " + midiThruText
                            + "  |  " + midiLearnText
                            + "  |  " + outputRouteText
                            + "  |  " + musicalState
                            + "  |  " + tempoState
                            + "  |  " + cpuState
                            + "  |  " + perfState
                            + "  |  " + ioState
                            + "  |  " + monitorRouteText
                            + "  |  " + liveInState
                            + "  |  " + monitorSafeState
                            + "  |  " + inputSafetyState
                            + "  |  " + countInState
                            + "  |  " + punchState
                            + "  |  " + pendingRecordState
                            + "  |  " + transportState + " / " + recState
                            + "  |  " + followState
                            + "  |  " + auxState
                            + "  |  " + outputState
                            + "  |  " + meterState
                            + "  |  " + guardState
                            + "  |  " + renderState
                            + "  |  " + startupSafetyState
                            + "  |  " + scanState
                            + "  |  Tips: click I1-I4, right-click tracks/strips (send level + mode + bus), vertical-drag empty lanes to reorder",
                            juce::dontSendNotification);
    }

    void MainComponent::applyUiStyling()
    {
        const float uiScale = theme::ThemeManager::instance().uiScaleFor(getWidth());
        const auto baseButton = theme::Colours::panel().brighter(0.12f);
        const auto buttonOn = theme::Colours::accent().withBrightness(0.85f);

        const auto styleButton = [&](juce::TextButton& button, juce::Colour normal, juce::Colour onColour)
        {
            button.setColour(juce::TextButton::buttonColourId, normal);
            button.setColour(juce::TextButton::buttonOnColourId, onColour);
            button.setColour(juce::TextButton::textColourOffId, theme::Colours::text());
            button.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
            button.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
            button.setTriggeredOnMouseDown(false);
            button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
        };

        styleButton(playButton, baseButton, juce::Colour::fromRGB(82, 176, 112));
        styleButton(stopButton, baseButton, juce::Colour::fromRGB(170, 170, 170));
        styleButton(recordButton, baseButton, juce::Colour::fromRGB(210, 74, 74));
        styleButton(panicButton, baseButton, juce::Colour::fromRGB(224, 114, 62));
        styleButton(loopButton, baseButton, juce::Colour::fromRGB(220, 150, 62));
        styleButton(metroButton, baseButton, juce::Colour::fromRGB(95, 148, 210));
        styleButton(tempoMenuButton, baseButton, juce::Colour::fromRGB(102, 176, 225));
        styleButton(clipToolsButton, baseButton, juce::Colour::fromRGB(113, 192, 150));
        styleButton(transportStartButton, baseButton, buttonOn);
        styleButton(transportPrevBarButton, baseButton, buttonOn);
        styleButton(transportNextBarButton, baseButton, buttonOn);
        styleButton(undoButton, baseButton, buttonOn);
        styleButton(redoButton, baseButton, buttonOn);
        styleButton(timelineZoomOutButton, baseButton, buttonOn);
        styleButton(timelineZoomInButton, baseButton, buttonOn);
        styleButton(trackZoomOutButton, baseButton, buttonOn);
        styleButton(trackZoomInButton, baseButton, buttonOn);
        styleButton(resetZoomButton, baseButton, buttonOn);
        styleButton(scanButton, baseButton, buttonOn);
        styleButton(addTrackButton, baseButton, buttonOn);
        styleButton(showEditorButton, baseButton, buttonOn);
        styleButton(freezeButton, baseButton, juce::Colour::fromRGB(125, 190, 222));
        styleButton(rackButton, baseButton, buttonOn);
        styleButton(inspectorButton, baseButton, buttonOn);
        styleButton(recordSetupButton, baseButton, buttonOn);
        styleButton(projectButton, baseButton, juce::Colour::fromRGB(118, 174, 226));
        styleButton(settingsButton, baseButton, buttonOn);
        styleButton(helpButton, baseButton, buttonOn);
        styleButton(exportButton, baseButton, buttonOn);
        styleButton(saveButton, baseButton, buttonOn);
        auxEnableButton.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        auxEnableButton.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        softClipButton.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        softClipButton.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        limiterButton.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        limiterButton.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        midiLearnArmToggle.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        midiLearnArmToggle.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        midiThruToggle.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        midiThruToggle.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        externalClockToggle.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        externalClockToggle.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        lowLatencyToggle.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        lowLatencyToggle.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        backgroundRenderToggle.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        backgroundRenderToggle.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());
        followPlayheadButton.setColour(juce::ToggleButton::textColourId, theme::Colours::text());
        followPlayheadButton.setColour(juce::ToggleButton::tickColourId, theme::Colours::accent());

        const auto styleCombo = [](juce::ComboBox& combo)
        {
            combo.setColour(juce::ComboBox::backgroundColourId, theme::Colours::panel().brighter(0.08f));
            combo.setColour(juce::ComboBox::textColourId, theme::Colours::text());
            combo.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.22f));
            combo.setColour(juce::ComboBox::arrowColourId, theme::Colours::text().withAlpha(0.85f));
        };

        styleCombo(gridSelector);
        styleCombo(sampleRateSelector);
        styleCombo(bufferSizeSelector);
        styleCombo(keySelector);
        styleCombo(scaleSelector);
        styleCombo(transposeSelector);
        styleCombo(midiInputSelector);
        styleCombo(midiOutputSelector);
        styleCombo(controlSurfaceInputSelector);
        styleCombo(midiLearnTargetSelector);

        masterOutLabel.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.85f));
        masterOutLabel.setFont(theme::Typography::label(uiScale));
        masterOutSlider.setColour(juce::Slider::trackColourId, theme::Colours::accent().withAlpha(0.8f));
        masterOutSlider.setColour(juce::Slider::thumbColourId, theme::Colours::accent().brighter(0.15f));
        masterOutSlider.setColour(juce::Slider::backgroundColourId, theme::Colours::panel().brighter(0.12f));

        auxReturnLabel.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.85f));
        auxReturnLabel.setFont(theme::Typography::label(uiScale));
        auxReturnSlider.setColour(juce::Slider::trackColourId, theme::Colours::accent().withAlpha(0.8f));
        auxReturnSlider.setColour(juce::Slider::thumbColourId, theme::Colours::accent());
        auxReturnSlider.setColour(juce::Slider::backgroundColourId, theme::Colours::panel().brighter(0.12f));

        timelineZoomSlider.setColour(juce::Slider::trackColourId, theme::Colours::accent().withAlpha(0.78f));
        timelineZoomSlider.setColour(juce::Slider::thumbColourId, theme::Colours::accent().brighter(0.25f));
        timelineZoomSlider.setColour(juce::Slider::backgroundColourId, theme::Colours::panel().brighter(0.12f));
        trackZoomSlider.setColour(juce::Slider::trackColourId, theme::Colours::accent().withAlpha(0.78f));
        trackZoomSlider.setColour(juce::Slider::thumbColourId, theme::Colours::accent().brighter(0.25f));
        trackZoomSlider.setColour(juce::Slider::backgroundColourId, theme::Colours::panel().brighter(0.12f));

        statusLabel.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.85f));
        statusLabel.setFont(theme::Typography::label(uiScale));
        pluginScanStatusLabel.setColour(juce::Label::textColourId, theme::Colours::text().withAlpha(0.90f));
        pluginScanStatusLabel.setFont(theme::Typography::mono(juce::jmax(0.9f, uiScale * 0.92f)));
        pluginScanStatusBar.setColour(juce::ProgressBar::foregroundColourId, theme::Colours::accent());
        pluginScanStatusBar.setColour(juce::ProgressBar::backgroundColourId, theme::Colours::panel().brighter(0.10f));

        bottomTabs.setColour(juce::TabbedComponent::backgroundColourId, theme::Colours::background());
        bottomTabs.setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::white.withAlpha(0.10f));
        bottomTabs.setColour(juce::TabbedButtonBar::frontOutlineColourId, juce::Colours::white.withAlpha(0.12f));
    }

    void MainComponent::refreshMidiInputSelector()
    {
        midiInputSelector.clear(juce::dontSendNotification);
        midiInputDeviceIds.clear();
        midiInputSelector.addItem("MIDI In: Internal (DAW MIDI+WAV)", 1);

        const auto inputs = juce::MidiInput::getAvailableDevices();
        int nextId = 2;
        int selectedId = 1;
        juce::String previousIdentifier;
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            previousIdentifier = activeMidiInputIdentifier;
        }
        for (const auto& input : inputs)
        {
            midiInputSelector.addItem("MIDI In: " + input.name, nextId);
            midiInputDeviceIds.add(input.identifier);
            if (input.identifier == previousIdentifier)
                selectedId = nextId;
            else if (selectedId == 1 && !input.name.containsIgnoreCase("bluetooth"))
                selectedId = nextId;
            ++nextId;
        }

        midiInputSelector.setSelectedId(selectedId, juce::dontSendNotification);
        applyMidiInputSelection(selectedId);
    }

    void MainComponent::applyMidiInputSelection(int selectedId)
    {
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            activeMidiInputIdentifier.clear();
        }
        if (selectedId <= 1)
        {
            lastMidiClockMessageMs.store(-1.0, std::memory_order_relaxed);
            lastMtcMessageMs.store(-1.0, std::memory_order_relaxed);
            transport.setSyncSource(TransportEngine::SyncSource::Internal);
            refreshMidiDeviceCallbacks();
            refreshStatusText();
            return;
        }

        const int inputIndex = selectedId - 2;
        if (!juce::isPositiveAndBelow(inputIndex, midiInputDeviceIds.size()))
        {
            lastMidiClockMessageMs.store(-1.0, std::memory_order_relaxed);
            lastMtcMessageMs.store(-1.0, std::memory_order_relaxed);
            transport.setSyncSource(TransportEngine::SyncSource::Internal);
            refreshMidiDeviceCallbacks();
            refreshStatusText();
            return;
        }

        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            activeMidiInputIdentifier = midiInputDeviceIds[inputIndex];
        }
        refreshMidiDeviceCallbacks();
        refreshStatusText();
    }

    void MainComponent::refreshMidiOutputSelector()
    {
        midiOutputSelector.clear(juce::dontSendNotification);
        midiOutputDeviceIds.clear();
        midiOutputSelector.addItem("MIDI Out: None", 1);
        midiOutputSelector.addItem("MIDI Out: Virtual Sampledex", 2);

        const auto outputs = midiRouter.getOutputs();
        int nextId = 3;
        juce::String previousIdentifier;
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            previousIdentifier = activeMidiOutputIdentifier;
        }
        int selectedId = previousIdentifier == "virtual" ? 2 : 1;
        for (const auto& output : outputs)
        {
            midiOutputSelector.addItem("MIDI Out: " + output.name, nextId);
            midiOutputDeviceIds.add(output.identifier);
            if (output.identifier == previousIdentifier)
                selectedId = nextId;
            ++nextId;
        }

        midiOutputSelector.setSelectedId(selectedId, juce::dontSendNotification);
        applyMidiOutputSelection(selectedId);
    }

    void MainComponent::applyMidiOutputSelection(int selectedId)
    {
        if (selectedId <= 1)
        {
            {
                const juce::ScopedLock sl(midiDeviceSelectionLock);
                activeMidiOutputIdentifier.clear();
            }
            midiOutputActiveRt.store(false, std::memory_order_relaxed);
            midiRouter.setVirtualOutputEnabled(false, {});
            midiRouter.setOutputByIndex(-1);
            midiThruEnabledRt.store(false, std::memory_order_relaxed);
            midiThruToggle.setToggleState(false, juce::dontSendNotification);
            midiThruToggle.setEnabled(false);
            refreshStatusText();
            return;
        }

        if (selectedId == 2)
        {
            {
                const juce::ScopedLock sl(midiDeviceSelectionLock);
                activeMidiOutputIdentifier = "virtual";
            }
            midiOutputActiveRt.store(true, std::memory_order_relaxed);
            midiRouter.setOutputByIndex(-1);
            midiRouter.setVirtualOutputEnabled(true, "Sampledex ChordLab");
            midiThruToggle.setEnabled(true);
            refreshStatusText();
            return;
        }

        const int outputIndex = selectedId - 3;
        if (!juce::isPositiveAndBelow(outputIndex, midiOutputDeviceIds.size()))
        {
            {
                const juce::ScopedLock sl(midiDeviceSelectionLock);
                activeMidiOutputIdentifier.clear();
            }
            midiOutputActiveRt.store(false, std::memory_order_relaxed);
            midiRouter.setVirtualOutputEnabled(false, {});
            midiRouter.setOutputByIndex(-1);
            midiThruEnabledRt.store(false, std::memory_order_relaxed);
            midiThruToggle.setToggleState(false, juce::dontSendNotification);
            midiThruToggle.setEnabled(false);
            refreshStatusText();
            return;
        }

        const auto desiredIdentifier = midiOutputDeviceIds[outputIndex];
        const auto outputs = midiRouter.getOutputs();
        int routerIndex = -1;
        for (int i = 0; i < static_cast<int>(outputs.size()); ++i)
        {
            if (outputs[static_cast<size_t>(i)].identifier == desiredIdentifier)
            {
                routerIndex = i;
                break;
            }
        }

        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            activeMidiOutputIdentifier = desiredIdentifier;
        }
        midiOutputActiveRt.store(routerIndex >= 0, std::memory_order_relaxed);
        midiRouter.setVirtualOutputEnabled(false, {});
        midiRouter.setOutputByIndex(routerIndex);
        midiThruToggle.setEnabled(routerIndex >= 0);
        refreshStatusText();
    }

    void MainComponent::refreshControlSurfaceInputSelector()
    {
        controlSurfaceInputSelector.clear(juce::dontSendNotification);
        controlSurfaceInputDeviceIds.clear();
        controlSurfaceInputSelector.addItem("Surface: None", 1);

        const auto inputs = juce::MidiInput::getAvailableDevices();
        int nextId = 2;
        int selectedId = 1;
        juce::String previousIdentifier;
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            previousIdentifier = activeControlSurfaceInputIdentifier;
        }
        for (const auto& input : inputs)
        {
            controlSurfaceInputSelector.addItem("Surface: " + input.name, nextId);
            controlSurfaceInputDeviceIds.add(input.identifier);
            if (input.identifier == previousIdentifier)
                selectedId = nextId;
            ++nextId;
        }

        controlSurfaceInputSelector.setSelectedId(selectedId, juce::dontSendNotification);
        applyControlSurfaceInputSelection(selectedId);
    }

    void MainComponent::applyControlSurfaceInputSelection(int selectedId)
    {
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            activeControlSurfaceInputIdentifier.clear();
        }
        if (selectedId > 1)
        {
            const int inputIndex = selectedId - 2;
            if (juce::isPositiveAndBelow(inputIndex, controlSurfaceInputDeviceIds.size()))
            {
                const juce::ScopedLock sl(midiDeviceSelectionLock);
                activeControlSurfaceInputIdentifier = controlSurfaceInputDeviceIds[inputIndex];
            }
        }

        for (auto& value : controlSurfaceLastCcValue)
            value = -1;

        refreshMidiDeviceCallbacks();
        refreshStatusText();
    }

    void MainComponent::refreshMidiDeviceCallbacks()
    {
        const auto inputs = juce::MidiInput::getAvailableDevices();
        for (const auto& input : inputs)
        {
            if (deviceManager.isMidiInputDeviceEnabled(input.identifier))
            {
                deviceManager.removeMidiInputDeviceCallback(input.identifier, this);
                deviceManager.setMidiInputDeviceEnabled(input.identifier, false);
            }
        }

        const auto enableInput = [this, &inputs](const juce::String& identifier)
        {
            if (identifier.isEmpty())
                return;
            const auto it = std::find_if(inputs.begin(), inputs.end(),
                                         [&identifier](const juce::MidiDeviceInfo& info)
                                         {
                                             return info.identifier == identifier;
                                         });
            if (it == inputs.end())
                return;
            deviceManager.setMidiInputDeviceEnabled(identifier, true);
            deviceManager.addMidiInputDeviceCallback(identifier, this);
        };

        juce::String midiInputIdentifier;
        juce::String controlSurfaceIdentifier;
        {
            const juce::ScopedLock sl(midiDeviceSelectionLock);
            midiInputIdentifier = activeMidiInputIdentifier;
            controlSurfaceIdentifier = activeControlSurfaceInputIdentifier;
        }

        enableInput(midiInputIdentifier);
        if (controlSurfaceIdentifier.isNotEmpty()
            && controlSurfaceIdentifier != midiInputIdentifier)
        {
            enableInput(controlSurfaceIdentifier);
        }
    }

    juce::String MainComponent::getMidiSourceIdentifier(const juce::MidiInput* source) const
    {
        if (source == nullptr)
            return {};

        auto sourceIdentifier = source->getIdentifier();
        if (sourceIdentifier.isNotEmpty())
            return sourceIdentifier;

        const auto sourceName = source->getName();
        for (const auto& info : juce::MidiInput::getAvailableDevices())
        {
            if (info.name == sourceName)
                return info.identifier;
        }

        return sourceName;
    }

    bool MainComponent::isTrackScopedMidiLearnTarget(MidiLearnTarget target) const
    {
        switch (target)
        {
            case MidiLearnTarget::TrackVolume:
            case MidiLearnTarget::TrackPan:
            case MidiLearnTarget::TrackSend:
            case MidiLearnTarget::TrackMute:
            case MidiLearnTarget::TrackSolo:
            case MidiLearnTarget::TrackArm:
            case MidiLearnTarget::TrackMonitor:
                return true;
            case MidiLearnTarget::MasterOutput:
            case MidiLearnTarget::TransportPlay:
            case MidiLearnTarget::TransportStop:
            case MidiLearnTarget::TransportRecord:
            default:
                return false;
        }
    }

    juce::String MainComponent::midiLearnTargetToLabel(MidiLearnTarget target) const
    {
        switch (target)
        {
            case MidiLearnTarget::TrackPan: return "Track Pan";
            case MidiLearnTarget::TrackSend: return "Track Send";
            case MidiLearnTarget::TrackMute: return "Track Mute";
            case MidiLearnTarget::TrackSolo: return "Track Solo";
            case MidiLearnTarget::TrackArm: return "Track Arm";
            case MidiLearnTarget::TrackMonitor: return "Track Monitor";
            case MidiLearnTarget::MasterOutput: return "Master Output";
            case MidiLearnTarget::TransportPlay: return "Transport Play";
            case MidiLearnTarget::TransportStop: return "Transport Stop";
            case MidiLearnTarget::TransportRecord: return "Transport Record";
            case MidiLearnTarget::TrackVolume:
            default: return "Track Volume";
        }
    }

    MainComponent::MidiLearnTarget MainComponent::midiLearnTargetFromId(int selectedId) const
    {
        switch (selectedId)
        {
            case 2: return MidiLearnTarget::TrackPan;
            case 3: return MidiLearnTarget::TrackSend;
            case 4: return MidiLearnTarget::TrackMute;
            case 5: return MidiLearnTarget::TrackSolo;
            case 6: return MidiLearnTarget::TrackArm;
            case 7: return MidiLearnTarget::TrackMonitor;
            case 8: return MidiLearnTarget::MasterOutput;
            case 9: return MidiLearnTarget::TransportPlay;
            case 10: return MidiLearnTarget::TransportStop;
            case 11: return MidiLearnTarget::TransportRecord;
            case 1:
            default: return MidiLearnTarget::TrackVolume;
        }
    }

    void MainComponent::clearMidiLearnArm()
    {
        midiLearnArmed = false;
        pendingMidiLearnTrackIndex = -1;
        midiLearnArmToggle.setToggleState(false, juce::dontSendNotification);
    }

    void MainComponent::armMidiLearnForSelectedTarget()
    {
        pendingMidiLearnTarget = midiLearnTargetFromId(midiLearnTargetSelector.getSelectedId());
        pendingMidiLearnTrackIndex = isTrackScopedMidiLearnTarget(pendingMidiLearnTarget)
            ? selectedTrackIndex
            : -1;
        midiLearnArmed = true;
        midiLearnArmToggle.setToggleState(true, juce::dontSendNotification);
    }

    void MainComponent::loadMidiLearnMappings()
    {
        {
            const juce::ScopedLock sl(midiLearnLock);
            midiLearnMappings.clear();
        }
        if (midiLearnMappingsFile == juce::File() || !midiLearnMappingsFile.existsAsFile())
            return;

        juce::StringArray lines;
        lines.addLines(midiLearnMappingsFile.loadFileAsString());
        for (const auto& rawLine : lines)
        {
            const auto line = rawLine.trim();
            if (line.isEmpty() || line.startsWith("#"))
                continue;

            juce::StringArray cols;
            cols.addTokens(line, "\t", "");
            if (cols.size() < 6)
                continue;

            MidiLearnMapping mapping;
            mapping.sourceIdentifier = cols[0];
            mapping.channel = cols[1].getIntValue();
            mapping.controller = cols[2].getIntValue();
            mapping.target = static_cast<MidiLearnTarget>(juce::jlimit(0, 10, cols[3].getIntValue()));
            mapping.trackIndex = cols[4].getIntValue();
            mapping.isToggle = cols[5].getIntValue() != 0;
            mapping.lastValue = -1;

            if (!juce::isPositiveAndBelow(mapping.controller, 128))
                continue;
            const juce::ScopedLock sl(midiLearnLock);
            midiLearnMappings.push_back(mapping);
        }
    }

    void MainComponent::saveMidiLearnMappings() const
    {
        if (midiLearnMappingsFile == juce::File())
            return;

        std::vector<MidiLearnMapping> snapshot;
        {
            const juce::ScopedLock sl(midiLearnLock);
            snapshot = midiLearnMappings;
        }

        juce::StringArray lines;
        lines.add("# sourceIdentifier<TAB>channel<TAB>controller<TAB>target<TAB>trackIndex<TAB>toggle");
        for (const auto& mapping : snapshot)
        {
            lines.add(mapping.sourceIdentifier + "\t"
                      + juce::String(mapping.channel) + "\t"
                      + juce::String(mapping.controller) + "\t"
                      + juce::String(static_cast<int>(mapping.target)) + "\t"
                      + juce::String(mapping.trackIndex) + "\t"
                      + juce::String(mapping.isToggle ? 1 : 0));
        }
        juce::ignoreUnused(midiLearnMappingsFile.replaceWithText(lines.joinIntoString("\n")));
    }

    void MainComponent::captureMidiLearnMapping(const juce::String& sourceIdentifier, const juce::MidiMessage& message)
    {
        if (!message.isController())
            return;

        MidiLearnMapping mapping;
        mapping.sourceIdentifier = sourceIdentifier;
        mapping.channel = message.getChannel();
        mapping.controller = message.getControllerNumber();
        mapping.target = pendingMidiLearnTarget;
        mapping.trackIndex = isTrackScopedMidiLearnTarget(mapping.target)
            ? juce::jlimit(0, juce::jmax(0, tracks.size() - 1), pendingMidiLearnTrackIndex)
            : -1;
        mapping.isToggle = mapping.target == MidiLearnTarget::TrackMute
                        || mapping.target == MidiLearnTarget::TrackSolo
                        || mapping.target == MidiLearnTarget::TrackArm
                        || mapping.target == MidiLearnTarget::TrackMonitor
                        || mapping.target == MidiLearnTarget::TransportPlay
                        || mapping.target == MidiLearnTarget::TransportStop
                        || mapping.target == MidiLearnTarget::TransportRecord;
        mapping.lastValue = message.getControllerValue();

        bool replaced = false;
        {
            const juce::ScopedLock sl(midiLearnLock);
            for (auto& existing : midiLearnMappings)
            {
                if (existing.sourceIdentifier == mapping.sourceIdentifier
                    && existing.channel == mapping.channel
                    && existing.controller == mapping.controller)
                {
                    existing = mapping;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                midiLearnMappings.push_back(mapping);
        }

        saveMidiLearnMappings();
        clearMidiLearnArm();
        refreshStatusText();
    }

    bool MainComponent::applyMappedMidiLearn(const juce::String& sourceIdentifier, const juce::MidiMessage& message)
    {
        if (!message.isController())
            return false;

        const int controller = message.getControllerNumber();
        const int channel = message.getChannel();
        const int value = message.getControllerValue();

        struct PendingApply
        {
            MidiLearnMapping mapping;
            bool risingEdge = false;
        };
        std::vector<PendingApply> pending;
        {
            const juce::ScopedLock sl(midiLearnLock);
            pending.reserve(midiLearnMappings.size());
            for (auto& mapping : midiLearnMappings)
            {
                if (mapping.controller != controller)
                    continue;
                if (mapping.channel > 0 && mapping.channel != channel)
                    continue;
                if (mapping.sourceIdentifier.isNotEmpty()
                    && mapping.sourceIdentifier != sourceIdentifier)
                {
                    continue;
                }

                const bool risingEdge = value >= 64 && mapping.lastValue < 64;
                mapping.lastValue = value;
                pending.push_back({ mapping, risingEdge });
            }
        }

        if (pending.empty())
            return false;

        if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
            messageManager != nullptr && !messageManager->isThisTheMessageThread())
        {
            auto pendingCopy = pending;
            juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this),
                                             pendingCopy,
                                             value]
            {
                if (safeThis == nullptr)
                    return;
                for (const auto& entry : pendingCopy)
                    safeThis->applyMidiLearnTargetValue(entry.mapping, value, entry.risingEdge);
            });
            return true;
        }

        for (const auto& entry : pending)
            applyMidiLearnTargetValue(entry.mapping, value, entry.risingEdge);

        return true;
    }

    void MainComponent::applyMidiLearnTargetValue(const MidiLearnMapping& mapping, int value, bool risingEdge)
    {
        const float normalised = juce::jlimit(0.0f, 1.0f, static_cast<float>(value) / 127.0f);
        int trackIndex = mapping.trackIndex;
        if (isTrackScopedMidiLearnTarget(mapping.target))
        {
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                trackIndex = selectedTrackIndexRt.load(std::memory_order_relaxed);
            if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                return;
        }

        switch (mapping.target)
        {
            case MidiLearnTarget::TrackVolume:
                tracks[trackIndex]->setVolume(juce::jlimit(0.0f, 1.2f, normalised * 1.2f));
                break;
            case MidiLearnTarget::TrackPan:
                tracks[trackIndex]->setPan((normalised * 2.0f) - 1.0f);
                break;
            case MidiLearnTarget::TrackSend:
                tracks[trackIndex]->setSendLevel(normalised);
                break;
            case MidiLearnTarget::TrackMute:
                if (risingEdge)
                    tracks[trackIndex]->setMute(!tracks[trackIndex]->isMuted());
                break;
            case MidiLearnTarget::TrackSolo:
                if (risingEdge)
                    tracks[trackIndex]->setSolo(!tracks[trackIndex]->isSolo());
                break;
            case MidiLearnTarget::TrackArm:
                if (risingEdge)
                    tracks[trackIndex]->setArm(!tracks[trackIndex]->isArmed());
                break;
            case MidiLearnTarget::TrackMonitor:
                if (risingEdge)
                    requestTrackInputMonitoringState(trackIndex,
                                                     !tracks[trackIndex]->isInputMonitoringEnabled(),
                                                     "MIDI learn monitor toggle");
                break;
            case MidiLearnTarget::MasterOutput:
            {
                const float gain = juce::jlimit(0.0f, 1.4f, normalised * 1.4f);
                masterOutputGain = gain;
                masterOutputGainRt.store(gain, std::memory_order_relaxed);
                juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this), gain]
                {
                    if (safeThis != nullptr)
                        safeThis->masterOutSlider.setValue(gain, juce::dontSendNotification);
                });
                break;
            }
            case MidiLearnTarget::TransportPlay:
                if (risingEdge)
                {
                    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                    {
                        if (safeThis != nullptr)
                            safeThis->playButton.triggerClick();
                    });
                }
                break;
            case MidiLearnTarget::TransportStop:
                if (risingEdge)
                {
                    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                    {
                        if (safeThis != nullptr)
                            safeThis->stopButton.triggerClick();
                    });
                }
                break;
            case MidiLearnTarget::TransportRecord:
                if (risingEdge)
                {
                    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                    {
                        if (safeThis != nullptr)
                            safeThis->recordButton.triggerClick();
                    });
                }
                break;
            default:
                break;
        }
    }

    void MainComponent::handleControlSurfaceMidi(const juce::MidiMessage& message)
    {
        if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
            messageManager != nullptr && !messageManager->isThisTheMessageThread())
        {
            auto messageCopy = message;
            juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this),
                                             messageCopy]
            {
                if (safeThis != nullptr)
                    safeThis->handleControlSurfaceMidi(messageCopy);
            });
            return;
        }

        const int selectedTrack = selectedTrackIndexRt.load(std::memory_order_relaxed);
        if (!juce::isPositiveAndBelow(selectedTrack, tracks.size()))
            return;

        auto* track = tracks[selectedTrack];
        if (track == nullptr)
            return;

        if (message.isController())
        {
            const int cc = juce::jlimit(0, 127, message.getControllerNumber());
            const int value = juce::jlimit(0, 127, message.getControllerValue());
            const bool risingEdge = value >= 64 && controlSurfaceLastCcValue[static_cast<size_t>(cc)] < 64;
            controlSurfaceLastCcValue[static_cast<size_t>(cc)] = value;
            const float normalised = static_cast<float>(value) / 127.0f;

            switch (cc)
            {
                case 7:  track->setVolume(juce::jlimit(0.0f, 1.2f, normalised * 1.2f)); break;
                case 10: track->setPan((normalised * 2.0f) - 1.0f); break;
                case 21: track->setSendLevel(normalised); break;
                case 22:
                    if (risingEdge)
                    {
                        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                        {
                            if (safeThis == nullptr || safeThis->tracks.isEmpty())
                                return;
                            const int nextTrack = juce::jmax(0, safeThis->selectedTrackIndex - 1);
                            safeThis->setSelectedTrackIndex(nextTrack);
                            safeThis->timeline.selectTrack(nextTrack);
                            safeThis->mixer.selectTrack(nextTrack);
                        });
                    }
                    break;
                case 23:
                    if (risingEdge)
                    {
                        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                        {
                            if (safeThis == nullptr || safeThis->tracks.isEmpty())
                                return;
                            const int nextTrack = juce::jmin(safeThis->tracks.size() - 1, safeThis->selectedTrackIndex + 1);
                            safeThis->setSelectedTrackIndex(nextTrack);
                            safeThis->timeline.selectTrack(nextTrack);
                            safeThis->mixer.selectTrack(nextTrack);
                        });
                    }
                    break;
                case 24: if (risingEdge) track->setMute(!track->isMuted()); break;
                case 25: if (risingEdge) track->setSolo(!track->isSolo()); break;
                case 26: if (risingEdge) track->setArm(!track->isArmed()); break;
                case 27:
                    if (risingEdge)
                    {
                        requestTrackInputMonitoringState(selectedTrack,
                                                         !track->isInputMonitoringEnabled(),
                                                         "Control surface monitor toggle");
                    }
                    break;
                case 28:
                    if (risingEdge)
                    {
                        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                        {
                            if (safeThis != nullptr)
                                safeThis->playButton.triggerClick();
                        });
                    }
                    break;
                case 29:
                    if (risingEdge)
                    {
                        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                        {
                            if (safeThis != nullptr)
                                safeThis->stopButton.triggerClick();
                        });
                    }
                    break;
                case 30:
                    if (risingEdge)
                    {
                        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
                        {
                            if (safeThis != nullptr)
                                safeThis->recordButton.triggerClick();
                        });
                    }
                    break;
                default:
                    break;
            }
            return;
        }

        if (message.isNoteOn())
        {
            const int note = message.getNoteNumber();
            if (note == 94)
                juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)] { if (safeThis != nullptr) safeThis->playButton.triggerClick(); });
            else if (note == 93)
                juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)] { if (safeThis != nullptr) safeThis->stopButton.triggerClick(); });
            else if (note == 95)
                juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)] { if (safeThis != nullptr) safeThis->recordButton.triggerClick(); });
        }
    }

    void MainComponent::renameTrack(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto* prompt = new juce::AlertWindow("Rename Track",
                                             "Enter a new track name:",
                                             juce::AlertWindow::NoIcon);
        prompt->addTextEditor("name", tracks[trackIndex]->getTrackName(), "Name");
        prompt->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        prompt->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, trackIndex, prompt](int result)
            {
                std::unique_ptr<juce::AlertWindow> owner(prompt);
                if (result != 1)
                    return;
                if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
                    return;

                const auto newName = prompt->getTextEditorContents("name").trim();
                if (newName.isEmpty())
                    return;

                tracks[trackIndex]->setTrackName(newName);

                if (pluginEditorWindow != nullptr
                    && pluginEditorTrackIndex == trackIndex
                    && juce::isPositiveAndBelow(pluginEditorTrackIndex, tracks.size()))
                {
                    juce::String pluginName = tracks[pluginEditorTrackIndex]->getPluginNameForSlotNonBlocking(pluginEditorSlotIndex);
                    if (pluginName.isEmpty())
                        pluginName = "Plugin";
                    const juce::String slotLabel = (pluginEditorSlotIndex == Track::instrumentSlotIndex)
                        ? juce::String("Instrument")
                        : juce::String("Insert " + juce::String(pluginEditorSlotIndex + 1));
                    pluginEditorWindow->setName(tracks[pluginEditorTrackIndex]->getTrackName()
                                                + "  |  " + slotLabel
                                                + "  |  " + pluginName);
                }

                if (channelRackWindow != nullptr
                    && channelRackTrackIndex == trackIndex
                    && juce::isPositiveAndBelow(channelRackTrackIndex, tracks.size()))
                {
                    channelRackWindow->setName("Channel Rack - " + tracks[channelRackTrackIndex]->getTrackName());
                }

                if (eqWindow != nullptr
                    && eqTrackIndex == trackIndex
                    && juce::isPositiveAndBelow(eqTrackIndex, tracks.size()))
                {
                    eqWindow->setName("Track EQ - " + tracks[eqTrackIndex]->getTrackName());
                }

                refreshChannelRackWindow();
                refreshStatusText();
                repaint();
            }));
    }

    void MainComponent::duplicateTrack(int sourceTrackIndex)
    {
        if (!juce::isPositiveAndBelow(sourceTrackIndex, tracks.size()))
            return;

        if (tracks.size() >= maxRealtimeTracks)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Track Limit",
                                                   "Maximum realtime track count reached.");
            return;
        }

        auto* sourceTrack = tracks[sourceTrackIndex];
        auto* clonedTrack = new Track(sourceTrack->getTrackName() + " Copy", formatManager);
        clonedTrack->setTransportPlayHead(&transport);
        clonedTrack->setVolume(sourceTrack->getVolume());
        clonedTrack->setPan(sourceTrack->getPan());
        clonedTrack->setSendLevel(sourceTrack->getSendLevel());
        clonedTrack->setSendTapMode(sourceTrack->getSendTapMode());
        clonedTrack->setSendTargetBus(sourceTrack->getSendTargetBus());
        clonedTrack->setMute(sourceTrack->isMuted());
        clonedTrack->setSolo(sourceTrack->isSolo());
        clonedTrack->setArm(false);
        clonedTrack->setInputMonitoring(false);
        clonedTrack->setInputSourcePair(sourceTrack->getInputSourcePair());
        clonedTrack->setMonitorTapMode(sourceTrack->getMonitorTapMode());
        clonedTrack->setInputMonitorGain(sourceTrack->getInputMonitorGain());
        clonedTrack->setChannelType(sourceTrack->getChannelType());
        clonedTrack->setOutputTargetType(sourceTrack->getOutputTargetType());
        clonedTrack->setOutputTargetBus(sourceTrack->getOutputTargetBus());
        clonedTrack->setEqEnabled(sourceTrack->isEqEnabled());
        clonedTrack->setEqBandGains(sourceTrack->getEqLowGainDb(),
                                    sourceTrack->getEqMidGainDb(),
                                    sourceTrack->getEqHighGainDb());
        clonedTrack->setBuiltInEffectsMask(sourceTrack->getBuiltInEffectsMask());

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        const double sampleRate = setup.sampleRate > 0.0 ? setup.sampleRate : sampleRateRt.load(std::memory_order_relaxed);
        const int blockSize = setup.bufferSize > 0 ? setup.bufferSize : 512;
        clonedTrack->prepareToPlay(sampleRate > 0.0 ? sampleRate : 44100.0, juce::jmax(128, blockSize));

        juce::String firstLoadError;
        juce::PluginDescription instrumentDescription;
        if (sourceTrack->getPluginDescriptionForSlot(Track::instrumentSlotIndex, instrumentDescription))
        {
            juce::String error;
            if (!clonedTrack->loadInstrumentPlugin(instrumentDescription, error))
            {
                if (firstLoadError.isEmpty() && error.isNotEmpty())
                    firstLoadError = error;
            }
            else
            {
                const auto instrumentState = sourceTrack->getPluginStateForSlot(Track::instrumentSlotIndex);
                if (instrumentState.isNotEmpty()
                    && !clonedTrack->setPluginStateForSlot(Track::instrumentSlotIndex, instrumentState)
                    && firstLoadError.isEmpty())
                {
                    firstLoadError = "Instrument state restore failed for duplicated track.";
                }
                clonedTrack->setPluginSlotBypassed(Track::instrumentSlotIndex,
                                                   sourceTrack->isPluginSlotBypassed(Track::instrumentSlotIndex));
                recordLastLoadedPlugin(instrumentDescription);
            }
        }
        else
        {
            const auto mode = sourceTrack->getBuiltInInstrumentMode();
            if (mode == Track::BuiltInInstrument::None)
            {
                clonedTrack->disableBuiltInInstrument();
            }
            else if (mode == Track::BuiltInInstrument::Sampler)
            {
                const auto samplePath = sourceTrack->getSamplerSamplePath();
                if (samplePath.isNotEmpty())
                {
                    juce::String error;
                    if (!clonedTrack->loadSamplerSoundFromFile(juce::File(samplePath), error))
                    {
                        if (firstLoadError.isEmpty() && error.isNotEmpty())
                            firstLoadError = error;
                    }
                }
                else
                {
                    clonedTrack->useBuiltInSynthInstrument();
                }
            }
            else
            {
                clonedTrack->useBuiltInSynthInstrument();
            }
        }

        for (int slot = 0; slot < sourceTrack->getPluginSlotCount(); ++slot)
        {
            juce::PluginDescription description;
            if (!sourceTrack->getPluginDescriptionForSlot(slot, description))
                continue;

            juce::String error;
            if (!clonedTrack->loadPluginInSlot(slot, description, error))
            {
                if (firstLoadError.isEmpty() && error.isNotEmpty())
                    firstLoadError = error;
                continue;
            }

            const auto state = sourceTrack->getPluginStateForSlot(slot);
            if (state.isNotEmpty()
                && !clonedTrack->setPluginStateForSlot(slot, state)
                && firstLoadError.isEmpty())
            {
                firstLoadError = "Insert state restore failed for duplicated track (slot "
                               + juce::String(slot + 1) + ").";
            }
            clonedTrack->setPluginSlotBypassed(slot, sourceTrack->isPluginSlotBypassed(slot));
            recordLastLoadedPlugin(description);
        }

        const int insertTrackIndex = sourceTrackIndex + 1;
        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            tracks.insert(insertTrackIndex, clonedTrack);
        }
        trackMidiBuffers[static_cast<size_t>(insertTrackIndex)].ensureSize(4096);

        if (pluginEditorTrackIndex >= insertTrackIndex)
            ++pluginEditorTrackIndex;
        if (channelRackTrackIndex >= insertTrackIndex)
            ++channelRackTrackIndex;
        if (eqTrackIndex >= insertTrackIndex)
            ++eqTrackIndex;

        applyArrangementEdit("Duplicate Track",
                             [sourceTrackIndex, insertTrackIndex](std::vector<Clip>& state, int& selected)
                             {
                                 std::vector<Clip> duplicatedClips;
                                 duplicatedClips.reserve(state.size());

                                 for (auto& clip : state)
                                 {
                                     if (clip.trackIndex >= insertTrackIndex)
                                         ++clip.trackIndex;
                                     if (clip.trackIndex == sourceTrackIndex)
                                     {
                                         Clip duplicate = clip;
                                         duplicate.trackIndex = insertTrackIndex;
                                         duplicate.name = clip.name + " Copy";
                                         duplicatedClips.push_back(std::move(duplicate));
                                     }
                                 }

                                 for (auto& duplicate : duplicatedClips)
                                     state.push_back(std::move(duplicate));

                                 juce::ignoreUnused(selected);
                             });

        std::vector<AutomationLane> duplicatedLanes;
        duplicatedLanes.reserve(automationLanes.size());
        for (auto& lane : automationLanes)
        {
            if (lane.trackIndex >= insertTrackIndex)
                ++lane.trackIndex;

            if (lane.trackIndex == sourceTrackIndex)
            {
                AutomationLane duplicate = lane;
                duplicate.trackIndex = insertTrackIndex;
                duplicate.laneId = nextAutomationLaneId++;
                duplicatedLanes.push_back(std::move(duplicate));
            }
        }
        for (auto& lane : duplicatedLanes)
            automationLanes.push_back(std::move(lane));

        mixer.rebuildFromTracks(tracks);
        timeline.refreshHeaders();

        setSelectedTrackIndex(insertTrackIndex);
        timeline.selectTrack(insertTrackIndex);
        mixer.selectTrack(insertTrackIndex);
        refreshChannelRackWindow();
        rebuildRealtimeSnapshot();
        resized();

        if (firstLoadError.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Track Duplicate",
                                                   "Track duplicated with partial plugin restore:\n" + firstLoadError);
        }
    }

    void MainComponent::deleteTrack(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        if (tracks.size() <= 1)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Delete Track",
                                                   "The project must keep at least one track.");
            return;
        }

        panicAllNotes();

        if (pluginEditorTrackIndex == trackIndex)
            closePluginEditorWindow();
        else if (pluginEditorTrackIndex > trackIndex)
            --pluginEditorTrackIndex;

        if (channelRackTrackIndex == trackIndex)
            closeChannelRackWindow();
        else if (channelRackTrackIndex > trackIndex)
            --channelRackTrackIndex;

        if (eqTrackIndex == trackIndex)
            closeEqWindow();
        else if (eqTrackIndex > trackIndex)
            --eqTrackIndex;

        {
            const juce::ScopedLock audioLock(deviceManager.getAudioCallbackLock());
            tracks.remove(trackIndex);
            rebuildRealtimeSnapshot();
        }

        applyArrangementEdit("Delete Track",
                             [trackIndex](std::vector<Clip>& state, int& selected)
                             {
                                 int removedBeforeSelected = 0;
                                 bool selectedClipRemoved = false;
                                 for (int i = 0; i < static_cast<int>(state.size()); ++i)
                                 {
                                     if (state[static_cast<size_t>(i)].trackIndex != trackIndex)
                                         continue;
                                     if (i < selected)
                                         ++removedBeforeSelected;
                                     if (i == selected)
                                         selectedClipRemoved = true;
                                 }

                                 state.erase(std::remove_if(state.begin(), state.end(),
                                                            [trackIndex](const Clip& clip)
                                                            {
                                                                return clip.trackIndex == trackIndex;
                                                            }),
                                             state.end());

                                 for (auto& clip : state)
                                 {
                                     if (clip.trackIndex > trackIndex)
                                         --clip.trackIndex;
                                 }

                                 if (selectedClipRemoved)
                                 {
                                     selected = -1;
                                 }
                                 else if (selected >= 0)
                                 {
                                     selected = juce::jmax(0, selected - removedBeforeSelected);
                                     if (selected >= static_cast<int>(state.size()))
                                         selected = state.empty() ? -1 : static_cast<int>(state.size()) - 1;
                                 }
                             });

        automationLanes.erase(std::remove_if(automationLanes.begin(),
                                             automationLanes.end(),
                                             [trackIndex](const AutomationLane& lane)
                                             {
                                                 return lane.trackIndex == trackIndex;
                                             }),
                              automationLanes.end());
        for (auto& lane : automationLanes)
        {
            if (lane.trackIndex > trackIndex)
                --lane.trackIndex;
        }

        mixer.rebuildFromTracks(tracks);
        timeline.refreshHeaders();

        const int newSelectedTrack = juce::jlimit(0,
                                                  juce::jmax(0, tracks.size() - 1),
                                                  selectedTrackIndex > trackIndex ? (selectedTrackIndex - 1)
                                                                                  : selectedTrackIndex);
        setSelectedTrackIndex(newSelectedTrack);
        timeline.selectTrack(newSelectedTrack);
        mixer.selectTrack(newSelectedTrack);
        refreshChannelRackWindow();
        rebuildRealtimeSnapshot();
        resized();
        refreshStatusText();
    }

    void MainComponent::panicAllNotes()
    {
        chordEngine.panic(midiScheduler);
        midiScheduler.reset();
        panicRequestedRt.store(true, std::memory_order_relaxed);
        {
            juce::SpinLock::ScopedLockType lock(previewMidiBuffersLock);
            for (auto& midiBuffer : previewMidiBuffers)
                midiBuffer.clear();
        }
        for (auto* track : tracks)
            track->panic();
    }

    void MainComponent::moveTrackPluginSlot(int trackIndex, int fromSlot, int toSlot)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto* track = tracks[trackIndex];
        if (track == nullptr || !track->movePluginSlot(fromSlot, toSlot))
            return;

        if (pluginEditorTrackIndex == trackIndex)
        {
            if (pluginEditorSlotIndex == fromSlot)
            {
                pluginEditorSlotIndex = toSlot;
            }
            else if (fromSlot < toSlot)
            {
                if (pluginEditorSlotIndex > fromSlot && pluginEditorSlotIndex <= toSlot)
                    --pluginEditorSlotIndex;
            }
            else
            {
                if (pluginEditorSlotIndex >= toSlot && pluginEditorSlotIndex < fromSlot)
                    ++pluginEditorSlotIndex;
            }

            if (pluginEditorWindow != nullptr
                && track->hasPluginInSlotNonBlocking(pluginEditorSlotIndex))
            {
                juce::String pluginName = track->getPluginNameForSlotNonBlocking(pluginEditorSlotIndex);
                if (pluginName.isEmpty())
                    pluginName = "Plugin";
                const juce::String slotLabel = (pluginEditorSlotIndex == Track::instrumentSlotIndex)
                    ? juce::String("Instrument")
                    : juce::String("Insert " + juce::String(pluginEditorSlotIndex + 1));
                const bool bypassed = track->isPluginSlotBypassedNonBlocking(pluginEditorSlotIndex);
                pluginEditorWindow->setName(track->getTrackName()
                                            + "  |  " + slotLabel
                                            + "  |  " + pluginName
                                            + (bypassed ? "  |  Off" : "  |  On"));
            }
        }

        refreshChannelRackWindow();
        refreshStatusText();
    }

    void MainComponent::openPluginEditorWindowForTrack(int trackIndex, int slotIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        auto* track = tracks[trackIndex];
        const int slotCount = track->getPluginSlotCount();
        int resolvedSlot = slotIndex;
        if (resolvedSlot != Track::instrumentSlotIndex)
            resolvedSlot = juce::jlimit(0, juce::jmax(0, slotCount - 1), resolvedSlot);

        if (resolvedSlot == Track::instrumentSlotIndex && !track->hasInstrumentPlugin())
        {
            const int firstLoadedInsert = track->getFirstLoadedPluginSlot();
            if (firstLoadedInsert != Track::instrumentSlotIndex && track->hasPluginInSlotNonBlocking(firstLoadedInsert))
            {
                resolvedSlot = firstLoadedInsert;
            }
            else
            {
                const auto builtInMode = track->getBuiltInInstrumentMode();
                if (builtInMode == Track::BuiltInInstrument::BasicSynth
                    || (builtInMode == Track::BuiltInInstrument::Sampler && track->hasSamplerSoundLoaded()))
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                           "Built-in Instrument",
                                                           "The built-in instrument currently has no dedicated editor UI.\n"
                                                           "Use track controls and load a third-party instrument for full plugin controls.");
                }
                return;
            }
        }
        else if (resolvedSlot != Track::instrumentSlotIndex && !track->hasPluginInSlotNonBlocking(resolvedSlot))
        {
            const int firstLoadedInsert = track->getFirstLoadedPluginSlot();
            if (firstLoadedInsert == Track::instrumentSlotIndex)
            {
                if (!track->hasInstrumentPlugin())
                    return;
                resolvedSlot = Track::instrumentSlotIndex;
            }
            else if (track->hasPluginInSlotNonBlocking(firstLoadedInsert))
            {
                resolvedSlot = firstLoadedInsert;
            }
            else
            {
                return;
            }
        }

        if (pluginEditorWindow != nullptr
            && pluginEditorTrackIndex == trackIndex
            && pluginEditorSlotIndex == resolvedSlot)
        {
            pluginEditorWindow->setVisible(true);
            pluginEditorWindow->toFront(true);
            return;
        }

        closePluginEditorWindow();

        auto* editor = track->createPluginEditorForSlot(resolvedSlot);
        if (editor == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "Plugin UI",
                                                   "This plugin does not provide an editor window.");
            return;
        }

        juce::String pluginName = track->getPluginNameForSlotNonBlocking(resolvedSlot);
        if (pluginName.isEmpty())
            pluginName = "Plugin";

        const juce::String slotLabel = (resolvedSlot == Track::instrumentSlotIndex)
            ? juce::String("Instrument")
            : juce::String("Insert " + juce::String(resolvedSlot + 1));
        const bool bypassed = track->isPluginSlotBypassedNonBlocking(resolvedSlot);
        const juce::String title = track->getTrackName() + "  |  " + slotLabel
                                 + "  |  " + pluginName
                                 + (bypassed ? "  |  Off" : "  |  On");
        const auto creationToken = ++pluginEditorWindowToken;
        pluginEditorWindow = std::make_unique<FloatingPluginWindow>(
            title,
            editor,
            [this, creationToken](FloatingPluginWindow* closingWindow)
            {
                juce::MessageManager::callAsync([this, closingWindow, creationToken]()
                {
                    if (pluginEditorWindow.get() == closingWindow && creationToken == pluginEditorWindowToken)
                    {
                        pluginEditorWindow.reset();
                        pluginEditorTrackIndex = -1;
                        pluginEditorSlotIndex = Track::instrumentSlotIndex;
                        ++pluginEditorWindowToken;
                    }
                });
            });
        pluginEditorTrackIndex = trackIndex;
        pluginEditorSlotIndex = resolvedSlot;
    }

    void MainComponent::closePluginEditorWindow()
    {
        if (pluginEditorWindow != nullptr)
        {
            pluginEditorWindow->setVisible(false);
            pluginEditorWindow->clearContentComponent();
        }
        pluginEditorWindow.reset();
        pluginEditorTrackIndex = -1;
        pluginEditorSlotIndex = Track::instrumentSlotIndex;
        ++pluginEditorWindowToken;
    }

    void MainComponent::openEqWindowForTrack(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        eqTrackIndex = trackIndex;

        if (eqWindow == nullptr)
        {
            auto* content = new TrackEqContent();
            const auto creationToken = ++eqWindowToken;
            eqWindow = std::make_unique<FloatingEqWindow>(
                "Track EQ",
                content,
                [this, creationToken](FloatingEqWindow* closingWindow)
                {
                    juce::MessageManager::callAsync([this, closingWindow, creationToken]()
                    {
                        if (eqWindow.get() == closingWindow && creationToken == eqWindowToken)
                        {
                            eqWindow.reset();
                            eqTrackIndex = -1;
                            ++eqWindowToken;
                        }
                    });
                });
        }

        if (eqWindow != nullptr)
        {
            eqWindow->setName("Track EQ - " + tracks[trackIndex]->getTrackName());
            if (auto* content = dynamic_cast<TrackEqContent*>(eqWindow->getContentComponent()))
                content->setTrack(tracks[trackIndex], trackIndex);
            eqWindow->setVisible(true);
            eqWindow->toFront(false);
        }
    }

    void MainComponent::closeEqWindow()
    {
        eqWindow.reset();
        eqTrackIndex = -1;
        ++eqWindowToken;
    }

    void MainComponent::openChannelRackForTrack(int trackIndex)
    {
        if (!juce::isPositiveAndBelow(trackIndex, tracks.size()))
            return;

        channelRackTrackIndex = trackIndex;

        if (channelRackWindow == nullptr)
        {
            auto* content = new ChannelRackContent();
            content->onLoadSlot = [this](int targetTrackIndex, int slotIndex)
            {
                showPluginListMenu(targetTrackIndex, channelRackWindow.get(), slotIndex);
            };
            content->onOpenSlot = [this](int targetTrackIndex, int slotIndex)
            {
                openPluginEditorWindowForTrack(targetTrackIndex, slotIndex);
            };
            content->onBypassChanged = [this](int targetTrackIndex, int slotIndex, bool bypassed)
            {
                if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                    return;
                tracks[targetTrackIndex]->setPluginSlotBypassed(slotIndex, bypassed);
                refreshChannelRackWindow();
            };
            content->onClearSlot = [this](int targetTrackIndex, int slotIndex)
            {
                if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                    return;
                tracks[targetTrackIndex]->clearPluginSlot(slotIndex);
                if (pluginEditorTrackIndex == targetTrackIndex && pluginEditorSlotIndex == slotIndex)
                    closePluginEditorWindow();
                refreshChannelRackWindow();
            };
            content->onMoveSlot = [this](int targetTrackIndex, int fromSlot, int toSlot)
            {
                moveTrackPluginSlot(targetTrackIndex, fromSlot, toSlot);
            };

            channelRackWindow = std::make_unique<FloatingChannelRackWindow>(
                "Channel Rack",
                content,
                [this, creationToken = ++channelRackWindowToken](FloatingChannelRackWindow* closingWindow)
                {
                    juce::MessageManager::callAsync([this, closingWindow, creationToken]()
                    {
                        if (channelRackWindow.get() == closingWindow && creationToken == channelRackWindowToken)
                        {
                            channelRackWindow.reset();
                            channelRackTrackIndex = -1;
                            ++channelRackWindowToken;
                        }
                    });
                });
        }

        if (channelRackWindow != nullptr)
        {
            channelRackWindow->setName("Channel Rack - " + tracks[trackIndex]->getTrackName());
            channelRackWindow->setVisible(true);
            channelRackWindow->toFront(false);
        }
        refreshChannelRackWindow();
    }

    void MainComponent::closeChannelRackWindow()
    {
        channelRackWindow.reset();
        channelRackTrackIndex = -1;
        ++channelRackWindowToken;
    }

    void MainComponent::refreshChannelRackWindow()
    {
        auto updateContent = [this](ChannelRackContent* content, int trackIndexForView)
        {
            if (content == nullptr)
                return;

            if (!juce::isPositiveAndBelow(trackIndexForView, tracks.size()))
            {
                content->setTrack(nullptr, -1);
                return;
            }

            content->setTrack(tracks[trackIndexForView], trackIndexForView);
        };

        auto updateInspector = [this](TrackInspectorContent* content, int trackIndexForView)
        {
            if (content == nullptr)
                return;

            if (!juce::isPositiveAndBelow(trackIndexForView, tracks.size()))
            {
                content->setTrack(nullptr, -1);
                return;
            }

            content->setTrack(tracks[trackIndexForView], trackIndexForView);
        };

        auto updateEq = [this](TrackEqContent* content, int trackIndexForView)
        {
            if (content == nullptr)
                return;

            if (!juce::isPositiveAndBelow(trackIndexForView, tracks.size()))
            {
                content->setTrack(nullptr, -1);
                return;
            }

            content->setTrack(tracks[trackIndexForView], trackIndexForView);
        };

        if (auto* dockedContent = dynamic_cast<ChannelRackContent*>(dockedChannelRack))
            updateContent(dockedContent, selectedTrackIndex);
        if (auto* inspector = dynamic_cast<TrackInspectorContent*>(trackInspectorView))
            updateInspector(inspector, selectedTrackIndex);

        if (channelRackWindow != nullptr)
        {
            if (juce::isPositiveAndBelow(channelRackTrackIndex, tracks.size()))
                channelRackWindow->setName("Channel Rack - " + tracks[channelRackTrackIndex]->getTrackName());
            else
                channelRackWindow->setName("Channel Rack");
            auto* floatingContent = dynamic_cast<ChannelRackContent*>(channelRackWindow->getContentComponent());
            updateContent(floatingContent, channelRackTrackIndex);
        }

        if (eqWindow != nullptr)
        {
            if (juce::isPositiveAndBelow(eqTrackIndex, tracks.size()))
                eqWindow->setName("Track EQ - " + tracks[eqTrackIndex]->getTrackName());
            else
                eqWindow->setName("Track EQ");
            auto* eqContent = dynamic_cast<TrackEqContent*>(eqWindow->getContentComponent());
            updateEq(eqContent, eqTrackIndex);
        }

        if (pluginEditorWindow != nullptr
            && juce::isPositiveAndBelow(pluginEditorTrackIndex, tracks.size()))
        {
            auto* track = tracks[pluginEditorTrackIndex];
            if (track != nullptr && track->hasPluginInSlotNonBlocking(pluginEditorSlotIndex))
            {
                juce::String pluginName = track->getPluginNameForSlotNonBlocking(pluginEditorSlotIndex);
                if (pluginName.isEmpty())
                    pluginName = "Plugin";
                const juce::String slotLabel = (pluginEditorSlotIndex == Track::instrumentSlotIndex)
                    ? juce::String("Instrument")
                    : juce::String("Insert " + juce::String(pluginEditorSlotIndex + 1));
                const bool bypassed = track->isPluginSlotBypassedNonBlocking(pluginEditorSlotIndex);
                pluginEditorWindow->setName(track->getTrackName()
                                            + "  |  " + slotLabel
                                            + "  |  " + pluginName
                                            + (bypassed ? "  |  Off" : "  |  On"));
            }
        }
    }

    void MainComponent::showPluginListMenu(int trackIndex, juce::Component* targetComponent, int targetSlotIndex)
    {
        if (tracks.isEmpty())
            return;

        int targetTrackIndex = trackIndex;
        if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
            targetTrackIndex = selectedTrackIndex;
        if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
            targetTrackIndex = 0;

        setSelectedTrackIndex(targetTrackIndex);
        timeline.selectTrack(targetTrackIndex);
        mixer.selectTrack(targetTrackIndex);

        auto* targetTrack = tracks[targetTrackIndex];
        const int slotCount = targetTrack->getPluginSlotCount();
        int slotIndex = targetSlotIndex;
        if (slotIndex != Track::instrumentSlotIndex)
            slotIndex = juce::jlimit(0, juce::jmax(0, slotCount - 1), slotIndex);
        const bool targetingInstrument = slotIndex == Track::instrumentSlotIndex;

        auto allPluginTypes = knownPluginList.getTypes();
        juce::Array<juce::PluginDescription> pluginTypes;
        juce::Array<juce::PluginDescription> quarantinedPluginTypes;
        bool usingCompatibilityPluginList = false;
        pluginTypes.ensureStorageAllocated(allPluginTypes.size());
        quarantinedPluginTypes.ensureStorageAllocated(allPluginTypes.size());
        for (const auto& desc : allPluginTypes)
        {
            const bool matchesType = targetingInstrument ? desc.isInstrument : !desc.isInstrument;
            if (!matchesType)
                continue;

            if (isPluginQuarantined(desc))
                quarantinedPluginTypes.add(desc);
            else
                pluginTypes.add(desc);
        }

        if (pluginTypes.isEmpty() && quarantinedPluginTypes.isEmpty() && !allPluginTypes.isEmpty())
        {
            usingCompatibilityPluginList = true;
            for (const auto& desc : allPluginTypes)
            {
                if (isPluginQuarantined(desc))
                    quarantinedPluginTypes.add(desc);
                else
                    pluginTypes.add(desc);
            }
        }

        std::sort(quarantinedPluginTypes.begin(), quarantinedPluginTypes.end(),
                  [this](const juce::PluginDescription& a, const juce::PluginDescription& b)
                  {
                      const auto manufacturerA = a.manufacturerName.trim().isNotEmpty() ? a.manufacturerName.trim() : juce::String("Other");
                      const auto manufacturerB = b.manufacturerName.trim().isNotEmpty() ? b.manufacturerName.trim() : juce::String("Other");
                      const int byManufacturer = manufacturerA.compareIgnoreCase(manufacturerB);
                      if (byManufacturer != 0)
                          return byManufacturer < 0;

                      const auto byName = a.name.compareIgnoreCase(b.name);
                      if (byName != 0)
                          return byName < 0;
                      const int rankA = getPluginFormatRank(a.pluginFormatName);
                      const int rankB = getPluginFormatRank(b.pluginFormatName);
                      if (rankA != rankB)
                          return rankA < rankB;
                      return a.pluginFormatName.compareIgnoreCase(b.pluginFormatName) < 0;
                  });

        std::sort(pluginTypes.begin(), pluginTypes.end(),
                  [this](const juce::PluginDescription& a, const juce::PluginDescription& b)
                  {
                      const auto manufacturerA = a.manufacturerName.trim().isNotEmpty() ? a.manufacturerName.trim() : juce::String("Other");
                      const auto manufacturerB = b.manufacturerName.trim().isNotEmpty() ? b.manufacturerName.trim() : juce::String("Other");
                      const int byManufacturer = manufacturerA.compareIgnoreCase(manufacturerB);
                      if (byManufacturer != 0)
                          return byManufacturer < 0;

                      const auto byName = a.name.compareIgnoreCase(b.name);
                      if (byName != 0)
                          return byName < 0;
                      const int rankA = getPluginFormatRank(a.pluginFormatName);
                      const int rankB = getPluginFormatRank(b.pluginFormatName);
                      if (rankA != rankB)
                          return rankA < rankB;
                      return a.pluginFormatName.compareIgnoreCase(b.pluginFormatName) < 0;
                  });

        juce::PopupMenu menu;
        const bool currentSlotLoaded = targetTrack->hasPluginInSlotNonBlocking(slotIndex);
        const bool canOpenCurrentUi = targetingInstrument
            ? targetTrack->hasInstrumentPlugin()
            : currentSlotLoaded;
        const bool currentInsertBypassed = currentSlotLoaded && targetTrack->isPluginSlotBypassedNonBlocking(slotIndex);
        const juce::String slotLabel = targetingInstrument
            ? juce::String("Instrument")
            : juce::String("Insert " + juce::String(slotIndex + 1));

        menu.addItem(menuIdOpenCurrentPluginEditor, "Open " + slotLabel + " UI", canOpenCurrentUi);
        menu.addItem(menuIdClosePluginEditor, "Close Plugin UI", pluginEditorWindow != nullptr);
        menu.addItem(menuIdToggleBypassCurrentInsert,
                     (currentInsertBypassed ? "Unbypass " : "Bypass ") + slotLabel,
                     currentSlotLoaded,
                     currentInsertBypassed);
        menu.addItem(menuIdClearCurrentInsert, "Clear " + slotLabel, currentSlotLoaded);
        menu.addSeparator();
        menu.addItem(menuIdScanPlugins, "Rescan AU + VST3");

        juce::PopupMenu loadedInsertsMenu;
        if (targetTrack->hasInstrumentPlugin())
        {
            juce::String loadedName = targetTrack->getPluginNameForSlotNonBlocking(Track::instrumentSlotIndex);
            if (loadedName.isEmpty())
                loadedName = "Instrument";
            loadedInsertsMenu.addItem(menuIdOpenLoadedInstrument, "Instrument: " + loadedName);
        }
        for (int slot = 0; slot < slotCount; ++slot)
        {
            if (!targetTrack->hasPluginInSlotNonBlocking(slot))
                continue;
            juce::String loadedName = targetTrack->getPluginNameForSlotNonBlocking(slot);
            if (loadedName.isEmpty())
                loadedName = "Plugin";
            loadedInsertsMenu.addItem(menuIdOpenLoadedInsertBase + slot,
                                      "Insert " + juce::String(slot + 1) + ": " + loadedName);
        }
        if (loadedInsertsMenu.getNumItems() > 0)
            menu.addSubMenu("Open Loaded Plugins", loadedInsertsMenu);

        juce::PopupMenu targetInsertMenu;
        auto instrumentSlotName = targetTrack->getPluginNameForSlotNonBlocking(Track::instrumentSlotIndex);
        if (instrumentSlotName.isEmpty())
            instrumentSlotName = "None";
        targetInsertMenu.addItem(menuIdSwitchTargetInstrument,
                                 "Instrument (" + instrumentSlotName + ")",
                                 true,
                                 targetingInstrument);
        for (int slot = 0; slot < slotCount; ++slot)
        {
            const bool loaded = targetTrack->hasPluginInSlotNonBlocking(slot);
            juce::String slotName = loaded ? targetTrack->getPluginNameForSlotNonBlocking(slot) : juce::String("Empty");
            if (slotName.isEmpty())
                slotName = "Plugin";
            targetInsertMenu.addItem(menuIdSwitchTargetInsertBase + slot,
                                     "Insert " + juce::String(slot + 1) + " (" + slotName + ")",
                                     true,
                                     !targetingInstrument && slot == slotIndex);
        }
        menu.addSubMenu("Target Slot", targetInsertMenu);

        {
            juce::PopupMenu builtInInstrumentMenu;
            builtInInstrumentMenu.addItem(menuIdUseBuiltInSynth, "Use Built-in Synth");
            builtInInstrumentMenu.addItem(menuIdUseBuiltInSampler, "Use Built-in Sampler (Load One-Shot...)");
            menu.addSeparator();
            menu.addSubMenu(targetingInstrument ? "Built-in Instruments"
                                                : "Built-in Instruments (Instrument Slot)",
                            builtInInstrumentMenu);
        }

        {
            juce::PopupMenu builtInDspMenu;
            for (int i = 0; i < Track::builtInEffectCount; ++i)
            {
                const auto effect = static_cast<Track::BuiltInEffect>(i);
                builtInDspMenu.addItem(menuIdToggleBuiltInDspBase + i,
                                       Track::getBuiltInEffectDisplayName(effect),
                                       true,
                                       targetTrack->isBuiltInEffectEnabled(effect));
            }
            builtInDspMenu.addSeparator();
            builtInDspMenu.addItem(menuIdDisableBuiltInDsp,
                                   "Disable All Built-in DSP",
                                   targetTrack->getBuiltInEffectsMask() != 0u);
            menu.addSubMenu("Built-in DSP Essentials", builtInDspMenu);
        }

        if (quarantinedPluginTypes.size() > 0)
        {
            juce::PopupMenu quarantineMenu;
            for (int i = 0; i < quarantinedPluginTypes.size(); ++i)
            {
                const auto& desc = quarantinedPluginTypes.getReference(i);
                quarantineMenu.addItem(menuIdUnquarantinePluginBase + i,
                                       "Re-enable " + desc.name + " (" + desc.pluginFormatName + ")");
            }
            menu.addSubMenu("Quarantined Plugins", quarantineMenu);
        }

        if (pluginTypes.isEmpty())
        {
            menu.addSeparator();
            menu.addItem(999,
                         targetingInstrument ? "No instrument plugins available (or all are quarantined)."
                                             : "No effect plugins available (or all are quarantined).",
                         false,
                         false);
        }
        else
        {
            menu.addSeparator();
            if (usingCompatibilityPluginList)
            {
                menu.addItem(998,
                             "Showing all plugins (plugin type metadata was incomplete).",
                             false,
                             false);
            }

            juce::StringArray manufacturers;
            for (const auto& desc : pluginTypes)
            {
                const auto manufacturer = desc.manufacturerName.trim().isNotEmpty()
                                            ? desc.manufacturerName.trim()
                                            : juce::String("Other");
                if (!manufacturers.contains(manufacturer))
                    manufacturers.add(manufacturer);
            }
            manufacturers.sortNatural();

            for (const auto& manufacturer : manufacturers)
            {
                juce::PopupMenu manufacturerMenu;
                for (int i = 0; i < pluginTypes.size(); ++i)
                {
                    const auto& desc = pluginTypes.getReference(i);
                    const auto descManufacturer = desc.manufacturerName.trim().isNotEmpty()
                                                    ? desc.manufacturerName.trim()
                                                    : juce::String("Other");
                    if (!descManufacturer.equalsIgnoreCase(manufacturer))
                        continue;

                    const juce::String itemLabel = desc.name + " (" + desc.pluginFormatName + ")";
                    manufacturerMenu.addItem(menuIdPluginBase + i, itemLabel);
                }

                if (manufacturerMenu.getNumItems() > 0)
                    menu.addSubMenu(manufacturer, manufacturerMenu);
            }
        }

        juce::Component* menuTarget = targetComponent != nullptr ? targetComponent : static_cast<juce::Component*>(&showEditorButton);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(menuTarget),
                           [this,
                            pluginTypes,
                            quarantinedPluginTypes,
                            targetTrackIndex,
                            slotIndex,
                            slotCount,
                            targetingInstrument](int selectedMenuId)
                           {
                               if (selectedMenuId == 0)
                                   return;

                               if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                                   return;
                               auto* track = tracks[targetTrackIndex];
                               if (track == nullptr)
                                   return;

                               if (selectedMenuId == menuIdOpenCurrentPluginEditor)
                               {
                                   openPluginEditorWindowForTrack(targetTrackIndex, slotIndex);
                                   return;
                               }

                               if (selectedMenuId == menuIdClosePluginEditor)
                               {
                                   closePluginEditorWindow();
                                   return;
                               }

                               if (selectedMenuId == menuIdToggleBypassCurrentInsert)
                               {
                                   if (!track->hasPluginInSlotNonBlocking(slotIndex))
                                       return;

                                   track->setPluginSlotBypassed(slotIndex, !track->isPluginSlotBypassedNonBlocking(slotIndex));
                                   refreshChannelRackWindow();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedMenuId == menuIdClearCurrentInsert)
                               {
                                   if (!track->hasPluginInSlotNonBlocking(slotIndex))
                                       return;

                                   track->clearPluginSlot(slotIndex);
                                   if (pluginEditorTrackIndex == targetTrackIndex && pluginEditorSlotIndex == slotIndex)
                                       closePluginEditorWindow();
                                   refreshChannelRackWindow();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedMenuId == menuIdOpenLoadedInstrument)
                               {
                                   openPluginEditorWindowForTrack(targetTrackIndex, Track::instrumentSlotIndex);
                                   return;
                               }

                               if (selectedMenuId >= menuIdOpenLoadedInsertBase
                                   && selectedMenuId < menuIdSwitchTargetInsertBase)
                               {
                                   const int selectedSlot = selectedMenuId - menuIdOpenLoadedInsertBase;
                                   if (!juce::isPositiveAndBelow(selectedSlot, slotCount))
                                       return;
                                   openPluginEditorWindowForTrack(targetTrackIndex, selectedSlot);
                                   return;
                               }

                               if (selectedMenuId == menuIdSwitchTargetInstrument)
                               {
                                   showPluginListMenu(targetTrackIndex, nullptr, Track::instrumentSlotIndex);
                                   return;
                               }

                               if (selectedMenuId >= menuIdSwitchTargetInsertBase
                                   && selectedMenuId < menuIdPluginBase)
                               {
                                   const int selectedSlot = selectedMenuId - menuIdSwitchTargetInsertBase;
                                   if (!juce::isPositiveAndBelow(selectedSlot, slotCount))
                                       return;
                                   showPluginListMenu(targetTrackIndex, nullptr, selectedSlot);
                                   return;
                               }

                               if (selectedMenuId == menuIdUseBuiltInSynth)
                               {
                                   if (pluginEditorTrackIndex == targetTrackIndex && pluginEditorSlotIndex == Track::instrumentSlotIndex)
                                       closePluginEditorWindow();
                                   track->useBuiltInSynthInstrument();
                                   refreshChannelRackWindow();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedMenuId == menuIdUseBuiltInSampler)
                               {
                                   samplerFileChooser = std::make_unique<juce::FileChooser>(
                                       "Load Sampler One-Shot",
                                       juce::File{},
                                       "*.wav;*.aif;*.aiff;*.flac;*.mp3");
                                   const auto chooserFlags = juce::FileBrowserComponent::openMode
                                                           | juce::FileBrowserComponent::canSelectFiles;
                                   samplerFileChooser->launchAsync(chooserFlags,
                                                                   [this, targetTrackIndex](const juce::FileChooser& chooser)
                                                                   {
                                                                       const auto selectedFile = chooser.getResult();
                                                                       if (!selectedFile.existsAsFile())
                                                                           return;
                                                                       if (!juce::isPositiveAndBelow(targetTrackIndex, tracks.size()))
                                                                           return;

                                                                       juce::String error;
                                                                       tracks[targetTrackIndex]->loadSamplerSoundFromFile(selectedFile, error);
                                                                       if (error.isNotEmpty())
                                                                       {
                                                                           juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                                                  "Sampler Load Error",
                                                                                                                  error);
                                                                       }
                                                                       refreshChannelRackWindow();
                                                                       refreshStatusText();
                                                                   });
                                   return;
                               }

                               if (selectedMenuId >= menuIdToggleBuiltInDspBase
                                   && selectedMenuId < menuIdToggleBuiltInDspBase + Track::builtInEffectCount)
                               {
                                   const int effectIndex = selectedMenuId - menuIdToggleBuiltInDspBase;
                                   if (!juce::isPositiveAndBelow(effectIndex, Track::builtInEffectCount))
                                       return;
                                   const auto effect = static_cast<Track::BuiltInEffect>(effectIndex);
                                   track->setBuiltInEffectEnabled(effect, !track->isBuiltInEffectEnabled(effect));
                                   refreshChannelRackWindow();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedMenuId == menuIdDisableBuiltInDsp)
                               {
                                   track->setBuiltInEffectsMask(0u);
                                   refreshChannelRackWindow();
                                   refreshStatusText();
                                   return;
                               }

                               if (selectedMenuId >= menuIdUnquarantinePluginBase
                                   && selectedMenuId < menuIdUnquarantinePluginBase + quarantinedPluginTypes.size())
                               {
                                   const int quarantinedIndex = selectedMenuId - menuIdUnquarantinePluginBase;
                                   if (!juce::isPositiveAndBelow(quarantinedIndex, quarantinedPluginTypes.size()))
                                       return;
                                   unquarantinePlugin(quarantinedPluginTypes.getReference(quarantinedIndex));
                                   showPluginListMenu(targetTrackIndex, nullptr, slotIndex);
                                   return;
                               }

                               if (selectedMenuId == menuIdScanPlugins)
                               {
                                   beginPluginScan();
                                   return;
                               }

                               if (selectedMenuId < menuIdPluginBase)
                                   return;

                               const int pluginIndex = selectedMenuId - menuIdPluginBase;
                               if (!juce::isPositiveAndBelow(pluginIndex, pluginTypes.size()))
                                   return;

                               juce::String error;
                               closePluginEditorWindow();
                               const auto& selectedDescription = pluginTypes.getReference(pluginIndex);
                               const auto tryLoadDescription = [&](const juce::PluginDescription& candidate,
                                                                   juce::String& loadError)
                               {
                                   loadError.clear();
                                   if (!runPluginIsolationProbe(candidate, targetingInstrument, loadError))
                                   {
                                       const juce::String probeError = loadError.isNotEmpty()
                                           ? loadError
                                           : juce::String("Plugin failed isolation probe.");
                                       quarantinePlugin(candidate, probeError);
                                       loadError = probeError;
                                       return false;
                                   }

                                   const bool loaded = targetingInstrument
                                       ? track->loadInstrumentPlugin(candidate, loadError)
                                       : track->loadPluginInSlot(slotIndex, candidate, loadError);
                                   if (!loaded || loadError.isNotEmpty())
                                   {
                                       if (shouldQuarantinePluginLoadError(loadError))
                                           quarantinePlugin(candidate, loadError);
                                       return false;
                                   }

                                   return true;
                               };

                               juce::PluginDescription loadedDescription = selectedDescription;
                               bool loaded = tryLoadDescription(selectedDescription, error);

                               if (!loaded)
                               {
                                   juce::Array<juce::PluginDescription> alternatives;
                                   for (const auto& candidate : pluginTypes)
                                   {
                                       if (candidate.pluginFormatName.equalsIgnoreCase(selectedDescription.pluginFormatName))
                                           continue;
                                       if (!pluginDescriptionsShareIdentity(candidate, selectedDescription))
                                           continue;
                                       alternatives.add(candidate);
                                   }

                                   std::sort(alternatives.begin(), alternatives.end(),
                                             [this](const juce::PluginDescription& a,
                                                    const juce::PluginDescription& b)
                                             {
                                                 const int rankA = getPluginFormatRank(a.pluginFormatName);
                                                 const int rankB = getPluginFormatRank(b.pluginFormatName);
                                                 if (rankA != rankB)
                                                     return rankA < rankB;
                                                 return a.pluginFormatName.compareIgnoreCase(b.pluginFormatName) < 0;
                                             });

                                   for (const auto& candidate : alternatives)
                                   {
                                       if (tryLoadDescription(candidate, error))
                                       {
                                           loaded = true;
                                           loadedDescription = candidate;
                                           break;
                                       }
                                   }
                               }

                               if (!loaded)
                               {
                                   const juce::String composedError = error.isNotEmpty()
                                       ? error
                                       : juce::String("Unable to load plugin.");
                                   juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                          "Plugin Load Error",
                                                                          composedError);
                                   return;
                               }

                               recordLastLoadedPlugin(loadedDescription);
                               refreshChannelRackWindow();
                               openPluginEditorWindowForTrack(targetTrackIndex, slotIndex);

                               if (!loadedDescription.pluginFormatName.equalsIgnoreCase(selectedDescription.pluginFormatName))
                               {
                                   juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                                          "Plugin Format Fallback",
                                                                          "Loaded \"" + loadedDescription.name
                                                                              + "\" as "
                                                                              + loadedDescription.pluginFormatName
                                                                              + " because the selected "
                                                                              + selectedDescription.pluginFormatName
                                                                              + " variant failed to load.");
                               }
                           });
    }
    juce::StringArray MainComponent::getMenuBarNames()
    {
        return { "File", "View" };
    }

    juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String& name)
    {
        juce::PopupMenu menu;
        juce::ignoreUnused(name);

        if (index == 0)
        {
            const bool hasSelectedTrack = juce::isPositiveAndBelow(selectedTrackIndex, tracks.size());
            const bool hasFrozenSelectedTrack = hasSelectedTrack
                && tracks[selectedTrackIndex] != nullptr
                && tracks[selectedTrackIndex]->isFrozenPlaybackOnly();
            menu.addItem(menuIdFileOpenProject, "Open Project...");
            menu.addItem(menuIdFileSaveProject, "Save Project...");
            menu.addItem(menuIdFileSaveProjectAs, "Save Project As...");
            menu.addSeparator();
            menu.addItem(menuIdFileExportMix, "Export Mixdown...");
            menu.addItem(menuIdFileExportStems, "Export Stems...");
            menu.addSeparator();
            menu.addItem(menuIdFileFreezeTrack,
                         hasFrozenSelectedTrack ? "Update Freeze (Selected Track)" : "Freeze Selected Track",
                         hasSelectedTrack);
            menu.addItem(menuIdFileUnfreezeTrack, "Unfreeze Selected Track", hasFrozenSelectedTrack);
            menu.addItem(menuIdFileCommitTrack, "Commit Selected Track To Audio", hasSelectedTrack);
            menu.addItem(menuIdFileCancelRenderTask,
                         "Cancel Active Render",
                         backgroundRenderBusyRt.load(std::memory_order_relaxed));
            menu.addSeparator();
            menu.addItem(menuIdFileQuit, "Quit");
            return menu;
        }

        if (index == 1)
        {
            menu.addItem(menuIdViewProjectSettings, "Project Settings...");
            menu.addItem(menuIdViewAudioSettings, "Audio Settings...");
            menu.addItem(menuIdViewScanPlugins, "Scan Plugins");
            menu.addItem(menuIdViewAutoScanPlugins,
                         "Scan Plugins On Startup",
                         true,
                         autoScanPluginsOnStartup);
            menu.addSeparator();
            menu.addItem(menuIdViewHelp, "Help");
            return menu;
        }

        return {};
    }

    void MainComponent::menuItemSelected(int menuID, int)
    {
        switch (menuID)
        {
            case menuIdFileOpenProject:
                openProject();
                break;
            case menuIdFileSaveProject:
                saveProject();
                break;
            case menuIdFileSaveProjectAs:
                saveProjectAs();
                break;
            case menuIdFileExportMix:
                exportMixdown();
                break;
            case menuIdFileExportStems:
                exportStems();
                break;
            case menuIdFileFreezeTrack:
                freezeTrackToAudio(selectedTrackIndex);
                break;
            case menuIdFileUnfreezeTrack:
                unfreezeTrack(selectedTrackIndex);
                break;
            case menuIdFileCommitTrack:
                commitTrackToAudio(selectedTrackIndex);
                break;
            case menuIdFileCancelRenderTask:
                cancelActiveRenderTask();
                break;
            case menuIdFileQuit:
                requestApplicationClose();
                break;
            case menuIdViewAudioSettings:
                showAudioSettings();
                break;
            case menuIdViewProjectSettings:
                showProjectSettingsMenu();
                break;
            case menuIdViewScanPlugins:
                beginPluginScan();
                break;
            case menuIdViewAutoScanPlugins:
                autoScanPluginsOnStartup = !autoScanPluginsOnStartup;
                saveStartupPreferences();
                break;
            case menuIdViewHelp:
                showHelpGuide();
                break;
            default:
                break;
        }
    }
}
