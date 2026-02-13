# Sampledex ChordLab (Standalone macOS app)

**Sampledex ChordLab** is a standalone JUCE app that turns single-note playing into full chords (as MIDI), with a built-in **Practice Mode**, **MIDI recording/export**, and a lightweight **AU/VST3 host** so you can hear chords immediately.

This repo now uses a deterministic macOS release script that hard-cleans stale bundles
and produces one canonical app output path.

---

## What's new in v2

### Clear tabs
- **Play** – chord generator + voicing + virtual MIDI output (route into any DAW)
- **Practice** – target-chord game (play the right chord to score)
- **Record** – record what you play and/or what ChordLab outputs; export standard `.mid`
- **Plugins** – load an AU/VST3 and route ChordLab's generated MIDI into it
- **Settings** – light/dark mode + quick guide

### Visible feedback
- Bottom **Status Bar** shows:
  - MIDI IN activity LED
  - MIDI OUT activity LED
  - REC indicator
  - PLUGIN loaded indicator

---

## Build

### Prereqs
- JUCE repo cloned somewhere on disk
- CMake 3.22+
- Xcode + command line tools

### Build (recommended, deterministic)
```bash
export JUCE_DIR=~/JUCE   # or your JUCE repo root
bash ./scripts/release_macos.sh
```

Canonical app output:
`/Users/robertclemons/Downloads/sampledex_daw-main/build/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app`

The script also writes:
`build/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app/Contents/Resources/BuildManifest.json`

### Direct CMake fallback (manual)
```bash
export JUCE_DIR=~/JUCE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j8
```

---

## Using ChordLab

### 1) Route into any DAW (recommended workflow)
1. Launch **Sampledex ChordLab**
2. **Play tab → MIDI Routing**
   - Select your MIDI keyboard as **Input**
   - Enable **Virtual output** (default name: “Sampledex ChordLab”)
3. In your DAW:
   - Create an instrument track
   - Set track **MIDI input** to “Sampledex ChordLab”
   - Arm the track and play

### 2) Practice mode
Go to **Practice tab** and play notes until the target chord is detected.

### 3) Record + export
Go to **Record tab**:
- Record **Input** (what you play) and/or **Output** (generated chords)
- Export to `.mid` and drag it into any DAW

### 4) Load a plugin (mini DAW teacher)
Go to **Plugins tab**:
- **Load AU/VST3...** and pick a plugin
- Enable **Send generated chords to plugin** to drive the plugin
- Use **Audio settings...** to choose output

---

## Notes

- Virtual MIDI output creation is supported on macOS (and some other desktop platforms). On platforms where it isn't available, the toggle will turn itself off.
- AU/VST3 hosting depends on your JUCE build and OS plugin support.

---

## Bundle ID
`com.sampledex.chordlab`

---

## License
This project uses JUCE. Ensure you comply with your JUCE licensing (GPL/commercial).
