#pragma once
#ifndef BATCHEXPORTERUI_H
#define BATCHEXPORTERUI_H

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QRadioButton>
#include <QButtonGroup>
#include <QProgressBar>
#include <QStatusBar>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>
#include <QStyledItemDelegate>
#include <QGroupBox>
#include <QFrame>

#include "NamingUtils.h"
#include "AnimExporter.h"

#include <string>
#include <vector>

// Delegate for editable filename column
class FilenameDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit FilenameDelegate(QObject* parent = nullptr);
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override;
};

class BatchExporterUI : public QDialog {
    Q_OBJECT

public:
    explicit BatchExporterUI(QWidget* parent = nullptr);
    ~BatchExporterUI() override;

    static BatchExporterUI* instance();
    static void showUI();

private slots:
    void onBrowseOutput();
    void onFrameModeChanged();
    void onScanScene();
    void onSelectAll();
    void onSelectNone();
    void onExport();
    void onCancel();
    void onToggleFbxOptions();
    void onCheckboxChanged(int row, int col);

private:
    void setupUI();
    void refreshList();
    void syncFilenamesFromUI();
    void setStatus(const std::string& text);
    std::pair<int, int> getFrameRange();
    FbxExportOptions collectFbxOptions() const;
    void setExportingUI(bool exporting);

    // UI widgets
    QLineEdit* outputDirField_;
    QRadioButton* radioTimeline_;
    QRadioButton* radioCustom_;
    QSpinBox* customStartSpin_;
    QSpinBox* customEndSpin_;
    QPushButton* scanBtn_;
    QPushButton* selectAllBtn_;
    QPushButton* selectNoneBtn_;
    QPushButton* exportBtn_;
    QPushButton* cancelBtn_;
    QPushButton* fbxOptionsToggleBtn_;
    QWidget* fbxOptionsContainer_;
    QTableWidget* tableWidget_;
    QProgressBar* progressBar_;
    QStatusBar* statusBar_;

    // FBX Options widgets — Skeleton
    QCheckBox* skelAnimOnlyCheck_;
    QCheckBox* skelBakeComplexCheck_;
    QCheckBox* skelSkeletonDefsCheck_;
    QCheckBox* skelConstraintsCheck_;
    QCheckBox* skelInputConnsCheck_;
    QCheckBox* skelBlendShapeCheck_;

    // FBX Options widgets — BlendShape
    QCheckBox* bsShapesCheck_;
    QCheckBox* bsSmoothMeshCheck_;
    QCheckBox* bsIncludeSkeletonCheck_;

    // FBX Options widgets — Common
    QComboBox* fbxVersionCombo_;
    QComboBox* fbxUpAxisCombo_;

    // FPS Override
    QCheckBox* fpsOverrideCheck_;
    QSpinBox*  fpsOverrideSpin_;

    // Log option
    QCheckBox* frameRangeLogCheck_;

    // Cancel flag
    bool cancelRequested_;

    // Data
    std::vector<ExportItem> exportItems_;

    static BatchExporterUI* instance_;
};

#endif // BATCHEXPORTERUI_H
