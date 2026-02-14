#include <JuceHeader.h>
#include <cstdint>
#include "SmfPipeline.h"

using namespace sampledex;

namespace
{
    juce::File writeFixtureToTemp(const juce::String& name, const std::vector<std::uint8_t>& bytes)
    {
        const auto fixtureFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sampledex_smf_fixture_" + name);
        fixtureFile.deleteFile();
        fixtureFile.appendData(bytes.data(), static_cast<int>(bytes.size()));
        return fixtureFile;
    }

    bool runFixture(const juce::String& fixtureName, const std::vector<std::uint8_t>& bytes)
    {
        const auto fixture = writeFixtureToTemp(fixtureName, bytes);

        SmfPipeline::ImportResult imported;
        if (!SmfPipeline::importSmfFile(fixture, SmfPipeline::ImportMode::PreserveSourceTracks, imported))
            return false;
        if (imported.clips.empty() || imported.tempoMap.empty())
            return false;

        juce::OwnedArray<Track> tracks;
        juce::AudioPluginFormatManager formatManager;
        formatManager.addDefaultFormats();
        for (int i = 0; i < 4; ++i)
            tracks.add(new Track("Track " + juce::String(i + 1), formatManager));

        std::vector<Clip> arrangement;
        for (auto& c : imported.clips)
        {
            c.clip.trackIndex = juce::jlimit(0, 3, c.sourceTrackIndex);
            arrangement.push_back(c.clip);
        }

        const auto out = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sampledex_smf_roundtrip_" + fixtureName);

        SmfPipeline::ExportSelection selection;
        selection.trackIndices = { 0, 1, 2, 3 };

        if (!SmfPipeline::exportSmfFile(out, arrangement, tracks, imported.tempoMap, imported.timeSignatureMap, selection))
            return false;

        SmfPipeline::ImportResult reImported;
        if (!SmfPipeline::importSmfFile(out, SmfPipeline::ImportMode::PreserveSourceTracks, reImported))
            return false;

        return !reImported.clips.empty() && !reImported.tempoMap.empty();
    }
}

int main()
{
    const std::vector<std::uint8_t> multiChannelNamed {
        0x4d, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, 0x01, 0xe0, 0x4d, 0x54,
        0x72, 0x6b, 0x00, 0x00, 0x00, 0x13, 0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20, 0x00, 0xff, 0x58,
        0x04, 0x04, 0x02, 0x18, 0x08, 0x00, 0xff, 0x2f, 0x00, 0x4d, 0x54, 0x72, 0x6b, 0x00, 0x00, 0x00,
        0x1f, 0x00, 0xff, 0x03, 0x05, 0x50, 0x69, 0x61, 0x6e, 0x6f, 0x00, 0x90, 0x3c, 0x64, 0x83, 0x60,
        0x80, 0x3c, 0x00, 0x00, 0x91, 0x40, 0x64, 0x83, 0x60, 0x81, 0x40, 0x00, 0x00, 0xff, 0x2f, 0x00
    };

    const std::vector<std::uint8_t> tempoSignatureMap {
        0x4d, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x02, 0x01, 0xe0, 0x4d, 0x54,
        0x72, 0x6b, 0x00, 0x00, 0x00, 0x23, 0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20, 0x00, 0xff, 0x58,
        0x04, 0x03, 0x02, 0x18, 0x08, 0x87, 0x40, 0xff, 0x51, 0x03, 0x06, 0x1a, 0x80, 0x00, 0xff, 0x58,
        0x04, 0x04, 0x02, 0x18, 0x08, 0x00, 0xff, 0x2f, 0x00, 0x4d, 0x54, 0x72, 0x6b, 0x00, 0x00, 0x00,
        0x15, 0x00, 0xff, 0x03, 0x04, 0x42, 0x61, 0x73, 0x73, 0x00, 0x92, 0x30, 0x64, 0x87, 0x40, 0x82,
        0x30, 0x00, 0x00, 0xff, 0x2f, 0x00
    };

    const bool okA = runFixture("multi_channel_named.mid", multiChannelNamed);
    const bool okB = runFixture("tempo_signature_map.mid", tempoSignatureMap);
    return (okA && okB) ? 0 : 1;
}
