#include "RefCheckerUI.h"
#include "SceneScanner.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MQtUtil.h>
#include <maya/MString.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QBrush>
#include <QColor>
#include <QMenu>
#include <QDesktopServices>
#include <QUrl>
#include <QProgressDialog>
#include <QPointer>
#include <QThread>
#include <QStringList>
#include <QRegularExpression>
#include <QByteArray>

#include <algorithm>
#include <sstream>
#include <cctype>
#include <set>
#include <functional>
#include <cstring>
#include <atomic>
#include <regex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// Convert MString to UTF-8 std::string safely on Windows
static std::string toUtf8(const MString& ms) {
#ifdef _WIN32
    const wchar_t* wstr = ms.asWChar();
    if (!wstr || !*wstr) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string(ms.asChar());  // fallback
    std::string result(len, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    if (ret <= 0) return std::string(ms.asChar());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
#else
    return std::string(ms.asChar());
#endif
}

// Convert UTF-8 std::string to MString safely on Windows
static MString utf8ToMString(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return MString();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return MString(utf8.c_str());
    std::wstring wstr(wlen, L'\0');
    int ret = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], wlen);
    if (ret <= 0) return MString(utf8.c_str());
    if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
    return MString(wstr.c_str());
#else
    return MString(utf8.c_str());
#endif
}

// Execute a Python command via MGlobal::executePythonCommand.
// Returns true if the command succeeded (MStatus::kSuccess).
// Uses utf8ToMString to correctly handle Chinese/Unicode in the Python code.
static std::string indentPythonBlock(const std::string& code) {
    // Indent each line so it can be embedded safely under a try: block.
    // (Needed because many callers pass multi-line Python code.)
    if (code.empty()) return "    pass\n";
    std::string out;
    out.reserve(code.size() + 16);
    size_t start = 0;
    while (start <= code.size()) {
        size_t end = code.find('\n', start);
        if (end == std::string::npos) end = code.size();
        std::string line = code.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out += "    ";
        out += line;
        out += "\n";
        if (end == code.size()) break;
        start = end + 1;
    }
    return out;
}

static bool execPython(const std::string& pyCode) {
    MString mpy = utf8ToMString(pyCode);
    MStatus st = MGlobal::executePythonCommand(mpy);
    if (st != MS::kSuccess) {
        // Capture the Python exception by running the command inside try/except
        std::string wrapped = "import traceback as __tb\n__pt_err = ''\ntry:\n"
            + indentPythonBlock(pyCode) +
            "except Exception:\n    __pt_err = __tb.format_exc()\n";
        MString mWrapped = utf8ToMString(wrapped);
        MGlobal::executePythonCommand(mWrapped);
        MString errMsg;
        MGlobal::executePythonCommand(utf8ToMString("__pt_err"), errMsg);
        std::string errStr = toUtf8(errMsg);
        if (!errStr.empty() && errStr != "NoneType: None\n" && errStr != "None" && errStr != "") {
            PluginLog::warn("RefChecker", "Python error: " + errStr);
        }
    }
    return (st == MS::kSuccess);
}

// Execute a Python expression that returns an int (0/1) result.
static int execPythonInt(const std::string& pyCode) {
    MString mpy = utf8ToMString(pyCode);
    int result = 0;
    MGlobal::executePythonCommand(mpy, result);
    return result;
}

// Build a Python-safe string literal from a UTF-8 std::string.
static std::string pyStr(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '\'') r += "\\'";
        else r += c;
    }
    return r;
}

// Forward declaration
static bool globMatch(const char* pattern, const char* str);

namespace {

static QString utf8ToQString(const std::string& s)
{
    return QString::fromUtf8(s.c_str(), static_cast<int>(s.size()));
}

static std::string qStringToUtf8(const QString& s)
{
    QByteArray u8 = s.toUtf8();
    return std::string(u8.constData(), static_cast<size_t>(u8.size()));
}

// Unicode-safe lowercase via QString
static std::string qLower(const std::string& s)
{
    return QString::fromUtf8(s.c_str(), static_cast<int>(s.size()))
               .toLower()
               .toUtf8()
               .toStdString();
}

// ASCII-only lowercase (for path comparison where Unicode doesn't matter)
static std::string lowerString(const std::string& s)
{
    std::string out = s;
    for (auto& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

static std::string normalizePathForCompare(const std::string& path)
{
    std::string p = path;
    for (auto& ch : p) {
        if (ch == '\\') ch = '/';
    }
    // Strip Maya copy-number suffix like "{2}".
    p = std::regex_replace(p, std::regex(R"(\{\d+\}$)"), "");
    return lowerString(p);
}

static std::string queryReferenceUnresolvedPath(const std::string& refNode)
{
    if (refNode.empty()) return std::string();
    MString result;
    // referenceQuery flags like -filename and -referenceNode are mutually exclusive.
    // For a reference node, pass it as the command argument.
    MString cmd = MString("referenceQuery -filename -unresolvedName \"")
        + refNode.c_str() + "\"";
    if (MGlobal::executeCommand(cmd, result) != MS::kSuccess) {
        return std::string();
    }
    return toUtf8(result);
}

static bool isReferenceLoaded(const std::string& refNode)
{
    if (refNode.empty()) return false;
    int loaded = 0;
    // For a reference node, pass it as the argument (avoid -referenceNode flag).
    MString qCmd = MString("referenceQuery -isLoaded \"") + refNode.c_str() + "\"";
    MGlobal::executeCommand(qCmd, loaded);
    return loaded != 0;
}

static bool updateReferencePathNoLoad(const std::string& refNode,
                                      const std::string& targetPathRaw,
                                      const std::string& targetPathResolved)
{
    if (refNode.empty() || targetPathRaw.empty()) return false;
    // Use loadReferenceDepth='none' to update the reference target path without
    // forcing a heavy load in this phase.
    std::string pyCmd =
        "import maya.cmds as cmds\n"
        "try:\n"
        "    cmds.file('" + pyStr(targetPathRaw) + "', loadReference='" + pyStr(refNode)
        + "', loadReferenceDepth='none')\n"
        "except RuntimeError:\n"
        "    pass";
    execPython(pyCmd);

    std::string current = queryReferenceUnresolvedPath(refNode);
    if (current.empty()) return false;
    const std::string curNorm = normalizePathForCompare(current);
    if (curNorm == normalizePathForCompare(targetPathRaw)) return true;
    if (!targetPathResolved.empty() &&
        curNorm == normalizePathForCompare(targetPathResolved)) return true;
    return false;
}

static bool loadReferenceByNode(const std::string& refNode)
{
    if (refNode.empty()) return false;
    std::string pyLoad =
        "import maya.cmds as cmds\n"
        "try:\n"
        "    cmds.file(loadReference='" + pyStr(refNode) + "')\n"
        "except RuntimeError:\n"
        "    pass";
    execPython(pyLoad);
    return isReferenceLoaded(refNode);
}

// Simple file cache builder — indexes ALL files, no filtering.
// Key = filename.toLower() via QString (Unicode-safe).
// Uses only Win32 API + wstring in worker thread — no QDir (crashes in Maya worker threads).
static std::pair<std::map<std::string, std::vector<std::string>>, int>
buildFileCacheInternal(const std::string& searchDir,
                       const std::function<bool(int)>& progressCb,
                       std::atomic<bool>* cancelFlag)
{
    std::map<std::string, std::vector<std::string>> cache;
    int scannedFiles = 0;
    bool cancelled = false;

    static const std::set<std::string> skippedDirs = {
        "__pycache__", "node_modules", ".git", ".svn"
    };

    // Convert UTF-8 std::string <-> std::wstring via Win32 API (no Qt needed)
    auto utf8ToWide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        if (len <= 0) return {};
        std::wstring w(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
        return w;
    };
    auto wideToUtf8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string s(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
        return s;
    };
    auto wideToLowerUtf8 = [&](const std::wstring& w) -> std::string {
        // LOCALE_INVARIANT ensures identical results on any machine regardless of system locale
        std::wstring low(w.size(), 0);
        int ret = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE,
                               w.c_str(), (int)w.size(), &low[0], (int)low.size());
        if (ret == 0) {
            // fallback: ASCII-only lowercase
            low = w;
            for (auto& c : low) { if (c >= L'A' && c <= L'Z') c += 32; }
        }
        return wideToUtf8(low);
    };

    std::function<void(const std::wstring&)> walkDir;
    walkDir = [&](const std::wstring& dirW) {
        if (cancelled) return;
        if (cancelFlag && cancelFlag->load()) {
            cancelled = true;
            return;
        }

        std::wstring pattern = dirW + L"\\*";

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (cancelled) break;
            if (cancelFlag && cancelFlag->load()) {
                cancelled = true;
                break;
            }

            std::wstring nameW(fd.cFileName);
            if (nameW == L"." || nameW == L"..") continue;

            // Build full path: dir + "/" + name (use forward slash for consistency)
            std::wstring fullW = dirW + L"/" + nameW;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                std::string nameLow = wideToLowerUtf8(nameW);
                if (nameW[0] == L'.' || skippedDirs.count(nameLow)) continue;
                walkDir(fullW);
            } else {
                ++scannedFiles;

                std::string key = wideToLowerUtf8(nameW);
                std::string fullPath = wideToUtf8(fullW);
                cache[key].push_back(fullPath);

                if (progressCb && (scannedFiles % 200 == 0)) {
                    if (!progressCb(scannedFiles)) {
                        cancelled = true;
                        break;
                    }
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    };

    walkDir(utf8ToWide(searchDir));
    return std::make_pair(cache, scannedFiles);
}

} // namespace

// ============================================================================
// Static members
// ============================================================================

RefCheckerUI* RefCheckerUI::instance_ = nullptr;

BatchLocateWorker::BatchLocateWorker(const std::string& searchDir,
                                     const FileScanFilter& filter,
                                     QObject* parent)
    : QObject(parent)
    , searchDir_(searchDir)
    , filter_(filter)
    , cancelled_(false)
{
}

void BatchLocateWorker::requestCancel()
{
    cancelled_.store(true);
}

const BatchLocateWorker::Result& BatchLocateWorker::result() const
{
    return result_;
}

void BatchLocateWorker::run()
{
    emit statusText("Scanning files...");

    auto progressCb = [&](int count) -> bool {
        emit progress(count);
        return !cancelled_.load();
    };

    auto scanResult = buildFileCacheInternal(searchDir_, progressCb, &cancelled_);
    result_.cache = std::move(scanResult.first);
    result_.scannedCount = scanResult.second;
    result_.cancelled = cancelled_.load();

    emit finished();
}

RefCheckerUI* RefCheckerUI::instance()
{
    return instance_;
}

void RefCheckerUI::showUI()
{
    if (instance_) {
        instance_->raise();
        instance_->activateWindow();
        return;
    }

    QWidget* mayaMainWindow = MQtUtil::mainWindow();
    instance_ = new RefCheckerUI(mayaMainWindow);
    instance_->setAttribute(Qt::WA_DeleteOnClose, true);
    instance_->show();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RefCheckerUI::RefCheckerUI(QWidget* parent)
    : QDialog(parent)
    , searchDirField_(nullptr)
    , filterAllCheck_(nullptr)
    , filterMissingCheck_(nullptr)
    , typeFilterCombo_(nullptr)
    , maxDisplayField_(nullptr)
    , searchFilterField_(nullptr)
    , statsLabel_(nullptr)
    , tableWidget_(nullptr)
    , pathModeCombo_(nullptr)
    , scanThread_(nullptr)
    , scanWorker_(nullptr)
    , scanInProgress_(false)
{
    fileCache_.totalCount = 0;
    setupUI();
}

RefCheckerUI::~RefCheckerUI()
{
    if (scanThread_ && scanThread_->isRunning()) {
        if (scanWorker_) {
            scanWorker_->requestCancel();
        }
        scanThread_->quit();
        scanThread_->wait(3000);
    }
    instance_ = nullptr;
}

// ============================================================================
// setupUI
// ============================================================================

void RefCheckerUI::setupUI()
{
    setWindowTitle("Reference Checker");
    setMinimumSize(1000, 600);
    resize(1060, 720);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 4);
    mainLayout->setSpacing(4);

    // ----- Toolbar row -----
    {
        QHBoxLayout* row = new QHBoxLayout();

        QPushButton* scanBtn = new QPushButton("Scan");
        scanBtn->setToolTip(QString::fromUtf8(
            u8"扫描当前场景中的所有依赖文件，\n"
            u8"包括引用、贴图、缓存和音频。\n"
            u8"扫描后会在列表中显示每个文件的状态。"));
        scanBtn->setMinimumWidth(75);
        connect(scanBtn, &QPushButton::clicked, this, &RefCheckerUI::onScan);
        row->addWidget(scanBtn);

        QPushButton* selectMissingBtn = new QPushButton("Select Missing");
        selectMissingBtn->setToolTip(QString::fromUtf8(
            u8"勾选列表中所有状态为 MISSING 或 UNLOADED 的条目，\n"
            u8"方便后续批量定位或加载。"));
        selectMissingBtn->setMinimumWidth(100);
        connect(selectMissingBtn, &QPushButton::clicked,
                this, &RefCheckerUI::onSelectAllMissing);
        row->addWidget(selectMissingBtn);

        row->addSpacing(10);

        QPushButton* batchLocateBtn = new QPushButton("Batch Locate Dir");
        batchLocateBtn->setToolTip(QString::fromUtf8(
            u8"选择一个文件夹，扫描其中所有文件，\n"
            u8"自动匹配缺失的依赖。\n"
            u8"可多次点击添加多个搜索目录。"));
        batchLocateBtn->setMinimumWidth(125);
        batchLocateBtn->setStyleSheet(
            "QPushButton { background-color: #668866; color: white; }");
        connect(batchLocateBtn, &QPushButton::clicked,
                this, &RefCheckerUI::onBatchLocate);
        row->addWidget(batchLocateBtn);

        row->addSpacing(10);

        searchDirField_ = new QLineEdit();
        searchDirField_->setPlaceholderText("Search directories...");
        searchDirField_->setReadOnly(true);
        searchDirField_->setToolTip(QString::fromUtf8(
            u8"显示已添加的搜索目录列表。\n"
            u8"通过 Batch Locate Dir 按钮添加目录。"));
        row->addWidget(searchDirField_, 1);

        QPushButton* applyBtn = new QPushButton("Apply Fixes");
        applyBtn->setToolTip(QString::fromUtf8(
            u8"将匹配到的新路径应用到场景中，修复缺失的依赖。\n"
            u8"优先修复勾选的条目；如果没有勾选，则修复所有已匹配的条目。\n"
            u8"操作可通过 Ctrl+Z 撤销。"));
        applyBtn->setMinimumWidth(95);
        applyBtn->setStyleSheet(
            "QPushButton { background-color: #557799; color: white; }");
        connect(applyBtn, &QPushButton::clicked,
                this, &RefCheckerUI::onApplyFixes);
        row->addWidget(applyBtn);

        QPushButton* loadAllBtn = new QPushButton("Load All");
        loadAllBtn->setToolTip(QString::fromUtf8(
            u8"加载所有状态为 UNLOADED 的引用。\n"
            u8"适用于通过 OpenWithoutReferences 打开场景后，\n"
            u8"批量加载已找到的引用文件。\n"
            u8"加载过程中如遇到缺失插件等非致命错误，\n"
            u8"引用仍会被加载（Maya 会跳过无法识别的节点）。"));
        loadAllBtn->setMinimumWidth(80);
        loadAllBtn->setStyleSheet(
            "QPushButton { background-color: #558855; color: white; }");
        connect(loadAllBtn, &QPushButton::clicked,
                this, &RefCheckerUI::onLoadAllUnloaded);
        row->addWidget(loadAllBtn);

        mainLayout->addLayout(row);
    }

    // ----- Filter row -----
    {
        QHBoxLayout* row = new QHBoxLayout();

        row->addWidget(new QLabel("Filter:"));

        filterAllCheck_ = new QCheckBox("All");
        filterAllCheck_->setChecked(true);
        filterAllCheck_->setToolTip(QString::fromUtf8(
            u8"显示所有依赖文件（包括正常和缺失的）。"));
        connect(filterAllCheck_, &QCheckBox::stateChanged,
                this, &RefCheckerUI::onFilterChanged);
        row->addWidget(filterAllCheck_);

        filterMissingCheck_ = new QCheckBox("Missing");
        filterMissingCheck_->setChecked(false);
        filterMissingCheck_->setToolTip(QString::fromUtf8(
            u8"仅显示缺失的依赖文件。"));
        connect(filterMissingCheck_, &QCheckBox::stateChanged,
                this, &RefCheckerUI::onFilterChanged);
        row->addWidget(filterMissingCheck_);

        row->addSpacing(15);
        row->addWidget(new QLabel("Type:"));

        typeFilterCombo_ = new QComboBox();
        typeFilterCombo_->addItem("All Types");
        typeFilterCombo_->addItem("Reference");
        typeFilterCombo_->addItem("Texture");
        typeFilterCombo_->addItem("Cache");
        typeFilterCombo_->addItem("Audio");
        typeFilterCombo_->setToolTip(QString::fromUtf8(
            u8"按依赖类型筛选列表：\n"
            u8"Reference — Maya 引用文件（.ma/.mb）\n"
            u8"Texture — 贴图文件\n"
            u8"Cache — 缓存文件（Alembic/GPU Cache）\n"
            u8"Audio — 音频文件"));
        connect(typeFilterCombo_,
                &QComboBox::currentIndexChanged,
                this, &RefCheckerUI::onTypeFilterChanged);
        row->addWidget(typeFilterCombo_);

        row->addSpacing(15);
        row->addWidget(new QLabel("Max:"));

        maxDisplayField_ = new QLineEdit("3000");
        maxDisplayField_->setFixedWidth(60);
        maxDisplayField_->setToolTip(QString::fromUtf8(
            u8"列表最多显示的条目数量。\n"
            u8"依赖文件很多时，限制显示数量可避免界面卡顿。"));
        row->addWidget(maxDisplayField_);

        row->addSpacing(15);
        row->addWidget(new QLabel("Search:"));

        searchFilterField_ = new QLineEdit();
        searchFilterField_->setPlaceholderText("name / path / node");
        searchFilterField_->setToolTip(QString::fromUtf8(
            u8"输入关键词搜索列表，\n"
            u8"可匹配文件名、路径或节点名。"));
        connect(searchFilterField_, &QLineEdit::textChanged,
                this, &RefCheckerUI::onSearchTextChanged);
        row->addWidget(searchFilterField_, 1);

        row->addStretch();
        mainLayout->addLayout(row);
    }

    // ----- Stats row -----
    {
        statsLabel_ = new QLabel("Click [Scan] to begin");
        QFont boldFont = statsLabel_->font();
        boldFont.setBold(true);
        statsLabel_->setFont(boldFont);
        mainLayout->addWidget(statsLabel_);
    }

    // ----- Table -----
    {
        tableWidget_ = new QTableWidget(0, 6);
        tableWidget_->setHorizontalHeaderLabels(
            QStringList() << "" << "Type" << "Status"
                          << "Original Path" << "Matched Path" << "Action");

        QHeaderView* header = tableWidget_->horizontalHeader();
        header->resizeSection(0, 30);   // Checkbox
        header->resizeSection(1, 80);   // Type
        header->resizeSection(2, 70);   // Status
        header->resizeSection(3, 300);  // Original Path
        header->resizeSection(4, 300);  // Matched Path
        header->resizeSection(5, 80);   // Action
        header->setStretchLastSection(false);
        header->setSectionResizeMode(4, QHeaderView::Stretch);

        tableWidget_->verticalHeader()->setVisible(false);
        tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
        tableWidget_->setAlternatingRowColors(true);

        connect(tableWidget_, &QTableWidget::cellChanged,
                this, &RefCheckerUI::onCheckboxChanged);

        mainLayout->addWidget(tableWidget_, 1);
    }

    // ----- Bottom row: path mode -----
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel("Path Mode:"));

        pathModeCombo_ = new QComboBox();
        pathModeCombo_->addItem("Relative Path");
        pathModeCombo_->addItem("Absolute Path");
        pathModeCombo_->setToolTip(QString::fromUtf8(
            u8"Apply Fixes 时使用的路径模式：\n"
            u8"Absolute Path — 使用完整绝对路径\n"
            u8"Relative Path — 使用相对于场景文件的路径（推荐）"));
        row->addWidget(pathModeCombo_);

        row->addStretch();
        mainLayout->addLayout(row);
    }
}

// ============================================================================
// onScan
// ============================================================================

void RefCheckerUI::onScan()
{
    if (scanInProgress_) {
        QMessageBox::information(this, "Scan",
            "Batch Locate is running. Please wait for it to finish.");
        return;
    }

    dependencies_.clear();
    fileCache_.cache.clear();
    fileCache_.cacheKeys.clear();
    fileCache_.cacheIndex.clear();
    fileCache_.totalCount = 0;
    searchDirs_.clear();

    searchDirField_->setText("");

    QApplication::setOverrideCursor(Qt::WaitCursor);

    // Scan all dependency types
    {
        std::vector<DependencyInfo> refs = SceneScanner::scanReferences();
        dependencies_.insert(dependencies_.end(), refs.begin(), refs.end());
    }
    {
        std::vector<DependencyInfo> texs = SceneScanner::scanTextures();
        dependencies_.insert(dependencies_.end(), texs.begin(), texs.end());
    }
    {
        std::vector<DependencyInfo> caches = SceneScanner::scanCaches();
        dependencies_.insert(dependencies_.end(), caches.begin(), caches.end());
    }
    {
        std::vector<DependencyInfo> audios = SceneScanner::scanAudio();
        dependencies_.insert(dependencies_.end(), audios.begin(), audios.end());
    }

    QApplication::restoreOverrideCursor();

    refreshList();
    updateStats();
    checkAndWarnRisks();

    // Log scan summary
    {
        int total = static_cast<int>(dependencies_.size());
        int missing = 0;
        int ok = 0;
        for (const auto& dep : dependencies_) {
            if (!dep.exists) ++missing;
            else ++ok;
        }
        PluginLog::ScanSummary ss;
        ss.module = "RefChecker";
        MString sceneName;
        MGlobal::executeCommand("file -q -sn", sceneName);
        ss.scenePath = toUtf8(sceneName);
        ss.totalItems = total;
        ss.okItems = ok;
        ss.missingItems = missing;
        PluginLog::logScanSummary(ss);
    }
}

// ============================================================================
// onSelectAllMissing
// ============================================================================

void RefCheckerUI::onSelectAllMissing()
{
    for (auto& dep : dependencies_) {
        if (!dep.exists || (dep.type == "reference" && !dep.isLoaded)) {
            dep.selected = true;
        }
    }
    refreshList();
}

// ============================================================================
// onFilterChanged / onTypeFilterChanged
// ============================================================================

void RefCheckerUI::onFilterChanged()
{
    QCheckBox* sender = qobject_cast<QCheckBox*>(QObject::sender());
    if (sender == filterAllCheck_ && filterAllCheck_->isChecked()) {
        filterMissingCheck_->setChecked(false);
    } else if (sender == filterMissingCheck_ && filterMissingCheck_->isChecked()) {
        filterAllCheck_->setChecked(false);
    }
    refreshList();
}

void RefCheckerUI::onTypeFilterChanged(int /*index*/)
{
    refreshList();
}

void RefCheckerUI::onSearchTextChanged(const QString& /*text*/)
{
    refreshList();
}

// ============================================================================
// onCheckboxChanged
// ============================================================================

void RefCheckerUI::onCheckboxChanged(int row, int col)
{
    if (col != 0) return;
    if (row < 0 || row >= tableWidget_->rowCount()) return;

    QTableWidgetItem* checkItem = tableWidget_->item(row, 0);
    if (!checkItem) return;

    // Map visible row back to dependency index via stored data
    QVariant data = checkItem->data(Qt::UserRole);
    if (!data.isValid()) return;
    int depIdx = data.toInt();
    if (depIdx < 0 || depIdx >= static_cast<int>(dependencies_.size())) return;

    dependencies_[depIdx].selected = (checkItem->checkState() == Qt::Checked);
}

// ============================================================================
// onLoadReference
// ============================================================================

void RefCheckerUI::onLoadReference(int depIndex)
{
    if (depIndex < 0 || depIndex >= static_cast<int>(dependencies_.size())) return;
    DependencyInfo& dep = dependencies_[depIndex];

    if (dep.node.empty() || dep.node == "unknown") {
        QMessageBox::warning(this, "Load Reference",
            QString::fromUtf8(u8"无法加载：引用节点未知。"));
        return;
    }

    PluginLog::info("RefChecker", "onLoadReference: loading " + dep.node);

    bool loaded = loadReferenceByNode(dep.node);

    // If simple load failed, try with explicit resolved path
    if (!loaded && !dep.path.empty()) {
        std::string resolved = resolvePathForApply(dep.path);
        PluginLog::info("RefChecker", "onLoadReference: retry with path: " + resolved);
        updateReferencePathNoLoad(dep.node, dep.path, resolved);
        loaded = loadReferenceByNode(dep.node);
    }

    if (loaded) {
        dep.isLoaded = true;
        PluginLog::info("RefChecker", "onLoadReference: loaded OK (with possible warnings)");
        refreshList();
        updateStats();
    } else {
        PluginLog::error("RefChecker", "onLoadReference: reference still unloaded after attempt: " + dep.node);
        QMessageBox::warning(this, "Load Reference",
            QString::fromUtf8(u8"加载引用失败：%1\n"
                    u8"文件可能已损坏、路径不存在或不兼容。")
                .arg(utf8ToQString(dep.node)));
    }
}

void RefCheckerUI::onLoadAllUnloaded()
{
    int total = 0;
    int success = 0;
    int failed = 0;

    // Count first
    for (const auto& dep : dependencies_) {
        if (dep.exists && !dep.isLoaded && dep.type == "reference") ++total;
    }
    if (total == 0) {
        QMessageBox::information(this, "Load All",
            QString::fromUtf8(u8"没有需要加载的未加载引用。"));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);

    for (auto& dep : dependencies_) {
        if (!dep.exists || dep.isLoaded || dep.type != "reference") continue;
        if (dep.node.empty() || dep.node == "unknown") {
            ++failed;
            continue;
        }

        PluginLog::info("RefChecker", "onLoadAllUnloaded: loading " + dep.node);

        bool loaded = loadReferenceByNode(dep.node);

        // Strategy 2: if still unloaded, query filename and load with explicit resolved path
        if (!loaded) {
            MString refFilename;
            MString fnCmd = MString("referenceQuery -filename \"") + dep.node.c_str() + "\"";
            MGlobal::executeCommand(fnCmd, refFilename);
            std::string refPath = toUtf8(refFilename);

            if (!refPath.empty()) {
                std::string resolved = resolvePathForApply(refPath);
                PluginLog::info("RefChecker", "onLoadAllUnloaded: retry with path: " + resolved);
                updateReferencePathNoLoad(dep.node, refPath, resolved);
                loaded = loadReferenceByNode(dep.node);
            }
        }

        // Strategy 3: if still unloaded, try using dep.path directly
        if (!loaded && !dep.path.empty()) {
            std::string resolved = resolvePathForApply(dep.path);
            PluginLog::info("RefChecker", "onLoadAllUnloaded: retry with dep.path: " + resolved);
            updateReferencePathNoLoad(dep.node, dep.path, resolved);
            loaded = loadReferenceByNode(dep.node);
        }

        if (loaded) {
            dep.isLoaded = true;
            ++success;
        } else {
            ++failed;
            PluginLog::warn("RefChecker", "onLoadAllUnloaded: still unloaded: " + dep.node);
        }
    }

    QApplication::restoreOverrideCursor();

    refreshList();
    updateStats();

    QMessageBox::information(this, "Load All",
        QString::fromUtf8(u8"加载完成。\n成功：%1\n失败：%2")
            .arg(success).arg(failed));
}

// ============================================================================
// onLocateSingle
// ============================================================================

void RefCheckerUI::onLocateSingle(int depIndex)
{
    PluginLog::info("RefChecker", "onLocateSingle: ENTER (button clicked)");
    // Defer to next event loop iteration to avoid QFileDialog deadlock in Maya.
    QTimer::singleShot(0, this, [this, depIndex]() {
        PluginLog::info("RefChecker", "onLocateSingle: QTimer fired, calling deferred");
        onLocateSingleDeferred(depIndex);
    });
    PluginLog::info("RefChecker", "onLocateSingle: EXIT (timer scheduled)");
}

void RefCheckerUI::onLocateSingleDeferred(int depIndex)
{
    PluginLog::info("RefChecker", "onLocateSingleDeferred: ENTER");
    if (depIndex < 0 || depIndex >= static_cast<int>(dependencies_.size())) {
        PluginLog::info("RefChecker", "onLocateSingleDeferred: invalid depIndex, returning");
        return;
    }
    DependencyInfo& dep = dependencies_[depIndex];

    // Step 1: Try auto-match from existing cache (no dialog)
    if (!fileCache_.cache.empty()) {
        PluginLog::info("RefChecker", "onLocateSingleDeferred: attempting autoMatch from existing cache...");
        std::string best = autoMatchDependency(dep);
        if (!best.empty()) {
            dep.matchedPath = best;
            PluginLog::info("RefChecker", "Matched from existing cache: " + best);
            refreshList();
            updateStats();
            return;
        }
        PluginLog::warn("RefChecker", "No match in existing cache: " + dep.path);
    }

    // Step 2: Offer directory search (one dialog — Cancel skips to file picker)
    {
        PluginLog::info("RefChecker", "onLocateSingleDeferred: offering dir search dialog...");
        QFileDialog dirDlg(this,
            QString::fromUtf8(u8"选择搜索目录（取消 = 直接手动选择文件）"));
        dirDlg.setFileMode(QFileDialog::Directory);
        dirDlg.setOption(QFileDialog::ShowDirsOnly, true);
        dirDlg.setOption(QFileDialog::DontUseNativeDialog, true);
        QString dir;
        if (dirDlg.exec() == QDialog::Accepted && !dirDlg.selectedFiles().isEmpty())
            dir = dirDlg.selectedFiles().first();

        if (!dir.isEmpty()) {
            std::string dirStr = qStringToUtf8(dir);

            // Duplicate check
            bool isDuplicate = false;
            for (const auto& existing : searchDirs_) {
                QString existQ = utf8ToQString(existing);
                if (existQ.compare(dir, Qt::CaseInsensitive) == 0) {
                    isDuplicate = true;
                    break;
                }
            }

            if (!isDuplicate) {
                searchDirs_.push_back(dirStr);
                QString displayText;
                for (size_t i = 0; i < searchDirs_.size(); ++i) {
                    if (i > 0) displayText += " ; ";
                    displayText += utf8ToQString(searchDirs_[i]);
                }
                searchDirField_->setText(displayText);

                PluginLog::info("RefChecker", "Scanning dir: " + dirStr);
                auto scanResult = buildFileCache(dirStr);
                mergeCache(scanResult.first);
            }

            // Retry auto-match with new cache
            std::string best = autoMatchDependency(dep);
            if (!best.empty()) {
                dep.matchedPath = best;
                PluginLog::info("RefChecker", "Matched after dir search: " + best);
                refreshList();
                updateStats();
                return;
            }
            PluginLog::warn("RefChecker", "Still no match after dir search: " + dep.path);
        }
    }

    // Step 3: Fall back to manual file dialog
    QString filter = "All Files (*.*)";
    if (dep.type == "reference") {
        filter = "Maya Files (*.ma *.mb);;All Files (*.*)";
    } else if (dep.type == "texture") {
        filter = "Image Files (*.png *.jpg *.jpeg *.tif *.tiff *.exr *.tga *.bmp *.tx);;All Files (*.*)";
    } else if (dep.type == "cache") {
        filter = "Cache Files (*.abc *.fbx);;All Files (*.*)";
    } else if (dep.type == "audio") {
        filter = "Audio Files (*.wav *.mp3 *.aif);;All Files (*.*)";
    }

    QString startDir;
    if (!searchDirs_.empty()) {
        startDir = utf8ToQString(searchDirs_.back());
    } else {
        size_t pos = dep.path.rfind('/');
        if (pos == std::string::npos) pos = dep.path.rfind('\\');
        if (pos != std::string::npos) {
            startDir = utf8ToQString(dep.path.substr(0, pos));
        }
    }

    PluginLog::info("RefChecker", "onLocateSingleDeferred: opening file dialog...");
    QFileDialog fileDlg(this,
        QString("Locate: %1").arg(
            utf8ToQString(getCleanFilename(dep.path))),
        startDir, filter);
    fileDlg.setFileMode(QFileDialog::ExistingFile);
    fileDlg.setOption(QFileDialog::DontUseNativeDialog, true);
    QString filePath;
    if (fileDlg.exec() == QDialog::Accepted && !fileDlg.selectedFiles().isEmpty())
        filePath = fileDlg.selectedFiles().first();
    PluginLog::info("RefChecker", "onLocateSingleDeferred: file dialog returned: " + qStringToUtf8(filePath));

    if (!filePath.isEmpty()) {
        dep.matchedPath = qStringToUtf8(filePath);
        refreshList();
        updateStats();
    }
    PluginLog::info("RefChecker", "onLocateSingleDeferred: EXIT");
}

// ============================================================================
// onBatchLocate
// ============================================================================

void RefCheckerUI::onBatchLocate()
{
    PluginLog::info("RefChecker", "onBatchLocate: ENTER (button clicked)");
    // Defer the file dialog to the next event loop iteration.
    // Calling QFileDialog static methods directly from a button slot inside
    // a Maya QDialog subclass can deadlock on some Windows + Maya versions
    // because the button-click event is still being processed.
    QTimer::singleShot(0, this, [this]() {
        PluginLog::info("RefChecker", "onBatchLocate: QTimer fired, calling deferred");
        onBatchLocateDeferred();
    });
    PluginLog::info("RefChecker", "onBatchLocate: EXIT (timer scheduled)");
}

void RefCheckerUI::onBatchLocateDeferred()
{
    PluginLog::info("RefChecker", "onBatchLocateDeferred: ENTER");
    PluginLog::info("RefChecker", "onBatchLocateDeferred: about to open dir dialog...");
    QFileDialog dlg(this, "Choose search directory");
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setOption(QFileDialog::ShowDirsOnly, true);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    QString dir;
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedFiles().isEmpty())
        dir = dlg.selectedFiles().first();
    PluginLog::info("RefChecker", "onBatchLocateDeferred: dir dialog returned: " + qStringToUtf8(dir));
    if (dir.isEmpty()) return;

    std::string dirStr = qStringToUtf8(dir);

    // Check for duplicate
    for (const auto& existing : searchDirs_) {
        QString existQ = utf8ToQString(existing);
        if (existQ.compare(dir, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, "Batch Locate",
                "Directory already added: " + dir);
            return;
        }
    }

    searchDirs_.push_back(dirStr);

    // Update search dir display
    QString displayText;
    for (size_t i = 0; i < searchDirs_.size(); ++i) {
        if (i > 0) displayText += " ; ";
        displayText += utf8ToQString(searchDirs_[i]);
    }
    searchDirField_->setText(displayText);

    PluginLog::info("RefChecker", "Scanning: " + dirStr);

    // Progress dialog (non-modal busy indicator)
    QProgressDialog progressDlg("Scanning files...", "Cancel", 0, 0, this);
    progressDlg.setWindowTitle("Batch Locate");
    progressDlg.setWindowModality(Qt::WindowModal);
    progressDlg.setMinimumDuration(0);
    progressDlg.show();
    QApplication::processEvents();

    // ---- Phase 1: Scan files (main thread, synchronous) ----
    bool cancelled = false;
    auto progressCb = [&](int count) -> bool {
        progressDlg.setLabelText(QString("Scanning files... %1 found").arg(count));
        QApplication::processEvents();
        if (progressDlg.wasCanceled()) {
            cancelled = true;
            return false;
        }
        return true;
    };

    auto scanResult = buildFileCacheInternal(dirStr, progressCb, nullptr);

    if (cancelled) {
        PluginLog::info("RefChecker", "Batch Locate cancelled by user.");
        searchDirs_.pop_back();
        QString dt;
        for (size_t i = 0; i < searchDirs_.size(); ++i) {
            if (i > 0) dt += " ; ";
            dt += utf8ToQString(searchDirs_[i]);
        }
        searchDirField_->setText(dt);
        return;
    }

    // ---- Phase 2: Merge cache ----
    progressDlg.setLabelText("Merging file cache...");
    QApplication::processEvents();

    int addedCount = mergeCache(scanResult.first);

    {
        std::ostringstream oss;
        oss << "Scanned " << scanResult.second << " files, added " << addedCount
            << " new, total " << fileCache_.totalCount
            << " (" << fileCache_.cache.size() << " names)";
        PluginLog::info("RefChecker", oss.str());
    }

    if (scanResult.second == 0) {
        QMessageBox::warning(this, "Batch Locate",
            "No files were found under the selected directory.\n"
            "Please verify the path and permissions.");
    }

    // ---- Phase 3: Auto-match ----
    progressDlg.setLabelText("Auto-matching dependencies...");
    QApplication::processEvents();

    int matchedCount = 0;
    int missingCount = 0;
    int matchCounter = 0;

    for (auto& dep : dependencies_) {
        if (dep.exists) continue;
        if (!dep.matchedPath.empty()) continue;
        ++missingCount;

        std::string best = autoMatchDependency(dep);
        if (!best.empty()) {
            dep.matchedPath = best;
            ++matchedCount;
        }

        if (++matchCounter % 20 == 0) {
            progressDlg.setLabelText(
                QString("Auto-matching... %1/%2").arg(matchCounter).arg(missingCount));
            QApplication::processEvents();
            if (progressDlg.wasCanceled()) break;
        }
    }

    {
        std::ostringstream matchOss;
        matchOss << "Auto-match checked " << missingCount
                 << " items, matched " << matchedCount << " items";
        PluginLog::info("RefChecker", matchOss.str());
    }

    progressDlg.close();

    refreshList();
    updateStats();

    QString msg = QString("Cache total: %1 files\nMatched: %2 items\nUnmatched: %3 items")
        .arg(fileCache_.totalCount)
        .arg(matchedCount)
        .arg(missingCount - matchedCount);
    QMessageBox::information(this, "Batch Locate Complete", msg);
}

// ============================================================================
// onApplyFixes
// ============================================================================

void RefCheckerUI::onApplyFixes()
{
    // Collect candidates: prefer selected, fall back to all matched
    std::vector<int> selectedIndices;
    std::vector<int> allMatchedIndices;

    for (int i = 0; i < static_cast<int>(dependencies_.size()); ++i) {
        const DependencyInfo& dep = dependencies_[i];
        if (dep.exists) continue;
        if (dep.matchedPath.empty()) continue;

        allMatchedIndices.push_back(i);
        if (dep.selected) {
            selectedIndices.push_back(i);
        }
    }

    std::vector<int>& toFix = selectedIndices.empty() ? allMatchedIndices : selectedIndices;
    if (toFix.empty()) {
        QMessageBox::information(this, "Apply Fixes", "Nothing to fix.");
        return;
    }

    // Determine path mode
    bool useRelative = (pathModeCombo_->currentText().contains("Relative"));
    if (useRelative) {
        MString scenePath;
        MGlobal::executeCommand("file -q -sceneName", scenePath);
        if (scenePath.length() == 0) {
            QMessageBox::warning(this, "Apply Fixes",
                "Scene is unsaved; falling back to absolute paths.");
            useRelative = false;
        }
    }

    QString scopeText = selectedIndices.empty() ? "all matched rows" : "selected rows";
    QString msg = QString("Will fix %1 dependencies\nScope: %2\nPath mode: %3")
        .arg(toFix.size())
        .arg(scopeText)
        .arg(pathModeCombo_->currentText());

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "Confirm", msg,
        QMessageBox::Ok | QMessageBox::Cancel);
    if (confirm != QMessageBox::Ok) return;

    struct UndoChunkGuard {
        UndoChunkGuard() { MGlobal::executeCommand("undoInfo -openChunk"); }
        ~UndoChunkGuard() { MGlobal::executeCommand("undoInfo -closeChunk"); }
    } undoGuard;

    int success = 0;
    int failed = 0;

    for (size_t i = 0; i < toFix.size(); ++i) {
        int idx = toFix[i];
        DependencyInfo& dep = dependencies_[idx];

        std::string newPath = dep.matchedPath;
        if (useRelative) {
            newPath = toRelativePath(newPath);
        }

        std::ostringstream logMsg;
        logMsg << "ApplyFixes: [" << (i + 1) << "/" << toFix.size()
               << "] " << dep.type << " : " << dep.node;
        PluginLog::info("RefChecker", logMsg.str());

        if (applyPath(dep, newPath)) {
            // Always check existence using the absolute path (matchedPath
            // is always absolute from the file cache).  newPath may be
            // relative when the user chose "Relative Path" mode.
            dep.exists = SceneScanner::pathExists(dep.matchedPath);
            dep.path = newPath;
            dep.matchedPath = "";
            dep.selected = false;

            // For references: query actual loaded state (applyPath already attempted loading)
            if (dep.type == "reference" && !dep.node.empty() && dep.node != "unknown") {
                int loaded = 0;
                MString qCmd = MString("referenceQuery -isLoaded \"")
                    + dep.node.c_str() + "\"";
                MGlobal::executeCommand(qCmd, loaded);
                dep.isLoaded = (loaded != 0);
            }

            ++success;
        } else {
            ++failed;
        }
    }

    refreshList();
    updateStats();

    QString resultMsg = QString("Success: %1\nFailed: %2").arg(success).arg(failed);
    QMessageBox::information(this, "Apply Fixes Complete", resultMsg);
}

// ============================================================================
// refreshList
// ============================================================================

void RefCheckerUI::refreshList()
{
    tableWidget_->blockSignals(true);

    bool showMissingOnly = filterMissingCheck_->isChecked();
    QString typeFilter = typeFilterCombo_->currentText();
    QString searchText = searchFilterField_ ? searchFilterField_->text().trimmed() : "";
    int maxDisplay = 3000;
    bool ok = false;
    int parsed = maxDisplayField_->text().toInt(&ok);
    if (ok && parsed > 0) maxDisplay = parsed;

    std::vector<int> visibleIndices;
    for (int i = 0; i < static_cast<int>(dependencies_.size()); ++i) {
        const DependencyInfo& dep = dependencies_[i];
        if (showMissingOnly && dep.exists && dep.isLoaded) continue;
        if (typeFilter != "All Types" && dep.typeLabel != qStringToUtf8(typeFilter)) continue;
        if (!searchText.isEmpty()) {
            QString pathQ = utf8ToQString(dep.path);
            QString matchQ = utf8ToQString(dep.matchedPath);
            QString nodeQ = utf8ToQString(dep.node);
            if (!pathQ.contains(searchText, Qt::CaseInsensitive) &&
                !matchQ.contains(searchText, Qt::CaseInsensitive) &&
                !nodeQ.contains(searchText, Qt::CaseInsensitive)) {
                continue;
            }
        }
        visibleIndices.push_back(i);
        if (static_cast<int>(visibleIndices.size()) >= maxDisplay) break;
    }

    int rowCount = static_cast<int>(visibleIndices.size());
    tableWidget_->setRowCount(rowCount);

    for (int row = 0; row < rowCount; ++row) {
        int depIdx = visibleIndices[row];
        const DependencyInfo& dep = dependencies_[depIdx];

        // Column 0: Checkbox
        QTableWidgetItem* checkItem = tableWidget_->item(row, 0);
        if (!checkItem) {
            checkItem = new QTableWidgetItem();
            checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            tableWidget_->setItem(row, 0, checkItem);
        }
        checkItem->setCheckState(dep.selected ? Qt::Checked : Qt::Unchecked);
        checkItem->setData(Qt::UserRole, depIdx);

        // Column 1: Type
        QTableWidgetItem* typeItem = tableWidget_->item(row, 1);
        if (!typeItem) {
            typeItem = new QTableWidgetItem();
            typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 1, typeItem);
        }
        typeItem->setText(utf8ToQString(dep.typeLabel));

        // Column 2: Status
        QTableWidgetItem* statusItem = tableWidget_->item(row, 2);
        if (!statusItem) {
            statusItem = new QTableWidgetItem();
            statusItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 2, statusItem);
        }
        if (dep.exists) {
            // For references, loaded state matters: users expect ApplyFixes to end in OK (loaded),
            // otherwise it should stay UNLOADED to signal the reference still isn't in the scene.
            if (dep.type == "reference" && !dep.isLoaded) {
                statusItem->setText("UNLOADED");
                statusItem->setForeground(QBrush(QColor(200, 140, 40)));
            } else {
                statusItem->setText("OK");
                statusItem->setForeground(QBrush(QColor(50, 160, 50)));
            }
        } else if (!dep.matchedPath.empty()) {
            statusItem->setText("MATCHED");
            statusItem->setForeground(QBrush(QColor(50, 100, 180)));
        } else {
            statusItem->setText("MISSING");
            statusItem->setForeground(QBrush(QColor(200, 50, 50)));
        }

        // Column 3: Original Path
        QTableWidgetItem* origItem = tableWidget_->item(row, 3);
        if (!origItem) {
            origItem = new QTableWidgetItem();
            origItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 3, origItem);
        }
        QString origPath = utf8ToQString(dep.path);
        if (origPath.length() > 60) {
            origItem->setText("..." + origPath.right(57));
        } else {
            origItem->setText(origPath);
        }
        origItem->setToolTip(origPath);

        // Column 4: Matched Path
        QTableWidgetItem* matchItem = tableWidget_->item(row, 4);
        if (!matchItem) {
            matchItem = new QTableWidgetItem();
            matchItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 4, matchItem);
        }
        if (!dep.matchedPath.empty()) {
            QString matchPath = utf8ToQString(dep.matchedPath);
            if (matchPath.length() > 60) {
                matchItem->setText("..." + matchPath.right(57));
            } else {
                matchItem->setText(matchPath);
            }
            matchItem->setToolTip(matchPath);
            matchItem->setBackground(QBrush(QColor(60, 90, 60)));
        } else {
            matchItem->setText("-");
            matchItem->setToolTip("");
            matchItem->setBackground(QBrush());
        }

        // Column 5: Action - Load button for UNLOADED references, Locate for others
        QPushButton* actionBtn =
            qobject_cast<QPushButton*>(tableWidget_->cellWidget(row, 5));
        if (!actionBtn) {
            actionBtn = new QPushButton();
            actionBtn->setMinimumWidth(70);
            tableWidget_->setCellWidget(row, 5, actionBtn);
        }
        actionBtn->disconnect();
        int capturedIdx = depIdx;
        if (dep.exists && !dep.isLoaded && dep.type == "reference") {
            actionBtn->setText("Load");
            actionBtn->setToolTip(QString::fromUtf8(
                u8"加载此未加载的引用到场景中。\n"
                u8"如遇缺失插件等非致命错误，引用仍会被加载。"));
            actionBtn->setStyleSheet(
                "QPushButton { background-color: #558855; color: white; }");
            connect(actionBtn, &QPushButton::clicked, this,
                    [this, capturedIdx]() { onLoadReference(capturedIdx); });
        } else {
            actionBtn->setText("Locate");
            actionBtn->setToolTip(QString::fromUtf8(
                u8"为此缺失的依赖文件手动定位替代路径。\n"
                u8"先尝试从已有缓存自动匹配，\n"
                u8"然后可选择搜索目录，最后手动选择文件。"));
            actionBtn->setStyleSheet("");
            connect(actionBtn, &QPushButton::clicked, this,
                    [this, capturedIdx]() { onLocateSingle(capturedIdx); });
        }
    }

    tableWidget_->blockSignals(false);
}

// ============================================================================
// updateStats
// ============================================================================

void RefCheckerUI::updateStats()
{
    int total = static_cast<int>(dependencies_.size());
    int missing = 0;
    int unloaded = 0;
    int matched = 0;
    for (const auto& dep : dependencies_) {
        if (!dep.exists) ++missing;
        if (dep.exists && !dep.isLoaded && dep.type == "reference") ++unloaded;
        if (!dep.matchedPath.empty()) ++matched;
    }

    QString text = QString("Total: %1 | Missing: %2 | Unloaded: %3 | Matched: %4")
        .arg(total).arg(missing).arg(unloaded).arg(matched);
    statsLabel_->setText(text);
}

// ============================================================================
// checkAndWarnRisks
// ============================================================================

void RefCheckerUI::checkAndWarnRisks()
{
    std::vector<std::string> warnings;
    std::vector<std::string> largeFiles;
    std::set<std::string> servers;
    int networkCount = 0;

    for (const auto& dep : dependencies_) {
        const std::string& path = dep.path;
        const std::string& unresolved =
            dep.unresolvedPath.empty() ? path : dep.unresolvedPath;

        // Check network paths
        if (unresolved.size() >= 2 &&
            (unresolved.substr(0, 2) == "//" || unresolved.substr(0, 2) == "\\\\")) {
            ++networkCount;
            std::string clean = unresolved;
            for (auto& c : clean) { if (c == '\\') c = '/'; }
            size_t start = clean.find_first_not_of('/');
            if (start != std::string::npos) {
                size_t end = clean.find('/', start);
                if (end != std::string::npos) {
                    servers.insert(clean.substr(start, end - start));
                } else {
                    servers.insert(clean.substr(start));
                }
            }
        }

        // Check large .ma files
        std::string checkPath = dep.matchedPath.empty() ? path : dep.matchedPath;
        if (dep.exists || !dep.matchedPath.empty()) {
            std::string resolved = SceneScanner::resolveSceneRelative(checkPath);
            if (resolved.size() > 3) {
                std::string ext = resolved.substr(resolved.size() - 3);
                ext = lowerString(ext);
                if (ext == ".ma") {
                    QFileInfo fi(utf8ToQString(resolved));
                    if (fi.exists() && fi.size() > 50 * 1024 * 1024) {
                        double sizeMb = fi.size() / (1024.0 * 1024.0);
                        std::ostringstream oss;
                        oss << qStringToUtf8(fi.fileName())
                            << " (" << static_cast<int>(sizeMb) << "MB)";
                        largeFiles.push_back(oss.str());
                    }
                }
            }
        }
    }

    if (networkCount > 0) {
        std::ostringstream oss;
        oss << "Detected " << networkCount << " network paths (servers: ";
        bool first = true;
        for (const auto& s : servers) {
            if (!first) oss << ", ";
            oss << s;
            first = false;
        }
        oss << "). Unreachable servers may stall loading.";
        warnings.push_back(oss.str());
    }

    if (!largeFiles.empty()) {
        std::ostringstream oss;
        oss << "Large .ma text files found:\n";
        for (const auto& lf : largeFiles) {
            oss << "  - " << lf << "\n";
        }
        oss << "Consider converting to .mb for better load speed.";
        warnings.push_back(oss.str());
    }

    if (!warnings.empty()) {
        std::string combined;
        for (size_t i = 0; i < warnings.size(); ++i) {
            if (i > 0) combined += "\n\n";
            combined += warnings[i];
        }
        QMessageBox::warning(this, "Risk Warning",
                             utf8ToQString(combined));
    }
}

// ============================================================================
// File cache: buildFileCache / scanDirIntoCache / mergeCache
// ============================================================================

int RefCheckerUI::mergeCache(
    const std::map<std::string, std::vector<std::string>>& newCache)
{
    int added = 0;
    for (const auto& kv : newCache) {
        const std::string& nameLower = kv.first;
        const std::vector<std::string>& paths = kv.second;

        auto& bucket = fileCache_.cache[nameLower];
        for (const auto& candidate : paths) {
            std::string norm = candidate;
            norm = lowerString(norm);
            for (auto& ch : norm) { if (ch == '\\') ch = '/'; }

            if (fileCache_.cacheIndex.count(norm) == 0) {
                fileCache_.cacheIndex.insert(norm);
                bucket.push_back(candidate);
                ++added;
            }
        }
    }
    fileCache_.totalCount += added;

    fileCache_.cacheKeys.clear();
    for (const auto& kv : fileCache_.cache) {
        fileCache_.cacheKeys.push_back(kv.first);
    }

    return added;
}

std::pair<std::map<std::string, std::vector<std::string>>, int>
RefCheckerUI::buildFileCache(const std::string& searchDir,
                             std::function<bool(int)> progressCb,
                             const FileScanFilter* /*filter*/,
                             std::atomic<bool>* cancelFlag)
{
    return buildFileCacheInternal(searchDir, progressCb, cancelFlag);
}

int RefCheckerUI::scanDirIntoCache(
    const std::string& directory,
    std::map<std::string, std::vector<std::string>>& cache)
{
    auto result = buildFileCache(directory);
    for (auto& kv : result.first) {
        auto& bucket = cache[kv.first];
        bucket.insert(bucket.end(), kv.second.begin(), kv.second.end());
    }
    return result.second;
}

FileScanFilter RefCheckerUI::buildScanFilter()
{
    FileScanFilter filter;

    static const std::set<std::string> kAllowedExts = {
        ".ma", ".mb", ".fbx", ".abc",
        ".png", ".jpg", ".jpeg", ".tif", ".tiff", ".exr", ".tga", ".bmp", ".tx",
        ".hdr", ".psd", ".dds",
        ".wav", ".mp3", ".aif", ".aiff", ".ogg"
    };
    filter.allowedExts = kAllowedExts;

    std::set<std::string> wildcardSet;
    std::set<std::string> exactSet;

    auto insertKey = [&](const std::string& key) {
        if (key.empty()) return;
        if (isWildcardPattern(key)) {
            wildcardSet.insert(key);
        } else {
            exactSet.insert(key);
        }
    };

    auto addKeyVariants = [&](const std::string& key) {
        insertKey(key);
#ifdef _WIN32
        QString qUtf8 = QString::fromUtf8(key.c_str());
        QString qLocal = QString::fromLocal8Bit(key.c_str());
        std::string utf8Key = lowerString(qUtf8.toUtf8().toStdString());
        std::string localKey = lowerString(qLocal.toLocal8Bit().toStdString());
        insertKey(utf8Key);
        insertKey(localKey);
#endif
    };

    for (const auto& dep : dependencies_) {
        if (dep.exists) continue;
        if (!dep.matchedPath.empty()) continue;

        std::vector<std::string> keys = collectMatchKeys(dep);
        for (const auto& key : keys) {
            addKeyVariants(key);
        }
    }

    filter.exactNames = std::move(exactSet);
    filter.wildcardNames.assign(wildcardSet.begin(), wildcardSet.end());
    return filter;
}

// ============================================================================
// Auto-matching
// ============================================================================

void RefCheckerUI::runAutoMatch()
{
    int matchedCount = 0;
    int missingCount = 0;

    for (auto& dep : dependencies_) {
        if (dep.exists) continue;
        if (!dep.matchedPath.empty()) continue;
        ++missingCount;

        std::string best = autoMatchDependency(dep);
        if (!best.empty()) {
            dep.matchedPath = best;
            ++matchedCount;
            PluginLog::info("RefChecker",
                "Matched: " + getCleanFilename(dep.path) + " -> " + best);
        }
    }

    std::ostringstream oss;
    oss << "Auto-match checked " << missingCount
        << " items, matched " << matchedCount << " items";
    PluginLog::info("RefChecker", oss.str());

    refreshList();
    updateStats();

    QString msg = QString("Cache total: %1 files\nMatched: %2 items\nUnmatched: %3 items")
        .arg(fileCache_.totalCount)
        .arg(matchedCount)
        .arg(missingCount - matchedCount);
    QMessageBox::information(this, "Batch Locate Complete", msg);
}

std::string RefCheckerUI::autoMatchDependency(const DependencyInfo& dep)
{
    if (fileCache_.cache.empty()) return "";

    std::string originalPath = dep.unresolvedPath.empty() ? dep.path : dep.unresolvedPath;
    std::vector<std::string> candidates;

    std::vector<std::string> keys = collectMatchKeys(dep);

    for (const auto& key : keys) {
        if (isWildcardPattern(key)) {
            std::vector<std::string> matched = matchByPattern(key);
            candidates.insert(candidates.end(), matched.begin(), matched.end());
        } else {
            auto it = fileCache_.cache.find(key);
            if (it != fileCache_.cache.end()) {
                candidates.insert(candidates.end(),
                                  it->second.begin(), it->second.end());
            }
        }
    }

    candidates = dedupePaths(candidates);
    if (candidates.empty()) {
        return "";
    }

    std::string best = findBestMatch(originalPath, candidates);
    return best;
}

std::vector<std::string> RefCheckerUI::collectMatchKeys(const DependencyInfo& dep)
{
    std::vector<std::string> matchKeys;
    std::set<std::string> seen;

    auto addKey = [&](const std::string& key) {
        if (key.empty()) return;
        if (seen.count(key) == 0) {
            seen.insert(key);
            matchKeys.push_back(key);
        }
    };

    std::vector<std::string> sourcePaths;
    if (!dep.unresolvedPath.empty()) sourcePaths.push_back(dep.unresolvedPath);
    if (!dep.path.empty()) sourcePaths.push_back(dep.path);

    for (const auto& sourcePath : sourcePaths) {
        std::string cleanName = getCleanFilename(sourcePath);
        if (cleanName.empty()) continue;

        addKey(cleanName);

        // .ma / .mb alternation for references
        size_t dotPos = cleanName.rfind('.');
        if (dotPos != std::string::npos && dep.type == "reference") {
            std::string ext = cleanName.substr(dotPos);
            std::string stem = cleanName.substr(0, dotPos);
            std::string altExt;
            if (ext == ".ma") altExt = ".mb";
            else if (ext == ".mb") altExt = ".ma";

            if (!altExt.empty()) {
                addKey(stem + altExt);
            }
        }

        // Wildcard for sequence patterns
        std::string wildcard = toWildcardFilename(cleanName, dep.type);
        if (!wildcard.empty()) {
            addKey(wildcard);
        }
    }

    return matchKeys;
}

// ============================================================================
// String / path utilities for matching
// ============================================================================

std::string RefCheckerUI::getCleanFilename(const std::string& path)
{
    if (path.empty()) return "";
    QString p = QString::fromUtf8(path.c_str());
    p.replace('\\', '/');
    int pos = p.lastIndexOf('/');
    QString filename = (pos >= 0) ? p.mid(pos + 1) : p;
    // Remove Maya copy number suffix {N}, e.g. "file.ma{2}" -> "file.ma"
    static QRegularExpression copyNumRe("\\{\\d+\\}$");
    filename.replace(copyNumRe, "");
    std::string result = filename.toLower().trimmed().toUtf8().toStdString();

    return result;
}

std::string RefCheckerUI::toWildcardFilename(
    const std::string& filename, const std::string& depType)
{
    std::string pattern = lowerString(filename);
    bool replaced = false;

    // Replace known tokens with *
    static const std::vector<std::string> tokens = {
        "<udim>", "<uvtile>", "{udim}", "{uvtile}",
        "<f>", "<frame>", "$f", "$f4", "$f3", "$f2", "$f1",
        "%04d", "%03d", "%02d", "%d",
        "####", "###", "##"
    };

    for (const auto& token : tokens) {
        if (token.empty()) {
            continue;
        }
        size_t pos = pattern.find(token);
        while (pos != std::string::npos) {
            pattern.replace(pos, token.size(), "*");
            replaced = true;
            pos = pattern.find(token, pos + 1);
        }
    }

    // Handle numeric sequences like texture.1001.exr -> texture.*.exr
    // Matches: [._-] followed by 3-6 digits followed by a dot
    if ((depType == "texture" || depType == "cache") && !replaced) {
        std::string result;
        size_t len = pattern.size();
        size_t i = 0;
        bool didReplace = false;
        while (i < len) {
            // Look for separator + 3-6 digits + dot
            if ((pattern[i] == '.' || pattern[i] == '_' || pattern[i] == '-') && i + 1 < len) {
                size_t digitStart = i + 1;
                size_t j = digitStart;
                while (j < len && std::isdigit((unsigned char)pattern[j])) ++j;
                size_t digitCount = j - digitStart;
                if (digitCount >= 3 && digitCount <= 6 && j < len && pattern[j] == '.') {
                    result += '*';
                    i = j; // skip separator + digits, continue from the dot
                    didReplace = true;
                    continue;
                }
            }
            result += pattern[i];
            ++i;
        }
        if (didReplace) {
            pattern = result;
            replaced = true;
        }
    }

    if (replaced && isWildcardPattern(pattern)) {
        return pattern;
    }
    return "";
}

bool RefCheckerUI::isWildcardPattern(const std::string& pattern)
{
    return pattern.find('*') != std::string::npos ||
           pattern.find('?') != std::string::npos ||
           pattern.find('[') != std::string::npos;
}

// Simple glob match: supports * and ? (case-insensitive, both inputs assumed lowercase)
static bool globMatch(const char* pattern, const char* str)
{
    while (*pattern) {
        if (*pattern == '*') {
            ++pattern;
            // Skip consecutive *
            while (*pattern == '*') ++pattern;
            if (!*pattern) return true;
            // Try matching * against 0..N chars
            for (const char* s = str; *s; ++s) {
                if (globMatch(pattern, s)) return true;
            }
            return globMatch(pattern, str + std::strlen(str)); // match empty tail
        } else if (*pattern == '?') {
            if (!*str) return false;
            ++pattern;
            ++str;
        } else {
            if (*pattern != *str) return false;
            ++pattern;
            ++str;
        }
    }
    return *str == '\0';
}

std::vector<std::string> RefCheckerUI::matchByPattern(const std::string& pattern)
{
    std::vector<std::string> matched;

    for (const auto& cachedName : fileCache_.cacheKeys) {
        if (globMatch(pattern.c_str(), cachedName.c_str())) {
            auto it = fileCache_.cache.find(cachedName);
            if (it != fileCache_.cache.end()) {
                matched.insert(matched.end(),
                               it->second.begin(), it->second.end());
            }
        }
    }

    return matched;
}

std::vector<std::string> RefCheckerUI::dedupePaths(
    const std::vector<std::string>& paths)
{
    std::vector<std::string> unique;
    std::set<std::string> seen;

    for (const auto& p : paths) {
        std::string norm = p;
        norm = lowerString(norm);
        for (auto& ch : norm) { if (ch == '\\') ch = '/'; }

        if (seen.count(norm) == 0) {
            seen.insert(norm);
            unique.push_back(p);
        }
    }
    return unique;
}

std::string RefCheckerUI::findBestMatch(
    const std::string& originalPath,
    const std::vector<std::string>& candidates)
{
    if (candidates.empty()) return "";
    if (candidates.size() == 1) return candidates[0];

    // Normalize original path
    std::string origNorm = originalPath;
    origNorm = lowerString(origNorm);
    for (auto& c : origNorm) { if (c == '\\') c = '/'; }

    // Split into parts
    std::vector<std::string> origParts;
    {
        std::istringstream iss(origNorm);
        std::string part;
        while (std::getline(iss, part, '/')) {
            if (!part.empty()) origParts.push_back(part);
        }
    }
    std::string origName = origParts.empty() ? "" : origParts.back();
    std::string origExt;
    {
        size_t dot = origName.rfind('.');
        if (dot != std::string::npos) origExt = origName.substr(dot);
    }
    std::set<std::string> origSet(origParts.begin(), origParts.end());

    int bestScore = -1;
    int bestTieBreak = 0;
    std::string bestMatch = candidates[0];

    for (const auto& candidate : candidates) {
        std::string candNorm = candidate;
        candNorm = lowerString(candNorm);
        for (auto& c : candNorm) { if (c == '\\') c = '/'; }

        std::vector<std::string> candParts;
        {
            std::istringstream iss(candNorm);
            std::string part;
            while (std::getline(iss, part, '/')) {
                if (!part.empty()) candParts.push_back(part);
            }
        }
        std::string candName = candParts.empty() ? "" : candParts.back();
        std::string candExt;
        {
            size_t dot = candName.rfind('.');
            if (dot != std::string::npos) candExt = candName.substr(dot);
        }

        int score = 0;
        if (candName == origName) score += 120;
        if (!candExt.empty() && candExt == origExt) score += 25;

        // Suffix directory matching
        int suffixMatches = 0;
        int oi = static_cast<int>(origParts.size()) - 2;
        int ci = static_cast<int>(candParts.size()) - 2;
        while (oi >= 0 && ci >= 0 && origParts[oi] == candParts[ci]) {
            ++suffixMatches;
            --oi;
            --ci;
        }
        score += suffixMatches * 15;

        // Common parts
        std::set<std::string> candSet(candParts.begin(), candParts.end());
        int common = 0;
        for (const auto& p : candSet) {
            if (origSet.count(p)) ++common;
        }
        score += common;

        int tieBreak = -static_cast<int>(candNorm.size());

        if (score > bestScore || (score == bestScore && tieBreak > bestTieBreak)) {
            bestScore = score;
            bestTieBreak = tieBreak;
            bestMatch = candidate;
        }
    }

    return bestMatch;
}

// ============================================================================
// Path utilities: resolvePathForApply, applyPath, toRelativePath
// ============================================================================

std::string RefCheckerUI::resolvePathForApply(const std::string& rawPath)
{
    if (rawPath.empty()) return rawPath;

    std::string normalized = rawPath;
    for (auto& c : normalized) { if (c == '\\') c = '/'; }

    // Check if absolute
    bool isAbs = false;
    if (!normalized.empty() && normalized[0] == '/') isAbs = true;
    if (normalized.size() >= 2 && std::isalpha((unsigned char)normalized[0])
        && normalized[1] == ':') isAbs = true;

    if (!isAbs) {
        // Resolve relative to scene directory
        std::string sceneDir = SceneScanner::getSceneDir();
        if (!sceneDir.empty()) {
            normalized = sceneDir + "/" + normalized;
        }
    }

    // Canonicalize: resolve . and .. segments so Maya gets a clean path
    std::vector<std::string> parts;
    std::string segment;
    // Preserve drive prefix (e.g. "D:")
    std::string prefix;
    size_t start = 0;
    if (normalized.size() >= 2 && std::isalpha((unsigned char)normalized[0])
        && normalized[1] == ':') {
        prefix = normalized.substr(0, 2);
        start = 2;
        if (start < normalized.size() && normalized[start] == '/') {
            prefix += '/';
            start++;
        }
    } else if (!normalized.empty() && normalized[0] == '/') {
        prefix = "/";
        start = 1;
    }

    std::istringstream ss(normalized.substr(start));
    while (std::getline(ss, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(segment);
        }
    }

    std::string result = prefix;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += '/';
        result += parts[i];
    }
    return result;
}

std::string RefCheckerUI::toRelativePath(const std::string& absPath)
{
    std::string sceneDir = SceneScanner::getSceneDir();
    if (sceneDir.empty()) return absPath;

    // Normalize both paths
    std::string normAbs = absPath;
    for (auto& c : normAbs) { if (c == '\\') c = '/'; }
    std::string normScene = sceneDir;
    for (auto& c : normScene) { if (c == '\\') c = '/'; }

    // Ensure scene dir ends with /
    if (!normScene.empty() && normScene.back() != '/') {
        normScene += '/';
    }

    // Case-insensitive comparison helpers
    std::string lowerAbs = lowerString(normAbs);
    std::string lowerScene = lowerString(normScene);

    // Simple prefix strip if file is under scene directory
    if (lowerAbs.substr(0, lowerScene.size()) == lowerScene) {
        return normAbs.substr(normScene.size());
    }

    // Different drive letters on Windows -> cannot make relative
    if (lowerAbs.size() >= 2 && lowerScene.size() >= 2 &&
        lowerAbs[1] == ':' && lowerScene[1] == ':' &&
        lowerAbs[0] != lowerScene[0]) {
        return normAbs;
    }

    // Compute ../  relative path by finding common ancestor
    // Split both paths into components
    auto splitPath = [](const std::string& p) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::istringstream iss(p);
        std::string part;
        while (std::getline(iss, part, '/')) {
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    };

    // Remove trailing slash from scene dir for splitting
    std::string sceneNoSlash = normScene;
    if (!sceneNoSlash.empty() && sceneNoSlash.back() == '/') {
        sceneNoSlash.pop_back();
    }
    std::vector<std::string> sceneParts = splitPath(sceneNoSlash);
    std::vector<std::string> absParts = splitPath(normAbs);

    std::vector<std::string> scenePartsLower = splitPath(lowerString(sceneNoSlash));
    std::vector<std::string> absPartsLower = splitPath(lowerAbs);

    // Find common prefix length (case-insensitive)
    size_t common = 0;
    size_t minLen = std::min(scenePartsLower.size(), absPartsLower.size());
    while (common < minLen && scenePartsLower[common] == absPartsLower[common]) {
        ++common;
    }

    if (common == 0) {
        // No common ancestor at all, return absolute
        return normAbs;
    }

    // Build relative path: go up from scene dir, then down to target
    std::string rel;
    for (size_t i = common; i < sceneParts.size(); ++i) {
        rel += "../";
    }
    for (size_t i = common; i < absParts.size(); ++i) {
        if (i > common) rel += "/";
        rel += absParts[i];
    }

    return rel;
}

bool RefCheckerUI::applyPath(DependencyInfo& dep, const std::string& newPath)
{
    // newPath may be relative (user-selected "Relative Path" mode).
    // Use the raw string for Maya so the scene can store a relative path,
    // but also compute a resolved absolute path for validation/logging.
    std::string mayaPath = newPath;
    for (auto& c : mayaPath) { if (c == '\\') c = '/'; }
    std::string resolvedPath = resolvePathForApply(mayaPath);
    std::string depType = dep.type;
    std::string node = dep.node;

    // Convert path to MString via wide-char to preserve Unicode
    MString mMayaPath = utf8ToMString(mayaPath);

    PluginLog::info("RefChecker", "ApplyFix: type=" + depType + ", node=" + node);
    PluginLog::info("RefChecker", "ApplyFix: new path(raw): " + mayaPath);
    if (resolvedPath != mayaPath) {
        PluginLog::info("RefChecker", "ApplyFix: new path(resolved): " + resolvedPath);
    }

    if (depType == "reference") {
        if (node.empty() || node == "unknown") {
            PluginLog::error("RefChecker", "ApplyFix: reference node does not exist: " + node);
            return false;
        }

        // Check node exists (node names are ASCII-safe, MEL is fine here)
        MString checkCmd = MString("objExists \"") + node.c_str() + "\"";
        int exists = 0;
        MGlobal::executeCommand(checkCmd, exists);
        if (!exists) {
            PluginLog::error("RefChecker", "ApplyFix: reference node does not exist: " + node);
            return false;
        }

        // Query current loaded state
        int isLoaded = isReferenceLoaded(node) ? 1 : 0;

        // Unload first if loaded (we need it unloaded to change the path)
        if (isLoaded) {
            std::string pyUnload = "import maya.cmds as cmds; cmds.file(unloadReference='" + pyStr(node) + "')";
            if (!execPython(pyUnload)) {
                PluginLog::warn("RefChecker", "ApplyFix: unload failed");
            }
        }

        // For references, update the stored path first and then try to load.
        // Phase 1: update the stored reference path without forcing a deep load
        // (this avoids heavy scene changes and reduces failure surface).
        // Phase 2: attempt to load by node name.

        bool pathUpdated = updateReferencePathNoLoad(node, mayaPath, resolvedPath);
        if (!pathUpdated) {
            PluginLog::warn("RefChecker", "ApplyFix: failed to update stored reference path: " + node);
        }

        // Try loading after path update.
        PluginLog::info("RefChecker", "ApplyFix: Phase2 loading reference: " + node);
        bool loaded = loadReferenceByNode(node);

        if (loaded) {
            PluginLog::info("RefChecker", "ApplyFix: reference loaded OK: " + node);
            dep.isLoaded = true;
        } else {
            PluginLog::warn("RefChecker", "ApplyFix: loadReference failed: " + node);
            dep.isLoaded = false;
        }

        // Return true if we successfully updated the stored path (or loaded it).
        // dep.isLoaded indicates the actual load state.
        return pathUpdated;

    } else if (depType == "texture") {
        MString nodeTypeResult;
        MString ntCmd = MString("nodeType \"") + node.c_str() + "\"";
        MGlobal::executeCommand(ntCmd, nodeTypeResult);
        std::string nodeType = toUtf8(nodeTypeResult);

        MString setCmd;
        if (nodeType == "file") {
            setCmd = MString("setAttr -type \"string\" \"")
                + node.c_str() + ".fileTextureName\" \"" + mMayaPath + "\"";
        } else if (nodeType == "aiImage") {
            setCmd = MString("setAttr -type \"string\" \"")
                + node.c_str() + ".filename\" \"" + mMayaPath + "\"";
        } else {
            PluginLog::error("RefChecker", "ApplyFix: unknown texture node type: " + nodeType);
            return false;
        }
        MStatus st = MGlobal::executeCommand(setCmd);
        return (st == MS::kSuccess);

    } else if (depType == "cache") {
        MString nodeTypeResult;
        MString ntCmd = MString("nodeType \"") + node.c_str() + "\"";
        MGlobal::executeCommand(ntCmd, nodeTypeResult);
        std::string nodeType = toUtf8(nodeTypeResult);

        MString setCmd;
        if (nodeType == "AlembicNode") {
            setCmd = MString("setAttr -type \"string\" \"")
                + node.c_str() + ".abc_File\" \"" + mMayaPath + "\"";
        } else if (nodeType == "gpuCache") {
            setCmd = MString("setAttr -type \"string\" \"")
                + node.c_str() + ".cacheFileName\" \"" + mMayaPath + "\"";
        } else {
            PluginLog::error("RefChecker", "ApplyFix: unknown cache node type: " + nodeType);
            return false;
        }
        MStatus st = MGlobal::executeCommand(setCmd);
        return (st == MS::kSuccess);

    } else if (depType == "audio") {
        MString setCmd = MString("setAttr -type \"string\" \"")
            + node.c_str() + ".filename\" \"" + mMayaPath + "\"";
        MStatus st = MGlobal::executeCommand(setCmd);
        return (st == MS::kSuccess);
    }

    PluginLog::error("RefChecker", "ApplyFix: unknown dep type: " + depType);
    return false;
}
