#include "ChordEngine.h"
#include "ScheduledMidiOutput.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sampledex
{
    ChordEngine::ChordEngine()
    {
        rng.setSeedRandomly();
    }

    ChordEngine::~ChordEngine() = default;

    void ChordEngine::setSettings (Settings s)
    {
        const juce::ScopedLock sl (settingsLock);
        settings = s;
    }

    ChordEngine::Settings ChordEngine::getSettings() const
    {
        const juce::ScopedLock sl (settingsLock);
        return settings;
    }

    void ChordEngine::processIncoming (const juce::MidiMessage& msg, ScheduledMidiOutput& out)
    {
        if (msg.isNoteOn())
        {
            handleNoteOn (msg, out);
            return;
        }

        // JUCE treats "note-on with velocity 0" as note-off in isNoteOff() for most sources,
        // so this stays compatible across MIDI devices.
        if (msg.isNoteOff())
        {
            handleNoteOff (msg, out);
            return;
        }

        // Always allow emergency MIDI to pass.
        if (msg.isAllNotesOff() || msg.isAllSoundOff() || msg.isResetAllControllers())
        {
            out.schedule (msg, 0.0);
            return;
        }

        const auto s = getSettings();
        if (s.passthroughCC)
            out.schedule (msg, 0.0);
    }

    void ChordEngine::panic (ScheduledMidiOutput& out)
    {
        // Cancel pending strums for any active key.
        for (const auto& kv : active)
            out.cancelTag (makeTag (kv.first));

        // Send note-offs for currently sounding notes.
        for (const auto& kv : active)
        {
            const auto& k = kv.first;
            const auto& chordNotes = kv.second;
            for (auto n : chordNotes)
                out.schedule (juce::MidiMessage::noteOff ((int) k.channel, n), 0.0);
        }

        active.clear();

        // Extra safety: standard All Notes Off on all channels.
        for (int ch = 1; ch <= 16; ++ch)
            out.schedule (juce::MidiMessage::allNotesOff (ch), 0.0);
    }

    void ChordEngine::handleNoteOn (const juce::MidiMessage& msg, ScheduledMidiOutput& out)
    {
        const auto s = getSettings();

        const auto channel = (uint8_t) msg.getChannel();
        const auto inputNote = (uint8_t) msg.getNoteNumber();
        const VoiceKey key { channel, inputNote };

        if (s.latch)
            panic (out);

        // Cancel any queued events for this key before re-triggering.
        out.cancelTag (makeTag (key));

        auto chordNotes = buildChordNotes ((int) inputNote);
        if (chordNotes.empty())
            chordNotes.push_back ((int) inputNote);

        // Velocity: handle both JUCE variants (0..1 float or 0..127 int-ish).
        const float v = (float) msg.getVelocity();
        int baseVelocity = (v <= 1.0f ? (int) std::lround (v * 127.0f) : (int) std::lround (v));
        baseVelocity = juce::jlimit (1, 127, baseVelocity);

        const int velHuman = juce::jlimit (0, 127, s.velocityHumanize);
        const auto tag = makeTag (key);

        // Strum order.
        std::vector<int> order = chordNotes;
        if (s.strumDirection == StrumDirection::Down)
            std::reverse (order.begin(), order.end());

        // Schedule note-ons.
        const double perNote = std::max (0.0, s.strumMs);
        const double human = std::max (0.0, s.humanizeMs);

        for (size_t i = 0; i < order.size(); ++i)
        {
            const int note = clampMidiNote (order[i]);

            int vel = baseVelocity;
            if (velHuman > 0)
                vel = juce::jlimit (1, 127, vel + (rng.nextInt (velHuman * 2 + 1) - velHuman));

            double delay = perNote * (double) i;
            if (human > 0.0)
            {
                const double jitter = (rng.nextDouble() * 2.0 - 1.0) * human;
                delay = std::max (0.0, delay + jitter);
            }

            out.schedule (juce::MidiMessage::noteOn ((int) channel, note, (juce::uint8) vel), delay, tag);
        }

        active[key] = chordNotes;
    }

    void ChordEngine::handleNoteOff (const juce::MidiMessage& msg, ScheduledMidiOutput& out)
    {
        const auto s = getSettings();
        if (s.latch)
            return;

        const auto channel = (uint8_t) msg.getChannel();
        const auto inputNote = (uint8_t) msg.getNoteNumber();
        const VoiceKey key { channel, inputNote };

        const auto it = active.find (key);
        if (it == active.end())
            return;

        // Cancel any pending strum note-ons that haven't fired yet.
        out.cancelTag (makeTag (key));

        for (auto n : it->second)
            out.schedule (juce::MidiMessage::noteOff ((int) channel, n), 0.0);

        active.erase (it);
    }

    std::vector<int> ChordEngine::buildChordNotes (int inputNoteNumber) const
    {
        const auto s = getSettings();

        std::vector<int> notes;

        if (s.mode == Mode::Chromatic)
            notes = buildChromaticChord (inputNoteNumber);
        else
            notes = buildDiatonicChord (inputNoteNumber);

        applyOctaveShift (notes, s.octaveShift);
        applyInversion (notes, s.inversion);
        applySpread (notes, s.spread);
        sortUniqueClamp (notes);

        return notes;
    }

    std::vector<int> ChordEngine::buildChromaticChord (int rootNoteNumber) const
    {
        const auto s = getSettings();

        std::vector<int> intervals;
        switch (s.chromaticQuality)
        {
            case ChromaticQuality::Major:      intervals = { 0, 4, 7 }; break;
            case ChromaticQuality::Minor:      intervals = { 0, 3, 7 }; break;
            case ChromaticQuality::Dominant7:  intervals = { 0, 4, 7, 10 }; break;
            case ChromaticQuality::Major7:     intervals = { 0, 4, 7, 11 }; break;
            case ChromaticQuality::Minor7:     intervals = { 0, 3, 7, 10 }; break;
            case ChromaticQuality::Sus2:       intervals = { 0, 2, 7 }; break;
            case ChromaticQuality::Sus4:       intervals = { 0, 5, 7 }; break;
            case ChromaticQuality::Power5:     intervals = { 0, 7 }; break;
            default:                           intervals = { 0, 4, 7 }; break;
        }

        std::vector<int> out;
        out.reserve (intervals.size());
        for (auto semis : intervals)
            out.push_back (clampMidiNote (rootNoteNumber + semis));

        return out;
    }

    std::vector<int> ChordEngine::buildDiatonicChord (int inputNoteNumber) const
    {
        const auto s = getSettings();

        auto steps = scaleSteps (s.scale);
        const int keyRootPc = juce::jlimit (0, 11, s.keyRootPc);

        const int inputPc = ((inputNoteNumber % 12) + 12) % 12;
        const int relPc = (inputPc - keyRootPc + 12) % 12;

        // Find nearest scale degree (by pitch-class distance, wrap-aware).
        auto nearestDegree = [&] ()
        {
            int bestIdx = 0;
            int bestDist = 999;
            for (int i = 0; i < 7; ++i)
            {
                const int d0 = (steps[i] - relPc + 12) % 12;      // upward distance
                const int dn = (d0 == 0 ? 0 : d0 - 12);           // downward distance
                const int dist = std::min (std::abs (dn), std::abs (d0));
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestIdx = i;
                }
            }
            return bestIdx;
        };

        int rootNote = inputNoteNumber;
        int degree = nearestDegree();

        // Optionally snap the input note to the nearest scale tone.
        if (s.snapToScale)
        {
            const int targetPc = (keyRootPc + steps[degree]) % 12;

            // Choose signed delta (closest wrap-around adjustment).
            const int up = (targetPc - inputPc + 12) % 12;
            const int down = (up == 0 ? 0 : up - 12);
            const int chosen = (std::abs (down) <= std::abs (up)) ? down : up;

            rootNote = clampMidiNote (inputNoteNumber + chosen);

            // Now the snapped note should be in-scale; recompute exact degree.
            const int snappedPc = ((rootNote % 12) + 12) % 12;
            const int snappedRel = (snappedPc - keyRootPc + 12) % 12;
            for (int i = 0; i < 7; ++i)
                if (steps[i] == snappedRel)
                    degree = i;
        }

        auto degreeOffsetSemis = [&] (int fromDeg, int toDeg) -> int
        {
            // toDeg can be >= 7 (wraps to next octave).
            const int fromStep = steps[fromDeg];
            const int toStep = steps[toDeg % 7] + 12 * (toDeg / 7);
            return toStep - fromStep;
        };

        std::vector<int> offsets;
        switch (s.diatonicStack)
        {
            case DiatonicStack::Triad:
                offsets = { 0,
                            degreeOffsetSemis (degree, degree + 2),
                            degreeOffsetSemis (degree, degree + 4) };
                break;

            case DiatonicStack::Seventh:
                offsets = { 0,
                            degreeOffsetSemis (degree, degree + 2),
                            degreeOffsetSemis (degree, degree + 4),
                            degreeOffsetSemis (degree, degree + 6) };
                break;

            case DiatonicStack::Add9:
            default:
                offsets = { 0,
                            degreeOffsetSemis (degree, degree + 2),
                            degreeOffsetSemis (degree, degree + 4),
                            degreeOffsetSemis (degree, degree + 8) };
                break;
        }

        std::vector<int> out;
        out.reserve (offsets.size());
        for (auto semis : offsets)
            out.push_back (clampMidiNote (rootNote + semis));

        return out;
    }

    std::array<int, 7> ChordEngine::scaleSteps (Scale scale)
    {
        switch (scale)
        {
            case Scale::Major:        return { 0, 2, 4, 5, 7, 9, 11 };
            case Scale::NaturalMinor: return { 0, 2, 3, 5, 7, 8, 10 };
            case Scale::Dorian:       return { 0, 2, 3, 5, 7, 9, 10 };
            case Scale::Mixolydian:   return { 0, 2, 4, 5, 7, 9, 10 };
            default:                  return { 0, 2, 4, 5, 7, 9, 11 };
        }
    }

    void ChordEngine::applyInversion (std::vector<int>& notes, int inversion)
    {
        if (notes.empty())
            return;

        inversion = juce::jlimit (0, (int) notes.size() - 1, inversion);
        for (int i = 0; i < inversion; ++i)
        {
            const int n = notes.front();
            notes.erase (notes.begin());
            notes.push_back (n + 12);
        }
    }

    void ChordEngine::applySpread (std::vector<int>& notes, int spread)
    {
        spread = juce::jlimit (0, 2, spread);
        if (spread == 0 || notes.size() < 2)
            return;

        for (size_t i = 1; i < notes.size(); ++i)
        {
            if (spread == 1)
            {
                if ((i % 2) == 1)
                    notes[i] += 12;
            }
            else
            {
                // Moderate widening across ~2 octaves.
                notes[i] += 12 * (int) ((i + 1) / 2);
            }
        }
    }

    void ChordEngine::applyOctaveShift (std::vector<int>& notes, int octaveShift)
    {
        if (notes.empty() || octaveShift == 0)
            return;

        const int delta = juce::jlimit (-2, 2, octaveShift) * 12;
        for (auto& n : notes)
            n += delta;
    }

    void ChordEngine::sortUniqueClamp (std::vector<int>& notes)
    {
        for (auto& n : notes)
            n = clampMidiNote (n);

        std::sort (notes.begin(), notes.end());
        notes.erase (std::unique (notes.begin(), notes.end()), notes.end());
    }
} // namespace sampledex
