#pragma once
#include <JuceHeader.h>
#include <unordered_map>

namespace sampledex
{
    class ScheduledMidiOutput;

    class ChordEngine final
    {
    public:
        enum class Mode
        {
            Chromatic,   // fixed chord qualities (maj, min, dom7, etc.)
            Diatonic     // build triads/7ths from a selected key/scale
        };

        enum class Scale
        {
            Major,
            NaturalMinor,
            Dorian,
            Mixolydian
        };

        enum class ChromaticQuality
        {
            Major,
            Minor,
            Dominant7,
            Major7,
            Minor7,
            Sus2,
            Sus4,
            Power5
        };

        enum class DiatonicStack
        {
            Triad,
            Seventh,
            Add9
        };

        enum class StrumDirection
        {
            Up,
            Down
        };

        struct Settings
        {
            Mode mode = Mode::Chromatic;

            // Diatonic mode
            int keyRootPc = 0; // 0=C .. 11=B
            Scale scale = Scale::Major;
            bool snapToScale = true;
            DiatonicStack diatonicStack = DiatonicStack::Seventh;

            // Chromatic mode
            ChromaticQuality chromaticQuality = ChromaticQuality::Major7;

            // Voicing
            int inversion = 0;      // 0 = root position, 1..2 rotations
            int spread = 0;         // 0..2 (widens voicing by octaves)
            int octaveShift = 0;    // -2..+2

            // Performance
            bool latch = false;     // note-off ignored; next chord replaces previous
            double strumMs = 0.0;   // per-note delay
            StrumDirection strumDirection = StrumDirection::Up;
            double humanizeMs = 0.0; // +/- random delay added per note
            int velocityHumanize = 0; // +/- random velocity

            bool passthroughCC = true; // forward CC/pitchbend/aftertouch by default
        };

        ChordEngine();
        ~ChordEngine();

        void setSettings (Settings s);
        Settings getSettings() const;

        // Process an incoming MIDI message. Emits transformed MIDI via scheduler.
        // Thread-safe for typical JUCE usage (no heap allocations on the MIDI thread).
        void processIncoming (const juce::MidiMessage& msg, ScheduledMidiOutput& out);

        // Safety: send note-offs for any active chord notes.
        void panic (ScheduledMidiOutput& out);

    private:
        struct VoiceKey
        {
            uint8_t channel = 1; // 1..16
            uint8_t note = 0;    // 0..127

            bool operator== (const VoiceKey& other) const noexcept
            {
                return channel == other.channel && note == other.note;
            }
        };

        struct VoiceKeyHash
        {
            std::size_t operator() (const VoiceKey& k) const noexcept
            {
                return (std::size_t (k.channel) << 8) ^ std::size_t (k.note);
            }
        };

        uint64_t makeTag (const VoiceKey& k) const noexcept
        {
            return (uint64_t (k.channel) << 8) | uint64_t (k.note);
        }

        void handleNoteOn  (const juce::MidiMessage& msg, ScheduledMidiOutput& out);
        void handleNoteOff (const juce::MidiMessage& msg, ScheduledMidiOutput& out);

        std::vector<int> buildChordNotes (int inputNoteNumber) const;
        std::vector<int> buildChromaticChord (int rootNoteNumber) const;
        std::vector<int> buildDiatonicChord  (int inputNoteNumber) const;

        static std::array<int, 7> scaleSteps (Scale scale);

        // Utilities
        static void applyInversion (std::vector<int>& notes, int inversion);
        static void applySpread (std::vector<int>& notes, int spread);
        static void applyOctaveShift (std::vector<int>& notes, int octaveShift);
        static void sortUniqueClamp (std::vector<int>& notes);

        static int clampMidiNote (int n) { return juce::jlimit (0, 127, n); }

        mutable juce::CriticalSection settingsLock;
        Settings settings;

        juce::Random rng;

        // Map: (input note + channel) -> chord note numbers used for note-off.
        std::unordered_map<VoiceKey, std::vector<int>, VoiceKeyHash> active;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordEngine)
    };
} // namespace sampledex
