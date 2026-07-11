#include "mainwindow.h"
#include "workers.h"

#include <QApplication>
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
#include <QSettings>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTranslator>
#include <QListWidget>

#include <unistd.h>
#include <sys/stat.h>

#define APP_VERSION "1.0.0"

static const QString kCanceled = QStringLiteral("Canceled.");

static const char *lightStylesheet = R"(
QGroupBox { font-weight: bold; border: 1px solid #bdc3c7; border-radius: 4px;
margin-top: 8px; padding-top: 12px; padding-bottom: 2px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
QPushButton#btnGray { padding: 2px 8px; border-radius: 3px;
background-color: #ecf0f1; border: 1px solid #bdc3c7; }
QPushButton#btnGray:hover { background-color: #dfe6e9; }
QPushButton#btnSelect { padding: 3px 12px; border-radius: 3px; font-weight: bold; }
QPlainTextEdit { background-color: #1e1e1e; color: #a8e6a3;
font-family: 'Fira Code', 'Monospace', monospace; font-size: 10px; }
QComboBox { padding: 1px 4px; min-height: 18px; font-size: 11px; }
QLineEdit { padding: 1px 4px; min-height: 18px; font-size: 11px; }
QCheckBox { font-size: 11px; spacing: 4px; }
QLabel { font-size: 11px; }
QSpinBox { padding: 1px 2px; min-height: 18px; font-size: 11px; }
QSlider::handle:horizontal { width: 12px; margin: -4px 0; }
QProgressBar { font-size: 10px; text-align: center; }
)";

static const char *darkStylesheet = R"(
QGroupBox { font-weight: bold; border: 1px solid #30363d; border-radius: 4px;
margin-top: 8px; padding-top: 12px; padding-bottom: 2px; color: #58a6ff; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #58a6ff; }
QPushButton#btnGray { padding: 2px 8px; border-radius: 3px;
background-color: #21262d; border: 1px solid #30363d; color: #c9d1d9; }
QPushButton#btnGray:hover { background-color: #30363d; }
QPushButton#btnSelect { padding: 3px 12px; border-radius: 3px; font-weight: bold;
background-color: #238636; color: white; border: 1px solid #2ea043; }
QPushButton#btnSelect:hover { background-color: #2ea043; }
QPlainTextEdit { background-color: #0d1117; color: #58a6ff;
font-family: 'Fira Code', 'Monospace', monospace; font-size: 10px;
border: 1px solid #21262d; }
QComboBox { padding: 1px 4px; min-height: 18px; font-size: 11px;
background-color: #161b22; color: #c9d1d9; border: 1px solid #30363d; }
QComboBox::drop-down { border: none; }
QComboBox QAbstractItemView { background-color: #161b22; color: #c9d1d9;
selection-background-color: #1f6feb; }
QLineEdit { padding: 1px 4px; min-height: 18px; font-size: 11px;
background-color: #161b22; color: #c9d1d9; border: 1px solid #30363d; }
QCheckBox { font-size: 11px; spacing: 4px; color: #c9d1d9; }
QLabel { font-size: 11px; color: #c9d1d9; }
QSpinBox { padding: 1px 2px; min-height: 18px; font-size: 11px;
background-color: #161b22; color: #c9d1d9; border: 1px solid #30363d; }
QSlider::handle:horizontal { width: 12px; margin: -4px 0;
background: #58a6ff; border-radius: 6px; }
QSlider::groove:horizontal { height: 4px; background: #30363d; }
QProgressBar { background-color: #161b22; border: 1px solid #30363d;
color: #c9d1d9; font-size: 10px; text-align: center; }
QProgressBar::chunk { background-color: #238636; }
QToolBar { background-color: #161b22; border: none; spacing: 4px; }
QToolButton { color: #c9d1d9; padding: 2px 6px; }
QToolButton:hover { background-color: #30363d; border-radius: 4px; }
QScrollBar:vertical { background-color: #161b22; width: 10px; }
QScrollBar::handle:vertical { background-color: #30363d; border-radius: 5px; min-height: 20px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Rufus Qt v") + APP_VERSION);
    setMinimumSize(720, 560);
    resize(780, 600);

    setupUi();
    loadSettings();
    applyTheme();
    detectSystemRootDisk();
    checkSystemTools();

    connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, &MainWindow::applyTheme);

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

void MainWindow::applyTheme()
{
    Theme theme = m_currentTheme;
    if (theme == ThemeSystem) {
        auto cs = QApplication::styleHints()->colorScheme();
        theme = (cs == Qt::ColorScheme::Dark) ? ThemeDark : ThemeLight;
    }
    setStyleSheet(theme == ThemeDark ? darkStylesheet : lightStylesheet);
}

// ── UI Setup ──────────────────────────────────────────────────────────

void MainWindow::setupUi()
{
    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(4, 2, 4, 4);
    mainLayout->setSpacing(2);

    setupToolbar();
    mainLayout->addWidget(m_toolbar);

    // Two-column layout
    auto *columns = new QHBoxLayout();
    columns->setSpacing(4);

    // Left column: Drive Properties + Boot Selection
    auto *leftCol = new QVBoxLayout();
    leftCol->setSpacing(0);

    setupDriveProperties();
    leftCol->addWidget(m_advancedDriveGroup);

    auto *bootGroup = new QGroupBox(tr("Boot Selection"));
    auto *bootGrid = new QGridLayout(bootGroup);
    bootGrid->setSpacing(1);
    bootGrid->setContentsMargins(4, 10, 4, 4);

    auto *lblBoot = new QLabel(tr("Boot selection:"));
    m_comboBootSelection = new QComboBox();
    m_comboBootSelection->addItem(tr("Disk or ISO image (Please select)"));
    m_comboBootSelection->addItem(tr("Non bootable"));
    m_comboBootSelection->addItem("FreeDOS");
    connect(m_comboBootSelection, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onBootSelectionChanged);

    m_btnSelectIso = new QPushButton(tr("SELECT"));
    m_btnSelectIso->setObjectName("btnSelect");
    connect(m_btnSelectIso, &QPushButton::clicked, this, &MainWindow::onSelectIsoClicked);

    auto *bootRow = new QHBoxLayout();
    bootRow->setSpacing(2);
    bootRow->addWidget(m_comboBootSelection, 1);
    bootRow->addWidget(m_btnSelectIso);
    bootGrid->addLayout(bootRow, 0, 0, 1, 2);

    auto *lblImageOption = new QLabel(tr("Image option:"));
    m_comboImageOption = new QComboBox();
    m_comboImageOption->addItems({tr("Write in ISO Image mode"), tr("Write in DD Image mode")});
    m_comboImageOption->setEnabled(false);
    bootGrid->addWidget(lblImageOption, 1, 0);
    bootGrid->addWidget(m_comboImageOption, 1, 1);

    auto *lblIsoTitle = new QLabel(tr("Selected file:"));
    m_lblIsoName = new QLabel(tr("No image selected"));
    m_lblIsoName->setStyleSheet("color: #7f8c8d; font-style: italic; font-weight: normal;");
    bootGrid->addWidget(lblIsoTitle, 2, 0);
    bootGrid->addWidget(m_lblIsoName, 2, 1);

    auto *lblPartition = new QLabel(tr("Partition scheme:"));
    m_comboPartitionScheme = new QComboBox();
    m_comboPartitionScheme->addItems({"GPT", "MBR"});
    connect(m_comboPartitionScheme, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onPartitionSchemeChanged);

    auto *lblTarget = new QLabel(tr("Target system:"));
    m_comboTargetSystem = new QComboBox();
    m_comboTargetSystem->addItems({tr("UEFI (non CSM)"), tr("BIOS or UEFI-CSM")});
    connect(m_comboTargetSystem, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTargetSystemChanged);

    bootGrid->addWidget(lblPartition, 3, 0);
    bootGrid->addWidget(m_comboPartitionScheme, 3, 1);
    bootGrid->addWidget(lblTarget, 4, 0);
    bootGrid->addWidget(m_comboTargetSystem, 4, 1);
    leftCol->addWidget(bootGroup);
    columns->addLayout(leftCol, 1);

    // Right column: Format Options
    setupFormatOptions();
    columns->addWidget(m_advancedFormatGroup, 1);

    mainLayout->addLayout(columns);

    // Status
    setupStatusSection();
    mainLayout->addWidget(m_statusGroup, 1);

    // Bottom controls
    auto *bottomLayout = new QHBoxLayout();
    bottomLayout->setSpacing(6);

    m_btnLogToggle = new QPushButton(tr("Log"));
    m_btnLogToggle->setCheckable(true);
    m_btnLogToggle->setChecked(true);
    m_btnLogToggle->setObjectName("btnGray");
    connect(m_btnLogToggle, &QPushButton::toggled, this, &MainWindow::onLogToggleClicked);

    m_btnStart = new QPushButton(tr("START"));
    m_btnStart->setObjectName("btnStart");
    m_btnStart->setMinimumWidth(110);
    m_btnStart->setStyleSheet(
        "QPushButton#btnStart { background-color: #27ae60; color: white; font-weight: bold; "
        "padding: 5px 18px; border-radius: 3px; font-size: 12px; } "
        "QPushButton#btnStart:hover { background-color: #2ecc71; } "
        "QPushButton#btnStart:disabled { background-color: #7f8c8d; color: #ecf0f1; }");
    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStartClicked);

    m_btnClose = new QPushButton(tr("CLOSE"));
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
    m_toolbar = new QToolBar(tr("Main Toolbar"), this);
    m_toolbar->setMovable(false);
    m_toolbar->setIconSize(QSize(20, 20));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_langAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Language"));
    connect(m_langAction, &QAction::triggered, this, &MainWindow::onLanguageAction);

    m_settingsAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("Settings"));
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::onSettingsAction);

    m_logAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Log"));
    connect(m_logAction, &QAction::triggered, this, [this]() {
        m_btnLogToggle->toggle();
    });

    m_aboutAction = m_toolbar->addAction(
        style()->standardIcon(QStyle::SP_MessageBoxInformation), tr("About"));
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::onAboutAction);
}

void MainWindow::setupDriveProperties()
{
    m_advancedDriveGroup = new QGroupBox(tr("Drive Properties"), this);

    auto *layout = new QVBoxLayout(m_advancedDriveGroup);

    auto *deviceRow = new QHBoxLayout();
    auto *lblDevice = new QLabel(tr("Device:"));
    m_comboDevice = new QComboBox();
    m_comboDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_btnRefresh = new QPushButton();
    m_btnRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_btnRefresh->setToolTip(tr("Refresh device list"));
    connect(m_btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);

    deviceRow->addWidget(lblDevice);
    deviceRow->addWidget(m_comboDevice, 1);
    deviceRow->addWidget(m_btnRefresh);
    layout->addLayout(deviceRow);

    auto *advDriveLayout = new QVBoxLayout();
    m_checkShowAll = new QCheckBox(tr("List USB Hard Drives (Use with caution)"));
    connect(m_checkShowAll, &QCheckBox::toggled, this, &MainWindow::refreshDevices);
    advDriveLayout->addWidget(m_checkShowAll);

    m_checkOldBios = new QCheckBox(tr("Add fixes for old BIOSes (extra partition, alignment)"));
    advDriveLayout->addWidget(m_checkOldBios);

    auto *advDriveWidget = new QWidget();
    advDriveWidget->setLayout(advDriveLayout);

    auto *expander = new QGroupBox(tr("Show advanced drive properties"));
    expander->setCheckable(true);
    expander->setChecked(false);
    expander->setFlat(true);
    auto *expanderLayout = new QVBoxLayout(expander);
    expanderLayout->setContentsMargins(0, 0, 0, 0);
    expanderLayout->addWidget(advDriveWidget);
    layout->addWidget(expander);
}

void MainWindow::setupFormatOptions()
{
    m_advancedFormatGroup = new QGroupBox(tr("Format Options"), this);
    auto *layout = new QGridLayout(m_advancedFormatGroup);
    layout->setSpacing(2);
    layout->setContentsMargins(4, 10, 4, 4);

    auto *lblVolume = new QLabel(tr("Volume label:"));
    m_entryVolumeLabel = new QLineEdit();
    m_entryVolumeLabel->setPlaceholderText("RUFUS");
    layout->addWidget(lblVolume, 0, 0);
    layout->addWidget(m_entryVolumeLabel, 0, 1);

    m_checkExtendedLabel = new QCheckBox(tr("Extended label and icon files"));
    layout->addWidget(m_checkExtendedLabel, 1, 0, 1, 2);

    auto *lblFs = new QLabel(tr("File system:"));
    m_comboFilesystem = new QComboBox();
    m_comboFilesystem->addItems({"FAT32", "NTFS", "exFAT", "ext4"});
    layout->addWidget(lblFs, 2, 0);
    layout->addWidget(m_comboFilesystem, 2, 1);

    auto *lblCluster = new QLabel(tr("Cluster size:"));
    m_comboClusterSize = new QComboBox();
    layout->addWidget(lblCluster, 3, 0);
    layout->addWidget(m_comboClusterSize, 3, 1);

    m_checkQuickFormat = new QCheckBox(tr("Quick format"));
    m_checkQuickFormat->setChecked(true);
    layout->addWidget(m_checkQuickFormat, 4, 0, 1, 2);

    auto *badBlockRow = new QHBoxLayout();
    badBlockRow->setSpacing(2);
    m_checkBadBlocks = new QCheckBox(tr("Bad blocks"));
    m_comboNbPasses = new QComboBox();
    m_comboNbPasses->addItems({tr("1 pass"), tr("2 passes"), tr("3 passes"),
                                tr("4 passes"), tr("5 passes")});
    badBlockRow->addWidget(m_checkBadBlocks);
    badBlockRow->addWidget(m_comboNbPasses);
    badBlockRow->addStretch();
    layout->addLayout(badBlockRow, 5, 0, 1, 2);

    auto *persistRow = new QHBoxLayout();
    persistRow->setSpacing(2);
    m_checkPersistent = new QCheckBox(tr("Persistent"));
    m_persistenceSlider = new QSlider(Qt::Horizontal);
    m_persistenceSlider->setRange(0, 100);
    m_persistenceSlider->setValue(50);
    m_persistenceSize = new QSpinBox();
    m_persistenceSize->setRange(1, 99999);
    m_persistenceSize->setValue(4096);
    m_persistenceSize->setMinimumWidth(60);
    m_comboPersistenceUnits = new QComboBox();
    m_comboPersistenceUnits->addItems({tr("MB"), tr("GB")});
    persistRow->addWidget(m_checkPersistent);
    persistRow->addWidget(m_persistenceSlider, 1);
    persistRow->addWidget(m_persistenceSize);
    persistRow->addWidget(m_comboPersistenceUnits);
    layout->addLayout(persistRow, 6, 0, 1, 2);

    m_checkUefiValidation = new QCheckBox(tr("UEFI validation (Secure Boot)"));
    layout->addWidget(m_checkUefiValidation, 7, 0, 1, 2);

    auto updateClusterSizes = [this]() {
        m_comboClusterSize->clear();
        QString fs = m_comboFilesystem->currentText();
        if (fs == "FAT32") {
            m_comboClusterSize->addItems({"512", "1024", "2048", "4096",
                "8192", "16384", "32768", "65536"});
            m_comboClusterSize->setCurrentIndex(3);
        } else if (fs == "NTFS") {
            m_comboClusterSize->addItems({"512", "1024", "2048", "4096",
                "8192", "16384", "32768", "65536", "131072", "262144",
                "524288", "1048576"});
            m_comboClusterSize->setCurrentIndex(3);
        } else if (fs == "exFAT") {
            m_comboClusterSize->addItems({"512", "1024", "2048", "4096",
                "8192", "16384", "32768", "65536", "131072", "262144",
                "524288", "1048576", "2097152"});
            m_comboClusterSize->setCurrentIndex(7);
        } else {
            m_comboClusterSize->addItems({"1024", "2048", "4096",
                "8192", "16384", "32768", "65536"});
            m_comboClusterSize->setCurrentIndex(2);
        }
    };
    connect(m_comboFilesystem, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, updateClusterSizes);
    updateClusterSizes();
}

void MainWindow::setupStatusSection()
{
    m_statusGroup = new QGroupBox(tr("Status"), this);
    auto *layout = new QVBoxLayout(m_statusGroup);

    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedHeight(16);
    layout->addWidget(m_progressBar);

    auto *infoRow = new QHBoxLayout();
    m_lblStatus = new QLabel(tr("Ready"));
    m_lblSpeedEta = new QLabel();
    m_lblSpeedEta->setAlignment(Qt::AlignRight);
    infoRow->addWidget(m_lblStatus, 1);
    infoRow->addWidget(m_lblSpeedEta);
    layout->addLayout(infoRow);

    m_logView = new QPlainTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(1000);
    m_logView->setFixedHeight(80);
    m_logView->setUndoRedoEnabled(false);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_logView, 1);
}

// ── System Tools Check ────────────────────────────────────────────────

void MainWindow::checkSystemTools()
{
    QStringList tools = {"dd", "parted", "lsblk", "findmnt", "umount",
                         "mkfs.vfat", "mkfs.ntfs", "mkfs.ext4", "mkfs.exfat", "sync"};
    QStringList missing;

    for (const QString &tool : tools) {
        QProcess which;
        which.start("which", {tool});
        which.waitForFinished(2000);
        if (which.exitCode() != 0)
            missing << tool;
    }

    if (!missing.isEmpty()) {
        logMessage(tr("WARNING: Missing system tools: ") + missing.join(", "));
    } else {
        logMessage(tr("All required system tools found."));
    }
}

// ── Settings ──────────────────────────────────────────────────────────

void MainWindow::loadSettings()
{
    QSettings s("Rufus", "rufus-qt");

    restoreGeometry(s.value("geometry").toByteArray());

    m_currentTheme = static_cast<Theme>(s.value("theme", ThemeSystem).toInt());
    m_lastIsoDir = s.value("lastIsoDir", "").toString();
    m_checkShowAll->setChecked(s.value("showAllDrives", false).toBool());
    m_checkOldBios->setChecked(s.value("oldBiosFixes", false).toBool());
    m_checkQuickFormat->setChecked(s.value("quickFormat", true).toBool());
    m_checkUefiValidation->setChecked(s.value("uefiValidation", false).toBool());
}

void MainWindow::saveSettings()
{
    QSettings s("Rufus", "rufus-qt");

    s.setValue("geometry", saveGeometry());
    s.setValue("theme", static_cast<int>(m_currentTheme));
    s.setValue("lastIsoDir", m_lastIsoDir);
    s.setValue("showAllDrives", m_checkShowAll->isChecked());
    s.setValue("oldBiosFixes", m_checkOldBios->isChecked());
    s.setValue("quickFormat", m_checkQuickFormat->isChecked());
    s.setValue("uefiValidation", m_checkUefiValidation->isChecked());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

// ── Device Detection ──────────────────────────────────────────────────

void MainWindow::detectSystemRootDisk()
{
    QProcess proc;
    proc.start("findmnt", {"-n", "-o", "SOURCE", "/"});
    proc.waitForFinished(3000);
    QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();

    if (output.startsWith("/dev/")) {
        QString dev = output.mid(5);

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
    logMessage(tr("Detected system root disk to protect: /dev/") + m_systemRootDisk);
}

void MainWindow::refreshDevices()
{
    m_comboDevice->clear();
    m_disks.clear();

    QProcess proc;
    proc.start("lsblk", {"-d", "-n", "-P", "-b", "-o", "NAME,SIZE,MODEL,RO,RM"});
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
        disk.sizeBytes = getVal("SIZE").toLongLong();
        disk.model = getVal("MODEL");
        if (disk.model.isEmpty()) disk.model = tr("Generic Flash Drive");
        disk.removable = removable;
        disk.readOnly = readOnly;

        if (disk.sizeBytes < 1024LL * 1024)
            disk.size = QString::number(disk.sizeBytes / 1024.0, 'f', 1) + "K";
        else if (disk.sizeBytes < 1024LL * 1024 * 1024)
            disk.size = QString::number(disk.sizeBytes / (1024.0 * 1024.0), 'f', 1) + "M";
        else
            disk.size = QString::number(disk.sizeBytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + "G";

        m_disks.append(disk);

        QString label = QString("/dev/%1 - %2 [%3]%4")
            .arg(disk.name, disk.size, disk.model,
                 removable ? "" : tr(" (Internal)"));
        m_comboDevice->addItem(label);
    }

    if (m_disks.isEmpty()) {
        m_comboDevice->addItem(tr("No compatible USB drives found"));
    }
}

// ── Boot Selection ────────────────────────────────────────────────────

void MainWindow::onBootSelectionChanged(int index)
{
    if (index == 0) {
        m_btnSelectIso->setEnabled(true);
        m_comboImageOption->setEnabled(!m_selectedIsoPath.isEmpty());
    } else {
        m_btnSelectIso->setEnabled(false);
        m_comboImageOption->setEnabled(false);
        m_lblIsoName->setText(tr("No image selected"));
        m_selectedIsoPath.clear();
        m_selectedIsoSize = 0;
    }
}

void MainWindow::onSelectIsoClicked()
{
    QString path = QFileDialog::getOpenFileName(this,
        tr("Open Bootable Image"), m_lastIsoDir,
        tr("Bootable ISO/IMG Images (*.iso *.iso.gz *.img *.img.gz);;All Files (*)"));

    if (path.isEmpty()) return;

    m_selectedIsoPath = path;
    QFileInfo fi(path);
    m_selectedIsoSize = fi.size();
    m_lastIsoDir = fi.absolutePath();

    m_comboImageOption->setEnabled(true);

    QString base = fi.fileName();
    m_lblIsoName->setText(base);

    if (m_entryVolumeLabel->text().isEmpty()) {
        QString suggested = base.left(11);
        int dotIdx = suggested.indexOf('.');
        if (dotIdx > 0) suggested = suggested.left(dotIdx);
        m_entryVolumeLabel->setText(suggested.toUpper());
    }

    logMessage(tr("Selected image file: %1 (%2 bytes)")
        .arg(m_selectedIsoPath).arg(m_selectedIsoSize));

    if (m_checkUefiValidation->isChecked()) {
        if (BurnWorker::checkIsoUefi(m_selectedIsoPath))
            logMessage(tr("UEFI pre-check: ISO appears UEFI-compatible."));
        else
            logMessage(tr("WARNING: ISO may not be UEFI-compatible (no EFI boot entries detected)."));
    }

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
    if (index == 1)
        m_comboTargetSystem->setCurrentIndex(1);
    else
        m_comboTargetSystem->setCurrentIndex(0);
}

void MainWindow::onTargetSystemChanged(int index)
{
    if (index == 1)
        m_comboPartitionScheme->setCurrentIndex(1);
    else
        m_comboPartitionScheme->setCurrentIndex(0);
}

// ── Start / Burn ──────────────────────────────────────────────────────

void MainWindow::onCancelClicked()
{
    if (m_burnWorker)
        m_burnWorker->cancel();
    logMessage(tr("Cancel requested by user."));
}

void MainWindow::onStartClicked()
{
    if (m_isRunning) {
        onCancelClicked();
        return;
    }

    int activeIdx = m_comboDevice->currentIndex();
    if (activeIdx < 0 || activeIdx >= m_disks.size()) {
        QMessageBox::warning(this, tr("No Target Device Selected"),
            tr("Please select a valid destination USB drive first."));
        return;
    }

    const DiskInfo &disk = m_disks[activeIdx];
    bool writeIso = false;

    if (m_comboBootSelection->currentIndex() == 0) {
        if (m_selectedIsoPath.isEmpty()) {
            QMessageBox::warning(this, tr("No Bootable ISO Selected"),
                tr("You have selected 'Disk or ISO image' as the boot selection, "
                   "but have not chosen a file. Please click 'SELECT' and choose an ISO first."));
            return;
        }
        writeIso = true;
    }

    if (writeIso && m_selectedIsoSize > disk.sizeBytes) {
        QMessageBox::critical(this, tr("ISO Too Large"),
            tr("The selected image file (%1 bytes) is larger than the target device (%2 bytes).\n\n"
               "Please select a smaller image or a larger USB drive.")
            .arg(m_selectedIsoSize).arg(disk.sizeBytes));
        return;
    }

    QString warning = tr(
        "WARNING: ALL DATA ON DEVICE '/dev/%1' WILL BE DESTROYED!\n\n"
        "Disk Model: %2\nCapacity: %3\n\n"
        "To continue with this operation, click OK. To quit click Cancel.")
        .arg(disk.name, disk.model, disk.size);

    auto reply = QMessageBox::warning(this, tr("WARNING: DESTROYING DATA!"),
        warning, QMessageBox::Ok | QMessageBox::Cancel);

    if (reply != QMessageBox::Ok) return;

    disableControls();

    QString devicePath = "/dev/" + disk.name;
    QString isoPath = m_selectedIsoPath;
    QString fs = m_comboFilesystem->currentText();
    QString partScheme = m_comboPartitionScheme->currentText();
    QString volumeLabel = m_entryVolumeLabel->text();
    if (volumeLabel.isEmpty()) volumeLabel = "RUFUS";
    int badBlocks = m_checkBadBlocks->isChecked() ? m_comboNbPasses->currentIndex() + 1 : 0;

    bool persistent = m_checkPersistent->isChecked();
    int persistentSize = m_persistenceSize->value();
    QString persistentUnits = m_comboPersistenceUnits->currentText();

    if (m_burnThread) {
        m_burnThread->quit();
        m_burnThread->wait();
    }
    m_burnThread = new QThread(this);
    m_burnWorker = new BurnWorker(devicePath, isoPath, fs, partScheme,
                                   volumeLabel, writeIso, m_selectedIsoSize,
                                   badBlocks, persistent, persistentSize, persistentUnits);
    m_burnWorker->moveToThread(m_burnThread);

    connect(m_burnThread, &QThread::started, m_burnWorker, &BurnWorker::burn);
    connect(m_burnWorker, &BurnWorker::progressUpdated, this, &MainWindow::onBurnProgress);
    connect(m_burnWorker, &BurnWorker::logMessage, this, &MainWindow::onBurnLog);
    connect(m_burnWorker, &BurnWorker::finished, this, &MainWindow::onBurnFinished);
    connect(m_burnWorker, &BurnWorker::finished, m_burnThread, &QThread::quit);
    connect(m_burnThread, &QThread::finished, m_burnWorker, &QObject::deleteLater);

    m_btnStart->setText(tr("CANCEL"));
    m_btnStart->setStyleSheet(
        "QPushButton#btnStart { background-color: #e74c3c; color: white; font-weight: bold; "
        "padding: 5px 18px; border-radius: 3px; font-size: 12px; } "
        "QPushButton#btnStart:hover { background-color: #c0392b; }");

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

    m_btnStart->setText(tr("START"));
    m_btnStart->setStyleSheet(
        "QPushButton#btnStart { background-color: #27ae60; color: white; font-weight: bold; "
        "padding: 5px 18px; border-radius: 3px; font-size: 12px; } "
        "QPushButton#btnStart:hover { background-color: #2ecc71; } "
        "QPushButton#btnStart:disabled { background-color: #7f8c8d; color: #ecf0f1; }");

    if (result == kCanceled) {
        m_progressBar->setValue(0);
        m_lblStatus->setText(tr("Ready"));
        m_lblSpeedEta->setText(tr("Canceled"));
    } else {
        m_progressBar->setValue(100);
        m_lblStatus->setText(tr("Ready"));
        m_lblSpeedEta->setText(tr("Success!"));
        QMessageBox::information(this, tr("Operation Completed"), result);
    }
}

void MainWindow::onHashFinished(const QString &result)
{
    QMessageBox::information(this, tr("Image Checksums"), result);
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
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Select Language"));
    dlg.setMinimumWidth(280);
    auto *layout = new QVBoxLayout(&dlg);

    struct Lang { QString code; QString label; };
    QVector<Lang> langs = {
        {"en", "English"},
        {"es", "Espa\u00f1ol"},
        {"pt_BR", "Portugu\u00eas (Brasil)"},
        {"ru", "\u0420\u0443\u0441\u0441\u043a\u0438\u0439"},
        {"fr", "Fran\u00e7ais"},
        {"de", "Deutsch"},
    };

    auto *list = new QListWidget();
    for (const auto &l : langs) {
        auto *item = new QListWidgetItem(l.label);
        item->setData(Qt::UserRole, l.code);
        list->addItem(item);
    }

    QSettings s("Rufus", "rufus-qt");
    QString current = s.value("language", "").toString();
    for (int i = 0; i < list->count(); i++) {
        if (list->item(i)->data(Qt::UserRole).toString() == current)
            list->setCurrentRow(i);
    }

    layout->addWidget(new QLabel(tr("Select application language (restart required):")));
    layout->addWidget(list, 1);

    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    if (dlg.exec() != QDialog::Accepted) return;

    auto *item = list->currentItem();
    if (!item) return;
    QString code = item->data(Qt::UserRole).toString();
    s.setValue("language", code);

    QMessageBox::information(this, tr("Language Changed"),
        tr("Language has been set to %1.\nPlease restart the application for the change to take effect.")
        .arg(item->text()));
}

void MainWindow::onAboutAction()
{
    QMessageBox::about(this, tr("About Rufus Qt"),
        QString("<h2>") + tr("Rufus Qt") + " v" APP_VERSION + "</h2>"
        + "<p>" + tr("The Reliable USB Formatting Utility") + "</p>"
        + "<p>" + tr("Linux port using Qt6 Widgets") + "</p>"
        + "<p>" + tr("Based on the original Rufus by Pete Batard") + "</p>"
        + "<p><a href='https://rufus.ie'>https://rufus.ie</a></p>");
}

void MainWindow::onSettingsAction()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Rufus Qt Preferences"));
    dlg.setMinimumWidth(320);
    auto *layout = new QVBoxLayout(&dlg);

    auto *checkQuick = new QCheckBox(tr("Quick format by default"));
    checkQuick->setChecked(m_checkQuickFormat->isChecked());

    auto *checkUefi = new QCheckBox(tr("UEFI validation by default"));
    checkUefi->setChecked(m_checkUefiValidation->isChecked());

    auto *checkShowAll = new QCheckBox(tr("Show USB hard drives by default"));
    checkShowAll->setChecked(m_checkShowAll->isChecked());

    auto *checkOldBios = new QCheckBox(tr("Old BIOS fixes by default"));
    checkOldBios->setChecked(m_checkOldBios->isChecked());

    layout->addWidget(checkQuick);
    layout->addWidget(checkUefi);
    layout->addWidget(checkShowAll);
    layout->addWidget(checkOldBios);

    auto *themeRow = new QHBoxLayout();
    themeRow->addWidget(new QLabel(tr("Theme:")));
    auto *themeCombo = new QComboBox();
    themeCombo->addItems({tr("System"), tr("Light"), tr("Dark")});
    themeCombo->setCurrentIndex(static_cast<int>(m_currentTheme));
    themeRow->addWidget(themeCombo, 1);
    layout->addLayout(themeRow);

    layout->addStretch();

    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btnBox);

    if (dlg.exec() != QDialog::Accepted) return;

    m_checkQuickFormat->setChecked(checkQuick->isChecked());
    m_checkUefiValidation->setChecked(checkUefi->isChecked());
    m_checkShowAll->setChecked(checkShowAll->isChecked());
    m_checkOldBios->setChecked(checkOldBios->isChecked());

    Theme newTheme = static_cast<Theme>(themeCombo->currentIndex());
    if (newTheme != m_currentTheme) {
        m_currentTheme = newTheme;
        applyTheme();
    }
}
