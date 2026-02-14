#include <JuceHeader.h>
#include "MainComponent.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <cmath>
#include <cctype>


namespace
{
    static bool formatNameLooksLikeVST3(const juce::String& formatName)
    {
        return formatName.containsIgnoreCase("VST3");
    }

    static bool formatNameLooksLikeAudioUnit(const juce::String& formatName)
    {
        return formatName.containsIgnoreCase("AudioUnit")
            || formatName.containsIgnoreCase("AU");
    }

    static bool formatLooksLikeVST3(const juce::AudioPluginFormat& format)
    {
        return formatNameLooksLikeVST3(format.getName());
    }

    static bool formatLooksLikeAudioUnit(const juce::AudioPluginFormat& format)
    {
        return formatNameLooksLikeAudioUnit(format.getName());
    }

    static bool formatMatchesRequested(const juce::AudioPluginFormat& format, const juce::String& requestedFormat)
    {
        if (requestedFormat.isEmpty())
            return true;

        const auto formatName = format.getName().trim();
        const auto requested = requestedFormat.trim();
        if (formatName.equalsIgnoreCase(requested))
            return true;

        if (formatNameLooksLikeAudioUnit(formatName) && formatNameLooksLikeAudioUnit(requested))
            return true;
        if (formatNameLooksLikeVST3(formatName) && formatNameLooksLikeVST3(requested))
            return true;

        return false;
    }

    static void addSearchDirectoryIfPresent(juce::FileSearchPath& path, const juce::File& directory)
    {
        if (!directory.isDirectory())
            return;

        const auto canonical = directory.getFullPathName();
        for (int i = 0; i < path.getNumPaths(); ++i)
        {
            if (path[i].getFullPathName() == canonical)
                return;
        }

        path.add(canonical);
    }

    static juce::FileSearchPath buildAugmentedSearchPathForFormat(juce::AudioPluginFormat& format)
    {
        auto searchPath = format.getDefaultLocationsToSearch();

        const auto userHome = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        const juce::File systemAudioPlugins("/Library/Audio/Plug-Ins");
        const juce::File userAudioPlugins = userHome.getChildFile("Library")
                                                    .getChildFile("Audio")
                                                    .getChildFile("Plug-Ins");

        const auto currentExe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        const auto appContents = currentExe.getParentDirectory().getParentDirectory();
        const auto appPlugIns = appContents.getChildFile("PlugIns");
        const auto appResourcesPlugins = appContents.getChildFile("Resources").getChildFile("Plugins");

        if (formatLooksLikeVST3(format))
        {
            addSearchDirectoryIfPresent(searchPath, systemAudioPlugins.getChildFile("VST3"));
            addSearchDirectoryIfPresent(searchPath, userAudioPlugins.getChildFile("VST3"));
            addSearchDirectoryIfPresent(searchPath, appPlugIns.getChildFile("VST3"));
            addSearchDirectoryIfPresent(searchPath, appResourcesPlugins.getChildFile("VST3"));
        }
        else if (formatLooksLikeAudioUnit(format))
        {
            addSearchDirectoryIfPresent(searchPath, systemAudioPlugins.getChildFile("Components"));
            addSearchDirectoryIfPresent(searchPath, userAudioPlugins.getChildFile("Components"));
            addSearchDirectoryIfPresent(searchPath, appPlugIns.getChildFile("Components"));
            addSearchDirectoryIfPresent(searchPath, appResourcesPlugins.getChildFile("Components"));
        }

        return searchPath;
    }

    static int parsePluginUidArg(const juce::String& token) noexcept
    {
        const auto trimmed = token.trim();
        if (trimmed.isEmpty())
            return 0;

        if (trimmed.containsOnly("0123456789abcdefABCDEF"))
            return static_cast<int>(trimmed.getHexValue32());

        return trimmed.getIntValue();
    }

    static juce::String fourCCFromInt(int value)
    {
        juce::String code;
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            const auto c = static_cast<char>((static_cast<unsigned int>(value) >> static_cast<unsigned int>(shift)) & 0xffu);
            if (!std::isalnum(static_cast<unsigned char>(c)))
                return {};
            code << juce::String::charToString(static_cast<juce::juce_wchar>(c));
        }
        return code;
    }

#if JUCE_MAC
    static bool runAuValidationIfPossible(const juce::String& formatName,
                                          const juce::String& pluginIdentifier,
                                          const juce::String& manufacturer,
                                          int uniqueId,
                                          int deprecatedUid,
                                          bool instrumentPlugin,
                                          juce::String& outError)
    {
        outError.clear();
        if (!formatNameLooksLikeAudioUnit(formatName))
            return true;

        const juce::File auvalTool("/usr/bin/auval");
        if (!auvalTool.existsAsFile())
            return true;

        juce::String componentType = instrumentPlugin ? "aumu" : "aufx";
        juce::String componentSubType;
        juce::String componentManufacturer;

        juce::StringArray tokens;
        tokens.addTokens(pluginIdentifier, ":/|,; ", {});
        tokens.removeEmptyStrings();
        tokens.trim();
        for (const auto& token : tokens)
        {
            if (token.length() != 4)
                continue;
            if (!token.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"))
                continue;

            if (componentType.isEmpty())
                componentType = token;
            else if (componentSubType.isEmpty())
                componentSubType = token;
            else if (componentManufacturer.isEmpty())
                componentManufacturer = token;
        }

        if (componentSubType.isEmpty())
            componentSubType = fourCCFromInt(uniqueId);
        if (componentSubType.isEmpty())
            componentSubType = fourCCFromInt(deprecatedUid);

        if (componentManufacturer.isEmpty())
        {
            auto mfrCode = manufacturer.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
            mfrCode = mfrCode.substring(0, 4).paddedRight(' ', 4).substring(0, 4);
            if (mfrCode.length() == 4)
                componentManufacturer = mfrCode;
        }

        if (componentSubType.length() != 4 || componentManufacturer.length() != 4)
            return true;

        const juce::String command = auvalTool.getFullPathName().quoted()
                                   + " -v " + componentType.quoted()
                                   + " " + componentSubType.quoted()
                                   + " " + componentManufacturer.quoted();

        juce::ChildProcess process;
        if (!process.start(command) || !process.waitForProcessToFinish(15000))
        {
            process.kill();
            outError = "Audio Unit validation timed out.";
            return false;
        }

        const auto exitCode = process.getExitCode();
        const auto output = process.readAllProcessOutput();
        if (exitCode != 0)
        {
            outError = "Audio Unit validation failed."
                     + (output.isNotEmpty() ? ("\n" + output.trim()) : juce::String());
            return false;
        }

        return true;
    }
#endif

    static juce::String getCommandArgValue(const juce::StringArray& tokens, const juce::String& key)
    {
        const auto keyWithEquals = key + "=";
        for (int i = 0; i < tokens.size(); ++i)
        {
            auto token = tokens[i].trim();
            if (token.startsWithIgnoreCase(keyWithEquals))
                return token.fromFirstOccurrenceOf("=", false, false).trim().unquoted();
            if (token.equalsIgnoreCase(key) && i + 1 < tokens.size())
                return tokens[i + 1].trim().unquoted();
        }
        return {};
    }

    static int parseScanTimeoutMsArg(const juce::StringArray& tokens,
                                     const juce::String& key,
                                     int defaultValueMs) noexcept
    {
        const auto timeoutArg = getCommandArgValue(tokens, key);
        if (timeoutArg.isEmpty())
            return defaultValueMs;

        const auto parsed = timeoutArg.getIntValue();
        if (parsed <= 0)
            return defaultValueMs;

        return juce::jlimit(1000, 600000, parsed);
    }

    static juce::StringArray loadDeadMansPedalEntries(const juce::File& deadMansPedalFile)
    {
        juce::StringArray entries;
        if (!deadMansPedalFile.existsAsFile())
            return entries;

        entries.addLines(deadMansPedalFile.loadFileAsString());
        entries.trim();
        entries.removeEmptyStrings();
        return entries;
    }

    static juce::String getLikelyCurrentScanTarget(const juce::String& pluginName,
                                                   const juce::StringArray& beforeEntries,
                                                   const juce::StringArray& afterEntries)
    {
        if (pluginName.trim().isNotEmpty())
            return pluginName.trim();
        if (afterEntries.size() > 0)
            return afterEntries[afterEntries.size() - 1].trim();
        if (beforeEntries.size() > 0)
            return beforeEntries[beforeEntries.size() - 1].trim();
        return "<unknown plugin>";
    }

    static void appendDeadMansPedalEntryIfNeeded(const juce::File& deadMansPedalFile,
                                                  const juce::String& entry)
    {
        const auto trimmed = entry.trim();
        if (trimmed.isEmpty())
            return;

        auto existingEntries = loadDeadMansPedalEntries(deadMansPedalFile);
        if (existingEntries.contains(trimmed))
            return;

        juce::String content = deadMansPedalFile.existsAsFile()
            ? deadMansPedalFile.loadFileAsString()
            : juce::String();
        if (content.isNotEmpty() && !content.endsWithChar('\n'))
            content << "\n";
        content << trimmed << "\n";
        deadMansPedalFile.replaceWithText(content);
    }

    static int runPluginProbeMode(const juce::StringArray& tokens)
    {
        const auto formatName = getCommandArgValue(tokens, "--format");
        const auto pluginIdentifier = getCommandArgValue(tokens, "--id");
        const auto pluginName = getCommandArgValue(tokens, "--name");
        const auto manufacturer = getCommandArgValue(tokens, "--mfr");
        const auto uniqueIdArg = getCommandArgValue(tokens, "--uid2");
        const auto deprecatedUidArg = getCommandArgValue(tokens, "--uid");
        const auto sampleRateArg = getCommandArgValue(tokens, "--sr");
        const auto blockSizeArg = getCommandArgValue(tokens, "--bs");
        const auto instrumentArg = getCommandArgValue(tokens, "--instrument");

        const double sampleRate = juce::jmax(8000.0, sampleRateArg.getDoubleValue());
        const int blockSize = juce::jlimit(64, 8192, juce::jmax(64, blockSizeArg.getIntValue()));
        const bool instrumentPlugin = instrumentArg.getIntValue() != 0;

        if (formatName.isEmpty() || pluginIdentifier.isEmpty())
        {
            std::cout << "ERROR: Missing plugin probe arguments.\n";
            return 2;
        }

        juce::PluginDescription desc;
        desc.pluginFormatName = formatName;
        desc.fileOrIdentifier = pluginIdentifier;
        desc.name = pluginName.isNotEmpty() ? pluginName : pluginIdentifier;
        desc.manufacturerName = manufacturer;
        desc.uniqueId = parsePluginUidArg(uniqueIdArg);
        desc.deprecatedUid = parsePluginUidArg(deprecatedUidArg);

#if JUCE_MAC
        juce::String auValidationError;
        if (!runAuValidationIfPossible(formatName,
                                       pluginIdentifier,
                                       manufacturer,
                                       desc.uniqueId,
                                       desc.deprecatedUid,
                                       instrumentPlugin,
                                       auValidationError))
        {
            std::cout << "ERROR: " << auValidationError << "\n";
            return 2;
        }
#endif
        desc.isInstrument = instrumentPlugin;

        juce::AudioPluginFormatManager formatManager;
        formatManager.addDefaultFormats();

        juce::String error;
        std::unique_ptr<juce::AudioPluginInstance> instance(formatManager.createPluginInstance(desc,
                                                                                                sampleRate,
                                                                                                blockSize,
                                                                                                error));
        if (instance == nullptr)
        {
            std::cout << "ERROR: " << (error.isNotEmpty() ? error.toStdString()
                                                           : std::string("Failed to create plugin instance."))
                      << "\n";
            return 2;
        }

        try
        {
            auto makeSet = [](int channels)
            {
                if (channels <= 0)
                    return juce::AudioChannelSet::disabled();
                if (channels == 1)
                    return juce::AudioChannelSet::mono();
                if (channels == 2)
                    return juce::AudioChannelSet::stereo();
                return juce::AudioChannelSet::discreteChannels(channels);
            };

            const auto tryLayout = [&](int inChannels, int outChannels)
            {
                juce::AudioProcessor::BusesLayout layout;
                if (instance->getBusCount(true) > 0)
                    layout.inputBuses.add(makeSet(inChannels));
                if (instance->getBusCount(false) > 0)
                    layout.outputBuses.add(makeSet(outChannels));
                if (layout.inputBuses.isEmpty() && layout.outputBuses.isEmpty())
                    return false;
                return instance->checkBusesLayoutSupported(layout)
                    && instance->setBusesLayout(layout);
            };

            instance->enableAllBuses();
            instance->disableNonMainBuses();

            if (instrumentPlugin)
            {
                if (!tryLayout(0, 2) && !tryLayout(0, 1))
                {
                    std::cout << "ERROR: Plugin probe could not configure instrument bus layout.\n";
                    return 2;
                }
            }
            else
            {
                if (!tryLayout(2, 2) && !tryLayout(1, 1) && !tryLayout(2, 1))
                {
                    std::cout << "ERROR: Plugin probe could not configure effect bus layout.\n";
                    return 2;
                }
            }

            const int mainInChannels = juce::jmax(0, juce::jmin(2, instance->getMainBusNumInputChannels()));
            const int mainOutChannels = juce::jmax(1, juce::jmin(2, instance->getMainBusNumOutputChannels()));
            if (mainOutChannels <= 0)
            {
                std::cout << "ERROR: Plugin does not expose a usable output bus.\n";
                return 2;
            }

            const int testBlockSizes[] =
            {
                juce::jlimit(64, 2048, blockSize),
                juce::jlimit(64, 2048, juce::jmax(64, blockSize / 2)),
                juce::jlimit(64, 2048, juce::jmax(64, blockSize * 2))
            };

            for (const int testBlockSize : testBlockSizes)
            {
                const int inChannels = instrumentPlugin ? 0 : juce::jmax(1, mainInChannels);
                const int outChannels = juce::jmax(1, mainOutChannels);
                instance->setPlayConfigDetails(inChannels, outChannels, sampleRate, testBlockSize);
                instance->setRateAndBufferSizeDetails(sampleRate, testBlockSize);
                instance->prepareToPlay(sampleRate, testBlockSize);

                juce::AudioBuffer<float> audio(outChannels, testBlockSize);
                bool noteOnSent = false;

                for (int pass = 0; pass < 12; ++pass)
                {
                    audio.clear();
                    juce::MidiBuffer midi;
                    if (instrumentPlugin)
                    {
                        if (!noteOnSent)
                        {
                            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                            noteOnSent = true;
                        }
                        if (pass == 8)
                            midi.addEvent(juce::MidiMessage::noteOff(1, 60), juce::jmax(1, testBlockSize / 2));
                    }

                    instance->processBlock(audio, midi);

                    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                    {
                        const auto* read = audio.getReadPointer(ch);
                        if (read == nullptr)
                            continue;
                        for (int i = 0; i < audio.getNumSamples(); ++i)
                        {
                            const float v = read[i];
                            if (!std::isfinite(v))
                            {
                                std::cout << "ERROR: Plugin produced non-finite output.\n";
                                instance->releaseResources();
                                return 2;
                            }
                        }
                    }
                }

                instance->releaseResources();
            }
        }
        catch (const std::exception& e)
        {
            std::cout << "ERROR: Plugin probe exception: " << e.what() << "\n";
            return 2;
        }
        catch (...)
        {
            std::cout << "ERROR: Plugin probe crashed.\n";
            return 2;
        }

        std::cout << "OK: Plugin probe passed.\n";
        return 0;
    }

    static int runPluginScanPassMode(const juce::StringArray& tokens)
    {
        const auto knownListPath = getCommandArgValue(tokens, "--known");
        const auto deadMansPedalPath = getCommandArgValue(tokens, "--deadman");
        const auto requestedFormat = getCommandArgValue(tokens, "--plugin-scan-format");
        const int scanTimeoutMs = parseScanTimeoutMsArg(tokens,
                                                        "--plugin-scan-timeout-ms",
                                                        45000);

        if (knownListPath.isEmpty() || deadMansPedalPath.isEmpty())
        {
            std::cout << "ERROR: Missing plugin scan pass arguments.\n";
            return 2;
        }

        const juce::File knownListFile(knownListPath);
        const juce::File deadMansPedalFile(deadMansPedalPath);
        knownListFile.getParentDirectory().createDirectory();
        deadMansPedalFile.getParentDirectory().createDirectory();

        juce::KnownPluginList pluginList;
        if (knownListFile.existsAsFile())
        {
            if (std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(knownListFile)); xml != nullptr)
                pluginList.recreateFromXml(*xml);
        }

        juce::AudioPluginFormatManager formatManager;
        formatManager.addDefaultFormats();

        juce::StringArray failedFiles;
        juce::StringArray blacklistedEntries;
        int scannedFormatCount = 0;
        for (int formatIndex = 0; formatIndex < formatManager.getNumFormats(); ++formatIndex)
        {
            auto* format = formatManager.getFormat(formatIndex);
            if (format == nullptr)
                continue;
            if (!formatMatchesRequested(*format, requestedFormat))
                continue;

            const auto searchPath = buildAugmentedSearchPathForFormat(*format);
            if (searchPath.getNumPaths() <= 0)
                continue;

            ++scannedFormatCount;
            deadMansPedalFile.deleteFile();

            juce::PluginDirectoryScanner scanner(pluginList,
                                                 *format,
                                                 searchPath,
                                                 true,
                                                 deadMansPedalFile);

            const double formatStartMs = juce::Time::getMillisecondCounterHiRes();
            juce::String pluginName;
            while (true)
            {
                const auto beforeEntries = loadDeadMansPedalEntries(deadMansPedalFile);
                const double fileStartMs = juce::Time::getMillisecondCounterHiRes();
                const bool hasNext = scanner.scanNextFile(true, pluginName);
                const double nowMs = juce::Time::getMillisecondCounterHiRes();

                const auto afterEntries = loadDeadMansPedalEntries(deadMansPedalFile);
                const auto target = getLikelyCurrentScanTarget(pluginName, beforeEntries, afterEntries);
                const auto fileElapsedMs = static_cast<int>(std::llround(nowMs - fileStartMs));
                const auto formatElapsedMs = static_cast<int>(std::llround(nowMs - formatStartMs));

                if (fileElapsedMs > scanTimeoutMs)
                {
                    const auto timeoutLabel = target + " (timeout after "
                                            + juce::String(fileElapsedMs) + " ms)";
                    failedFiles.addIfNotAlreadyThere(timeoutLabel);
                    blacklistedEntries.addIfNotAlreadyThere(format->getName() + ": " + timeoutLabel);
                    appendDeadMansPedalEntryIfNeeded(deadMansPedalFile, target);
                }

                if (formatElapsedMs > scanTimeoutMs)
                {
                    const auto timeoutLabel = target + " (format timeout after "
                                            + juce::String(formatElapsedMs) + " ms)";
                    failedFiles.addIfNotAlreadyThere(timeoutLabel);
                    blacklistedEntries.addIfNotAlreadyThere(format->getName() + ": " + timeoutLabel);
                    appendDeadMansPedalEntryIfNeeded(deadMansPedalFile, target);
                    break;
                }

                if (!hasNext)
                    break;
            }

            for (const auto& failed : scanner.getFailedFiles())
                failedFiles.addIfNotAlreadyThere(failed);

            if (deadMansPedalFile.existsAsFile())
            {
                juce::StringArray deadmanLines;
                deadmanLines.addLines(deadMansPedalFile.loadFileAsString());
                for (auto line : deadmanLines)
                {
                    line = line.trim();
                    if (line.isNotEmpty())
                        blacklistedEntries.addIfNotAlreadyThere(format->getName() + ": " + line);
                }
            }

            juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal(pluginList, deadMansPedalFile);
            deadMansPedalFile.deleteFile();

            if (std::unique_ptr<juce::XmlElement> xml(pluginList.createXml()); xml != nullptr)
                xml->writeTo(knownListFile);
        }

        if (requestedFormat.isNotEmpty() && scannedFormatCount == 0)
        {
            std::cout << "ERROR: Requested plugin format not available: "
                      << requestedFormat.toStdString() << "\n";
            return 2;
        }

        if (std::unique_ptr<juce::XmlElement> xml(pluginList.createXml()); xml != nullptr)
            xml->writeTo(knownListFile);

        for (const auto& failed : failedFiles)
            std::cout << "FAILED: " << failed << "\n";
        for (const auto& blacklisted : blacklistedEntries)
            std::cout << "BLACKLISTED: " << blacklisted << "\n";

        std::cout << "OK: Plugin scan pass complete.\n";
        return 0;
    }
}

namespace sampledex
{
    class SampledexChordLabApplication final : public juce::JUCEApplication
    {
    public:
        SampledexChordLabApplication() = default;

		const juce::String getApplicationName() override       { return "Sampledex ChordLab"; }
		const juce::String getApplicationVersion() override
		{
		   #if defined (JUCE_APP_VERSION_STRING)
			return JUCE_APP_VERSION_STRING;
		   #elif defined (JUCE_APPLICATION_VERSION_STRING)
			return JUCE_APPLICATION_VERSION_STRING;
		   #elif defined (JUCE_PROJECT_VERSION)
			return JUCE_PROJECT_VERSION;
		   #else
			return "2.0.0";
		   #endif
		}
        bool moreThanOneInstanceAllowed() override             { return true; }

        void initialise (const juce::String& commandLine) override
        {
            const bool shiftSafeMode = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
            const bool argSafeMode = commandLine.containsIgnoreCase("--safe")
                                  || commandLine.containsIgnoreCase("--safe-mode");
            const bool startInSafeMode = shiftSafeMode || argSafeMode;
            mainWindow = std::make_unique<MainWindow> (getApplicationName(), startInSafeMode);
        }

        void shutdown() override
        {
            mainWindow.reset();
        }

        void systemRequestedQuit() override
        {
            if (mainWindow != nullptr)
            {
                if (auto* mainComponent = dynamic_cast<MainComponent*>(mainWindow->getContentComponent()))
                {
                    mainComponent->requestApplicationClose();
                    return;
                }
            }
            quit();
        }

        void anotherInstanceStarted (const juce::String&) override {}

    private:
        class MainWindow final : public juce::DocumentWindow
        {
        public:
            explicit MainWindow (juce::String name, bool startInSafeMode)
                : DocumentWindow (std::move (name),
                                  juce::Desktop::getInstance().getDefaultLookAndFeel()
                                      .findColour (juce::ResizableWindow::backgroundColourId),
                                  DocumentWindow::allButtons)
            {
                setUsingNativeTitleBar (true);
                setContentOwned (new MainComponent(startInSafeMode), true);
                centreWithSize (getWidth(), getHeight());
                setResizable (true, true);
                setResizeLimits (860, 620, 1800, 1200);
                setVisible (true);
            }

            void closeButtonPressed() override
            {
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }
        };

        std::unique_ptr<MainWindow> mainWindow;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampledexChordLabApplication)
    };
} // namespace sampledex

static juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new sampledex::SampledexChordLabApplication();
}

int main(int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add(juce::String(argv[i]));

    const auto hasArg = [&args](const juce::String& key)
    {
        const auto keyWithEquals = key + "=";
        for (auto token : args)
        {
            token = token.trim();
            if (token.equalsIgnoreCase(key) || token.startsWithIgnoreCase(keyWithEquals))
                return true;
        }
        return false;
    };

    if (hasArg("--plugin-scan-pass"))
        return runPluginScanPassMode(args);

    if (hasArg("--plugin-probe"))
        return runPluginProbeMode(args);

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main(argc, (const char**) argv);
}
