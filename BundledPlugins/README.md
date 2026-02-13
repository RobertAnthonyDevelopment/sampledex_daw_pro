# Bundled Plugins

Drop third-party plugins here if you want them copied into the app bundle at release time.

- VST3 plugins: `BundledPlugins/VST3`
- Audio Unit components: `BundledPlugins/Components`

The release script copies these folders into:

- `Sampledex ChordLab.app/Contents/PlugIns/VST3`
- `Sampledex ChordLab.app/Contents/PlugIns/Components`

The plugin scanner now searches both standard system/user plugin locations and these app-local paths.
