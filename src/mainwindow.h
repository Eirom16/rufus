#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QProgressBar>
#include <QLabel>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QSlider>
#include <QSpinBox>
#include <QToolBar>
#include <QAction>
#include <QThread>
#include <QProcess>

struct DiskInfo {
    QString name;      // e.g. "sdb"
    QString model;     // e.g. "Cruzer Blade"
    QString size;      // e.g. "14.9G"
    bool removable;    // true for USB
    bool readOnly;     // true for read-only
};

class BurnWorker;
class HashWorker;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void refreshDevices();
    void onBootSelectionChanged(int index);
    void onSelectIsoClicked();
    void onPartitionSchemeChanged(int index);
    void onTargetSystemChanged(int index);
    void onStartClicked();
    void onLogToggleClicked(bool checked);
    void onLanguageAction();
    void onAboutAction();
    void onSettingsAction();

    // Worker callbacks
    void onBurnProgress(double pct, const QString &status, const QString &speedEta);
    void onBurnLog(const QString &msg);
    void onBurnFinished(const QString &result);
    void onHashFinished(const QString &result);

private:
    void setupUi();
    void setupToolbar();
    void setupDriveProperties();
    void setupBootSelection();
    void setupFormatOptions();
    void setupStatusSection();
    void detectSystemRootDisk();
    void disableControls();
    void enableControls();
    void logMessage(const QString &msg);

    // UI state
    QString m_selectedIsoPath;
    qint64 m_selectedIsoSize = 0;
    QString m_systemRootDisk;
    QList<DiskInfo> m_disks;
    bool m_isRunning = false;

    // Toolbar
    QToolBar *m_toolbar = nullptr;
    QAction *m_langAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_logAction = nullptr;
    QAction *m_aboutAction = nullptr;

    // Drive Properties
    QComboBox *m_comboDevice = nullptr;
    QPushButton *m_btnRefresh = nullptr;
    QCheckBox *m_checkShowAll = nullptr;
    QCheckBox *m_checkOldBios = nullptr;
    QGroupBox *m_advancedDriveGroup = nullptr;

    // Boot Selection
    QComboBox *m_comboBootSelection = nullptr;
    QPushButton *m_btnSelectIso = nullptr;
    QComboBox *m_comboImageOption = nullptr;
    QLabel *m_lblIsoName = nullptr;
    QComboBox *m_comboPartitionScheme = nullptr;
    QComboBox *m_comboTargetSystem = nullptr;

    // Format Options
    QLineEdit *m_entryVolumeLabel = nullptr;
    QComboBox *m_comboFilesystem = nullptr;
    QComboBox *m_comboClusterSize = nullptr;
    QCheckBox *m_checkQuickFormat = nullptr;
    QCheckBox *m_checkExtendedLabel = nullptr;
    QCheckBox *m_checkBadBlocks = nullptr;
    QComboBox *m_comboNbPasses = nullptr;
    QCheckBox *m_checkPersistent = nullptr;
    QSlider *m_persistenceSlider = nullptr;
    QSpinBox *m_persistenceSize = nullptr;
    QComboBox *m_comboPersistenceUnits = nullptr;
    QCheckBox *m_checkUefiValidation = nullptr;
    QGroupBox *m_advancedFormatGroup = nullptr;

    // Status
    QGroupBox *m_statusGroup = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_lblStatus = nullptr;
    QLabel *m_lblSpeedEta = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QPushButton *m_btnLogToggle = nullptr;

    // Buttons
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnClose = nullptr;

    // Workers
    QThread *m_burnThread = nullptr;
    BurnWorker *m_burnWorker = nullptr;
    QThread *m_hashThread = nullptr;
    HashWorker *m_hashWorker = nullptr;
};

#endif // MAINWINDOW_H
