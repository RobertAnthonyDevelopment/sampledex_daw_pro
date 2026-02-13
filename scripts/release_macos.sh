#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
CANONICAL_APP="$BUILD_DIR/SampledexChordLab_artefacts/Release/Sampledex ChordLab.app"
CANONICAL_BIN="$CANONICAL_APP/Contents/MacOS/Sampledex ChordLab"
MANIFEST_PATH="$CANONICAL_APP/Contents/Resources/BuildManifest.json"
MIN_BIN_SIZE_BYTES=$((5 * 1024 * 1024))

log() {
  printf '[release] %s\n' "$1"
}

die() {
  printf '[release] ERROR: %s\n' "$1" >&2
  exit 1
}

log "Force quitting running app instances"
osascript -e 'tell application "Sampledex ChordLab" to quit saving no' >/dev/null 2>&1 || true
pkill -f '/Sampledex ChordLab.app/Contents/MacOS/Sampledex ChordLab' >/dev/null 2>&1 || true

log "Removing stale app bundles from ~/Downloads"
canonical_real=""
if [[ -d "$CANONICAL_APP" ]]; then
  canonical_real="$(cd "$CANONICAL_APP" && pwd -P)"
fi

while IFS= read -r -d '' app_path; do
  app_real="$(cd "$app_path" && pwd -P)"
  if [[ -n "$canonical_real" && "$app_real" == "$canonical_real" ]]; then
    continue
  fi
  log "Deleting stale bundle: $app_path"
  rm -rf "$app_path"
done < <(find "$HOME/Downloads" -type d -name 'Sampledex ChordLab.app' -print0)

log "Removing app-specific cache/state"
state_paths=(
  "$HOME/Library/Application Support/com.Sampledex.SampledexChordLab"
  "$HOME/Library/Sampledex/ChordLab"
  "$HOME/Library/Caches/com.Sampledex.SampledexChordLab"
  "$HOME/Library/HTTPStorages/com.Sampledex.SampledexChordLab"
  "$HOME/Library/Preferences/com.Sampledex.SampledexChordLab.plist"
)

for path in "${state_paths[@]}"; do
  if [[ -e "$path" ]]; then
    log "Deleting stale state: $path"
    rm -rf "$path"
  fi
done

log "Cleaning build directory"
rm -rf "$BUILD_DIR"

log "Configuring CMake (Release)"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

cpu_count="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
if [[ -z "$cpu_count" ]]; then
  cpu_count=8
fi

log "Building app"
cmake --build "$BUILD_DIR" --config Release -j "$cpu_count"

log "Copying optional bundled plugins into app (if present)"
BUNDLED_SRC="$REPO_ROOT/BundledPlugins"
APP_PLUGINS_DIR="$CANONICAL_APP/Contents/PlugIns"
if [[ -d "$BUNDLED_SRC" ]]; then
  mkdir -p "$APP_PLUGINS_DIR"
  if [[ -d "$BUNDLED_SRC/VST3" ]]; then
    mkdir -p "$APP_PLUGINS_DIR/VST3"
    cp -R "$BUNDLED_SRC/VST3/." "$APP_PLUGINS_DIR/VST3/"
  fi
  if [[ -d "$BUNDLED_SRC/Components" ]]; then
    mkdir -p "$APP_PLUGINS_DIR/Components"
    cp -R "$BUNDLED_SRC/Components/." "$APP_PLUGINS_DIR/Components/"
  fi
fi

log "Validating bundle"
[[ -d "$CANONICAL_APP" ]] || die "Bundle missing: $CANONICAL_APP"
[[ -f "$CANONICAL_APP/Contents/Info.plist" ]] || die "Info.plist missing"
[[ -d "$CANONICAL_APP/Contents/Resources" ]] || die "Resources directory missing"
find "$CANONICAL_APP/Contents/Resources" -mindepth 1 -print -quit | grep -q . || die "Resources directory is empty"
[[ -f "$CANONICAL_BIN" ]] || die "Executable missing: $CANONICAL_BIN"

bin_size="$(stat -f%z "$CANONICAL_BIN" 2>/dev/null || echo 0)"
if [[ "$bin_size" -lt "$MIN_BIN_SIZE_BYTES" ]]; then
  die "Executable too small: ${bin_size} bytes"
fi

file_out="$(file "$CANONICAL_BIN")"
printf '[release] file: %s\n' "$file_out"
echo "$file_out" | grep -q 'arm64' || die 'Executable missing arm64 architecture'
echo "$file_out" | grep -q 'x86_64' || die 'Executable missing x86_64 architecture'

bundle_size_bytes="$(( $(du -sk "$CANONICAL_APP" | awk '{print $1}') * 1024 ))"
timestamp_utc="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
git_hash="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo 'nogit')"

log "Writing BuildManifest.json"
mkdir -p "$(dirname "$MANIFEST_PATH")"
cat > "$MANIFEST_PATH" <<JSON
{
  "built_at_utc": "$timestamp_utc",
  "git_hash": "$git_hash",
  "canonical_app_path": "$CANONICAL_APP",
  "executable_path": "$CANONICAL_BIN",
  "bundle_size_bytes": $bundle_size_bytes,
  "executable_size_bytes": $bin_size,
  "architectures": ["arm64", "x86_64"]
}
JSON

log "Build complete"
printf '\nCanonical app:\n%s\n\n' "$CANONICAL_APP"
printf 'Launch command:\nopen "%s"\n' "$CANONICAL_APP"
