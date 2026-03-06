# Maya Pipeline Tools

A production-oriented Autodesk Maya C++ plugin for safe scene opening, dependency repair, selective reference loading, and batch FBX animation export.

> 中文简介：一个面向动画制作流程的 Maya 原生 C++ 插件，专注于安全开场景、依赖修复、引用管理和批量 FBX 导出。

## Highlights

- **Open Without References** — open heavy Maya scenes without loading references first.
- **Reference Checker** — scan references, textures, caches, and audio; find missing files; batch repair broken paths.
- **Safe Load References** — inspect reference status and load or remove them more safely after a lightweight open.
- **Batch Animation Exporter** — export cameras, skeleton animation, and BlendShape animation to FBX in batches.
- **Pipeline-friendly Logging** — keep export and plugin logs for troubleshooting in production.

## Why This Project

Large Maya scenes often become painful when:

- broken references trigger popup storms or freezes,
- assets move between projects or storage locations,
- animators need only a subset of references loaded,
- exporting multiple animation targets repeatedly costs too much time.

This plugin turns those repetitive operations into focused tools inside one Maya menu.

## Tools Included

### 1. Open Without References

Open a `.ma` or `.mb` scene with reference loading disabled, so the scene becomes accessible even when downstream assets are slow, missing, or unstable.

### 2. Reference Checker

Scan scene dependencies and show whether each file exists. Missing files can be auto-matched from search directories and then applied back to the scene.

### 3. Safe Load References

List every reference with load state, existence, and size. This is useful after a safe open when you want to bring references back in gradually.

### 4. Batch Animation Exporter

Batch export:

- cameras,
- skeleton animation,
- BlendShape animation,
- skeleton + BlendShape combinations.

The exporter also handles baking, naming, logging, and export-range control.

## Requirements

- Windows
- Autodesk Maya 2024+
- Maya SDK / Devkit
- Visual Studio 2022
- CMake 3.12+
- Qt6 headers from Maya 2026 or local `qt_include/`

## Build

```bash
mkdir build_local
cd build_local

cmake .. -G "Visual Studio 17 2022" -A x64 \
  -DMAYA_LOCATION="C:/Program Files/Autodesk/Maya2026"

cmake --build . --config Release
```

Output:

```text
build_local/Release/MayaRefCheckerPlugin.mll
```

## Install

Copy `MayaRefCheckerPlugin.mll` into one of these locations:

```text
%USERPROFILE%\Documents\maya\<version>\plug-ins\
C:\Program Files\Autodesk\Maya<version>\bin\plug-ins\
```

Then load it from Maya Plug-in Manager.

After loading, Maya adds a top-level menu:

```text
Pipeline Tools
```

## Project Structure

```text
src/
  pluginMain.cpp        Plugin entry and Maya menu registration
  SafeOpenCmd.*         Open scene without loading references
  SafeLoaderCmd/UI.*    Reference inspection / load / unload UI
  RefCheckerCmd/UI.*    Dependency scanning and path repair UI
  BatchExporterCmd/UI.* Batch export orchestration UI
  AnimExporter.*        FBX export core
  SceneScanner.*        Scene scanning helpers
  FileAnalyzer.*        Offline .ma / .mb dependency analysis
  NamingUtils.*         Export naming helpers
  ExportLogger.*        Export log output
  PluginLog.*           Shared logging helpers

docs/
  user-guide.md
  developer-guide.md
  中文字符与路径处理总结.md
```

## Documentation

- `docs/user-guide.md` — user workflows and UI behavior
- `docs/developer-guide.md` — architecture, module responsibilities, and build details
- `docs/中文字符与路径处理总结.md` — notes about Unicode / path handling on Windows + Maya

## Maintainer Notes

- The plugin entry lives in `src/pluginMain.cpp`.
- Path resolution should reuse `src/SceneScanner.cpp` helpers where possible.
- `Safe Loader` now checks the resolved on-disk path instead of Maya's raw unresolved path string.
- If plugin initialization fails, registered commands should be rolled back to avoid half-loaded Maya state.
- Before release, rebuild against the target Maya version you plan to ship.

## Roadmap Ideas

- richer screenshots / demo GIFs,
- sample scenes for testing,
- CI packaging for multiple Maya versions,
- more exporter presets for different game-engine pipelines.

## License

MIT
