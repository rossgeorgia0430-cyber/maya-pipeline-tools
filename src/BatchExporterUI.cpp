#include "BatchExporterUI.h"
#include "SceneScanner.h"
#include "AnimExporter.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MQtUtil.h>
#include <maya/MString.h>

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QByteArray>

#include <algorithm>
#include <sstream>
#include <regex>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
static std::string toUtf8(const MString& ms) {
    const wchar_t* wstr = ms.asWChar();
    if (!wstr || !*wstr) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string(ms.asChar());
    std::string result(len, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    if (ret <= 0) return std::string(ms.asChar());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}
#else
static std::string toUtf8(const MString& ms) {
    return std::string(ms.asChar());
}
#endif

static QString utf8ToQString(const std::string& s) {
    return QString::fromUtf8(s.c_str(), static_cast<int>(s.size()));
}

static std::string qStringToUtf8(const QString& s) {
    QByteArray u8 = s.toUtf8();
    return std::string(u8.constData(), static_cast<size_t>(u8.size()));
}

static std::string trimScanTokenDelimiters(const std::string& s) {
    if (s.empty()) return s;
    size_t start = 0;
    while (start < s.size() && (s[start] == '_' || s[start] == ':' || s[start] == '|')) ++start;
    size_t end = s.size();
    while (end > start && (s[end - 1] == '_' || s[end - 1] == ':' || s[end - 1] == '|')) --end;
    return s.substr(start, end - start);
}

static bool fillMissingTokensFromText(const std::string& source,
                                      SceneTokens& tokens,
                                      std::string* matchedScene = nullptr,
                                      std::string* matchedShot = nullptr) {
    if (source.empty()) return false;

    std::regex sceneRe("(Scene[A-Za-z0-9]+)", std::regex::icase);
    std::regex shotRe("(Shot[A-Za-z0-9]+)", std::regex::icase);
    std::smatch sceneMatch;
    std::smatch shotMatch;

    bool hasScene = std::regex_search(source, sceneMatch, sceneRe);
    bool hasShot = std::regex_search(source, shotMatch, shotRe);

    if (matchedScene) *matchedScene = hasScene ? sceneMatch[1].str() : "";
    if (matchedShot) *matchedShot = hasShot ? shotMatch[1].str() : "";

    bool changed = false;
    if (tokens.scene.empty() && hasScene) {
        tokens.scene = sceneMatch[1].str();
        changed = true;
    }
    if (tokens.shot.empty() && hasShot) {
        tokens.shot = shotMatch[1].str();
        changed = true;
    }

    if (tokens.project.empty() && (hasScene || hasShot)) {
        size_t firstPos = source.size();
        if (hasScene && sceneMatch.position(0) >= 0) {
            firstPos = std::min(firstPos, static_cast<size_t>(sceneMatch.position(0)));
        }
        if (hasShot && shotMatch.position(0) >= 0) {
            firstPos = std::min(firstPos, static_cast<size_t>(shotMatch.position(0)));
        }

        if (firstPos < source.size()) {
            std::string project = trimScanTokenDelimiters(source.substr(0, firstPos));
            if (!project.empty()) {
                tokens.project = project;
                changed = true;
            }
        }
    }

    return changed;
}

// ============================================================================
// FilenameDelegate
// ============================================================================

FilenameDelegate::FilenameDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QWidget* FilenameDelegate::createEditor(QWidget* parent,
                                        const QStyleOptionViewItem& /*option*/,
                                        const QModelIndex& /*index*/) const
{
    QLineEdit* editor = new QLineEdit(parent);
    editor->setFrame(false);
    return editor;
}

void FilenameDelegate::setEditorData(QWidget* editor,
                                     const QModelIndex& index) const
{
    QString value = index.model()->data(index, Qt::EditRole).toString();
    QLineEdit* lineEdit = static_cast<QLineEdit*>(editor);
    lineEdit->setText(value);
}

void FilenameDelegate::setModelData(QWidget* editor,
                                    QAbstractItemModel* model,
                                    const QModelIndex& index) const
{
    QLineEdit* lineEdit = static_cast<QLineEdit*>(editor);
    QString value = lineEdit->text().trimmed();
    if (!value.isEmpty()) {
        model->setData(index, value, Qt::EditRole);
    }
}

// ============================================================================
// BatchExporterUI  --  static members
// ============================================================================

BatchExporterUI* BatchExporterUI::instance_ = nullptr;

BatchExporterUI* BatchExporterUI::instance()
{
    return instance_;
}

void BatchExporterUI::showUI()
{
    if (instance_) {
        instance_->raise();
        instance_->activateWindow();
        return;
    }

    QWidget* mayaMainWindow = MQtUtil::mainWindow();
    instance_ = new BatchExporterUI(mayaMainWindow);
    instance_->setAttribute(Qt::WA_DeleteOnClose, true);
    instance_->show();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

BatchExporterUI::BatchExporterUI(QWidget* parent)
    : QDialog(parent)
    , outputDirField_(nullptr)
    , radioTimeline_(nullptr)
    , radioCustom_(nullptr)
    , customStartSpin_(nullptr)
    , customEndSpin_(nullptr)
    , scanBtn_(nullptr)
    , selectAllBtn_(nullptr)
    , selectNoneBtn_(nullptr)
    , exportBtn_(nullptr)
    , cancelBtn_(nullptr)
    , fbxOptionsToggleBtn_(nullptr)
    , fbxOptionsContainer_(nullptr)
    , tableWidget_(nullptr)
    , progressBar_(nullptr)
    , statusBar_(nullptr)
    , skelAnimOnlyCheck_(nullptr)
    , skelBakeComplexCheck_(nullptr)
    , skelSkeletonDefsCheck_(nullptr)
    , skelConstraintsCheck_(nullptr)
    , skelInputConnsCheck_(nullptr)
    , skelBlendShapeCheck_(nullptr)
    , bsShapesCheck_(nullptr)
    , bsSmoothMeshCheck_(nullptr)
    , bsIncludeSkeletonCheck_(nullptr)
    , fbxVersionCombo_(nullptr)
    , fbxUpAxisCombo_(nullptr)
    , fpsOverrideCheck_(nullptr)
    , fpsOverrideSpin_(nullptr)
    , frameRangeLogCheck_(nullptr)
    , cancelRequested_(false)
{
    setupUI();
}

BatchExporterUI::~BatchExporterUI()
{
    instance_ = nullptr;
}

// ============================================================================
// setupUI
// ============================================================================

void BatchExporterUI::setupUI()
{
    setWindowTitle("Batch Animation Exporter");
    setMinimumSize(960, 580);
    resize(1020, 680);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 8);
    mainLayout->setSpacing(2);

    // ----- Output directory group -----
    {
        QGroupBox* outputGroup = new QGroupBox("Output Settings");
        QVBoxLayout* groupLayout = new QVBoxLayout(outputGroup);
        groupLayout->setContentsMargins(10, 14, 10, 10);
        groupLayout->setSpacing(8);

        // Output dir row
        {
            QHBoxLayout* row = new QHBoxLayout();
            QLabel* lbl = new QLabel("Output Dir:");
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);

            outputDirField_ = new QLineEdit();
            outputDirField_->setPlaceholderText("Select output directory...");
            outputDirField_->setMinimumHeight(26);
            outputDirField_->setToolTip(QString::fromUtf8(
                u8"FBX 文件的输出目录。\n"
                u8"点击右侧 Browse 按钮选择文件夹。"));
            row->addWidget(outputDirField_, 1);

            QPushButton* browseBtn = new QPushButton("Browse...");
            browseBtn->setToolTip(QString::fromUtf8(u8"选择 FBX 输出目录"));
            browseBtn->setMinimumHeight(26);
            browseBtn->setMinimumWidth(80);
            connect(browseBtn, &QPushButton::clicked, this, &BatchExporterUI::onBrowseOutput);
            row->addWidget(browseBtn);

            groupLayout->addLayout(row);
        }

        // Frame range row
        {
            QHBoxLayout* row = new QHBoxLayout();
            QLabel* lbl = new QLabel("Frame Range:");
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);

            QButtonGroup* frameGroup = new QButtonGroup(this);

            radioTimeline_ = new QRadioButton("Timeline");
            radioTimeline_->setChecked(true);
            radioTimeline_->setToolTip(QString::fromUtf8(
                u8"使用 Maya 时间线的起止帧范围。"));
            frameGroup->addButton(radioTimeline_);
            row->addWidget(radioTimeline_);

            radioCustom_ = new QRadioButton("Custom");
            radioCustom_->setToolTip(QString::fromUtf8(
                u8"手动指定导出的起止帧范围。"));
            frameGroup->addButton(radioCustom_);
            row->addWidget(radioCustom_);

            row->addSpacing(20);
            row->addWidget(new QLabel("Start:"));

            customStartSpin_ = new QSpinBox();
            customStartSpin_->setRange(-100000, 1000000);
            customStartSpin_->setValue(1);
            customStartSpin_->setEnabled(false);
            customStartSpin_->setMinimumWidth(70);
            customStartSpin_->setMinimumHeight(24);
            row->addWidget(customStartSpin_);

            row->addSpacing(6);
            row->addWidget(new QLabel("End:"));

            customEndSpin_ = new QSpinBox();
            customEndSpin_->setRange(-100000, 1000000);
            customEndSpin_->setValue(100);
            customEndSpin_->setEnabled(false);
            customEndSpin_->setMinimumWidth(70);
            customEndSpin_->setMinimumHeight(24);
            row->addWidget(customEndSpin_);

            row->addSpacing(20);

            fpsOverrideCheck_ = new QCheckBox("FPS Override:");
            fpsOverrideCheck_->setChecked(true);
            fpsOverrideCheck_->setToolTip(QString::fromUtf8(
                u8"强制以指定帧率导出 FBX。\n"
                u8"开启后会在导出前临时修改 Maya 场景的时间单位，\n"
                u8"确保 FBX 文件头写入正确的帧率，\n"
                u8"UE 导入时能正确识别。\n"
                u8"导出完成后会自动恢复原始设置。"));
            row->addWidget(fpsOverrideCheck_);

            fpsOverrideSpin_ = new QSpinBox();
            fpsOverrideSpin_->setRange(1, 120);
            fpsOverrideSpin_->setValue(30);
            fpsOverrideSpin_->setSuffix(" fps");
            fpsOverrideSpin_->setMinimumWidth(80);
            fpsOverrideSpin_->setMinimumHeight(24);
            fpsOverrideSpin_->setToolTip(QString::fromUtf8(
                u8"目标导出帧率。常用值：24、30、60。"));
            row->addWidget(fpsOverrideSpin_);

            connect(fpsOverrideCheck_, &QCheckBox::toggled,
                    fpsOverrideSpin_, &QSpinBox::setEnabled);

            row->addStretch();

            connect(radioTimeline_, &QRadioButton::toggled,
                    this, &BatchExporterUI::onFrameModeChanged);
            connect(radioCustom_, &QRadioButton::toggled,
                    this, &BatchExporterUI::onFrameModeChanged);

            groupLayout->addLayout(row);
        }

        // Frame Range Log checkbox
        {
            QHBoxLayout* row = new QHBoxLayout();
            QLabel* lbl = new QLabel("");
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);

            frameRangeLogCheck_ = new QCheckBox("Generate Frame Range Log");
            frameRangeLogCheck_->setChecked(true);
            frameRangeLogCheck_->setToolTip(
                QString::fromUtf8(
                    u8"导出后，会在 Export 目录下生成一个 .txt 日志，\n"
                    u8"它会列出每个导出项的实际关键帧范围和持续时间。"));
            row->addWidget(frameRangeLogCheck_);
            

            row->addStretch();
            groupLayout->addLayout(row);
        }

        mainLayout->addWidget(outputGroup);
    }

    mainLayout->addSpacing(2);

    // ----- FBX Options (toggle button + collapsible container) -----
    {
        // Use QChar for arrow symbols to avoid codepage / encoding issues
        // 0x25B6 = right-pointing triangle, 0x25BC = down-pointing triangle
        fbxOptionsToggleBtn_ = new QPushButton(
            QString(QChar(0x25B6)) + "  FBX Export Options");
        fbxOptionsToggleBtn_->setToolTip(QString::fromUtf8(
            u8"点击展开/折叠 FBX 导出的高级选项"));
        fbxOptionsToggleBtn_->setFlat(true);
        fbxOptionsToggleBtn_->setStyleSheet(
            "QPushButton { text-align: left; font-weight: bold;"
            " padding: 6px 10px; border: 1px solid palette(mid);"
            " border-radius: 3px; background: palette(window); }"
            "QPushButton:hover { background: palette(midlight); }");
        connect(fbxOptionsToggleBtn_, &QPushButton::clicked,
                this, &BatchExporterUI::onToggleFbxOptions);
        mainLayout->addWidget(fbxOptionsToggleBtn_);

        fbxOptionsContainer_ = new QWidget();
        fbxOptionsContainer_->setVisible(false);
        fbxOptionsContainer_->setStyleSheet(
            "QWidget#fbxOptionsContainer { border: 1px solid palette(mid);"
            " border-top: none; border-radius: 0 0 3px 3px; }");
        fbxOptionsContainer_->setObjectName("fbxOptionsContainer");
        QVBoxLayout* fbxLayout = new QVBoxLayout(fbxOptionsContainer_);
        fbxLayout->setContentsMargins(14, 10, 14, 10);
        fbxLayout->setSpacing(8);

        // Skeleton options row
        {
            QHBoxLayout* row = new QHBoxLayout();
            row->setSpacing(12);
            QLabel* lbl = new QLabel("Skeleton:");
            lbl->setStyleSheet("font-weight: bold;");
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);

            skelAnimOnlyCheck_ = new QCheckBox("AnimationOnly");
            skelAnimOnlyCheck_->setChecked(false);
            skelAnimOnlyCheck_->setToolTip(
                QString::fromUtf8(
                    u8"仅导出动画数据，不包含网格模型。\n"
                    u8"开启后只导出骨骼运动，\n"
                    u8"适用于引擎中已有独立网格文件的情况。\n"
                    u8"\n"
                    u8"注意：若该骨骼触发“Skel+BS(骨骼+BlendShape)合并导出”，\n"
                    u8"为了导出 MorphTarget，仍会导出网格/蒙皮（此项将被忽略）。"));
            row->addWidget(skelAnimOnlyCheck_);

            skelBakeComplexCheck_ = new QCheckBox("BakeComplex");
            skelBakeComplexCheck_->setChecked(true);
            skelBakeComplexCheck_->setToolTip(
                QString::fromUtf8(
                    u8"将复杂动画（约束、表达式、驱动关键帧等）\n"
                    u8"烘焙为简单关键帧。\n"
                    u8"建议保持开启，确保游戏引擎兼容性。"));
            row->addWidget(skelBakeComplexCheck_);

            skelSkeletonDefsCheck_ = new QCheckBox("SkeletonDefs");
            skelSkeletonDefsCheck_->setChecked(true);
            skelSkeletonDefsCheck_->setToolTip(
                QString::fromUtf8(
                    u8"在导出的 FBX 中包含骨骼定义（骨骼层级信息）。\n"
                    u8"引擎需要此信息来正确识别骨骼结构。\n"
                    u8"建议保持开启。"));
            row->addWidget(skelSkeletonDefsCheck_);

            skelConstraintsCheck_ = new QCheckBox("Constraints");
            skelConstraintsCheck_->setChecked(false);
            skelConstraintsCheck_->setToolTip(
                QString::fromUtf8(
                    u8"导出约束（目标、方向、父子等）。\n"
                    u8"通常关闭，因为大多数游戏引擎\n"
                    u8"不支持 Maya 约束。"));
            row->addWidget(skelConstraintsCheck_);

            skelInputConnsCheck_ = new QCheckBox("InputConns");
            skelInputConnsCheck_->setChecked(false);
            skelInputConnsCheck_->setToolTip(
                QString::fromUtf8(
                    u8"导出输入连接（动画曲线、驱动关键帧、表达式）。\n"
                    u8"通常关闭。仅在目标软件需要\n"
                    u8"读取原始动画图表时才开启。"));
            row->addWidget(skelInputConnsCheck_);

            skelBlendShapeCheck_ = new QCheckBox("BlendShape");
            skelBlendShapeCheck_->setChecked(true);
            skelBlendShapeCheck_->setToolTip(
                QString::fromUtf8(
                    u8"若骨骼的蒙皮网格上存在 BlendShape 变形器，\n"
                    u8"则将 BlendShape 权重动画与骨骼动画一并导出\n"
                    u8"到同一个 FBX 文件中。\n"
                    u8"适用于 UE 中需要同时驱动骨骼和表情的情况。"));
            row->addWidget(skelBlendShapeCheck_);

            row->addStretch();
            fbxLayout->addLayout(row);
        }

        // BlendShape options row
        {
            QHBoxLayout* row = new QHBoxLayout();
            row->setSpacing(12);
            QLabel* lbl = new QLabel("BlendShape:");
            lbl->setStyleSheet("font-weight: bold;");
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);

            bsShapesCheck_ = new QCheckBox("Shapes");
            bsShapesCheck_->setChecked(true);
            bsShapesCheck_->setToolTip(
                QString::fromUtf8(
                    u8"导出 BlendShape / 变形目标的几何数据。\n"
                    u8"面部动画和基于形状的变形\n"
                    u8"需要此选项才能在引擎中正常工作。"));
            row->addWidget(bsShapesCheck_);

            bsIncludeSkeletonCheck_ = new QCheckBox("IncludeSkeleton");
            bsIncludeSkeletonCheck_->setChecked(true);
            bsIncludeSkeletonCheck_->setToolTip(
                QString::fromUtf8(
                    u8"导出 BlendShape 时同时包含网格绑定的骨骼\n"
                    u8"（如 Face_Root、Face01）。\n"
                    u8"UE 导入 MorphTarget 时需要识别骨骼\n"
                    u8"才能正确匹配。\n"
                    u8"关闭后仅导出网格和 BlendShape 曲线，\n"
                    u8"不包含骨骼。"));
            row->addWidget(bsIncludeSkeletonCheck_);

            bsSmoothMeshCheck_ = new QCheckBox("SmoothMesh");
            bsSmoothMeshCheck_->setChecked(false);
            bsSmoothMeshCheck_->setToolTip(
                QString::fromUtf8(
                    u8"对导出的网格应用平滑网格预览（细分）。\n"
                    u8"通常关闭——会显著增加多边形数量。\n"
                    u8"仅在确实需要时才开启。"));
            row->addWidget(bsSmoothMeshCheck_);

            row->addStretch();
            fbxLayout->addLayout(row);
        }

        // Separator
        {
            QFrame* line = new QFrame();
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            fbxLayout->addWidget(line);
        }

        // Common options row
        {
            QHBoxLayout* row = new QHBoxLayout();
            row->setSpacing(12);
            QLabel* lbl = new QLabel("Common:");
            lbl->setStyleSheet("font-weight: bold;");
            lbl->setMinimumWidth(80);
            row->addWidget(lbl);

            QLabel* verLabel = new QLabel("File Version:");
            verLabel->setToolTip(
                QString::fromUtf8(u8"FBX 文件格式版本"));
            row->addWidget(verLabel);
            fbxVersionCombo_ = new QComboBox();
            fbxVersionCombo_->addItem("FBX202000");
            fbxVersionCombo_->addItem("FBX201800");
            fbxVersionCombo_->setMinimumWidth(110);
            fbxVersionCombo_->setToolTip(
                QString::fromUtf8(
                    u8"FBX 文件格式版本。\n"
                    u8"FBX202000 —— 推荐，与 UE4/5、Unity 2021+ 及新工具兼容性最佳。\n"
                    u8"FBX201800 —— 用于不支持 2020 格式的旧版引擎或流程。"));
            row->addWidget(fbxVersionCombo_);

            row->addSpacing(20);
            QLabel* axisLabel = new QLabel("Up Axis:");
            axisLabel->setToolTip(
                QString::fromUtf8(
                    u8"导出 FBX 中的世界向上轴方向"));
            row->addWidget(axisLabel);
            fbxUpAxisCombo_ = new QComboBox();
            fbxUpAxisCombo_->addItem("Y");
            fbxUpAxisCombo_->addItem("Z");
            fbxUpAxisCombo_->setMinimumWidth(60);
            fbxUpAxisCombo_->setToolTip(
                QString::fromUtf8(
                    u8"Y — Maya / UE 默认（Y 轴朝上）\n"
                    u8"Z — 3ds Max / Blender 默认（Z 轴朝上）"));
            row->addWidget(fbxUpAxisCombo_);

            row->addStretch();
            fbxLayout->addLayout(row);
        }

        mainLayout->addWidget(fbxOptionsContainer_);
    }

    mainLayout->addSpacing(4);

    // ----- Action buttons row -----
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->setSpacing(8);

        scanBtn_ = new QPushButton("Scan Scene");
        scanBtn_->setToolTip(QString::fromUtf8(
            u8"扫描当前场景，列出所有可导出的项目：\n"
            u8"相机动画、骨骼动画、BlendShape 动画。"));
        scanBtn_->setMinimumHeight(30);
        scanBtn_->setMinimumWidth(100);
        connect(scanBtn_, &QPushButton::clicked, this, &BatchExporterUI::onScanScene);
        row->addWidget(scanBtn_);

        row->addSpacing(4);

        selectAllBtn_ = new QPushButton("Select All");
        selectAllBtn_->setToolTip(QString::fromUtf8(u8"勾选列表中的所有项目"));
        selectAllBtn_->setMinimumHeight(30);
        connect(selectAllBtn_, &QPushButton::clicked, this, &BatchExporterUI::onSelectAll);
        row->addWidget(selectAllBtn_);

        selectNoneBtn_ = new QPushButton("Select None");
        selectNoneBtn_->setToolTip(QString::fromUtf8(u8"取消勾选列表中的所有项目"));
        selectNoneBtn_->setMinimumHeight(30);
        connect(selectNoneBtn_, &QPushButton::clicked, this, &BatchExporterUI::onSelectNone);
        row->addWidget(selectNoneBtn_);

        row->addStretch();

        exportBtn_ = new QPushButton("Export Selected");
        exportBtn_->setToolTip(QString::fromUtf8(
            u8"将列表中勾选的项目导出为 FBX 文件。\n"
            u8"请先设置输出目录和帧范围。"));
        exportBtn_->setMinimumWidth(130);
        exportBtn_->setMinimumHeight(32);
        exportBtn_->setStyleSheet(
            "QPushButton { font-weight: bold; }");
        connect(exportBtn_, &QPushButton::clicked, this, &BatchExporterUI::onExport);
        row->addWidget(exportBtn_);

        cancelBtn_ = new QPushButton("Cancel Export");
        cancelBtn_->setToolTip(QString::fromUtf8(u8"取消正在进行的导出操作"));
        cancelBtn_->setMinimumWidth(130);
        cancelBtn_->setMinimumHeight(32);
        cancelBtn_->setStyleSheet(
            "QPushButton { background-color: #994444; color: white; font-weight: bold; }");
        cancelBtn_->setVisible(false);
        connect(cancelBtn_, &QPushButton::clicked, this, &BatchExporterUI::onCancel);
        row->addWidget(cancelBtn_);

        mainLayout->addLayout(row);
    }

    mainLayout->addSpacing(2);

    // ----- Table -----
    {
        tableWidget_ = new QTableWidget(0, 5);
        tableWidget_->setHorizontalHeaderLabels(
            QStringList() << "" << "Type" << "Source" << "Output Filename" << "Status");

        QHeaderView* header = tableWidget_->horizontalHeader();
        header->resizeSection(0, 32);   // Checkbox
        header->resizeSection(1, 90);   // Type
        header->resizeSection(2, 280);  // Source
        header->resizeSection(3, 380);  // Output Filename
        header->resizeSection(4, 90);   // Status
        header->setStretchLastSection(true);

        tableWidget_->verticalHeader()->setVisible(false);
        tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
        tableWidget_->setAlternatingRowColors(true);

        // Install the filename delegate on column 3 (Output Filename)
        FilenameDelegate* fnDelegate = new FilenameDelegate(tableWidget_);
        tableWidget_->setItemDelegateForColumn(3, fnDelegate);

        connect(tableWidget_, &QTableWidget::cellChanged,
                this, &BatchExporterUI::onCheckboxChanged);

        mainLayout->addWidget(tableWidget_, 1);
    }

    // ----- Progress bar -----
    {
        progressBar_ = new QProgressBar();
        progressBar_->setRange(0, 100);
        progressBar_->setValue(0);
        progressBar_->setTextVisible(true);
        progressBar_->setMinimumHeight(20);
        mainLayout->addWidget(progressBar_);
    }

    // ----- Status bar -----
    {
        statusBar_ = new QStatusBar();
        statusBar_->setSizeGripEnabled(false);
        statusBar_->showMessage("Ready. Click \"Scan Scene\" to begin.");
        mainLayout->addWidget(statusBar_);
    }
}

// ============================================================================
// Slots
// ============================================================================

void BatchExporterUI::onBrowseOutput()
{
    QFileDialog dlg(this, "Select Output Directory", outputDirField_->text());
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setOption(QFileDialog::ShowDirsOnly, true);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedFiles().isEmpty()) {
        outputDirField_->setText(dlg.selectedFiles().first());
    }
}

void BatchExporterUI::onFrameModeChanged()
{
    bool custom = radioCustom_->isChecked();
    customStartSpin_->setEnabled(custom);
    customEndSpin_->setEnabled(custom);
}

// ============================================================================
// onScanScene
// ============================================================================

void BatchExporterUI::onScanScene()
{
    setStatus("Scanning scene...");
    QApplication::processEvents();

    exportItems_.clear();

    // Parse scene tokens once
    SceneTokens tokens = NamingUtils::parseSceneTokens();

    auto tokensReady = [&](const SceneTokens& t) {
        return !t.project.empty() && !t.scene.empty() && !t.shot.empty();
    };

    auto logTokens = [&](const char* stage) {
        std::ostringstream dbg;
        dbg << "scan tokens(" << stage << "): project='" << tokens.project
            << "', scene='" << tokens.scene
            << "', shot='" << tokens.shot
            << "', basename='" << tokens.basename << "'";
        PluginLog::info("BatchExporter", dbg.str());
    };

    logTokens("initial");

    // Scan scene first, then infer missing tokens from actual node paths/names.
    std::vector<CameraInfo> cameras = SceneScanner::findNonDefaultCameras();
    std::vector<CharacterInfo> characters = SceneScanner::findCharacters();
    std::vector<BlendShapeGroupInfo> bsGroups = SceneScanner::findBlendShapeGroups();

    if (!tokensReady(tokens)) {
        int attempts = 0;
        int hits = 0;

        auto tryInfer = [&](const std::string& source, const char* tag) {
            if (source.empty() || tokensReady(tokens)) return;
            ++attempts;

            std::string matchedScene;
            std::string matchedShot;
            if (!fillMissingTokensFromText(source, tokens, &matchedScene, &matchedShot)) {
                return;
            }

            ++hits;
            std::ostringstream dbg;
            dbg << "token inference: tag=" << tag
                << ", source='" << source << "'"
                << ", matchedScene='" << matchedScene << "'"
                << ", matchedShot='" << matchedShot << "'"
                << ", result{project='" << tokens.project
                << "', scene='" << tokens.scene
                << "', shot='" << tokens.shot << "'}";
            PluginLog::info("BatchExporter", dbg.str());
        };

        // Prefer skeleton sources first: they usually carry the most stable namespace tokens.
        for (const auto& ch : characters) {
            tryInfer(ch.nsOrName, "character.nsOrName");
            tryInfer(ch.rootJoint, "character.rootJoint");
            tryInfer(ch.display, "character.display");
        }
        for (const auto& cam : cameras) {
            tryInfer(cam.transform, "camera.transform");
            tryInfer(cam.display, "camera.display");
        }
        for (const auto& bs : bsGroups) {
            tryInfer(bs.nsOrName, "blendshape.nsOrName");
            tryInfer(bs.mesh, "blendshape.mesh");
            tryInfer(bs.display, "blendshape.display");
        }

        std::ostringstream dbg;
        dbg << "token inference summary: attempts=" << attempts
            << ", hits=" << hits
            << ", ready=" << (tokensReady(tokens) ? "true" : "false");
        PluginLog::info("BatchExporter", dbg.str());

        if (!tokensReady(tokens)) {
            PluginLog::warn("BatchExporter", "token inference incomplete: filenames may miss project/scene/shot");
        }
    }

    logTokens("final");

    // --- Cameras ---
    for (const auto& cam : cameras) {
        ExportItem item;
        item.type     = "camera";
        item.node     = cam.transform;
        item.name     = cam.display;
        item.nsOrName = cam.display;
        item.filename = NamingUtils::buildCameraFilename(cam.transform, tokens);
        item.selected = true;
        item.status   = "pending";
        item.message.clear();
        exportItems_.push_back(item);
    }

    // --- Characters (skeletons) ---
    for (const auto& ch : characters) {
        ExportItem item;
        item.type     = "skeleton";
        item.node     = ch.rootJoint;
        item.name     = ch.display;
        item.nsOrName = ch.nsOrName;
        item.filename = NamingUtils::buildSkeletonFilename(ch.nsOrName, tokens);
        item.selected = true;
        item.status   = "pending";
        item.message.clear();

        // Check if root bone needs rename during export
        {
            std::string sn = ch.rootJoint;
            // Get leaf name (last segment after |)
            size_t pos = sn.rfind('|');
            if (pos != std::string::npos) sn = sn.substr(pos + 1);
            // Strip namespace (last segment after :)
            pos = sn.rfind(':');
            if (pos != std::string::npos) sn = sn.substr(pos + 1);
            // Case-insensitive check
            std::string lower = sn;
            for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (lower != "root") {
                item.message = "Root bone \"" + sn + "\" will be renamed to \"Root\" during export";
            }
        }

        exportItems_.push_back(item);
    }

    // --- BlendShape groups ---
    for (const auto& bs : bsGroups) {
        ExportItem item;
        item.type     = "blendshape";
        item.node     = bs.mesh;
        item.name     = bs.display;
        item.nsOrName = bs.nsOrName;
        item.filename = NamingUtils::buildBlendShapeFilename(bs.nsOrName, tokens);
        item.selected = true;
        item.status   = "pending";
        item.message.clear();
        exportItems_.push_back(item);
    }

    // --- Skeleton+BlendShape detection: attach BS info to matching skeleton items ---
    std::vector<SkeletonBlendShapeInfo> skelBsCombos = SceneScanner::findSkeletonBlendShapeCombos();
    for (const auto& combo : skelBsCombos) {
        // Find the matching skeleton item by root joint path
        for (auto& item : exportItems_) {
            if (item.type == "skeleton" && item.node == combo.rootJoint) {
                item.bsMeshes      = combo.bsMeshes;
                 item.bsNodes       = combo.bsNodes;
                 item.bsWeightAttrs = combo.bsWeightAttrs;
                 // Change type so UI and export dispatch can distinguish Skel+BS from pure skeleton
                 item.type = "skeleton+blendshape";
                 item.name = combo.display;
                 // Use the combined naming rule for the output filename
                 item.filename = NamingUtils::buildSkeletonBlendShapeFilename(item.nsOrName, tokens);
                 {
                     std::ostringstream dbg;
                     dbg << "scan: promoted skeleton to skeleton+blendshape '" << item.nsOrName
                        << "': " << combo.bsMeshes.size() << " BS meshes, "
                        << combo.bsWeightAttrs.size() << " weight attrs"
                        << ", filename='" << item.filename << "'";
                    PluginLog::info("BatchExporter", dbg.str());
                }
                break;
            }
        }
    }

    // Resolve duplicate filenames
    NamingUtils::deduplicateFilenames(exportItems_, tokens);

    for (size_t i = 0; i < exportItems_.size(); ++i) {
        const ExportItem& item = exportItems_[i];
        std::ostringstream dbg;
        dbg << "scan item[" << (i + 1) << "]: type=" << item.type
            << ", nsOrName='" << item.nsOrName << "'"
            << ", filename='" << item.filename << "'";
        if (!item.bsWeightAttrs.empty()) {
            dbg << ", bsWeightAttrs=" << item.bsWeightAttrs.size();
        }
        PluginLog::info("BatchExporter", dbg.str());
    }

    refreshList();

    int skelWithBs = 0;
    for (const auto& item : exportItems_) {
        if (item.type == "skeleton+blendshape") ++skelWithBs;
    }

    std::ostringstream oss;
    oss << "Scan complete: " << exportItems_.size() << " item(s) found ("
        << cameras.size() << " cameras, "
        << characters.size() << " skeletons";
    if (skelWithBs > 0) {
        oss << " (" << skelWithBs << " with BlendShape)";
    }
    oss << ", " << bsGroups.size() << " blendshape groups).";
    setStatus(oss.str());
}


// ============================================================================
// onSelectAll / onSelectNone
// ============================================================================

void BatchExporterUI::onSelectAll()
{
    for (auto& item : exportItems_) {
        item.selected = true;
    }
    refreshList();
}

void BatchExporterUI::onSelectNone()
{
    for (auto& item : exportItems_) {
        item.selected = false;
    }
    refreshList();
}

// ============================================================================
// onCheckboxChanged
// ============================================================================

void BatchExporterUI::onCheckboxChanged(int row, int col)
{
    if (col != 0) return;
    if (row < 0 || row >= static_cast<int>(exportItems_.size())) return;

    QTableWidgetItem* checkItem = tableWidget_->item(row, 0);
    if (!checkItem) return;

    exportItems_[row].selected =
        (checkItem->checkState() == Qt::Checked);
}

// ============================================================================
// onExport
// ============================================================================

void BatchExporterUI::onExport()
{
    // Sync any user-edited filenames back into exportItems_
    syncFilenamesFromUI();

    // --- Validate output directory ---
    QString outDir = outputDirField_->text().trimmed();
    if (outDir.isEmpty()) {
        QMessageBox::warning(this, "Export", "Please select an output directory.");
        return;
    }
    QDir dir(outDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            QMessageBox::critical(this, "Export",
                "Could not create output directory:\n" + outDir);
            return;
        }
    }

    // --- Validate frame range ---
    std::pair<int, int> range = getFrameRange();
    int startFrame = range.first;
    int endFrame   = range.second;
    if (endFrame < startFrame) {
        QMessageBox::warning(this, "Export",
            "Invalid frame range: end frame is before start frame.");
        return;
    }

    // --- Collect selected items ---
    std::vector<ExportItem> selectedItems;
    std::vector<size_t> selectedIndices;
    for (size_t i = 0; i < exportItems_.size(); ++i) {
        if (exportItems_[i].selected) {
            selectedIndices.push_back(i);
            selectedItems.push_back(exportItems_[i]);
        }
    }

    {
        std::ostringstream dbg;
        dbg << "export start: selectedItems=" << selectedItems.size()
            << ", uiRange=" << startFrame << "-" << endFrame;
        PluginLog::info("BatchExporter", dbg.str());
    }

    // --- Optional camera-driven range override (Timeline mode only) ---
    // In Custom mode, user-entered range must be respected exactly.
    bool rangeOverriddenByCamera = false;
    std::string rangeCameraNode;
    const bool customRangeMode = (radioCustom_ && radioCustom_->isChecked());
    if (!customRangeMode) {
        for (const auto& item : selectedItems) {
            if (item.type == "camera") {
                FrameRangeInfo camRange = AnimExporter::queryFrameRange(item);
                if (camRange.valid) {
                    startFrame = static_cast<int>(camRange.firstKey);
                    endFrame   = static_cast<int>(camRange.lastKey);
                    rangeOverriddenByCamera = true;
                    rangeCameraNode = item.node;
                }
                break;
            }
        }
    }

    {
        std::ostringstream dbg;
        dbg << "export range resolved: finalRange=" << startFrame << "-" << endFrame
            << ", overriddenByCamera=" << (rangeOverriddenByCamera ? "true" : "false")
            << ", cameraNode='" << (rangeCameraNode.empty() ? "<none>" : rangeCameraNode) << "'";
        PluginLog::info("BatchExporter", dbg.str());
    }

    if (selectedIndices.empty()) {
        QMessageBox::information(this, "Export", "No items selected for export.");
        return;
    }

    struct EnvGuard {
        bool hadStart = false;
        bool hadEnd = false;
        bool hadDebug = false;
        QByteArray prevStart;
        QByteArray prevEnd;
        QByteArray prevDebug;

        EnvGuard() {
            hadStart = qEnvironmentVariableIsSet("MAYA_REF_EXPORT_RANGE_START");
            hadEnd = qEnvironmentVariableIsSet("MAYA_REF_EXPORT_RANGE_END");
            hadDebug = qEnvironmentVariableIsSet("MAYA_REF_EXPORT_DEBUG_LOG");
            prevStart = qgetenv("MAYA_REF_EXPORT_RANGE_START");
            prevEnd = qgetenv("MAYA_REF_EXPORT_RANGE_END");
            prevDebug = qgetenv("MAYA_REF_EXPORT_DEBUG_LOG");
        }

        void setRange(int startFrame, int endFrame) {
            qputenv("MAYA_REF_EXPORT_RANGE_START", QString::number(startFrame).toUtf8());
            qputenv("MAYA_REF_EXPORT_RANGE_END", QString::number(endFrame).toUtf8());
        }

        void setDebugLogPath(const QByteArray& path) {
            qputenv("MAYA_REF_EXPORT_DEBUG_LOG", path);
        }

        ~EnvGuard() {
            if (hadStart) qputenv("MAYA_REF_EXPORT_RANGE_START", prevStart);
            else qunsetenv("MAYA_REF_EXPORT_RANGE_START");

            if (hadEnd) qputenv("MAYA_REF_EXPORT_RANGE_END", prevEnd);
            else qunsetenv("MAYA_REF_EXPORT_RANGE_END");

            if (hadDebug) qputenv("MAYA_REF_EXPORT_DEBUG_LOG", prevDebug);
            else qunsetenv("MAYA_REF_EXPORT_DEBUG_LOG");
        }
    } envGuard;

    // Provide export range hints to AnimExporter::queryFrameRange (for constraint-driven rigs)
    envGuard.setRange(startFrame, endFrame);

    for (size_t i = 0; i < selectedItems.size(); ++i) {
        const ExportItem& item = selectedItems[i];
        std::ostringstream dbg;
        dbg << "export item[" << (i + 1) << "]: type=" << item.type
            << ", node='" << item.node << "'"
            << ", filename='" << item.filename << "'";
        PluginLog::info("BatchExporter", dbg.str());
    }

    // --- Ensure FBX plugin ---
    if (!AnimExporter::ensureFbxPlugin()) {
        QMessageBox::critical(this, "Export",
            "Failed to load the FBX plugin (fbxmaya). Export aborted.");
        return;
    }

    // --- Playback range override (critical for UE "Exported Time" import mode) ---
    // UE uses the FBX take/local time span when AnimationLength=ExportedTime.
    // Maya's FBX exporter commonly derives that span from Maya playbackOptions
    // (animationStart/End + min/max), which can become inconsistent after changing
    // scene time unit. We clamp playbackOptions to the requested export range so
    // the FBX take span matches exactly [startFrame, endFrame] at the export fps.
    struct PlaybackRangeGuard {
        bool captured = false;
        double prevMin = 0.0;
        double prevMax = 0.0;
        double prevAnimStart = 0.0;
        double prevAnimEnd = 0.0;

        PlaybackRangeGuard() {
            if (MGlobal::executeCommand("playbackOptions -q -minTime", prevMin) != MS::kSuccess) return;
            if (MGlobal::executeCommand("playbackOptions -q -maxTime", prevMax) != MS::kSuccess) return;
            if (MGlobal::executeCommand("playbackOptions -q -animationStartTime", prevAnimStart) != MS::kSuccess) return;
            if (MGlobal::executeCommand("playbackOptions -q -animationEndTime", prevAnimEnd) != MS::kSuccess) return;
            captured = true;
        }

        void setRange(double startFrame, double endFrame) {
            if (!captured) return;
            if (endFrame < startFrame) std::swap(startFrame, endFrame);
            std::ostringstream cmd;
            cmd << "playbackOptions"
                << " -minTime " << startFrame
                << " -maxTime " << endFrame
                << " -animationStartTime " << startFrame
                << " -animationEndTime " << endFrame;
            MGlobal::executeCommand(cmd.str().c_str());

            std::ostringstream dbg;
            dbg << "Playback range override: min/max & animStart/End -> "
                << static_cast<int>(startFrame) << "-" << static_cast<int>(endFrame);
            PluginLog::info("BatchExporter", dbg.str());
        }

        ~PlaybackRangeGuard() {
            if (!captured) return;
            std::ostringstream cmd;
            cmd << "playbackOptions"
                << " -minTime " << prevMin
                << " -maxTime " << prevMax
                << " -animationStartTime " << prevAnimStart
                << " -animationEndTime " << prevAnimEnd;
            MGlobal::executeCommand(cmd.str().c_str());
        }
    } playbackGuard;

    // --- FPS Override ---
    double exportFps = AnimExporter::querySceneFps();
    std::string prevTimeUnit;
    bool fpsOverridden = false;
    if (fpsOverrideCheck_ && fpsOverrideCheck_->isChecked() && fpsOverrideSpin_) {
        double targetFps = static_cast<double>(fpsOverrideSpin_->value());
        double sceneFps = exportFps;
        exportFps = targetFps;
        prevTimeUnit = AnimExporter::setSceneTimeUnit(targetFps);
        fpsOverridden = true;

        std::ostringstream dbg;
        dbg << "FPS override: scene=" << sceneFps << " -> export=" << targetFps;
        PluginLog::info("BatchExporter", dbg.str());
    }

    // Clamp Maya playback range after any time-unit change so FBX take span matches export range.
    playbackGuard.setRange(startFrame, endFrame);

    // --- Collect FBX options from UI ---
    FbxExportOptions fbxOpts = collectFbxOptions();
    {
        std::ostringstream dbg;
        dbg << "fbx options: "
            << "skel{AnimationOnly=" << (fbxOpts.skelAnimationOnly ? "true" : "false")
            << ", BakeComplex=" << (fbxOpts.skelBakeComplex ? "true" : "false")
            << ", SkeletonDefs=" << (fbxOpts.skelSkeletonDefs ? "true" : "false")
            << ", Constraints=" << (fbxOpts.skelConstraints ? "true" : "false")
            << ", InputConns=" << (fbxOpts.skelInputConns ? "true" : "false")
            << ", BlendShape=" << (fbxOpts.skelBlendShape ? "true" : "false")
            << "}, bs{Shapes=" << (fbxOpts.bsShapes ? "true" : "false")
            << ", IncludeSkeleton=" << (fbxOpts.bsIncludeSkeleton ? "true" : "false")
            << ", SmoothMesh=" << (fbxOpts.bsSmoothMesh ? "true" : "false")
            << "}, common{fileVersion=" << fbxOpts.fileVersion
            << ", upAxis=" << fbxOpts.upAxis << "}";
        PluginLog::info("BatchExporter", dbg.str());
    }

    // --- Bind per-run debug file path (used by AnimExporter debug helpers) ---
    QString debugStamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString debugLogPath = dir.absoluteFilePath("BatchExportDebug_" + debugStamp + ".log");
    envGuard.setDebugLogPath(debugLogPath.toUtf8());
    {
        std::string infoMsg = "Debug log file: " + qStringToUtf8(debugLogPath);
        PluginLog::info("BatchExporter", infoMsg);
    }

    // --- Generate export_log at the very beginning of export ---
    // Filename: "导出区间 {start} - {end}.txt"  (no colon — illegal on Windows)
    QString exportLogName = QString::fromUtf8(u8"\u5bfc\u51fa\u533a\u95f4 %1 - %2.txt")
        .arg(startFrame).arg(endFrame);
    QString exportLogPath = dir.absoluteFilePath(exportLogName);
    {
        std::string logPathUtf8 = qStringToUtf8(exportLogPath);
#ifdef _WIN32
        std::wstring wLogPath;
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, logPathUtf8.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                wLogPath.resize(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, logPathUtf8.c_str(), -1, &wLogPath[0], wlen);
                if (!wLogPath.empty() && wLogPath.back() == L'\0') wLogPath.pop_back();
            }
        }
        std::ofstream elog(wLogPath, std::ios::binary);
#else
        std::ofstream elog(logPathUtf8, std::ios::binary);
#endif
        if (elog.is_open()) {
            elog << "\xEF\xBB\xBF"; // UTF-8 BOM
            elog.close();
            PluginLog::info("BatchExporter", "Export log created: " + logPathUtf8);
        }
    }

    // --- Switch to exporting UI state ---
    cancelRequested_ = false;
    setExportingUI(true);

    int totalItems = static_cast<int>(selectedIndices.size());
    progressBar_->setRange(0, totalItems + 1); // +1 for bake phase
    progressBar_->setValue(0);
    progressBar_->setFormat("Baking... %v / %m");

    // =====================================================================
    // Phase 1: Batch-bake all selected items in a single pass
    // =====================================================================
    setStatus("Phase 1: Baking animations... (this may take a while)");
    progressBar_->setFormat("Baking animations...");
    progressBar_->setRange(0, 0); // indeterminate / busy indicator
    QApplication::processEvents();

    // Honor the UI "BlendShape" flag: if disabled, do not bake BS attrs from skeleton+blendshape
    // items (they may still exist in scan results).
    if (!fbxOpts.skelBlendShape) {
        for (auto& it : selectedItems) {
            it.bsWeightAttrs.clear();
        }
    }

    std::set<int> failedBakeIndices = AnimExporter::batchBakeAll(selectedItems, startFrame, endFrame);

    // Mark items that failed during bake collection
    for (int fi : failedBakeIndices) {
        if (fi >= 0 && fi < static_cast<int>(selectedIndices.size())) {
            size_t idx = selectedIndices[fi];
            exportItems_[idx].status = "error";
            if (exportItems_[idx].type == "camera") {
                exportItems_[idx].message = "Camera node missing";
            } else if (exportItems_[idx].type == "skeleton" || exportItems_[idx].type == "skeleton+blendshape") {
                exportItems_[idx].message = "Skeleton root missing";
            } else {
                exportItems_[idx].message = "Node missing or no blendShape found";
            }
        }
    }

    // Restore determinate progress for export phase
    progressBar_->setRange(0, totalItems);
    progressBar_->setValue(0);
    progressBar_->setFormat("Exporting %v / %m");
    refreshList();
    QApplication::processEvents();

    // =====================================================================
    // Phase 2: Export each selected item to FBX
    // =====================================================================
    setStatus("Phase 2: Exporting FBX files...");
    QApplication::processEvents();

    int exportedCount = 0;
    int errorCount    = 0;
    int cancelledCount = 0;

    for (int i = 0; i < totalItems; ++i) {
        // Check cancel flag before each item
        if (cancelRequested_) {
            // Mark remaining items as cancelled
            for (int j = i; j < totalItems; ++j) {
                size_t jIdx = selectedIndices[j];
                if (exportItems_[jIdx].status == "pending" ||
                    exportItems_[jIdx].status == "exporting") {
                    exportItems_[jIdx].status = "cancelled";
                    exportItems_[jIdx].message = "Cancelled by user";
                    ++cancelledCount;
                }
            }
            break;
        }

        size_t idx = selectedIndices[i];
        ExportItem& item = exportItems_[idx];

        // Skip items that failed during bake
        if (failedBakeIndices.count(i)) {
            ++errorCount;
            progressBar_->setValue(i + 1);
            QApplication::processEvents();
            continue;
        }

        item.status = "exporting";
        setStatus("Exporting [" + std::to_string(i + 1) + "/"
                  + std::to_string(totalItems) + "]: " + item.name);
        refreshList();
        QApplication::processEvents();

        // Build full output path
        std::string outputPath =
            qStringToUtf8(dir.absoluteFilePath(utf8ToQString(item.filename)));

        ExportResult result;

        if (item.type == "camera") {
            result = AnimExporter::exportCameraFbx(
                item.node, outputPath, startFrame, endFrame, fbxOpts);
        } else if (item.type == "skeleton+blendshape") {
            if (fbxOpts.skelBlendShape && !item.bsWeightAttrs.empty()) {
                // Combined skeleton+blendshape export
                result = AnimExporter::exportSkeletonBlendShapeFbx(
                    item.node, item.bsMeshes, item.bsWeightAttrs,
                    outputPath, startFrame, endFrame, fbxOpts);
            } else {
                // User disabled Skel+BS export; fall back to skeleton-only.
                result = AnimExporter::exportSkeletonFbx(
                    item.node, outputPath, startFrame, endFrame, fbxOpts);
            }
        } else if (item.type == "skeleton") {
            result = AnimExporter::exportSkeletonFbx(
                item.node, outputPath, startFrame, endFrame, fbxOpts);
        } else if (item.type == "blendshape") {
            result = AnimExporter::exportBlendShapeFbx(
                item.node, outputPath, startFrame, endFrame, fbxOpts);
        } else {
            item.status  = "error";
            item.message = "Unknown export type: " + item.type;
            ++errorCount;
            progressBar_->setValue(i + 1);
            QApplication::processEvents();
            continue;
        }

        if (result.success) {
            item.status = "done";
            std::ostringstream msg;
            msg << "OK";
            if (result.fileSize > 0) {
                msg << " (" << (result.fileSize / 1024) << "KB)";
            }
            if (!result.warnings.empty()) {
                msg << " " << result.warnings.size() << " warning(s)";
            }
            item.message = msg.str();
            ++exportedCount;
        } else {
            item.status = "error";
            if (!result.errors.empty()) {
                item.message = result.errors.front();
            } else {
                item.message = "Export failed (unknown error)";
            }
            ++errorCount;
        }

        progressBar_->setValue(i + 1);
        refreshList();
        QApplication::processEvents();
    }
    // =====================================================================
    // Phase 3: Generate export log (if checkbox is checked)
    // =====================================================================
    if (frameRangeLogCheck_ && frameRangeLogCheck_->isChecked() && exportedCount > 0) {
        setStatus("Phase 3: Writing export log...");
        QApplication::processEvents();

        std::vector<FrameRangeInfo> ranges;
        for (size_t i = 0; i < selectedIndices.size(); ++i) {
            size_t idx = selectedIndices[i];
            if (exportItems_[idx].status == "done") {
                FrameRangeInfo fri;
                fri.name     = exportItems_[idx].name;
                fri.type     = exportItems_[idx].type;
                fri.filename = exportItems_[idx].filename;
                ranges.push_back(fri);
            }
        }

        if (!ranges.empty()) {
            std::string logPath = AnimExporter::writeFrameRangeLog(
                qStringToUtf8(outDir), ranges, startFrame, endFrame, exportFps);

            if (!logPath.empty()) {
                std::string infoMsg = "FrameRangeLog written to: " + logPath;
                PluginLog::info("BatchExporter", infoMsg);
            }
        }
    }

    // --- Restore FPS ---
    if (fpsOverridden) {
        AnimExporter::restoreSceneTimeUnit(prevTimeUnit);
    }

    // --- Restore UI state ---
    setExportingUI(false);
    progressBar_->setFormat("%p%");
    progressBar_->setRange(0, 100);
    progressBar_->setValue(100);

    // --- Summary ---
    std::ostringstream summary;
    summary << "Export complete: " << exportedCount << " succeeded, "
            << errorCount << " failed";
    if (cancelledCount > 0) {
        summary << ", " << cancelledCount << " cancelled";
    }
    summary << " out of " << totalItems << " selected.";
    setStatus(summary.str());

    QString msgText = utf8ToQString(summary.str());
    if (cancelledCount > 0) {
        QMessageBox::warning(this, "Export Cancelled", msgText);
    } else if (errorCount > 0) {
        QMessageBox::warning(this, "Export Complete", msgText);
    } else {
        QMessageBox::information(this, "Export Complete", msgText);
    }

    // Log scan summary
    {
        PluginLog::ScanSummary ss;
        ss.module = "BatchExporter";
        MString sceneName;
        MGlobal::executeCommand("file -q -sn", sceneName);
        ss.scenePath = toUtf8(sceneName);
        ss.totalItems = totalItems;
        ss.okItems = exportedCount;
        ss.missingItems = errorCount;
        if (cancelledCount > 0) {
            ss.notes.push_back("Cancelled: " + std::to_string(cancelledCount));
        }
        ss.notes.push_back("Output: " + qStringToUtf8(outDir));
        PluginLog::logScanSummary(ss);
    }
}

// ============================================================================
// onCancel
// ============================================================================

void BatchExporterUI::onCancel()
{
    cancelRequested_ = true;
    setStatus("Cancelling... will stop after current item finishes.");
}

// ============================================================================
// onToggleFbxOptions — show/hide the FBX options panel
// ============================================================================

void BatchExporterUI::onToggleFbxOptions()
{
    bool visible = !fbxOptionsContainer_->isVisible();
    fbxOptionsContainer_->setVisible(visible);
    // 0x25BC = down-pointing triangle, 0x25B6 = right-pointing triangle
    fbxOptionsToggleBtn_->setText(visible
        ? QString(QChar(0x25BC)) + "  FBX Export Options"
        : QString(QChar(0x25B6)) + "  FBX Export Options");
}

// ============================================================================
// setExportingUI — toggle UI between normal and exporting states
// ============================================================================

void BatchExporterUI::setExportingUI(bool exporting)
{
    scanBtn_->setEnabled(!exporting);
    selectAllBtn_->setEnabled(!exporting);
    selectNoneBtn_->setEnabled(!exporting);
    exportBtn_->setVisible(!exporting);
    cancelBtn_->setVisible(exporting);
}

// ============================================================================
// collectFbxOptions — read FBX options from UI widgets
// ============================================================================

FbxExportOptions BatchExporterUI::collectFbxOptions() const
{
    FbxExportOptions opts;

    if (skelAnimOnlyCheck_)    opts.skelAnimationOnly  = skelAnimOnlyCheck_->isChecked();
    if (skelBakeComplexCheck_) opts.skelBakeComplex     = skelBakeComplexCheck_->isChecked();
    if (skelSkeletonDefsCheck_) opts.skelSkeletonDefs   = skelSkeletonDefsCheck_->isChecked();
    if (skelConstraintsCheck_) opts.skelConstraints     = skelConstraintsCheck_->isChecked();
    if (skelInputConnsCheck_)  opts.skelInputConns      = skelInputConnsCheck_->isChecked();
    if (skelBlendShapeCheck_)  opts.skelBlendShape      = skelBlendShapeCheck_->isChecked();

    if (bsShapesCheck_)            opts.bsShapes          = bsShapesCheck_->isChecked();
    if (bsIncludeSkeletonCheck_)    opts.bsIncludeSkeleton = bsIncludeSkeletonCheck_->isChecked();
    if (bsSmoothMeshCheck_)         opts.bsSmoothMesh      = bsSmoothMeshCheck_->isChecked();

    if (fbxVersionCombo_) opts.fileVersion = qStringToUtf8(fbxVersionCombo_->currentText());
    if (fbxUpAxisCombo_)  opts.upAxis      = qStringToUtf8(fbxUpAxisCombo_->currentText().toLower());

    return opts;
}

// ============================================================================
// syncFilenamesFromUI
// ============================================================================

void BatchExporterUI::syncFilenamesFromUI()
{
    int rowCount = tableWidget_->rowCount();
    for (int row = 0; row < rowCount; ++row) {
        if (row >= static_cast<int>(exportItems_.size())) break;

        QTableWidgetItem* fnItem = tableWidget_->item(row, 3);
        if (fnItem) {
            std::string edited = qStringToUtf8(fnItem->text());
            if (!edited.empty()) {
                exportItems_[row].filename = edited;
            }
        }
    }
}

// ============================================================================
// refreshList
// ============================================================================

void BatchExporterUI::refreshList()
{
    // Block signals while rebuilding to avoid spurious cellChanged events
    tableWidget_->blockSignals(true);

    int count = static_cast<int>(exportItems_.size());
    tableWidget_->setRowCount(count);

    for (int row = 0; row < count; ++row) {
        const ExportItem& item = exportItems_[row];

        // Column 0: Checkbox
        QTableWidgetItem* checkItem = tableWidget_->item(row, 0);
        if (!checkItem) {
            checkItem = new QTableWidgetItem();
            checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            tableWidget_->setItem(row, 0, checkItem);
        }
        checkItem->setCheckState(item.selected ? Qt::Checked : Qt::Unchecked);

        // Column 1: Type
        QTableWidgetItem* typeItem = tableWidget_->item(row, 1);
        if (!typeItem) {
            typeItem = new QTableWidgetItem();
            typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 1, typeItem);
        }
        typeItem->setText(utf8ToQString(item.type));

        // Column 2: Source (node / display name)
        QTableWidgetItem* srcItem = tableWidget_->item(row, 2);
        if (!srcItem) {
            srcItem = new QTableWidgetItem();
            srcItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 2, srcItem);
        }
        srcItem->setText(utf8ToQString(item.name));
        srcItem->setToolTip(utf8ToQString(item.node));

        // Column 3: Output Filename (editable via delegate)
        QTableWidgetItem* fnItem = tableWidget_->item(row, 3);
        if (!fnItem) {
            fnItem = new QTableWidgetItem();
            fnItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            tableWidget_->setItem(row, 3, fnItem);
        }
        fnItem->setText(utf8ToQString(item.filename));

        // Column 4: Status
        QTableWidgetItem* statusItem = tableWidget_->item(row, 4);
        if (!statusItem) {
            statusItem = new QTableWidgetItem();
            statusItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 4, statusItem);
        }
        statusItem->setText(utf8ToQString(item.status));

        // Color-code status
        if (item.status == "done") {
            statusItem->setForeground(QBrush(QColor(0, 160, 0)));
        } else if (item.status == "error") {
            statusItem->setForeground(QBrush(QColor(200, 0, 0)));
            statusItem->setToolTip(utf8ToQString(item.message));
        } else if (item.status == "exporting") {
            statusItem->setForeground(QBrush(QColor(0, 100, 200)));
        } else if (item.status == "cancelled") {
            statusItem->setForeground(QBrush(QColor(180, 140, 0)));
            statusItem->setToolTip(utf8ToQString(item.message));
        } else {
            statusItem->setForeground(QBrush(QColor(120, 120, 120)));
            if (!item.message.empty()) {
                statusItem->setToolTip(utf8ToQString(item.message));
                statusItem->setText(utf8ToQString(item.status + " *"));
            }
        }
    }

    tableWidget_->blockSignals(false);
}

// ============================================================================
// setStatus
// ============================================================================

void BatchExporterUI::setStatus(const std::string& text)
{
    statusBar_->showMessage(utf8ToQString(text));
}

// ============================================================================
// getFrameRange
// ============================================================================

std::pair<int, int> BatchExporterUI::getFrameRange()
{
    if (radioCustom_->isChecked()) {
        return std::make_pair(customStartSpin_->value(),
                              customEndSpin_->value());
    }

    // Query Maya timeline range via MEL
    int startFrame = 1;
    int endFrame   = 100;

    MString startResult;
    MStatus startStat = MGlobal::executeCommand(
        "playbackOptions -q -minTime", startResult);
    if (startStat == MStatus::kSuccess) {
        startFrame = startResult.asInt();
    }

    MString endResult;
    MStatus endStat = MGlobal::executeCommand(
        "playbackOptions -q -maxTime", endResult);
    if (endStat == MStatus::kSuccess) {
        endFrame = endResult.asInt();
    }

    return std::make_pair(startFrame, endFrame);
}

