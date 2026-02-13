# Sampledex ChordLab (Standalone macOS app)

**Sampledex ChordLab** is a standalone JUCE app that turns single-note playing into full chords (as MIDI), with a built-in **Practice Mode**, **MIDI recording/export**, and a lightweight **AU/VST3 host** so you can hear chords immediately.

This repo now uses a deterministic macOS release script that hard-cleans stale bundles
and produces one canonical app output path.

---

## Project status (as of February 13, 2026)

This project is currently in an **unstable, in-progress DAW state** and is **not release-ready for production sessions** yet.

### What is working
- The app builds successfully on macOS via `scripts/release_macos.sh` and produces a universal app binary.
- Core DAW UI/components are present (tracks, mixer, arrangement/timeline, plugin hosting surface).
- AU/VST3 scan flow now runs in isolated passes per format with timeout/continuation behavior.

### Known blocking reliability issues
- Startup buzz/noise can occur on launch on some systems.
- Some plugins can still hard-crash the audio callback thread.
- AU discovery/scan reliability is still inconsistent on some machines.
- Format fallback/restore behavior still needs hardening and validation.

### Tracked issues (handoff list)
- [#1 Startup buzz/noise on launch](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/1)
- [#2 Audio callback crash hardening](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/2)
- [#3 AU scan hang/timeout handling](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/3)
- [#4 AU discovery missing from known list](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/4)
- [#5 AU-first format policy consistency](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/5)
- [#6 Project restore cross-format fallback](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/6)
- [#7 Auto-quarantine after unclean termination](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/7)
- [#8 UI/RT lock contention in plugin metadata](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/8)
- [#9 Scan UX per-format diagnostics](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/9)
- [#10 QA regression matrix for AU+VST3 reliability](https://github.com/RobertAnthonyDevelopment/sampledex_daw_pro/issues/10)

If you are evaluating this project now, treat it as a development snapshot and expect instability while the above issues are being addressed.

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

Optional bundled plugins:
- Put AU plugins in `BundledPlugins/Components`
- Put VST3 plugins in `BundledPlugins/VST3`
- The release script copies them into:
  `Sampledex ChordLab.app/Contents/PlugIns/Components` and
  `Sampledex ChordLab.app/Contents/PlugIns/VST3`
- Plugin scan includes both system/user plugin folders and these app-local plugin folders.

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
