# MayaRefCheckerPlugin — 开发者技术文档

## 1. 项目概述

MayaRefCheckerPlugin（对外名称 **PipelineTools**）是一个 Autodesk Maya C++ 插件（`.mll`），提供四个核心功能：

1. **Safe Open** — 不加载引用地打开场景文件
2. **Reference Checker** — 扫描场景依赖（引用/贴图/缓存/音频），定位缺失文件并批量修复路径
3. **Safe Loader** — 逐个加载/卸载引用，避免一次性加载导致卡死
4. **Batch Animation Exporter** — 批量烘焙并导出相机、骨骼、BlendShape 动画为 FBX

插件使用 Maya C++ API + Qt6（Maya 内置）构建 UI，编译产物为 `MayaRefCheckerPlugin.mll`。

---

## 2. 目录结构

```
MayaRefCheckerPlugin/
├── CMakeLists.txt              # 构建配置
├── src/
│   ├── pluginMain.cpp          # 插件入口：注册命令、创建菜单
│   │
│   ├── SafeOpenCmd.h/cpp       # MEL 命令 safeOpenScene
│   ├── SafeLoaderCmd.h/cpp     # MEL 命令 safeLoadRefs
│   ├── SafeLoaderUI.h/cpp      # Safe Loader 的 Qt 对话框
│   │
│   ├── RefCheckerCmd.h/cpp     # MEL 命令 refChecker
│   ├── RefCheckerUI.h/cpp      # Reference Checker 的 Qt 对话框（最大的文件）
│   │
│   ├── BatchExporterCmd.h/cpp  # MEL 命令 batchAnimExporter
│   ├── BatchExporterUI.h/cpp   # Batch Exporter 的 Qt 对话框
│   │
│   ├── AnimExporter.h/cpp      # FBX 导出底层函数（烘焙 + 导出）
│   ├── SceneScanner.h/cpp      # 场景扫描：查找相机/骨骼/BS/依赖
│   ├── FileAnalyzer.h/cpp      # 离线文件分析（解析 .ma/.mb 提取依赖路径）
│   ├── NamingUtils.h/cpp       # 文件命名规则（场景 token 解析 + 文件名生成）
│   └── ExportLogger.h/cpp      # 导出日志记录器
│
├── build/                      # Maya 2024 构建目录
│   └── Release/
│       └── MayaRefCheckerPlugin.mll
│
└── build2026/                  # Maya 2026 构建目录
```

Python 原型位于 `D:\PythonSourceCode\Maya2020_RefChecker\Maya_RefChecker\`，C++ 版本是从该原型移植而来。

---

## 3. 构建系统

### 3.1 依赖

| 依赖 | 来源 | 说明 |
|------|------|------|
| Maya SDK | `MAYA_LOCATION/include` + `MAYA_LOCATION/lib` | OpenMaya 系列库 |
| Qt6 | Maya 内置 (`MAYA_LOCATION/bin/Qt6*.dll`) | Core, Gui, Widgets |
| MOC | Maya 内置 (`MAYA_LOCATION/bin/moc.exe`) | 处理 Q_OBJECT 宏 |
| Shlwapi.lib | Windows SDK | 路径操作 |

### 3.2 构建步骤

```bash
# 创建构建目录
mkdir build2026 && cd build2026

# 配置（指定 Maya 安装路径）
cmake .. -G "Visual Studio 17 2022" -A x64 \
  -DMAYA_LOCATION="C:/Program Files/Autodesk/Maya2026"

# 编译 Release
cmake --build . --config Release
```

产物：`build2026/Release/MayaRefCheckerPlugin.mll`

### 3.3 MOC 处理

由于不使用 `find_package(Qt6)`，CMakeLists.txt 中手动调用 Maya 自带的 `moc.exe` 处理含 `Q_OBJECT` 的头文件：

- `RefCheckerUI.h` → `moc_RefCheckerUI.cpp`
- `BatchExporterUI.h` → `moc_BatchExporterUI.cpp`
- `SafeLoaderUI.h` → `moc_SafeLoaderUI.cpp`

### 3.4 编译选项

- C++17 标准
- `/Zc:__cplusplus /permissive- /utf-8`（Qt6 要求 + 源码/执行字符集均为 UTF-8，允许源码中直接书写中文字符串）
- 定义 `WIN32`, `NT_PLUGIN`, `REQUIRE_IOSTREAM`
- 输出后缀 `.mll`，无前缀

---

## 4. 架构设计

### 4.1 插件入口 (`pluginMain.cpp`)

`initializePlugin` 注册四个 MEL 命令并创建顶层菜单 **Pipeline Tools**：

| MEL 命令 | 类 | 菜单项 |
|----------|-----|--------|
| `safeOpenScene` | `SafeOpenCmd` | Open Without References |
| `refChecker` | `RefCheckerCmd` | Reference Checker |
| `safeLoadRefs` | `SafeLoaderCmd` | Safe Load References |
| `batchAnimExporter` | `BatchExporterCmd` | Batch Animation Exporter |

每个命令类继承 `MPxCommand`，`doIt()` 中调用对应 UI 类的 `showUI()` 静态方法。

### 4.2 模块依赖关系

```
pluginMain
  ├── RefCheckerCmd → RefCheckerUI → SceneScanner
  ├── BatchExporterCmd → BatchExporterUI → AnimExporter
  │                                      → SceneScanner
  │                                      → NamingUtils
  │                                      → ExportLogger
  ├── SafeOpenCmd (独立)
  └── SafeLoaderCmd → SafeLoaderUI
```

`FileAnalyzer` 是独立的离线分析模块，不依赖 Maya 运行时（可在 Maya 外使用）。

### 4.3 UI 架构模式

所有 UI 类遵循相同模式：

```cpp
class XxxUI : public QDialog {
    Q_OBJECT
public:
    static XxxUI* instance();     // 单例访问
    static void showUI();         // 创建或激活窗口
private:
    void setupUI();               // 构建 Qt 控件
    static XxxUI* instance_;      // 单例指针
};
```

- 使用 `WA_DeleteOnClose` 属性，关闭时自动销毁并清空 `instance_`
- 父窗口设为 `MQtUtil::mainWindow()` 以保持在 Maya 窗口之上

---

## 5. 核心模块详解

### 5.1 SceneScanner (`SceneScanner.h/cpp`)

**职责**：通过 Maya API/MEL 查询当前场景内容。

**关键函数**：

| 函数 | 返回类型 | 说明 |
|------|---------|------|
| `findNonDefaultCameras()` | `vector<CameraInfo>` | 过滤默认相机（persp/top/front/side 等），返回用户相机 |
| `findCharacters()` | `vector<CharacterInfo>` | 找到所有根关节，按命名空间分组，选层级最深的作为主骨骼 |
| `findBlendShapeGroups()` | `vector<BlendShapeGroupInfo>` | 找到所有 blendShape 节点，按命名空间分组 |
| `scanReferences()` | `vector<DependencyInfo>` | 扫描场景引用文件 |
| `scanTextures()` | `vector<DependencyInfo>` | 扫描 file 节点和 aiImage 节点 |
| `scanCaches()` | `vector<DependencyInfo>` | 扫描 AlembicNode 和 gpuCache 节点 |
| `scanAudio()` | `vector<DependencyInfo>` | 扫描 audio 节点 |

**关键数据结构**：

```cpp
struct DependencyInfo {
    std::string type;           // "reference" / "texture" / "cache" / "audio"
    std::string typeLabel;      // 显示标签
    std::string node;           // Maya 节点名
    std::string path;           // 解析后路径
    std::string unresolvedPath; // 原始未解析路径
    bool exists;                // 文件是否存在
    bool selected;              // UI 中是否被选中
    std::string matchedPath;    // 自动匹配到的替换路径
};
```

### 5.2 RefCheckerUI (`RefCheckerUI.h/cpp`)

**职责**：依赖检查与修复的完整 UI 流程。

**核心流程**：

1. **Scan** → 调用 `SceneScanner::scan*()` 收集所有依赖
2. **Batch Locate** → 用户选择搜索目录，扫描文件建立缓存（当前实现为主线程同步扫描 + 进度对话框，可取消）
3. **Auto Match** → 用文件名匹配算法自动关联缺失文件
4. **Apply Fixes** → 对选中的匹配项执行路径修复

**文件缓存系统**：

```cpp
struct FileCache {
    map<string, vector<string>> cache;  // 小写文件名 → 完整路径列表
    vector<string> cacheKeys;           // 所有缓存键（用于通配符匹配）
    set<string> cacheIndex;             // 归一化路径去重集合
    int totalCount;
};
```

- `buildFileCacheInternal()` 使用 Win32 API (`FindFirstFileW/FindNextFileW`) 递归扫描目录
- 跳过 `__pycache__`, `node_modules`, `.git`, `.svn` 等目录
- 文件名使用 `LCMapStringW(LOCALE_INVARIANT)` 做 Unicode 安全的小写转换
- 代码中保留了 `BatchLocateWorker` / `QThread` 版本的框架，但当前 UI 入口走的是同步扫描实现（更容易保证 Maya 内稳定性）；如需恢复后台线程扫描，可在后续版本接入该 Worker

**自动匹配算法** (`autoMatchDependency`)：

1. 从依赖路径提取文件名作为查找键
2. 对引用类型，额外生成 `.ma`↔`.mb` 交替键
3. 对贴图/缓存，生成通配符键（处理 UDIM、序列帧等模式）
4. 在缓存中查找所有候选路径
5. `findBestMatch()` 按评分排序：精确文件名匹配 +120，扩展名匹配 +25，目录后缀匹配 +15/层，公共路径部分 +1/个

**路径修复策略** (`applyPath`)（与当前代码一致）：

对于 **reference** 类型（引用节点）：

1. 若引用已加载：先 `cmds.file(unloadReference=refNode)` 卸载（避免直接改路径时触发重载/卡死）
2. 用 `cmds.file(path, loadReference=refNode, loadReferenceDepth='none')` **更新引用目标路径**（不做深度加载）
3. 再用 `cmds.file(loadReference=refNode)` 尝试加载

返回值语义：`applyPath` 返回“引用目标路径是否成功更新”；引用是否最终成功加载由 `dep.isLoaded` 体现。

> **Path Mode 说明**：UI 允许 Absolute/Relative 两种模式。Relative 模式会把路径写成相对于场景文件目录的相对路径；同时代码会计算其 resolve 后的绝对路径用于校验与日志。

对于 **texture/cache/audio** 类型：直接 `setAttr -type "string"` 修改对应属性（file/aiImage/AlembicNode/gpuCache/audio）。

### 5.3 AnimExporter (`AnimExporter.h/cpp`)

**职责**：封装批量烘焙、三类 FBX 导出（camera/skeleton/blendshape）、关键帧范围查询与导出日志写入。

**核心接口**：

- `ensureFbxPlugin()`：确保 `fbxmaya` 已加载
- `batchBakeAll(...)`：单次收集并批量烘焙 transform 与 blendShape 权重
- `exportCameraFbx(...)` / `exportSkeletonFbx(...)` / `exportBlendShapeFbx(...)`
- `queryFrameRange(...)`：查询导出项真实关键帧范围
- `writeFrameRangeLog(...)`：写出 `export_log_YYYYMMDD_HHMMSS.txt`

**Skeleton 导出关键实现（当前版本）**：

1. 若输入节点不是 joint，会在其子层级中自动选择最合适的根骨架（无 joint 父级且后代最多）
2. 若根骨架有 DAG 父节点，临时 `parent -world`，避免导出多余 group/null
3. 仅对该骨架链做**临时重命名去命名空间**（深层优先），导出后恢复原场景命名
4. 顶层骨骼做 root 规范化（如 `Root_M` → `Root` / `root`）
5. 导出前校验骨骼名是否仍含 `:`；若源骨架可改名失败（常见于引用只读），自动走"临时复制骨架"兜底流程
6. 强制 `FBXExportSkeletonDefinitions -v true`，保证骨骼层级稳定识别
7. 兜底流程：复制骨架、约束+烘焙到复制骨架、去命名空间后导出，再清理临时节点

**BlendShape 导出关键实现（当前版本）**：

BlendShape 导出有两个分支，由 `bsIncludeSkeleton` 选项控制：

**公共步骤**（两个分支共享）：

1. `listHistory` 查询 mesh 的 DG 历史，收集 `blendShape` 和 `skinCluster` 节点
2. 若 `bsIncludeSkeleton=true` 且存在 skinCluster，通过 `skinCluster -q -influence` 收集绑定骨骼（joint）及其非 joint 父 transform（如 `Face_Root`）
3. `duplicate -un` 复制 mesh（`-un` 保留上游 blendShape 节点），`parent -world` 到世界根

**Skeleton-inclusive 分支**（`includeSkeleton=true`）：

4. 保留 duplicate mesh 上的 skinCluster（不删除）
5. 收集骨骼节点的命名空间，`undoInfo -openChunk` 开启 undo 块
6. `namespace -mergeNamespaceWithRoot` 临时去除命名空间（注意：`namespace -exists` 返回 int，必须用 `MGlobal::executeCommand` 的 int 重载查询）
7. 重新查询骨骼路径（namespace merge 后 DAG 路径改变）
8. 选中 duplicate mesh + 骨骼节点，设置 FBX 选项（`Skins=true`, `Shapes=true`, `SkeletonDefs=true`, `AnimationOnly=false`）
9. `FBXExport -s` 导出选中项
10. `undoInfo -closeChunk` + `undo` 恢复命名空间
11. 删除 duplicate mesh

**Mesh-only 分支**（`includeSkeleton=false`）：

4. 删除 duplicate mesh 上的 skinCluster
5. 仅选中 duplicate mesh，设置 FBX 选项（`Skins=false`, `Shapes=true`, `AnimationOnly=false`）
6. `FBXExport -s` 导出
7. 删除 duplicate mesh

**异常安全**：使用 `bool undoChunkOpen` 标志追踪 undo chunk 状态。catch 块中若 undo chunk 未关闭，会执行 `closeChunk` + `undo` 恢复场景。

> **技术要点**：duplicate mesh 的 skinCluster 引用的是**原始场景中的骨骼节点**（非复制品），因此 namespace merge 必须在全场景级别执行（通过 undo chunk + undo 恢复），而非局部重命名。这与 Skeleton 导出的局部重命名策略不同。

**性能相关优化（当前代码）**：

- `batchBakeAll()` 将 N 个对象的 bake 合并为两次 `bakeResults` 调用（camera transforms + blendShape attrs）。骨骼不在此阶段统一 bake（避免把约束/IK 驱动的关节提前烘焙成静态曲线）。
- BlendShape 发现阶段先 `listHistory` 再逐节点 `nodeType` 过滤，兼容性更稳，并输出调试计数
- Skeleton 导出的命名空间处理采用"局部骨架链临时改名 + 恢复"，降低风险与开销；BlendShape 导出因 skinCluster 引用原始骨骼，采用"全场景 namespace merge + undo chunk + undo"策略


### 5.4 BatchExporterUI (`BatchExporterUI.h/cpp`)

**职责**：管理批量导出 UI 流程、参数收集、进度展示与取消控制。

**导出流程（onExport）**：

1. 校验输出目录与帧范围
2. 收集选中项；若包含相机则以首个相机关键帧范围覆盖导出范围
3. 收集 UI 的 `FbxExportOptions`
4. Phase 1：调用 `batchBakeAll()` 做批量烘焙
5. Phase 2：逐项导出 FBX（camera/skeleton/blendshape）
6. Phase 3：可选调用 `queryFrameRange()` + `writeFrameRangeLog()` 生成导出日志
7. 恢复 UI 状态并弹出汇总

**取消机制**：

- 通过 `cancelRequested_` 在 Phase 2 每轮导出前检查
- 当前项完成后停止，后续项标记 `cancelled`
- 烘焙阶段为单条 MEL，无法中途抢占

**注意**：骨骼导出中的 `SkeletonDefs` 当前在导出层被强制开启（见 5.3）。


### 5.5 NamingUtils (`NamingUtils.h/cpp`)

**职责**：从场景文件名解析 token，生成标准化的输出文件名。

**场景文件名模式**：`<Project>_<SceneXX>_<ShotNN>[_extra].ma`

例如 `TritzPV_Scene3B_Shot20.ma` → `project="TritzPV"`, `scene="Scene3B"`, `shot="Shot20"`

**文件名生成规则**：

| 类型 | 前缀 | 格式 | 示例 |
|------|------|------|------|
| Camera | `Cam` | `Cam_{project}_{scene}_{shot}.fbx` | `Cam_TritzPV_Scene3B_Shot20.fbx` |
| Skeleton | `A` | `A_{project}_{charName}_{scene}_{shot}.fbx` | `A_TritzPV_M113_Zealot_Scene3B_Shot20.fbx` |
| BlendShape | `A` | `A_{project}_{charName}_{scene}_{shot}_Face.fbx` | `A_TritzPV_Tritz_Rider_Scene3B_Shot20_Face.fbx` |

**角色名清理** (`cleanCharacterName`)：
- 去除 `SK_` 前缀
- 去除 `_Skin_Rig` / `_PV_Rig` / `_Rig` 后缀及尾随数字
- 去除尾随的 `_数字`（拷贝编号）

**去重**：当同一角色有多个 Rig 拷贝时，提取 Rig 编号追加到文件名中。

### 5.6 FileAnalyzer (`FileAnalyzer.h/cpp`)

**职责**：不依赖 Maya 运行时，直接解析 `.ma`/`.mb` 文件提取依赖路径。

- `.ma` 文件：用正则表达式匹配 `file ... "path" ;` 和 `setAttr ... -type "string" "path"` 模式
- `.mb` 文件：提取 ASCII 字符串和 UTF-16LE 字符串，用 `looksLikePath()` 过滤

### 5.7 ExportLogger (`ExportLogger.h/cpp`)

**职责**：通用导出日志模块（entry/summary/文本落盘）。

**当前状态（与代码一致）**：

- `ExportLogger` 模块仍保留且可编译
- Batch Animation Exporter 当前主路径未直接使用 `ExportLogger`
- 批量导出日志目前由 `AnimExporter::writeFrameRangeLog()` 输出（`export_log_*.txt`，中文紧凑格式，UTF-8 BOM）

**维护建议**：若后续需要统一日志体系，可将 frame-range log 的输出接入 `ExportLogger`，并保持可读性与中文字段一致。


### 5.8 SafeOpenCmd / SafeLoaderUI

- `SafeOpenCmd`：弹出文件选择对话框，用 `file -open -force -loadReferenceDepth "none"` 打开场景
- `SafeLoaderUI`：列出场景中所有引用，显示加载状态和文件是否存在，支持逐个加载/卸载/移除缺失引用

---

## 6. 关键数据结构

### ExportItem (`NamingUtils.h`)

```cpp
struct ExportItem {
    std::string type;       // "camera" / "skeleton" / "blendshape"
    std::string node;       // Maya 节点完整路径
    std::string name;       // 显示名称
    std::string nsOrName;   // 命名空间或裸名
    std::string filename;   // 输出文件名
    bool selected;          // 是否选中导出
    std::string status;     // "pending" / "exporting" / "done" / "error" / "cancelled"
    std::string message;    // 状态消息
};
```

### FbxExportOptions (`AnimExporter.h`)

```cpp
struct FbxExportOptions {
    // Skeleton options
    bool skelAnimationOnly  = false;
    bool skelBakeComplex    = true;
    bool skelSkeletonDefs   = true;
    bool skelConstraints    = false;
    bool skelInputConns     = false;

    // BlendShape options
    bool bsShapes           = true;
    bool bsSmoothMesh       = false;
    bool bsIncludeSkeleton  = true;   // 导出BS时包含绑定骨骼

    // Common options
    std::string fileVersion = "FBX202000";
    std::string upAxis      = "y";
};
```

> **注意**：早期版本有 `bsAnimationOnly` 字段，已移除。BlendShape 导出始终需要包含 mesh 几何数据（`AnimationOnly=false`），否则 UE 无法识别 MorphTarget。

### ExportResult (`AnimExporter.h`)

```cpp
struct ExportResult {
    bool success;
    std::string filePath;
    int64_t fileSize;
    double duration;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};
```

---

## 7. 与 Maya 的交互方式

插件通过两种方式与 Maya 交互：

1. **MEL 命令执行**：大部分操作通过 `MGlobal::executeCommand()` 执行 MEL 命令字符串
   - 查询：`ls`, `listRelatives`, `referenceQuery`, `getAttr`, `playbackOptions`
   - 修改：`setAttr`, `file -loadReference`, `bakeResults`, `FBXExport`
   - FBX 设置：`FBXExportAnimationOnly`, `FBXExportShapes` 等

2. **Maya C++ API**：少量使用
   - `MQtUtil::mainWindow()` 获取 Maya 主窗口
   - `MFnPlugin` 注册/注销命令
   - `MGlobal::displayInfo/Warning/Error` 输出日志

> **设计决策**：优先使用 MEL 命令而非 C++ API，因为 FBX 导出相关功能只有 MEL 接口，且 MEL 命令更容易调试和从 Python 原型移植。

---

## 8. 线程模型

- **主线程**：所有 Maya API 调用和 UI 操作必须在主线程执行
- **UI 响应**：导出循环中使用 `QApplication::processEvents()` 处理 UI 事件（取消按钮点击、进度更新）
- **文件扫描**：当前 UI 入口为主线程同步扫描（Win32 API `FindFirstFileW/FindNextFileW`），通过 `QProgressDialog` + `processEvents()` 展示进度并支持取消；代码中保留 `BatchLocateWorker` / `QThread` 方案骨架，后续可接入以进一步改善 UI 流畅性

---

## 9. 平台相关代码

当前仅支持 **Windows**：

- 文件扫描使用 `FindFirstFileW/FindNextFileW`（Win32 API）
- 文件名小写转换使用 `LCMapStringW(LOCALE_INVARIANT)`
- 环境变量展开使用 `ExpandEnvironmentStringsW`
- 目录创建使用 `_wmkdir` / `_mkdir`
- 编译定义 `WIN32`, `_WIN32`, `NT_PLUGIN`

如需跨平台支持，需要替换这些 Win32 调用。

---

## 10. 常见开发任务指南

### 10.1 添加新的 MEL 命令

1. 创建 `src/NewCmd.h/cpp`，继承 `MPxCommand`
2. 在 `pluginMain.cpp` 中 `#include` 并注册命令
3. 在 `createMenu()` 中添加菜单项
4. 在 `CMakeLists.txt` 的 `PLUGIN_SOURCES` 和 `PLUGIN_HEADERS` 中添加文件

### 10.2 添加新的 UI 对话框

1. 创建 `src/NewUI.h/cpp`，继承 `QDialog`，添加 `Q_OBJECT` 宏
2. 遵循单例模式（`instance_` + `showUI()`）
3. 在 `CMakeLists.txt` 的 `MOC_HEADERS` 中添加头文件
4. 在对应的 Cmd 类中调用 `NewUI::showUI()`

### 10.3 添加新的依赖类型扫描

1. 在 `SceneScanner.h` 中添加扫描函数声明
2. 在 `SceneScanner.cpp` 中实现（参考 `scanTextures()` 的模式）
3. 在 `RefCheckerUI::onScan()` 中调用新函数
4. 在 `RefCheckerUI::applyPath()` 中添加对应的修复逻辑

### 10.4 添加新的 FBX 导出选项

1. 在 `AnimExporter.h` 的 `FbxExportOptions` 中添加字段
2. 在对应的 `export*Fbx()` 函数中添加 `melExec()` 调用
3. 在 `BatchExporterUI::setupUI()` 的 FBX Options 面板中添加控件
4. 在 `BatchExporterUI::collectFbxOptions()` 中读取控件值

### 10.5 调试技巧

- 所有关键操作都通过 `MGlobal::displayInfo/Warning/Error` 输出日志，可在 Maya Script Editor 中查看
- `[BatchBake]`, `[BatchExportDebug]`, `[ApplyFix]`, `[DEBUG autoMatch]` 等前缀标识不同模块日志
- 调试日志会同时写入文件：环境变量 `MAYA_REF_EXPORT_DEBUG_LOG` 指定路径；若未设置则写入 `TEMP/MayaRefChecker_BatchExportDebug.log`。UI 导出时会自动写到输出目录下的 `BatchExportDebug_*.log`（与 FBX 输出目录一致）。
- UI 导出期间会临时设置 `MAYA_REF_EXPORT_RANGE_START` / `MAYA_REF_EXPORT_RANGE_END`，用于 `queryFrameRange()` 在“无显式关键帧（约束驱动）”兜底采样时对齐实际导出区间；导出结束后会恢复原环境变量值。
- 文件缓存构建时会输出前 20 个缓存键用于诊断编码问题
- `getCleanFilename()` 会输出文件名的十六进制字节用于编码诊断
- FBX 导出后会调用 `scanFbxContent()` 扫描 FBX 二进制内容，输出 mesh/limbNode/skeleton/deformer 等计数，便于验证导出结果

### 10.6 MEL 命令返回值陷阱

以下 MEL 命令返回 **int** 而非 string，必须使用 `MGlobal::executeCommand` 的 int 重载：

| MEL 命令 | 返回类型 | 错误用法 | 正确用法 |
|----------|---------|---------|---------|
| `about -api` | int (如 20260300) | `executeCommand(..., MStringVar)` → 可能记录为空 | `MGlobal::executeCommand(..., intVar)` |
| `namespace -exists "ns"` | int (0/1) | `melQueryString(...)` → 返回空字符串 | `MGlobal::executeCommand(..., intVar)` |
| `referenceQuery -isLoaded "file"` | int (0/1) | 同上 | 同上 |

> **教训**：`melQueryString` 内部使用 `executeCommand(cmd, stringResult)`，当 MEL 命令返回 int 时会静默失败返回空字符串，不会报错。这类 bug 很难通过日志发现，需要检查 MEL 命令文档确认返回类型。

### 10.7 Undo Chunk 安全模式

当需要临时修改场景状态（如 `namespace -mergeNamespaceWithRoot`）并在导出后恢复时，使用以下模式：

```cpp
bool undoChunkOpen = false;
try {
    melExec("undoInfo -openChunk");
    undoChunkOpen = true;

    // ... 临时修改场景 ...
    // ... 导出 ...

    melExec("undoInfo -closeChunk");
    melExec("undo");
    undoChunkOpen = false;
} catch (...) {
    if (undoChunkOpen) {
        melExec("undoInfo -closeChunk");
        melExec("undo");
    }
    // ... cleanup ...
}
```

> **关键**：若 `undoInfo -openChunk` 后发生异常但未 `closeChunk`，Maya 的 undo 系统会处于损坏状态（后续所有操作都被记录在未关闭的 chunk 中）。`undoChunkOpen` 标志确保异常路径也能正确关闭。

---

## 11. 已知限制与注意事项

0. **编码转换安全性（关键）**：Windows 下 UTF-8↔UTF-16 转换必须按“包含终止符长度”分配缓冲区（`MultiByteToWideChar` / `WideCharToMultiByte`），否则会发生越界写入；当前代码已统一修正为安全实现。
1. **仅 Windows**：文件扫描、路径处理等使用 Win32 API
2. **Maya 版本**：需要 Maya 2024+ 的 Qt6 和 C++17 支持；build 目录对应 Maya 2024，build2026 对应 Maya 2026
3. **FBX 插件依赖**：导出功能依赖 `fbxmaya` 插件，代码中会自动尝试加载
4. **烘焙不可取消**：`bakeResults` 是单条 MEL 命令，执行期间无法中断
5. **编码**：源码使用 `/utf-8` 编译选项，中文字符串直接以 `u8"..."` 书写，通过 `QString::fromUtf8()` 转换。中文路径通过 `wstring` + Win32 API 处理，文件名匹配使用 `LOCALE_INVARIANT` 小写转换
6. **单例 UI**：每个对话框只能有一个实例，关闭后自动销毁
7. **BlendShape 导出的 skinCluster 引用**：duplicate mesh 的 skinCluster 引用原始场景骨骼（非复制品），因此去命名空间必须在全场景级别操作（undo chunk + undo 恢复），不能局部重命名
8. **`namespace -exists` 返回 int**：不能用 `melQueryString` 查询，必须用 `MGlobal::executeCommand` 的 int 重载（详见 10.6）

---

## 12. Python 原型对照

C++ 插件从 Python 原型移植而来，对应关系：

| Python 模块 | C++ 模块 |
|-------------|---------|
| `batch_exporter.py` → `BatchAnimExporter` 类 | `BatchExporterUI` + `BatchExporterCmd` |
| `batch_exporter.py` → `_batch_bake_all()` | `AnimExporter::batchBakeAll()` |
| `batch_exporter.py` → `_find_cameras/characters/blendshape_groups()` | `SceneScanner::findNonDefaultCameras/findCharacters/findBlendShapeGroups()` |
| `batch_exporter.py` → `_build_*_filename()` | `NamingUtils::build*Filename()` |
| `anim_exporter.py` → `export_*_anim_nobake()` | `AnimExporter::export*Fbx()` |
| `ref_checker.py` | `RefCheckerUI` |
| `file_analyzer.py` | `FileAnalyzer` |
| `safe_open.py` / `safe_loader.py` | `SafeOpenCmd` / `SafeLoaderUI` |

Python 原型可作为功能参考和测试对照。

---

## 13. 发布前检查清单（建议）

1. **基础加载**
   - Maya 2024 / 2026 均能加载 `.mll`，`initializePlugin` / `uninitializePlugin` 可重复执行
   - 菜单 **Pipeline Tools** 能创建/销毁且不会残留重复菜单项

2. **Safe Open**
   - `.ma` / `.mb` 均可通过对话框打开，引用保持 UNLOADED
   - 中文路径、空格路径、网络 UNC 路径（`\\\\server\\share`）不崩溃
   - `PipelineTools.log` 中能看到 SafeOpen 的 step 日志（用于崩溃定位）

3. **Reference Checker**
   - `Scan` 后四类依赖（reference/texture/cache/audio）统计正确
   - `Batch Locate Dir`：
     - 大目录扫描可取消；取消后不会污染缓存/搜索目录列表
     - 自动匹配（含 `.ma`↔`.mb` 互换、UDIM/序列帧通配）符合预期
   - `Apply Fixes`：
     - Absolute / Relative 两种 Path Mode 均能写入并生效（尤其是 reference 的“写入相对路径”）
     - `Ctrl+Z` 撤销行为符合预期（一次 Apply 为一个 undo chunk）

4. **Safe Load References**
   - Refresh / Load Selected / Load All / Unload All / Remove Missing 行为正确
   - 对缺失文件的引用，Exists/Size 显示与磁盘一致

5. **Batch Animation Exporter**
   - Phase 1 bake + Phase 2 export + Phase 3 log 的流程正确；Cancel 在 Phase 2 能按项停止
   - Skeleton：
     - 命名空间剥离与 Root 规范化符合预期；引用骨架能走 duplicate 兜底
     - 导出 FBX 中 limbNode/skin/deformer 计数合理（`scanFbxContent()` 日志）
   - BlendShape：
     - IncludeSkeleton=true 时 namespace merge + undo 恢复场景；失败时有明确 warning
   - 输出目录下同时生成 `BatchExportDebug_*.log` 和 `export_log_*.txt`（若开启）

6. **环境变量清理**
   - UI 导出结束后，`MAYA_REF_EXPORT_DEBUG_LOG`、`MAYA_REF_EXPORT_RANGE_START/END` 会恢复到导出前的值






