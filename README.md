# Maya Pipeline Tools

A native C++ plugin (`.mll`) for Autodesk Maya that streamlines animation production pipelines — covering safe scene opening, reference/dependency management, and batch FBX animation export.

## Features

**Open Without References** — Open heavy scenes instantly by skipping all reference loading, avoiding Maya freezes caused by missing or broken references.

**Reference Checker** — Scan every dependency in your scene (references, textures, caches, audio), identify missing files, auto-match replacements from a search directory, and batch-fix paths in one click.

**Safe Load References** — After a safe open, selectively load/unload individual references with full visibility into file existence and size. Remove dead references that point to missing files.

**Batch Animation Exporter** — One-click batch export of camera, skeleton, and BlendShape animations to FBX. Handles baking, namespace stripping, skeleton hierarchy cleanup, and standardized file naming automatically.

## Requirements

- Windows
- Autodesk Maya 2024+ (Qt6, C++17)
- Maya SDK (devkit)
- Visual Studio 2022
- CMake 3.12+

## Build

```bash
mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 \
  -DMAYA_LOCATION="C:/Program Files/Autodesk/Maya2026"

cmake --build . --config Release
```

Output: `build/Release/MayaRefCheckerPlugin.mll`

> Qt6 headers: Maya 2026 ships them as a zip (`qt_6.5.3_vc14-include.zip`). Extract to `qt_include/` in the project root, or set `-DQT_INCLUDE_DIR=<path>`.

## Install

Copy `MayaRefCheckerPlugin.mll` to one of:

```
%USERPROFILE%\Documents\maya\<version>\plug-ins\
C:\Program Files\Autodesk\Maya<version>\bin\plug-ins\
```

Or load manually via Maya's Plug-in Manager (Browse → select the `.mll` → check Loaded).

A **Pipeline Tools** menu will appear in Maya's main menu bar.

## Project Structure

```
├── CMakeLists.txt
├── src/
│   ├── pluginMain.cpp          # Plugin entry, command registration, menu
│   ├── SafeOpenCmd.*           # Open scene without loading references
│   ├── SafeLoaderCmd/UI.*      # Interactive reference load/unload
│   ├── RefCheckerCmd/UI.*      # Dependency scanner & path fixer
│   ├── BatchExporterCmd/UI.*   # Batch export UI & orchestration
│   ├── AnimExporter.*          # FBX export core (camera/skeleton/blendshape)
│   ├── SceneScanner.*          # Scene content discovery
│   ├── FileAnalyzer.*          # Offline .ma/.mb dependency parser
│   ├── NamingUtils.*           # Output filename generation
│   ├── ExportLogger.*          # Export logging
│   └── PluginLog.*             # Plugin-wide logging
└── docs/
    ├── user-guide.md
    └── developer-guide.md
```

## Documentation

- [User Guide](docs/user-guide.md) — installation, UI walkthrough, workflows
- [Developer Guide](docs/developer-guide.md) — architecture, module details, build system, extension guide

## License

MIT
