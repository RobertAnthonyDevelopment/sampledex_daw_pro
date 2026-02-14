# Sampledex ChordLab (Current Project State)

Sampledex ChordLab is now a **standalone macOS JUCE DAW/workstation prototype**, not just a chord utility app.

It includes:
- Multi-track arrangement/timeline
- Mixer and channel routing
- Piano roll + step sequencer
- MIDI/audio recording workflows
- AU + VST3 hosting
- Freeze/commit/export features

This repository is under active stabilization and is **not production-ready yet**.

---

## Current status (February 13, 2026)

### Reality check
- The app builds and launches.
- A lot of DAW functionality exists and works in many sessions.
- Reliability is still inconsistent under real plugin/device conditions.

### High-priority known problems
- Startup buzz/noise can occur on launch on some systems.
- Some plugin runtime failures still crash in audio callback context.
- AU scan/discovery is improved but still needs broader validation.
- Project restore + cross-format fallback paths need additional hardening.

### Tracked blocker issues
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

If you are evaluating this project, treat it as an active development snapshot.

---

## What this app currently contains

### DAW core
- Track model + transport + timeline clip arrangement
- Mixer with per-track controls and sends
- Aux/master processing chain
- Tempo map, loop, metronome, punch/count-in tools

### Editing and composition
- Piano roll editor
- Step sequencer tab
- Chord engine integration
- Track/channel rack and inspector workflows

### Plugin host
- AU + VST3 host enabled in build
- Isolated plugin scan + dead-man blacklist flow
- Plugin probe/quarantine safety path
- Format preference/fallback logic work in progress (see issues above)

### Audio/MIDI operations
- MIDI and audio capture paths
- Mixdown and stems export
- Freeze and commit-to-audio operations
- MIDI routing/control-surface related plumbing

---

## Compile review notes

A repository-wide compile-risk review was documented in `docs/compile_review.md` with executed checks and environment constraints.

---

## Build (macOS)

### Prerequisites
- macOS with Xcode + command line tools
- CMake 3.22+
- JUCE checkout available locally

Set `JUCE_DIR` to your JUCE repo root (or place JUCE at `~/JUCE`).

### Recommended deterministic build
```bash
export JUCE_DIR=~/JUCE
bash ./scripts/release_macos.sh
```

Canonical app output:
`/Users/robertclemons/Downloads/sampledex_daw-main/build/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app`

### Manual CMake fallback
```bash
export JUCE_DIR=~/JUCE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j8
```

---

## Important build-script behavior

`scripts/release_macos.sh` is intentionally aggressive for deterministic local builds:
- Kills running app instances
- Deletes stale app bundles from `~/Downloads`
- Deletes app state/cache paths under `~/Library/...`
- Removes and rebuilds `build/`

Do not run it if you need to preserve current local app state.

---

## Plugin locations

Scanner targets both system/user and app-local bundled paths.

System/user defaults:
- `/Library/Audio/Plug-Ins/VST3`
- `~/Library/Audio/Plug-Ins/VST3`
- `/Library/Audio/Plug-Ins/Components`
- `~/Library/Audio/Plug-Ins/Components`

Optional app-local bundle sources:
- `BundledPlugins/VST3`
- `BundledPlugins/Components`

These are copied into app bundle on release build under:
- `Sampledex ChordLab.app/Contents/PlugIns/VST3`
- `Sampledex ChordLab.app/Contents/PlugIns/Components`

---

## Safe mode / troubleshooting

- You can start in Safe Mode at launch (Shift at startup or safe-mode CLI arg path in app startup logic).
- If plugin instability occurs, use the issues above as the primary handoff/triage list.

---

## Bundle identifier

`com.Sampledex.SampledexChordLab`

---

## License notes

This project uses JUCE. You are responsible for complying with JUCE licensing terms (GPL/commercial as applicable).
