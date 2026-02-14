#pragma once
#ifndef REFCHECKERUI_H
#define REFCHECKERUI_H

#include <QDialog>
#include <QObject>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QThread>

#include "SceneScanner.h"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <atomic>

struct FileScanFilter {
    std::set<std::string> exactNames;        // lowercase filenames
    std::vector<std::string> wildcardNames;  // lowercase wildcard patterns
    std::set<std::string> allowedExts;       // lowercase extensions
};

class BatchLocateWorker : public QObject {
    Q_OBJECT

public:
    struct Result {
        std::map<std::string, std::vector<std::string>> cache;
        int scannedCount = 0;
        bool cancelled = false;
    };

    BatchLocateWorker(const std::string& searchDir,
                      const FileScanFilter& filter,
                      QObject* parent = nullptr);

    void requestCancel();
    const Result& result() const;

signals:
    void progress(int count);
    void statusText(const QString& text);
    void finished();

public slots:
    void run();

private:
    std::string searchDir_;
    FileScanFilter filter_;
    std::atomic<bool> cancelled_;
    Result result_;
};

class RefCheckerUI : public QDialog {
    Q_OBJECT

public:
    explicit RefCheckerUI(QWidget* parent = nullptr);
    ~RefCheckerUI() override;

    static RefCheckerUI* instance();
    static void showUI();

private slots:
    void onScan();
    void onSelectAllMissing();
    void onBatchLocate();
    void onApplyFixes();
    void onFilterChanged();
    void onTypeFilterChanged(int index);
    void onLocateSingle(int row);
    void onCheckboxChanged(int row, int col);
    void onSearchTextChanged(const QString& text);
    void onLoadReference(int depIndex);
    void onLoadAllUnloaded();

private:
    void setupUI();
    void refreshList();
    void updateStats();
    void checkAndWarnRisks();
    void onBatchLocateDeferred();
    void onLocateSingleDeferred(int depIndex);

    // File cache for batch locate
    struct FileCache {
        std::map<std::string, std::vector<std::string>> cache; // lowercase name -> paths
        std::vector<std::string> cacheKeys;
        std::set<std::string> cacheIndex; // normalized paths for dedup
        int totalCount;
    };

    int mergeCache(const std::map<std::string, std::vector<std::string>>& newCache);
    std::pair<std::map<std::string, std::vector<std::string>>, int>
        buildFileCache(const std::string& searchDir,
                       std::function<bool(int)> progressCb = nullptr,
                       const FileScanFilter* filter = nullptr,
                       std::atomic<bool>* cancelFlag = nullptr);
    int scanDirIntoCache(const std::string& directory,
                         std::map<std::string, std::vector<std::string>>& cache);
    FileScanFilter buildScanFilter();

    // Auto-matching
    void runAutoMatch();
    std::string autoMatchDependency(const DependencyInfo& dep);
    std::vector<std::string> collectMatchKeys(const DependencyInfo& dep);
    std::string getCleanFilename(const std::string& path);
    std::string toWildcardFilename(const std::string& filename, const std::string& depType);
    bool isWildcardPattern(const std::string& pattern);
    std::vector<std::string> matchByPattern(const std::string& pattern);
    std::vector<std::string> dedupePaths(const std::vector<std::string>& paths);
    std::string findBestMatch(const std::string& originalPath,
                              const std::vector<std::string>& candidates);

    // Path utilities
    std::string resolvePathForApply(const std::string& rawPath);
    bool applyPath(DependencyInfo& dep, const std::string& newPath);
    std::string toRelativePath(const std::string& absPath);

    // UI widgets
    QLineEdit* searchDirField_;
    QCheckBox* filterAllCheck_;
    QCheckBox* filterMissingCheck_;
    QComboBox* typeFilterCombo_;
    QLineEdit* maxDisplayField_;
    QLineEdit* searchFilterField_;
    QLabel* statsLabel_;
    QTableWidget* tableWidget_;
    QComboBox* pathModeCombo_;
    QThread* scanThread_;
    BatchLocateWorker* scanWorker_;
    bool scanInProgress_;

    // Data
    std::vector<DependencyInfo> dependencies_;
    std::vector<std::string> searchDirs_;
    FileCache fileCache_;

    static RefCheckerUI* instance_;
};

#endif // REFCHECKERUI_H
