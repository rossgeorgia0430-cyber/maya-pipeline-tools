# Pipeline Tools — Maya 插件使用手册

## 1. 简介

Pipeline Tools 是一款 Maya 插件，专为动画制作流程设计，解决以下常见痛点：

- 打开大型场景时因引用加载失败导致 Maya 卡死
- 场景文件迁移后大量引用/贴图/缓存路径失效，需要逐个手动修复
- 动画导出 FBX 时需要反复手动选择、烘焙、设置参数
- 多角色场景的批量动画导出效率低下

插件加载后，Maya 主菜单栏会出现 **Pipeline Tools** 菜单，包含四个功能入口。

> **提示**：所有按钮和关键控件都有中文工具提示（Tooltip），将鼠标悬停在按钮上即可查看功能说明。

---

## 2. 安装

### 2.1 获取插件文件

根据你的 Maya 版本，使用对应的编译产物：

| Maya 版本 | 插件文件 |
|-----------|---------|
| Maya 2024 | `build/Release/MayaRefCheckerPlugin.mll` |
| Maya 2026 | `build2026/Release/MayaRefCheckerPlugin.mll` |

### 2.2 加载插件

**方法一：通过插件管理器**

1. 打开 Maya → Windows → Settings/Preferences → **Plug-in Manager**
2. 点击 **Browse** 按钮
3. 选择 `MayaRefCheckerPlugin.mll` 文件
4. 勾选 **Loaded** 和 **Auto load**（下次启动自动加载）

**方法二：复制到插件目录**

将 `.mll` 文件复制到以下任一目录：

```
C:\Users\<用户名>\Documents\maya\<版本>\plug-ins\
C:\Program Files\Autodesk\Maya<版本>\bin\plug-ins\
```

重启 Maya 后在 Plug-in Manager 中勾选加载。

### 2.3 验证安装

加载成功后：
- Maya 主菜单栏出现 **Pipeline Tools** 菜单
- Script Editor 显示 `PipelineTools v1.1.0 loaded successfully.`

---

## 3. 功能一：Open Without References（安全打开场景）

### 3.1 使用场景

当你需要打开一个包含大量引用的场景文件，但不想等待所有引用加载完成时使用。特别适用于：

- 引用文件路径已失效，正常打开会弹出大量错误对话框
- 场景引用层级很深，加载耗时过长
- 只需要检查或修复场景本身的设置，不需要引用内容

### 3.2 操作步骤

1. 点击 **Pipeline Tools → Open Without References**
2. 在弹出的文件选择对话框中选择 `.ma` 或 `.mb` 文件
3. 场景打开后，所有引用处于**未加载**状态（Outliner 中显示为灰色）

### 3.3 注意事项

- 打开后场景中的引用内容不可见，需要通过 Safe Load References 或 Reference Editor 手动加载
- 此功能等同于 MEL 命令：`file -open -force -loadReferenceDepth "none" "文件路径"`

---

## 4. 功能二：Reference Checker（依赖检查与修复）

### 4.1 使用场景

场景文件迁移到新位置后，引用、贴图、缓存等依赖文件的路径可能失效。Reference Checker 可以：

- 一键扫描场景中所有依赖文件
- 清晰显示哪些文件缺失
- 在指定目录中自动搜索匹配文件
- 批量修复路径

### 4.2 界面说明

打开方式：**Pipeline Tools → Reference Checker**

界面从上到下分为以下区域：

| 区域 | 说明 |
|------|------|
| 搜索目录 | 设置自动匹配时的文件搜索范围 |
| 依赖列表 | 显示所有扫描到的依赖项，包含类型、节点名、路径、状态 |
| 操作按钮 | Scan、Select Missing、Batch Locate Dir、Apply Fixes |
| 状态栏 | 显示当前操作进度和结果 |

### 4.3 完整工作流程

#### 第一步：扫描场景

点击 **Scan** 按钮，插件会扫描当前场景中的所有依赖：

| 依赖类型 | 扫描内容 |
|---------|---------|
| Reference | 场景引用文件（.ma / .mb / .fbx / .abc） |
| Texture | file 节点和 aiImage 节点的贴图路径 |
| Cache | AlembicNode 和 gpuCache 节点的缓存路径 |
| Audio | audio 节点的音频文件路径 |

扫描完成后，列表中每一行显示一个依赖项：
- **绿色** = 文件存在
- **红色** = 文件缺失

#### 第二步：建立文件缓存并自动匹配

点击 **Batch Locate Dir** 按钮，选择一个搜索根目录（通常是项目根目录或资产库目录）。

插件会递归扫描该目录下的所有文件，建立文件名索引。扫描完成后，**自动为每个缺失的依赖项查找最佳匹配**（无需手动触发）。扫描过程中会显示进度对话框，支持取消。

> **提示**：搜索目录选择得越精确，扫描速度越快。建议选择包含所需资产的最小目录范围。

匹配算法会考虑：
- 文件名完全一致（最高优先级）
- 文件扩展名匹配
- 目录结构相似度
- 对于引用文件，会尝试 `.ma` ↔ `.mb` 互换匹配
- 对于贴图文件，会处理 UDIM 和序列帧模式

匹配结果会显示在列表的 "Matched Path" 列中。你可以：
- 双击修改匹配路径
- 勾选/取消勾选要修复的项目

#### 第三步：应用修复

确认匹配结果后，点击 **Apply Fixes** 按钮。

插件会对每个选中的项目执行路径修复。对于引用类型，插件会自动尝试多种修复策略，直到成功为止。

修复完成后，状态栏显示成功/失败数量。

> **路径模式**：Apply Fixes 默认使用**相对路径**模式（Relative Path），将修复后的路径记录为相对于当前 `.ma`/`.mb` 文件的相对路径。可通过界面底部的路径模式下拉框切换为绝对路径。

### 4.4 支持的文件类型

| 类型 | 支持的扩展名 |
|------|-------------|
| 引用 | .ma, .mb, .fbx, .abc |
| 贴图 | .png, .jpg, .jpeg, .tif, .tiff, .exr, .tga, .bmp, .tx, .hdr, .psd, .dds |
| 缓存 | .abc, .fbx |
| 音频 | 所有 Maya 支持的音频格式 |

### 4.5 常见问题

**Q：Auto Match 没有找到匹配怎么办？**

可能原因：
- 搜索目录范围不够大，文件不在扫描范围内
- 文件名已被重命名，无法自动匹配
- 解决方法：双击 "Matched Path" 列手动输入正确路径

**Q：Apply 后提示 "ALL strategies failed" 怎么办？**

这通常发生在引用文件类型不被识别的情况下。当前版本已支持 .fbx 和 .abc 类型的引用修复。如果仍然失败，可以尝试：
1. 在 Reference Editor 中手动替换路径
2. 先用 Safe Load References 加载引用后再修复

---

## 5. 功能三：Safe Load References（安全加载引用）

### 5.1 使用场景

通过 "Open Without References" 打开场景后，需要有选择地加载部分引用时使用。

### 5.2 界面说明

打开方式：**Pipeline Tools → Safe Load References**

界面显示场景中所有引用的列表：

| 列 | 说明 |
|----|------|
| ☑ (复选框) | 勾选后可通过 Load Selected 加载 |
| Status | 加载状态（Loaded / Unloaded） |
| Exists | 文件是否存在于磁盘（Yes / No） |
| Size | 文件大小（MB / KB） |
| File Path | 引用文件完整路径 |

### 5.3 操作

| 按钮 | 功能 |
|------|------|
| **Refresh** | 重新扫描引用列表 |
| **Load Selected** | 加载选中的引用 |
| **Load All** | 加载所有引用 |
| **Unload All** | 卸载所有引用 |
| **Remove Missing** | 移除文件不存在的引用 |

### 5.4 推荐工作流

1. 用 "Open Without References" 打开场景
2. 用 "Reference Checker" 修复缺失路径
3. 用 "Safe Load References" 逐个或批量加载引用

---

## 6. 功能四：Batch Animation Exporter（批量动画导出）

### 6.1 使用场景

当你需要从一个镜头场景中批量导出 **相机 / 骨骼 / BlendShape / 骨骼+BlendShape** 动画到 FBX 时使用。典型场景：

- 一个镜头里有多角色和相机，需要一次导出多个 FBX
- 需要统一命名规范，并支持人工微调输出文件名
- 需要在导出前统一烘焙动画，避免逐个手工 bake

### 6.2 界面说明

打开方式：**Pipeline Tools → Batch Animation Exporter**

| 区域 | 说明 |
|------|------|
| Output Dir | 输出目录，导出的 FBX 和日志会写入这里 |
| Frame Range | Timeline（时间线）或 Custom（自定义起止帧） |
| Generate Frame Range Log | 是否生成导出日志（默认开启） |
| 操作按钮 | Scan Scene / Select All / Select None / Export Selected / Cancel |
| ▶ FBX Export Options | FBX 选项折叠面板 |
| 导出列表 | 扫描到的可导出项（支持勾选与文件名编辑） |
| 进度条 & 状态栏 | 显示当前阶段与进度 |

### 6.3 实际导出流程（与代码一致）

点击 **Export Selected** 后，流程分三阶段：

1. **Phase 1（Baking）**：单次批量烘焙 BlendShape 权重
   - BlendShape 权重会一次性批量烘焙
   - 相机和骨骼不会在此阶段统一烘焙（相机在 Phase 2 逐帧采样导出；骨骼避免约束/IK 驱动骨架被提前烘焙成静态）
2. **Phase 2（Export）**：逐项导出 FBX，并支持中途取消（Cancel）
3. **Phase 3（Log）**：若勾选日志选项，生成 `导出区间 {start} - {end}.txt`

> 帧范围说明：在 **Timeline** 模式下，若本次选中项中包含相机，插件会优先读取**第一台相机**的关键帧范围，并用它覆盖导出范围。在 **Custom** 模式下，用户输入的帧范围始终被严格遵守，不会被相机覆盖。

### 6.4 相机导出行为（重要）

相机导出会创建一个临时相机，逐帧采样源相机的世界矩阵和 focalLength，确保导出的 FBX 包含完整的位置/旋转动画和焦距动画曲线。

- focalLength 在逐帧循环中直接从源相机采样并打 key，无论源相机的焦距是静态值、有关键帧、还是被表达式/约束驱动，都能保证每帧都有 key
- 这使得 UE Level Sequencer 能直接识别相机的 FOV，无需手动设置
- 其他相机属性（filmAperture、fStop、nearClipPlane 等）通过 connectAttr + bakeResults 烘焙

### 6.5 骨骼导出行为（重要）

为保证引擎导入质量，骨骼导出时会执行以下处理：

- 若传入节点不是 joint，会在其子层级中自动寻找最合适的根骨架
- 临时把根骨架提到 world 下导出，避免父级 Group/Null 污染 FBX 层级
- 仅对本次骨架链临时去命名空间（导出后恢复原场景命名）
- 导出前会校验骨骼名不包含 `:`；若源骨架是引用/只读导致无法改名，会自动走"临时复制骨架"路径后再导出
- 顶层骨骼若属于 root 语义（如 `Root_M`），会规范为 `Root` 或 `root`（大小写随文件习惯）
- 为确保骨骼层级可被稳定识别，导出时会强制开启 Skeleton Definitions

### 6.6 BlendShape 导出行为（重要）

BlendShape 导出用于将面部表情/变形动画导出为 FBX，供 UE 作为 MorphTarget 导入。

**FBX 选项说明**：

| 选项 | 默认 | 说明 |
|------|------|------|
| **Shapes** | 开启 | 导出 BlendShape 几何数据，面部动画必须开启 |
| **IncludeSkeleton** | 开启 | 导出时包含网格绑定的骨骼（如 Face_Root、Face01）。UE 导入 MorphTarget 时需要识别骨骼才能正确匹配 |
| **SmoothMesh** | 关闭 | 对导出网格应用细分，通常关闭以避免多边形暴增 |

**导出流程**：

1. 复制目标 Mesh（断开引用连接，保留 blendShape 变形器和已烘焙的动画关键帧）
2. 若 **IncludeSkeleton** 开启且 Mesh 有 skinCluster 绑定：
   - 保留 skinCluster，临时去除骨骼节点的命名空间（导出后自动恢复）
   - 导出 Mesh + 骨骼 + BlendShape 动画
3. 若 **IncludeSkeleton** 关闭：
   - 删除 skinCluster，仅导出 Mesh + BlendShape 动画
4. 导出完成后自动清理临时复制的 Mesh，场景恢复原状

> **提示**：若 UE 导入 MorphTarget FBX 时提示骨骼不匹配，请确认 IncludeSkeleton 已开启，并检查调试日志中的 `skelJoints` / `skelTransforms` 计数。



### 6.7 导出日志（Frame Range Log）

启用 **Generate Frame Range Log** 后，会在输出目录生成 `.txt` 日志，特点：

- 文件名：`导出区间 {start} - {end}.txt`（以实际导出帧范围命名，便于快速识别）
- 编码：UTF-8（含 BOM），便于 Windows 文本编辑器直接阅读
- 内容：除资源名外，其余字段为中文
- 结构：一行一条资源，格式更精简、便于查阅
- 调试：同目录还会生成 `BatchExportDebug_YYYYMMDD_HHMMSS.log`，用于排查导出细节

示例（示意）：

```text
==============================
导出日志
==============================
时间: 2026-02-14 15:47:34
导出区间: 【0 - 73】
总帧数: 73f
FPS: 30

[1] 相机 | TritzPV_Scene01_Shot08 | 文件: Cam_TritzPV_Scene01_Shot08.fbx | 导出区间: 【0 - 73】
[2] 骨骼 | SK_M113_Zealot | 文件: A_xxx.fbx | 导出区间: 【0 - 73】
```

### 6.8 常见问题

**Q：导出 FBX 里只有 group、没有骨骼层级怎么办？**

A：当前实现会优先导出 joint 根骨架，并临时剥离无关父层级。请确认 Scan Scene 识别到的 Skeleton 项是目标角色，并且该角色确实存在 joint 链。

可在 Maya Script Editor 搜索 `[BatchExportDebug]`，并结合输出目录中的 `BatchExportDebug_*.log`，重点检查 `constraintsCreated`、`sourceRootMotion`、`duplicateRootMotionBeforeExport` 等字段。

**Q：为什么导出的骨骼名不带命名空间？**

A：这是当前设计目标。导出时会临时去命名空间，保证骨骼名为引擎友好格式（例如 `Root`、`ShoulderPart1_R`），导出后会恢复 Maya 场景内原命名。

**Q：SkeletonDefs 关掉会怎样？**

A：当前版本为保证骨骼层级稳定导入，骨骼导出会强制开启 Skeleton Definitions（即使 UI 中关闭）。

**Q：BlendShape 导出提示失效怎么办？**

A：先确认目标 Mesh 的 history 中确实有 `blendShape` 节点。当前版本会在导出前做该检查；若失败，请在 Script Editor 中查看 `[BatchExportDebug]` 的 `historyNodes` / `blendShapeNodes` 计数。

**Q：UE 导入 BlendShape FBX 时提示骨骼不匹配怎么办？**

A：确认 FBX Export Options 中 BlendShape 的 **IncludeSkeleton** 已勾选（默认开启）。该选项会将 Mesh 绑定的骨骼（如 `Face_Root`、`Face01`）一起导出，UE 需要这些骨骼信息才能正确匹配 MorphTarget。

若仍有问题，检查调试日志中的 `skelJoints` 和 `skelTransforms` 计数是否大于 0。若为 0，说明 Mesh 没有 skinCluster 绑定，此时 IncludeSkeleton 不会生效。

**Q：为什么日志里有很多 `rename` 失败（只读节点）但仍可导出？**

A：当骨架来自引用文件时，Maya 不允许直接重命名。插件会自动切换到"临时复制骨架 → 约束烘焙 → 去命名空间 → 导出"的兜底流程，避免直接失败。


## 7. 推荐工作流程

### 场景迁移后的完整修复流程

```
1. Pipeline Tools → Open Without References
   → 选择迁移后的场景文件

2. Pipeline Tools → Reference Checker
   → Scan（扫描依赖）
   → Batch Locate Dir（选择新的资产根目录，自动匹配）
   → 检查匹配结果，手动修正不正确的匹配
   → Apply Fixes（应用修复）

3. Pipeline Tools → Safe Load References
   → Load All 或逐个 Load Selected
   → 确认引用加载正常

4. 保存场景
```

### 批量动画导出流程

```
1. 打开动画场景（正常打开或通过 Safe Open）

2. Pipeline Tools → Batch Animation Exporter
   → 设置 Output Dir
   → 设置 Frame Range
   → Scan Scene
   → 检查列表，调整文件名和选中项
   → 按需调整 FBX Export Options
   → Export Selected
   → 等待完成，检查结果
```

---

## 8. MEL 命令参考

插件注册了以下 MEL 命令，可以在 Script Editor 或脚本中直接调用：

| 命令 | 功能 |
|------|------|
| `safeOpenScene` | 打开文件选择对话框，以不加载引用的方式打开场景 |
| `refChecker` | 打开 Reference Checker 窗口 |
| `safeLoadRefs` | 打开 Safe Load References 窗口 |
| `batchAnimExporter` | 打开 Batch Animation Exporter 窗口 |

示例：在 Maya 启动脚本中自动打开 Reference Checker：

```mel
if (`pluginInfo -q -loaded "MayaRefCheckerPlugin"`) {
    refChecker;
}
```

---

## 9. 故障排除

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 菜单没有出现 | 插件未加载 | 在 Plug-in Manager 中检查并勾选 Loaded |
| 导出时提示 "Failed to load FBX plugin" | fbxmaya 插件未安装 | 在 Plug-in Manager 中加载 fbxmaya.mll |
| 路径修复失败 | 引用处于特殊状态 | 尝试先卸载再加载引用，或在 Reference Editor 中手动修复 |
| 扫描文件缓存很慢 | 搜索目录范围太大 | 选择更精确的搜索目录，避免扫描整个磁盘 |
| 烘焙后动画丢失 | 场景中有循环依赖或表达式冲突 | 检查 Maya Script Editor 中的警告信息 |
| 中文路径显示乱码 | 编码问题 | 插件已处理 Unicode 路径，如仍有问题请检查系统区域设置 |



