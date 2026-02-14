#pragma once
#ifndef SAFELOADERUI_H
#define SAFELOADERUI_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QApplication>

#include <string>
#include <vector>

struct RefEntry {
    std::string refNode;
    std::string filePath;
    bool isLoaded;
    bool fileExists;
    qint64 fileSize;
};

class SafeLoaderUI : public QDialog {
    Q_OBJECT

public:
    explicit SafeLoaderUI(QWidget* parent = nullptr);
    ~SafeLoaderUI() override;

    static SafeLoaderUI* instance();
    static void showUI();

private slots:
    void onRefresh();
    void onLoadSelected();
    void onLoadAll();
    void onUnloadAll();
    void onRemoveMissing();

private:
    void setupUI();
    void scanReferences();
    void refreshTable();

    QTableWidget* tableWidget_;
    QLabel* statusLabel_;

    std::vector<RefEntry> refs_;

    static SafeLoaderUI* instance_;
};

#endif // SAFELOADERUI_H
