#include "mainwindow.h"
#include "workers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QTimer>
#include <QStyle>
#include <QFormLayout>
#include <QScrollArea>
#include <QRegularExpression>
#include <QScrollBar>

#include <unistd.h>
#include <sys/stat.h>

#define APP_VERSION "1.0.0"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Rufus Qt v" APP_VERSION);
    setMinimumSize(760, 620);
    resize(820, 650);

    setupUi();
    detectSystemRootDisk();
    QTimer::singleShot(100, this, &MainWindow::refreshDevices);
}

MainWindow::~MainWindow()
{
    if (m_burnThread) {
        m_burnThread->quit();
        m_burnThread->wait();
    }
    if (m_hashThread) {
        m_hashThread->quit();
        m_hashThread->wait();
    }
}

// ── UI Setup ──────────────────────────────────────────────────────────

void MainWindow::setupUi()
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(8, 4, 8, 8);
    mainLayout->setSpacing(4);

    // Apply global stylesheet
    setStyleSheet(
        "QGroupBox { font-weight: bold; border: 1px solid #bdc3c7; border-radius: 6px; "
        "margin-top: 10px; padding-top: 16px; padding-bottom: 4px; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; } "
        "QGroupBox#flatGroup { border: none; margin-top: 0; padding-top: 4px; font-weight: normal; } "
        "QPushButton#btnGray { padding: 5px 12px; border-radius: 4px; "
        "background-color: #ecf0f1; border: 1px solid #bdc3c7; } "
        "QPushButton#btnGray:hover { background-color: #dfe6e9; } "
        "QPushButton#btnSelect { padding: 5px 18px; border-radius: 4px; font-weight: bold; } "
        "QPlainTextEdit { background-color: #1e1e1e; color: #a8e6a3; "
        "font-family: 'Fira Code', 'Monospace', monospace; font-size: 11px; } "
        "QComboBox { padding: 3px 6px; min-height: 22px; } "
        "QLineEdit { padding: 3px 6px; min-height: 22px; }");

    setupToolbar();
    mainLayout->addWidget(m_toolbar);

    // ── Drive Properties ──
    setupDriveProperties();
    mainLayout->addWidget(m_advancedDriveGroup);

    // ── Boot Selection ──
    auto *bootGroup = new QGroupBox("Boot Selection");
    auto *bootGrid = new QGridLayout(bootGroup);
    bootGrid->setSpacing(6);

    auto *lblBoot = new QLabel("Boot selection:");
    m_comboBootSelection = new QComboBox();
    m_comboBootSelection->addItem("Disk or ISO image (Please select)");
    m_comboBootSelection->addItem("Non bootable");
    m_comboBootSelection->addItem("FreeDOS");
    connect(m_comboBootSelection, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBootSelectionChanged);

    m_btnSelectIso = new QPushButton("SELECT");
    m_btnSelectIso->setObjectName("btnSelect");
    connect(m_btnSelectIso, &QPushButton::clicked, this, &MainWindow::onSelectIsoClicked);

    auto *bootRow = new QHBoxLayout();
    bootRow->addWidget(m_comboBootSelection, 1);
    bootRow->addWidget(m_btnSelectIso);
    bootGrid->addLayout(bootRow, 0, 0, 1, 2);

    auto *lblIsoTitle = new QLabel("Selected file:");
    m_lblIsoName = new QLabel("No image selected");
    m_lblIsoName->setStyleSheet("color: #7f8c8d; font-style: italic; font-weight: normal;");
    bootGrid->addWidget(lblIsoTitle, 1, 0);
    bootGrid->addWidget(m_lblIsoName, 1, 1);

    auto *lblPartition = new QLabel("Partition scheme:");
    m_comboPartitionScheme = new QComboBox();
    m_comboPartitionScheme->addItems({"GPT", "MBR"});
    connect(m_comboPartitionScheme, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPartitionSchemeChanged);

    auto *lblTarget = new QLabel("Target system:");
    m_comboTargetSystem = new QComboBox();
    m_comboTargetSystem->addItems({"UEFI (non CSM)", "BIOS or UEFI-CSM"});
    connect(m_comboTargetSystem, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTargetSystemChanged);

    bootGrid->addWidget(lblPartition, 2, 0);
    bootGrid->addWidget(m_comboPartitionScheme, 2, 1);
    bootGrid->addWidget(lblTarget, 3, 0);
    bootGrid->addWidget(m_comboTargetSystem, 3, 1);
    mainLayout->addWidget(bootGroup);

    // ── Format Options ──
    setupFormatOptions();
    mainLayout->addWidget(m_advancedFormatGroup);

    // ── Status ──
    setupStatusSection();
    mainLayout->addWidget(m_statusGroup, 1);

    // ── Bottom controls ──
    auto *bottomLayout = new QHBoxLayout();
    bottomLayout->setSpacing(10);

    m_btnLogToggle = new QPushButton("Log Drawer");
    m_btnLogToggle->setCheckable(true);
    m_btnLogToggle->setChecked(true);
    m_btnLogToggle->setObjectName("btnGray");
    connect(m_btnLogToggle, &QPushButton::toggled, this, &MainWindow::onLogToggleClicked);

    m_btnStart = new QPushButton("START");
    m_btnStart->setObjectName("btnStart");
    m_btnStart->setMinimumWidth(140);
    m_btnStart->setStyleSheet(
        "QPushButton#btnStart { background-color: #27ae60; color: white; font-weight: bold; "
        "padding: 8px 24px; border-radius: 4px; font-size: 14px; } "
        "QPushButton#btnStart:hover { background-color: #2ecc71; } "
        "QPushButton#btnStart:disabled { background-color: #7f8c8d; color: #ecf0f1; }");
    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStartClicked);

    m_btnClose = new QPushButton("CLOSE");
    m_btnClose->setObjectName("btnGray");
    connect(m_btnClose, &QPushButton::clicked, this, &QWidget::close);

    bottomLayout->addWidget(m_btnLogToggle);
    bottomLayout->addStretch();
    bottomLayout->addWidget(m_btnStart);
    bottomLayout->addWidget(m_btnClose);

    mainLayout->addLayout(bottomLayout);
    setCentralWidget(centralWidget);
}

void MainWindow::setupToolbar()
{
    m_toolbar = new QToolBar("Main Toolbar", this);
    m_toolbar->setMovable(false);
    m_toolbar->setIconSize(QSize(20, 20));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_langAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView), "Language");
    connect(m_langAction, &QAction::triggered, this, &MainWindow::onLanguageAction);

    m_settingsAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_ComputerIcon), "Settings");
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::onSettingsAction);

    m_logAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_FileDialogContentsView), "Log");
    connect(m_logAction, &QAction::triggered, this, [this]() {
        m_btnLogToggle->toggle();
    });

    m_aboutAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_MessageBoxInformation), "About");
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::onAboutAction);
}

void MainWindow::setupDriveProperties()
{
    m_advancedDriveGroup = new QGroupBox("Drive Properties", this);

    auto *layout = new QVBoxLayout(m_advancedDriveGroup);

    // Device row
    auto *deviceRow = new QHBoxLayout();
    auto *lblDevice = new QLabel("Device:");
    m_comboDevice = new QComboBox();
    m_comboDevice->setMinimumWidth(350);
    m_comboDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnRefresh = new QPushButton();
    m_btnRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_btnRefresh->setToolTip("Refresh device list");
    connect(m_btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);

    deviceRow->addWidget(lblDevice);
    deviceRow->addWidget(m_comboDevice, 1);
    deviceRow->addWidget(m_btnRefresh);
    layout->addLayout(deviceRow);

    // Advanced drive properties (collapsible)
    auto *advDriveLayout = new QVBoxLayout();
    m_checkShowAll = new QCheckBox("List USB Hard Drives (Use with caution)");
    connect(m_checkShowAll, &QCheckBox::toggled, this, &MainWindow::refreshDevices);
    advDriveLayout->addWidget(m_checkShowAll);

    m_checkOldBios = new QCheckBox("Add fixes for old BIOSes (extra partition, alignment)");
    advDriveLayout->addWidget(m_checkOldBios);

    auto *advDriveWidget = new QWidget();
    advDriveWidget->setLayout(advDriveLayout);

    auto *expander = new QGroupBox("Show advanced drive properties");
    expander->setCheckable(true);
    expander->setChecked(false);
    expander->setFlat(true);
    auto *expanderLayout = new QVBoxLayout(expander);
    expanderLayout->setContentsMargins(0, 0, 0, 0);
    expanderLayout->addWidget(advDriveWidget);
    layout->addWidget(expander);
}

void MainWindow::setupBootSelection()
{
}

void MainWindow::setupFormatOptions()
{
}

void MainWindow::setupStatusSection()
{
    m_statusGroup = new QGroupBox("Status", this);
    auto *layout = new QVBoxLayout(m_statusGroup);

    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedHeight(24);
    layout->addWidget(m_progressBar);

    auto *infoRow = new QHBoxLayout();
    m_lblStatus = new QLabel("Ready");
    m_lblSpeedEta = new QLabel();
    m_lblSpeedEta->setAlignment(Qt::AlignRight);
    m_lblSpeedEta->setStyleSheet("font-family: 'Fira Code', monospace; color: #2980b9;");
    infoRow->addWidget(m_lblStatus, 1);
    infoRow->addWidget(m_lblSpeedEta);
    layout->addLayout(infoRow);

    m_logView = new QPlainTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    m_logView->setFixedHeight(140);
    m_logView->setUndoRedoEnabled(false);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_logView, 1);
}

// ── Device Detection ──────────────────────────────────────────────────

void MainWindow::detectSystemRootDisk()
{
    QProcess proc;
    proc.start("findmnt", {"-n", "-o", "SOURCE", "/"});
    proc.waitForFinished(3000);
    QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();

    if (output.startsWith("/dev/")) {
        QString dev = output.mid(5); // remove "/dev/"

        if (dev.startsWith("nvme")) {
            int pIdx = dev.lastIndexOf('p');
            if (pIdx > 0)
                dev = dev.left(pIdx);
        } else {
            while (dev.length() > 0 && dev.back().isDigit())
                dev.chop(1);
        }
        m_systemRootDisk = dev;
    }
    logMessage("Detected system root disk to protect: /dev/" + m_systemRootDisk);
}

void MainWindow::refreshDevices()
{
    m_comboDevice->clear();
    m_disks.clear();

    QProcess proc;
    proc.start("lsblk", {"-d", "-n", "-P", "-o", "NAME,SIZE,MODEL,RO,RM"});
    proc.waitForFinished(3000);

    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    bool showAll = m_checkShowAll->isChecked();

    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
        auto getVal = [&](const QString &key) -> QString {
            QRegularExpression re(key + "=\"([^\"]*)\"");
            auto m = re.match(line);
            return m.hasMatch() ? m.captured(1) : QString();
        };

        QString name = getVal("NAME");
        if (name.isEmpty()) continue;
        if (name.startsWith("loop") || name.startsWith("zram")) continue;

        bool removable = getVal("RM").toInt();
        bool readOnly = getVal("RO").toInt();
        if (readOnly) continue;
        if (name == m_systemRootDisk) continue;
        if (!removable && !showAll) continue;

        DiskInfo disk;
        disk.name = name;
        disk.size = getVal("SIZE");
        disk.model = getVal("MODEL");
        if (disk.model.isEmpty()) disk.model = "Generic Flash Drive";
        disk.removable = removable;
        disk.readOnly = readOnly;
        m_disks.append(disk);

        QString label = QString("/dev/%1 - %2 [%3]%4")
            .arg(disk.name, disk.size, disk.model,
                 removable ? "" : " (Internal)");
        m_comboDevice->addItem(label);
    }

    if (m_disks.isEmpty()) {
        m_comboDevice->addItem("No compatible USB drives found");
    }
}

// ── Boot Selection ────────────────────────────────────────────────────

void MainWindow::onBootSelectionChanged(int index)
{
    QString sel = m_comboBootSelection->currentText();
    if (sel == "Disk or ISO image (Please select)") {
        m_btnSelectIso->setEnabled(true);
    } else {
        m_btnSelectIso->setEnabled(false);
        m_lblIsoName->setText("No image selected");
        m_selectedIsoPath.clear();
        m_selectedIsoSize = 0;
    }
}

void MainWindow::onSelectIsoClicked()
{
    QString path = QFileDialog::getOpenFileName(this,
        "Open Bootable Image", QString(),
        "Bootable ISO/IMG Images (*.iso *.iso.gz *.img *.img.gz);;All Files (*)");

    if (path.isEmpty()) return;

    m_selectedIsoPath = path;
    QFileInfo fi(path);
    m_selectedIsoSize = fi.size();

    QString base = fi.fileName();
    m_lblIsoName->setText(base);
    m_lblIsoName->setStyleSheet("color: #000; font-style: normal; font-weight: bold;");

    // Auto-suggest volume label
    if (m_entryVolumeLabel->text().isEmpty()) {
        QString suggested = base.left(11);
        int dotIdx = suggested.indexOf('.');
        if (dotIdx > 0) suggested = suggested.left(dotIdx);
        m_entryVolumeLabel->setText(suggested.toUpper());
    }

    logMessage(QString("Selected image file: %1 (%2 bytes)")
        .arg(m_selectedIsoPath).arg(m_selectedIsoSize));

    // Spawn hash calculation in background
    if (m_hashThread) {
        m_hashThread->quit();
        m_hashThread->wait();
    }
    m_hashThread = new QThread(this);
    m_hashWorker = new HashWorker(m_selectedIsoPath);
    m_hashWorker->moveToThread(m_hashThread);
    connect(m_hashThread, &QThread::started, m_hashWorker, &HashWorker::compute);
    connect(m_hashWorker, &HashWorker::finished, this, &MainWindow::onHashFinished);
    connect(m_hashThread, &QThread::finished, m_hashWorker, &QObject::deleteLater);
    m_hashThread->start();
}

// ── Partition Scheme / Target System linking ──────────────────────────

void MainWindow::onPartitionSchemeChanged(int index)
{
    QString scheme = m_comboPartitionScheme->currentText();
    if (scheme == "MBR") {
        m_comboTargetSystem->setCurrentIndex(1); // BIOS or UEFI-CSM
    } else if (scheme == "GPT") {
        m_comboTargetSystem->setCurrentIndex(0); // UEFI
    }
}

void MainWindow::onTargetSystemChanged(int index)
{
    QString target = m_comboTargetSystem->currentText();
    if (target == "BIOS or UEFI-CSM") {
        m_comboPartitionScheme->setCurrentIndex(1); // MBR
    } else if (target == "UEFI (non CSM)") {
        m_comboPartitionScheme->setCurrentIndex(0); // GPT
    }
}

// ── Start / Burn ──────────────────────────────────────────────────────

void MainWindow::onStartClicked()
{
    int activeIdx = m_comboDevice->currentIndex();
    if (activeIdx < 0 || activeIdx >= m_disks.size()) {
        QMessageBox::warning(this, "No Target Device Selected",
            "Please select a valid destination USB drive first.");
        return;
    }

    const DiskInfo &disk = m_disks[activeIdx];
    bool writeIso = false;

    if (m_comboBootSelection->currentIndex() == 0) {
        if (m_selectedIsoPath.isEmpty()) {
            QMessageBox::warning(this, "No Bootable ISO Selected",
                "You have selected 'Disk or ISO image' as the boot selection, "
                "but have not chosen a file. Please click 'SELECT' and choose an ISO first.");
            return;
        }
        writeIso = true;
    }

    // Warning dialog
    QString warning = QString(
        "WARNING: ALL DATA ON DEVICE '/dev/%1' WILL BE DESTROYED!\n\n"
        "Disk Model: %2\nCapacity: %3\n\n"
        "To continue with this operation, click OK. To quit click Cancel.")
        .arg(disk.name, disk.model, disk.size);

    auto reply = QMessageBox::warning(this, "WARNING: DESTROYING DATA!",
        warning, QMessageBox::Ok | QMessageBox::Cancel);

    if (reply != QMessageBox::Ok) return;

    // Disable controls
    disableControls();

    QString devicePath = "/dev/" + disk.name;
    QString isoPath = m_selectedIsoPath;
    QString fs = m_comboFilesystem->currentText();
    QString partScheme = m_comboPartitionScheme->currentText();
    QString volumeLabel = m_entryVolumeLabel->text();
    if (volumeLabel.isEmpty()) volumeLabel = "RUFUS";
    bool quickFormat = m_checkQuickFormat->isChecked();
    int badBlocks = m_checkBadBlocks->isChecked() ? m_comboNbPasses->currentIndex() + 1 : 0;

    // Spawn burn worker
    if (m_burnThread) {
        m_burnThread->quit();
        m_burnThread->wait();
    }
    m_burnThread = new QThread(this);
    m_burnWorker = new BurnWorker(devicePath, isoPath, fs, partScheme,
                                   volumeLabel, writeIso, m_selectedIsoSize);
    m_burnWorker->moveToThread(m_burnThread);

    connect(m_burnThread, &QThread::started, m_burnWorker, &BurnWorker::burn);
    connect(m_burnWorker, &BurnWorker::progressUpdated, this, &MainWindow::onBurnProgress);
    connect(m_burnWorker, &BurnWorker::logMessage, this, &MainWindow::onBurnLog);
    connect(m_burnWorker, &BurnWorker::finished, this, &MainWindow::onBurnFinished);
    connect(m_burnWorker, &BurnWorker::finished, m_burnThread, &QThread::quit);
    connect(m_burnThread, &QThread::finished, m_burnWorker, &QObject::deleteLater);

    m_isRunning = true;
    m_burnThread->start();
}

void MainWindow::disableControls()
{
    m_comboDevice->setEnabled(false);
    m_btnRefresh->setEnabled(false);
    m_comboBootSelection->setEnabled(false);
    m_btnSelectIso->setEnabled(false);
    m_comboPartitionScheme->setEnabled(false);
    m_comboTargetSystem->setEnabled(false);
    m_entryVolumeLabel->setEnabled(false);
    m_comboFilesystem->setEnabled(false);
    m_comboClusterSize->setEnabled(false);
    m_btnStart->setEnabled(false);
    m_btnClose->setEnabled(false);
}

void MainWindow::enableControls()
{
    m_comboDevice->setEnabled(true);
    m_btnRefresh->setEnabled(true);
    m_comboBootSelection->setEnabled(true);
    onBootSelectionChanged(m_comboBootSelection->currentIndex());
    m_comboPartitionScheme->setEnabled(true);
    m_comboTargetSystem->setEnabled(true);
    m_entryVolumeLabel->setEnabled(true);
    m_comboFilesystem->setEnabled(true);
    m_comboClusterSize->setEnabled(true);
    m_btnStart->setEnabled(true);
    m_btnClose->setEnabled(true);
}

// ── Worker Callbacks ──────────────────────────────────────────────────

void MainWindow::onBurnProgress(double pct, const QString &status, const QString &speedEta)
{
    m_progressBar->setValue(static_cast<int>(pct * 100));
    m_lblStatus->setText(status);
    m_lblSpeedEta->setText(speedEta);
}

void MainWindow::onBurnLog(const QString &msg)
{
    m_logView->appendPlainText(msg);
    m_logView->verticalScrollBar()->setValue(
        m_logView->verticalScrollBar()->maximum());
}

void MainWindow::onBurnFinished(const QString &result)
{
    m_isRunning = false;
    enableControls();
    m_progressBar->setValue(100);
    m_lblStatus->setText("Ready");
    m_lblSpeedEta->setText("Success!");

    QMessageBox::information(this, "Operation Completed", result);
}

void MainWindow::onHashFinished(const QString &result)
{
    QMessageBox::information(this, "Image Checksums", result);
}

void MainWindow::logMessage(const QString &msg)
{
    m_logView->appendPlainText(msg);
}

// ── Log Toggle ────────────────────────────────────────────────────────

void MainWindow::onLogToggleClicked(bool checked)
{
    m_logView->setVisible(checked);
}

// ── Toolbar Actions ───────────────────────────────────────────────────

void MainWindow::onLanguageAction()
{
    QMessageBox::information(this, "Language",
        "Language selection will be available in a future update.");
}

void MainWindow::onAboutAction()
{
    QMessageBox::about(this, "About Rufus Qt",
        "<h2>Rufus Qt v" APP_VERSION "</h2>"
        "<p>The Reliable USB Formatting Utility</p>"
        "<p>Linux port using Qt6 Widgets</p>"
        "<p>Based on the original Rufus by Pete Batard</p>"
        "<p><a href='https://rufus.ie'>https://rufus.ie</a></p>");
}

void MainWindow::onSettingsAction()
{
    QMessageBox::information(this, "Settings",
        "Settings dialog will be available in a future update.");
}
