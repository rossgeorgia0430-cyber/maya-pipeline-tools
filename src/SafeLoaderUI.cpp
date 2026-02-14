#include "SafeLoaderUI.h"
#include "PluginLog.h"

#include <maya/MGlobal.h>
#include <maya/MQtUtil.h>
#include <maya/MString.h>
#include <maya/MStringArray.h>

#include <QFileInfo>
#include <QByteArray>

#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

// Convert MString to UTF-8 std::string safely on Windows
static std::string toUtf8(const MString& ms) {
#ifdef _WIN32
    const wchar_t* wstr = ms.asWChar();
    if (!wstr || !*wstr) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string(ms.asChar());
    std::string result(len, '\0');
    int ret = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    if (ret <= 0) return std::string(ms.asChar());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
#else
    return std::string(ms.asChar());
#endif
}

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

static QString utf8ToQString(const std::string& s) {
    return QString::fromUtf8(s.c_str(), static_cast<int>(s.size()));
}

static std::string qStringToUtf8(const QString& s) {
    QByteArray u8 = s.toUtf8();
    return std::string(u8.constData(), static_cast<size_t>(u8.size()));
}

// ============================================================================
// Static members
// ============================================================================

SafeLoaderUI* SafeLoaderUI::instance_ = nullptr;

SafeLoaderUI* SafeLoaderUI::instance()
{
    return instance_;
}

void SafeLoaderUI::showUI()
{
    if (instance_) {
        instance_->scanReferences();
        instance_->refreshTable();
        instance_->raise();
        instance_->activateWindow();
        return;
    }

    QWidget* mayaMainWindow = MQtUtil::mainWindow();
    instance_ = new SafeLoaderUI(mayaMainWindow);
    instance_->setAttribute(Qt::WA_DeleteOnClose, true);
    instance_->show();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

SafeLoaderUI::SafeLoaderUI(QWidget* parent)
    : QDialog(parent)
    , tableWidget_(nullptr)
    , statusLabel_(nullptr)
{
    setupUI();
    scanReferences();
    refreshTable();
}

SafeLoaderUI::~SafeLoaderUI()
{
    instance_ = nullptr;
}

// ============================================================================
// setupUI
// ============================================================================

void SafeLoaderUI::setupUI()
{
    setWindowTitle("Safe Load References");
    setMinimumSize(800, 450);
    resize(900, 550);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 4);
    mainLayout->setSpacing(6);

    // ----- Button row -----
    {
        QHBoxLayout* row = new QHBoxLayout();

        QPushButton* refreshBtn = new QPushButton("Refresh");
        refreshBtn->setToolTip(QString::fromUtf8(
            u8"重新扫描场景中的所有引用，\n"
            u8"刷新列表中的加载状态和文件信息。"));
        connect(refreshBtn, &QPushButton::clicked, this, &SafeLoaderUI::onRefresh);
        row->addWidget(refreshBtn);

        row->addSpacing(10);

        QPushButton* loadSelBtn = new QPushButton("Load Selected");
        loadSelBtn->setToolTip(QString::fromUtf8(
            u8"加载列表中勾选的引用文件。\n"
            u8"先勾选左侧复选框，再点击此按钮。"));
        loadSelBtn->setStyleSheet(
            "QPushButton { background-color: #668866; color: white; }");
        connect(loadSelBtn, &QPushButton::clicked, this, &SafeLoaderUI::onLoadSelected);
        row->addWidget(loadSelBtn);

        QPushButton* loadAllBtn = new QPushButton("Load All");
        loadAllBtn->setToolTip(QString::fromUtf8(
            u8"一次性加载场景中所有未加载的引用。\n"
            u8"引用较多时可能需要较长时间。"));
        connect(loadAllBtn, &QPushButton::clicked, this, &SafeLoaderUI::onLoadAll);
        row->addWidget(loadAllBtn);

        QPushButton* unloadAllBtn = new QPushButton("Unload All");
        unloadAllBtn->setToolTip(QString::fromUtf8(
            u8"卸载场景中所有已加载的引用。\n"
            u8"卸载后引用仍保留在场景中，可随时重新加载。"));
        connect(unloadAllBtn, &QPushButton::clicked, this, &SafeLoaderUI::onUnloadAll);
        row->addWidget(unloadAllBtn);

        row->addSpacing(10);

        QPushButton* removeMissingBtn = new QPushButton("Remove Missing");
        removeMissingBtn->setToolTip(QString::fromUtf8(
            u8"从场景中移除所有文件不存在的引用。\n"
            u8"此操作不可撤销，请谨慎使用。"));
        removeMissingBtn->setStyleSheet(
            "QPushButton { background-color: #996655; color: white; }");
        connect(removeMissingBtn, &QPushButton::clicked, this, &SafeLoaderUI::onRemoveMissing);
        row->addWidget(removeMissingBtn);

        row->addStretch();

        mainLayout->addLayout(row);
    }

    // ----- Table -----
    {
        tableWidget_ = new QTableWidget(0, 5);
        tableWidget_->setHorizontalHeaderLabels(
            QStringList() << "" << "Status" << "Exists" << "Size" << "File Path");

        QHeaderView* header = tableWidget_->horizontalHeader();
        header->resizeSection(0, 30);   // Checkbox
        header->resizeSection(1, 80);   // Status
        header->resizeSection(2, 60);   // Exists
        header->resizeSection(3, 80);   // Size
        header->setStretchLastSection(true);

        tableWidget_->verticalHeader()->setVisible(false);
        tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tableWidget_->setAlternatingRowColors(true);

        mainLayout->addWidget(tableWidget_, 1);
    }

    // ----- Status label -----
    {
        statusLabel_ = new QLabel("Ready");
        QFont boldFont = statusLabel_->font();
        boldFont.setBold(true);
        statusLabel_->setFont(boldFont);
        mainLayout->addWidget(statusLabel_);
    }
}

// ============================================================================
// scanReferences
// ============================================================================

void SafeLoaderUI::scanReferences()
{
    refs_.clear();

    // Query all reference files via MEL
    MStringArray refFiles;
    MStatus status = MGlobal::executeCommand(
        "file -q -reference", refFiles);
    if (status != MS::kSuccess) return;

    for (unsigned int i = 0; i < refFiles.length(); ++i) {
        RefEntry entry;
        entry.filePath = toUtf8(refFiles[i]);

        // Get reference node name
        MString refNodeResult;
        MString rnCmd = MString("referenceQuery -referenceNode \"")
            + refFiles[i] + "\"";
        if (MGlobal::executeCommand(rnCmd, refNodeResult) == MS::kSuccess) {
            entry.refNode = toUtf8(refNodeResult);
        }

        // Check if loaded
        int loaded = 0;
        MString loadCmd = MString("referenceQuery -isLoaded \"")
            + refFiles[i] + "\"";
        MGlobal::executeCommand(loadCmd, loaded);
        entry.isLoaded = (loaded != 0);

        // Check file existence and size
        QFileInfo fi(utf8ToQString(entry.filePath));
        entry.fileExists = fi.exists();
        entry.fileSize = fi.exists() ? fi.size() : 0;

        refs_.push_back(entry);
    }
}

// ============================================================================
// refreshTable
// ============================================================================

void SafeLoaderUI::refreshTable()
{
    tableWidget_->blockSignals(true);

    int rowCount = static_cast<int>(refs_.size());
    tableWidget_->setRowCount(rowCount);

    int loadedCount = 0;
    int missingCount = 0;

    for (int row = 0; row < rowCount; ++row) {
        const RefEntry& ref = refs_[row];

        // Column 0: Checkbox
        QTableWidgetItem* checkItem = tableWidget_->item(row, 0);
        if (!checkItem) {
            checkItem = new QTableWidgetItem();
            checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            tableWidget_->setItem(row, 0, checkItem);
        }
        checkItem->setCheckState(Qt::Unchecked);

        // Column 1: Status (Loaded / Unloaded)
        QTableWidgetItem* statusItem = tableWidget_->item(row, 1);
        if (!statusItem) {
            statusItem = new QTableWidgetItem();
            statusItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 1, statusItem);
        }
        if (ref.isLoaded) {
            statusItem->setText("Loaded");
            statusItem->setForeground(QBrush(QColor(50, 160, 50)));
            ++loadedCount;
        } else {
            statusItem->setText("Unloaded");
            statusItem->setForeground(QBrush(QColor(180, 130, 50)));
        }

        // Column 2: File exists
        QTableWidgetItem* existsItem = tableWidget_->item(row, 2);
        if (!existsItem) {
            existsItem = new QTableWidgetItem();
            existsItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 2, existsItem);
        }
        if (ref.fileExists) {
            existsItem->setText("Yes");
            existsItem->setForeground(QBrush(QColor(50, 160, 50)));
        } else {
            existsItem->setText("No");
            existsItem->setForeground(QBrush(QColor(200, 50, 50)));
            ++missingCount;
        }

        // Column 3: Size
        QTableWidgetItem* sizeItem = tableWidget_->item(row, 3);
        if (!sizeItem) {
            sizeItem = new QTableWidgetItem();
            sizeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 3, sizeItem);
        }
        if (ref.fileExists && ref.fileSize > 0) {
            double sizeMb = ref.fileSize / (1024.0 * 1024.0);
            if (sizeMb >= 1.0) {
                sizeItem->setText(QString("%1 MB").arg(sizeMb, 0, 'f', 1));
            } else {
                double sizeKb = ref.fileSize / 1024.0;
                sizeItem->setText(QString("%1 KB").arg(sizeKb, 0, 'f', 0));
            }
        } else {
            sizeItem->setText("-");
        }

        // Column 4: File path
        QTableWidgetItem* pathItem = tableWidget_->item(row, 4);
        if (!pathItem) {
            pathItem = new QTableWidgetItem();
            pathItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            tableWidget_->setItem(row, 4, pathItem);
        }
        QString path = utf8ToQString(ref.filePath);
        pathItem->setText(path);
        pathItem->setToolTip(path);
    }

    tableWidget_->blockSignals(false);

    // Update status
    QString statusText = QString("Total: %1 | Loaded: %2 | Unloaded: %3 | Missing: %4")
        .arg(rowCount)
        .arg(loadedCount)
        .arg(rowCount - loadedCount)
        .arg(missingCount);
    statusLabel_->setText(statusText);
}

// ============================================================================
// onRefresh
// ============================================================================

void SafeLoaderUI::onRefresh()
{
    scanReferences();
    refreshTable();
}

// ============================================================================
// onLoadSelected
// ============================================================================

void SafeLoaderUI::onLoadSelected()
{
    int loaded = 0;
    int failed = 0;

    for (int row = 0; row < tableWidget_->rowCount(); ++row) {
        QTableWidgetItem* checkItem = tableWidget_->item(row, 0);
        if (!checkItem || checkItem->checkState() != Qt::Checked) continue;
        if (row >= static_cast<int>(refs_.size())) continue;

        RefEntry& ref = refs_[row];
        if (ref.isLoaded) continue;
        if (ref.refNode.empty()) continue;

        std::ostringstream logMsg;
        logMsg << "Loading: " << ref.filePath;
        PluginLog::info("SafeLoader", logMsg.str());

        std::string cmd = "file -loadReference \"" + ref.refNode + "\"";
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd));
        QApplication::processEvents();

        if (status == MS::kSuccess) {
            ref.isLoaded = true;
            ++loaded;
        } else {
            ++failed;
            PluginLog::warn("SafeLoader", "Failed to load: " + ref.filePath);
        }
    }

    refreshTable();

    QString msg = QString("Loaded: %1, Failed: %2").arg(loaded).arg(failed);
    PluginLog::info("SafeLoader", qStringToUtf8(msg));
}

// ============================================================================
// onLoadAll
// ============================================================================

void SafeLoaderUI::onLoadAll()
{
    int loaded = 0;
    int failed = 0;
    int total = 0;

    for (auto& ref : refs_) {
        if (ref.isLoaded) continue;
        if (ref.refNode.empty()) continue;
        ++total;
    }

    if (total == 0) {
        QMessageBox::information(this, "Load All", "All references are already loaded.");
        return;
    }

    int current = 0;
    for (auto& ref : refs_) {
        if (ref.isLoaded) continue;
        if (ref.refNode.empty()) continue;
        ++current;

        std::ostringstream logMsg;
        logMsg << "[" << current << "/" << total
               << "] Loading: " << ref.filePath;
        PluginLog::info("SafeLoader", logMsg.str());

        std::string cmd = "file -loadReference \"" + ref.refNode + "\"";
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd));
        QApplication::processEvents();

        if (status == MS::kSuccess) {
            ref.isLoaded = true;
            ++loaded;
        } else {
            ++failed;
            PluginLog::warn("SafeLoader", "Failed to load: " + ref.filePath);
        }
    }

    refreshTable();

    QString msg = QString("Load All complete.\nLoaded: %1\nFailed: %2")
        .arg(loaded).arg(failed);
    QMessageBox::information(this, "Load All", msg);
}

// ============================================================================
// onUnloadAll
// ============================================================================

void SafeLoaderUI::onUnloadAll()
{
    int unloaded = 0;

    for (auto& ref : refs_) {
        if (!ref.isLoaded) continue;
        if (ref.refNode.empty()) continue;

        std::string cmd = "file -unloadReference \"" + ref.refNode + "\"";
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd));
        QApplication::processEvents();

        if (status == MS::kSuccess) {
            ref.isLoaded = false;
            ++unloaded;
        } else {
            PluginLog::warn("SafeLoader", "Failed to unload: " + ref.filePath);
        }
    }

    refreshTable();

    QString msg = QString("Unloaded %1 reference(s).").arg(unloaded);
    PluginLog::info("SafeLoader", qStringToUtf8(msg));
}

// ============================================================================
// onRemoveMissing
// ============================================================================

void SafeLoaderUI::onRemoveMissing()
{
    // Count missing
    int missingCount = 0;
    for (const auto& ref : refs_) {
        if (!ref.fileExists) ++missingCount;
    }

    if (missingCount == 0) {
        QMessageBox::information(this, "Remove Missing",
            "No missing references found.");
        return;
    }

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "Remove Missing References",
        QString("Remove %1 reference(s) whose files do not exist?\n\n"
                "This cannot be undone.")
            .arg(missingCount),
        QMessageBox::Ok | QMessageBox::Cancel);
    if (confirm != QMessageBox::Ok) return;

    int removed = 0;
    // Iterate in reverse so removal doesn't invalidate indices
    for (int i = static_cast<int>(refs_.size()) - 1; i >= 0; --i) {
        RefEntry& ref = refs_[i];
        if (ref.fileExists) continue;
        if (ref.refNode.empty()) continue;

        PluginLog::info("SafeLoader", "Removing missing ref: " + ref.filePath);

        std::string cmd = "file -referenceNode \"" + ref.refNode + "\" -removeReference";
        MStatus status = MGlobal::executeCommand(utf8ToMString(cmd));
        QApplication::processEvents();

        if (status == MS::kSuccess) {
            refs_.erase(refs_.begin() + i);
            ++removed;
        } else {
            PluginLog::warn("SafeLoader", "Failed to remove: " + ref.filePath);
        }
    }

    refreshTable();

    QString msg = QString("Removed %1 missing reference(s).").arg(removed);
    QMessageBox::information(this, "Remove Missing", msg);
}
