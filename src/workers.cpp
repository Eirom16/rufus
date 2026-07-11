#include "workers.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDataStream>
#include <QRegularExpression>
#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>

#include <algorithm>

static QString sanitizeLabel(const QString &label, int maxLen = 11)
{
    QString clean;
    for (const QChar &c : label) {
        if (c.isLetterOrNumber() || c == '-' || c == '_')
            clean += c.toUpper();
    }
    return clean.left(maxLen);
}

static QString mountedTargetForSource(const QString &source)
{
    if (source.isEmpty())
        return QString();

    QProcess findmnt;
    findmnt.start("findmnt", {"-rn", "-S", source, "-o", "TARGET"});
    findmnt.waitForFinished(3000);
    const QString output = QString::fromUtf8(findmnt.readAllStandardOutput()).trimmed();
    return output.split('\n', Qt::SkipEmptyParts).value(0).trimmed();
}

static bool isRufusTempMount(const QString &target, const QString &prefix)
{
    return target == "/tmp/" + prefix || target.startsWith("/tmp/" + prefix + "-");
}

static QString formatBytes(qint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString("%1 %2").arg(value, 0, unit == 0 ? 'f' : 'f', unit == 0 ? 0 : 1).arg(units[unit]);
}

static QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        seconds = 0;

    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;

    if (hours > 0)
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));

    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

static bool isWindowsInstallerMount(const QString &root)
{
    const QDir dir(root);
    return QFileInfo::exists(dir.filePath("bootmgr"))
        || QFileInfo::exists(dir.filePath("bootmgr.efi"))
        || QFileInfo::exists(dir.filePath("sources/install.wim"))
        || QFileInfo::exists(dir.filePath("sources/install.esd"))
        || QFileInfo::exists(dir.filePath("efi/microsoft/boot/bootmgfw.efi"))
        || QFileInfo::exists(dir.filePath("EFI/Microsoft/Boot/bootmgfw.efi"));
}

// ── BurnWorker ─────────────────────────────────────────────────────────

BurnWorker::BurnWorker(const QString &devicePath, const QString &isoPath,
                       const QString &filesystem, const QString &partitionScheme,
                       const QString &volumeLabel, bool writeIso, qint64 isoSize,
                       int badBlocks, bool persistent, int persistentSize,
                       const QString &persistentUnits, bool isoMode,
                       int winTweaks,
                       QObject *parent)
    : QObject(parent)
    , m_devicePath(devicePath)
    , m_isoPath(isoPath)
    , m_filesystem(filesystem)
    , m_partitionScheme(partitionScheme)
    , m_volumeLabel(volumeLabel)
    , m_writeIso(writeIso)
    , m_isoSize(isoSize)
    , m_badBlocks(badBlocks)
    , m_persistent(persistent)
    , m_persistentSize(persistentSize)
    , m_persistentUnits(persistentUnits)
    , m_isoMode(isoMode)
    , m_winTweaks(winTweaks)
{
    m_volumeLabel = sanitizeLabel(m_volumeLabel);
}

void BurnWorker::cancel()
{
    m_canceled = true;
    if (m_activeProc && m_activeProc->state() != QProcess::NotRunning) {
        m_activeProc->kill();
        m_activeProc->waitForFinished(3000);
    }
}

bool BurnWorker::runCmd(const QString &program, const QStringList &args,
                         const QString &logPrefix, int timeoutSecs)
{
    auto *proc = new QProcess(this);
    m_activeProc = proc;
    proc->setProgram(program);
    proc->setArguments(args);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->start();

    if (!proc->waitForStarted(5000)) {
        m_activeProc = nullptr;
        delete proc;
        emit logMessage(QString("  [%1] %2").arg(logPrefix, tr("Failed to start: %1").arg(program)));
        return false;
    }

    QElapsedTimer timer;
    timer.start();

    while (proc->waitForReadyRead(1000) || proc->state() == QProcess::Running) {
        if (m_canceled) {
            proc->kill();
            proc->waitForFinished(2000);
            m_activeProc = nullptr;
            delete proc;
            return false;
        }
        if (timeoutSecs > 0 && timer.elapsed() > timeoutSecs * 1000) {
            proc->kill();
            proc->waitForFinished(2000);
            m_activeProc = nullptr;
            emit logMessage(QString("  [%1] %2").arg(logPrefix,
                tr("Command timed out after %1 seconds").arg(timeoutSecs)));
            delete proc;
            return false;
        }
        QByteArray data = proc->readAllStandardOutput();
        if (!data.isEmpty()) {
            for (const QByteArray &line : data.split('\n')) {
                if (!line.trimmed().isEmpty())
                    emit logMessage(QString("  [%1] %2").arg(logPrefix, QString::fromUtf8(line.trimmed())));
            }
        }
    }
    proc->waitForFinished(30000);
    m_activeProc = nullptr;

    bool ok = true;
    if (m_canceled) {
        ok = false;
    } else if (proc->exitCode() != 0) {
        emit logMessage(QString("  [%1] %2: %3")
            .arg(logPrefix, tr("Failed with exit code %1").arg(proc->exitCode()),
                 QString::fromUtf8(proc->readAll())));
        ok = false;
    }

    delete proc;
    return ok;
}

void BurnWorker::unmountDevice()
{
    emit logMessage(tr("Unmounting any active partitions on %1...").arg(m_devicePath));

    QStringList sources;
    QProcess list;
    list.start("lsblk", {"-ln", "-o", "NAME", m_devicePath});
    list.waitForFinished(3000);
    const QString output = QString::fromUtf8(list.readAllStandardOutput()).trimmed();

    if (!output.isEmpty()) {
        for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
            const QString name = line.trimmed().section(' ', 0, 0);
            if (!name.isEmpty())
                sources << "/dev/" + name;
        }
    }

    if (!sources.contains(m_devicePath))
        sources << m_devicePath;

    std::sort(sources.begin(), sources.end(), [](const QString &a, const QString &b) {
        return a.length() > b.length();
    });

    bool foundMount = false;
    for (const QString &source : sources) {
        QProcess findmnt;
        findmnt.start("findmnt", {"-rn", "-S", source, "-o", "TARGET"});
        findmnt.waitForFinished(3000);
        const QString mounts = QString::fromUtf8(findmnt.readAllStandardOutput()).trimmed();
        for (const QString &mountPoint : mounts.split('\n', Qt::SkipEmptyParts)) {
            foundMount = true;
            runCmd("umount", {mountPoint.trimmed()}, "UMOUNT");
        }
    }

    if (!foundMount)
        emit logMessage(tr("No mounted partitions found on %1.").arg(m_devicePath));

    QThread::msleep(1000);
}

bool BurnWorker::wipeDevice()
{
    emit logMessage(tr("Wiping partition table on %1...").arg(m_devicePath));
    return runCmd("dd", { "if=/dev/zero", "of=" + m_devicePath,
                          "bs=1M", "count=10", "oflag=sync", "conv=notrunc" }, "WIPE");
}

bool BurnWorker::scanBadBlocks()
{
    emit logMessage(tr("Scanning for bad blocks (%1 pass)...").arg(m_badBlocks));
    emit progressUpdated(0.08, tr("Scanning for bad blocks..."), tr("Busy"));

    QStringList args = {"-sv", "-b", "4096",
                        "-c", "64",
                        "-p", QString::number(m_badBlocks),
                        m_devicePath};
    QProcess bb;
    bb.setProgram("badblocks");
    bb.setArguments(args);
    bb.setProcessChannelMode(QProcess::MergedChannels);
    bb.start();

    if (!bb.waitForStarted(5000)) {
        emit logMessage("  [BADBLOCKS] " + tr("not available, skipping scan."));
        return true;
    }

    while (bb.waitForReadyRead(500) || bb.state() == QProcess::Running) {
        QByteArray data = bb.readAllStandardOutput();
        if (data.isEmpty()) continue;
        for (const QByteArray &line : data.split('\n')) {
            if (!line.trimmed().isEmpty())
                emit logMessage(QString("  [BADBLOCKS] %1").arg(QString::fromUtf8(line.trimmed())));
        }
    }
    bb.waitForFinished(60000);

    if (bb.exitCode() != 0) {
        emit logMessage(tr("WARNING: Bad blocks found or scan incomplete. Continuing anyway."));
    } else {
        emit logMessage(tr("Bad block scan complete \u2014 no errors found."));
    }
    return true;
}

QString BurnWorker::partitionPath(int num) const
{
    static const QRegularExpression needsSeparator(
        QStringLiteral("/(nvme\\d+n\\d+|mmcblk\\d+|loop\\d+)$"));
    return m_devicePath.contains(needsSeparator)
        ? m_devicePath + "p" + QString::number(num)
        : m_devicePath + QString::number(num);
}

bool BurnWorker::rescanPartitions()
{
    bool ok = runCmd("partprobe", {m_devicePath}, "PARTED", 30);
    if (!runCmd("udevadm", {"settle"}, "UDEV", 30))
        ok = false;
    QThread::msleep(1000);
    return ok;
}

bool BurnWorker::waitForPartition(int num, int timeoutSecs)
{
    const QString partPath = partitionPath(num);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutSecs * 1000) {
        if (QFileInfo::exists(partPath))
            return true;
        QThread::msleep(250);
    }

    emit logMessage(tr("Timed out waiting for partition node %1.").arg(partPath));
    return false;
}

bool BurnWorker::createPartitions()
{
    emit logMessage(tr("Creating partition table using parted..."));
    QString labelType = (m_partitionScheme == "GPT") ? "gpt" : "msdos";

    QString endPos = "100%";
    if (m_persistent) {
        qint64 sizeBytes = m_persistentSize * (m_persistentUnits == "GB" ? 1073741824LL : 1048576LL);
        double sizeMiB = sizeBytes / (1024.0 * 1024.0);
        endPos = QString("-%1MiB").arg(static_cast<int>(sizeMiB + 1));
    }

    bool ok = runCmd("parted", {"-s", m_devicePath, "mklabel", labelType,
                                 "mkpart", "primary", "1MiB", endPos}, "PARTED");
    if (!ok) return false;

    if (m_partitionScheme == "MBR") {
        if (!runCmd("parted", {"-s", m_devicePath, "set", "1", "boot", "on"}, "PARTED"))
            return false;
    } else {
        if (!runCmd("parted", {"-s", m_devicePath, "set", "1", "esp", "on"}, "PARTED"))
            return false;
    }

    rescanPartitions();
    return waitForPartition(1);
}

bool BurnWorker::formatPartition()
{
    QString partPath = partitionPath(1);

    emit logMessage(tr("Formatting partition %1 to %2 with label '%3'...")
        .arg(partPath, m_filesystem, m_volumeLabel));

    if (!waitForPartition(1))
        return false;

    if (m_filesystem == "FAT32") {
        return runCmd("mkfs.vfat", {"-F", "32", "-n", m_volumeLabel, partPath}, "MKFS");
    } else if (m_filesystem == "NTFS") {
        return runCmd("mkfs.ntfs", {"-f", "-L", m_volumeLabel, partPath}, "MKFS");
    } else if (m_filesystem == "exFAT") {
        return runCmd("mkfs.exfat", {"-n", m_volumeLabel, partPath}, "MKFS");
    } else {
        return runCmd("mkfs.ext4", {"-F", "-L", m_volumeLabel, partPath}, "MKFS");
    }
}

bool BurnWorker::writeIsoImage()
{
    emit logMessage(tr("Starting ISO burn process..."));
    m_timer.start();

    QProcess dd;
    dd.setProgram("dd");
    dd.setArguments({"if=" + m_isoPath, "of=" + m_devicePath,
                     "bs=4M", "status=progress", "conv=fdatasync"});
    dd.setProcessChannelMode(QProcess::MergedChannels);
    dd.start();

    if (!dd.waitForStarted(5000)) {
        emit logMessage(tr("CRITICAL ERROR: Failed to launch dd subprocess!"));
        return false;
    }

    QString partialLine;
    while (dd.waitForReadyRead(500) || dd.state() == QProcess::Running) {
        QByteArray data = dd.readAllStandardOutput();
        if (data.isEmpty()) continue;

        QString text = partialLine + QString::fromUtf8(data);
        QStringList lines = text.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

        if (!text.endsWith('\n') && !text.endsWith('\r'))
            partialLine = lines.takeLast();
        else
            partialLine.clear();

        for (const QString &line : lines) {
            if (line.trimmed().isEmpty()) continue;

            unsigned long long bytesCopied = line.trimmed().toULongLong();
            if (bytesCopied > 0 && m_isoSize > 0) {
                double progress = static_cast<double>(bytesCopied) / m_isoSize;
                if (progress > 0.99) progress = 0.99;

                double elapsed = m_timer.elapsed() / 1000.0;
                if (elapsed > 0.1) {
                    double speedBps = bytesCopied / elapsed;
                    double speedMb = speedBps / (1024.0 * 1024.0);

                    double remainingBytes = m_isoSize - bytesCopied;
                    double remainingSecs = remainingBytes / speedBps;

                    QString speedStr = tr("Speed: %1 MB/s").arg(speedMb, 0, 'f', 1);
                    QString etaStr;
                    int hours = static_cast<int>(remainingSecs / 3600);
                    int mins = static_cast<int>((remainingSecs - hours * 3600) / 60);
                    int secs = static_cast<int>(remainingSecs - hours * 3600 - mins * 60);

                    if (hours > 0)
                        etaStr = tr("ETA: %1:%2:%3")
                            .arg(hours, 2, 10, QChar('0'))
                            .arg(mins, 2, 10, QChar('0'))
                            .arg(secs, 2, 10, QChar('0'));
                    else
                        etaStr = tr("ETA: %1:%2")
                            .arg(mins, 2, 10, QChar('0'))
                            .arg(secs, 2, 10, QChar('0'));

                    QString statusStr = tr("Writing ISO... %1%")
                        .arg(progress * 100.0, 0, 'f', 1);

                    emit progressUpdated(progress, statusStr,
                        QString("%1 | %2").arg(speedStr, etaStr));
                }
            }
        }
    }

    dd.waitForFinished(60000);
    if (dd.exitCode() != 0) {
        emit logMessage(QString("  [DD] %1").arg(tr("Finished with exit code %1").arg(dd.exitCode())));
        return false;
    }
    return true;
}

bool BurnWorker::copyTreeWithProgress(const QString &sourceRoot, const QString &targetRoot)
{
    emit progressUpdated(0.58, tr("Preparing file copy..."), tr("Scanning ISO"));
    emit logMessage(tr("Scanning ISO contents to calculate copy progress..."));

    struct CopyEntry {
        QString source;
        QString target;
        QFileInfo info;
    };

    QList<CopyEntry> entries;
    qint64 totalBytes = 0;
    int totalFiles = 0;
    int totalDirs = 0;

    QDirIterator scanner(sourceRoot,
                         QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (scanner.hasNext()) {
        if (m_canceled)
            return false;

        scanner.next();
        QFileInfo info = scanner.fileInfo();
        const QString relativePath = QDir(sourceRoot).relativeFilePath(info.filePath());
        CopyEntry entry{info.filePath(), QDir(targetRoot).filePath(relativePath), info};
        entries.append(entry);

        if (info.isDir()) {
            ++totalDirs;
        } else if (info.isFile()) {
            ++totalFiles;
            totalBytes += info.size();
        }
    }

    emit logMessage(tr("Copy plan: %1 files, %2 directories, %3 total.")
                    .arg(totalFiles).arg(totalDirs).arg(formatBytes(totalBytes)));

    QElapsedTimer timer;
    QElapsedTimer uiTimer;
    timer.start();
    uiTimer.start();

    qint64 copiedBytes = 0;
    int copiedFiles = 0;
    QByteArray buffer;
    buffer.resize(1024 * 1024);

    auto updateProgress = [&](const QString &currentFile, bool force) {
        if (!force && uiTimer.elapsed() < 500)
            return;
        uiTimer.restart();

        const double fileProgress = totalBytes > 0
            ? static_cast<double>(copiedBytes) / static_cast<double>(totalBytes)
            : 1.0;
        const double overallProgress = 0.58 + (fileProgress * 0.32);
        const double elapsedSecs = qMax(0.001, timer.elapsed() / 1000.0);
        const double speed = copiedBytes / elapsedSecs;
        const qint64 remaining = totalBytes > copiedBytes ? totalBytes - copiedBytes : 0;
        const qint64 etaSecs = speed > 1.0 ? static_cast<qint64>(remaining / speed) : 0;

        emit progressUpdated(overallProgress,
            tr("Copying files... %1% (%2/%3)")
                .arg(fileProgress * 100.0, 0, 'f', 1)
                .arg(copiedFiles)
                .arg(totalFiles),
            tr("%1/s | ETA %2 | %3")
                .arg(formatBytes(static_cast<qint64>(speed)))
                .arg(formatDuration(etaSecs), QFileInfo(currentFile).fileName()));
    };

    for (const CopyEntry &entry : entries) {
        if (m_canceled)
            return false;

        if (entry.info.isDir()) {
            QDir().mkpath(entry.target);
            continue;
        }

        QFileInfo targetInfo(entry.target);
        QDir().mkpath(targetInfo.absolutePath());

        if (entry.info.isSymLink()) {
            QFile::remove(entry.target);
            if (!QFile::link(entry.info.symLinkTarget(), entry.target)) {
                emit logMessage(tr("WARNING: Could not create symbolic link %1; skipping.").arg(entry.target));
            }
            continue;
        }

        if (!entry.info.isFile())
            continue;

        QFile in(entry.source);
        if (!in.open(QIODevice::ReadOnly)) {
            emit logMessage(tr("Failed to read %1: %2").arg(entry.source, in.errorString()));
            return false;
        }

        QFile::remove(entry.target);
        QFile out(entry.target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit logMessage(tr("Failed to write %1: %2").arg(entry.target, out.errorString()));
            return false;
        }

        emit logMessage(tr("Copying %1 (%2)...")
                        .arg(QDir(sourceRoot).relativeFilePath(entry.source), formatBytes(entry.info.size())));

        while (!in.atEnd()) {
            if (m_canceled)
                return false;

            const qint64 read = in.read(buffer.data(), buffer.size());
            if (read < 0) {
                emit logMessage(tr("Failed to read %1: %2").arg(entry.source, in.errorString()));
                return false;
            }

            qint64 writtenTotal = 0;
            while (writtenTotal < read) {
                const qint64 written = out.write(buffer.constData() + writtenTotal, read - writtenTotal);
                if (written < 0) {
                    emit logMessage(tr("Failed to write %1: %2").arg(entry.target, out.errorString()));
                    return false;
                }
                writtenTotal += written;
                copiedBytes += written;
                updateProgress(entry.source, false);
            }
        }

        out.setPermissions(entry.info.permissions());
        out.close();
        in.close();
        ++copiedFiles;
        updateProgress(entry.source, true);
    }

    emit progressUpdated(0.90, tr("File copy complete."), tr("Copied %1 in %2")
                         .arg(formatBytes(copiedBytes), formatDuration(timer.elapsed() / 1000)));
    emit logMessage(tr("Copied %1 files (%2) in %3.")
                    .arg(copiedFiles)
                    .arg(formatBytes(copiedBytes), formatDuration(timer.elapsed() / 1000)));
    return true;
}

bool BurnWorker::extractIsoImage()
{
    emit logMessage(tr("Extracting ISO contents to USB..."));
    emit progressUpdated(0.58, tr("Extracting ISO..."), tr("Busy"));

    const QString part1 = partitionPath(1);
    QTemporaryDir isoTempDir(QStringLiteral("/tmp/rufus-iso-XXXXXX"));
    QTemporaryDir usbTempDir(QStringLiteral("/tmp/rufus-usb-XXXXXX"));
    if (!isoTempDir.isValid() || !usbTempDir.isValid()) {
        emit logMessage(tr("Failed to create temporary mount directories."));
        return false;
    }

    QString isoMnt;
    QString usbMnt;
    bool mountedIso = false;
    bool mountedUsb = false;

    const QString canonicalIso = QFileInfo(m_isoPath).canonicalFilePath();
    isoMnt = mountedTargetForSource(canonicalIso);
    if (isoMnt.isEmpty())
        isoMnt = mountedTargetForSource(m_isoPath);

    if (!isoMnt.isEmpty()) {
        emit logMessage(tr("Using existing mounted ISO at %1.").arg(isoMnt));
        mountedIso = isRufusTempMount(isoMnt, "rufus-iso");
    } else {
        isoMnt = isoTempDir.path();
        if (!runCmd("mount", {"-o", "loop,ro", m_isoPath, isoMnt}, "EXTRACT"))
            return false;
        mountedIso = true;
    }

    auto mountUsbPartition = [&]() -> bool {
        if (m_filesystem == "NTFS") {
            emit logMessage(tr("Mounting NTFS partition with kernel ntfs3 driver..."));
            if (runCmd("mount", {"-t", "ntfs3", "-o", "rw", part1, usbMnt}, "EXTRACT"))
                return true;

            emit logMessage(tr("Kernel ntfs3 mount failed; trying ntfs-3g..."));
            if (runCmd("ntfs-3g", {part1, usbMnt, "-o", "rw"}, "EXTRACT"))
                return true;

            emit logMessage(tr("Failed to mount NTFS partition. Install ntfs-3g or enable kernel ntfs3 support."));
            return false;
        }

        if (m_filesystem == "FAT32")
            return runCmd("mount", {"-t", "vfat", part1, usbMnt}, "EXTRACT");
        if (m_filesystem == "exFAT")
            return runCmd("mount", {"-t", "exfat", part1, usbMnt}, "EXTRACT");
        if (m_filesystem == "ext4")
            return runCmd("mount", {"-t", "ext4", part1, usbMnt}, "EXTRACT");

        return runCmd("mount", {part1, usbMnt}, "EXTRACT");
    };

    usbMnt = mountedTargetForSource(part1);
    if (!usbMnt.isEmpty()) {
        emit logMessage(tr("Using existing mounted USB partition at %1.").arg(usbMnt));
        mountedUsb = isRufusTempMount(usbMnt, "rufus-usb");
    } else {
        usbMnt = usbTempDir.path();
        if (!mountUsbPartition()) {
            if (mountedIso)
                runCmd("umount", {isoMnt}, "EXTRACT");
            return false;
        }
        mountedUsb = true;
    }

    auto cleanupMounts = [&]() {
        if (mountedUsb)
            runCmd("umount", {usbMnt}, "EXTRACT");
        if (mountedIso)
            runCmd("umount", {isoMnt}, "EXTRACT");
    };

    emit logMessage(tr("Copying files from ISO to USB... This can take several minutes."));
    if (!copyTreeWithProgress(isoMnt, usbMnt)) {
        cleanupMounts();
        return false;
    }
    emit logMessage(tr("Files copied successfully."));

    const bool windowsInstaller = isWindowsInstallerMount(isoMnt);
    if (windowsInstaller) {
        emit logMessage(tr("Windows installer detected: skipping GRUB installation."));
        emit logMessage(tr("Windows install media should boot through Windows Boot Manager, not GRUB."));
        if (m_partitionScheme == "MBR") {
            emit logMessage(tr("WARNING: Legacy BIOS/MBR boot for extracted Windows ISOs is not supported yet. Use DD Image mode, or use GPT/UEFI."));
        }
        if (m_filesystem == "NTFS") {
            emit logMessage(tr("WARNING: UEFI firmware usually cannot boot NTFS directly without UEFI:NTFS support. If the ISO fits, use FAT32; otherwise use DD Image mode."));
        }
    } else if (m_partitionScheme == "MBR") {
        emit progressUpdated(0.78, tr("Installing GRUB for BIOS boot..."), tr("Busy"));
        QProcess which;
        which.start("which", {"grub-install"});
        which.waitForFinished(2000);
        if (which.exitCode() == 0) {
            if (!runCmd("grub-install", {"--target=i386-pc",
                         "--boot-directory=" + usbMnt + "/boot",
                         m_devicePath}, "GRUB")) {
                emit logMessage(tr("WARNING: GRUB installation failed. "
                                 "Drive may not boot on BIOS systems."));
            } else {
                emit logMessage(tr("GRUB installed for BIOS boot."));
            }
        } else {
            emit logMessage(tr("WARNING: grub-install not found. "
                             "Install 'grub' package for BIOS boot support."));
        }
    }

    if (m_winTweaks != 0) {
        emit logMessage(tr("Applying Windows installation tweaks..."));
        writeWindowsTweaks(usbMnt);
    }

    emit progressUpdated(0.92, tr("Flushing copied files to USB..."), tr("Please wait"));
    emit logMessage(tr("Flushing copied files to USB. This may take a while on slow drives..."));
    runCmd("sync", {}, "EXTRACT", 0);
    cleanupMounts();
    emit progressUpdated(0.94, tr("ISO extraction complete."), tr("Done"));
    emit logMessage(tr("ISO extraction complete."));
    return true;
}

bool BurnWorker::writeWindowsTweaks(const QString &usbMnt)
{
    // Stage 1: autounattend.xml (works on retail Windows 10/11)
    emit logMessage(tr("Writing autounattend.xml for OOBE bypass..."));

    QStringList runSyncCommands;
    if (m_winTweaks & BypassTpm)
        runSyncCommands << "reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f";
    if (m_winTweaks & BypassRam)
        runSyncCommands << "reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f";
    if (m_winTweaks & BypassSecureBoot)
        runSyncCommands << "reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f";
    if (m_winTweaks & BypassMsAccount)
        runSyncCommands << "reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f";

    QString xml = R"(<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
    <settings pass="windowsPE">
        <component name="Microsoft-Windows-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
            <UserData>
                <AcceptEula>true</AcceptEula>
            </UserData>
)";

    if (!runSyncCommands.isEmpty()) {
        xml += "            <RunSynchronous>\n";
        int order = 1;
        for (const QString &cmd : runSyncCommands) {
            xml += QString(R"(                <RunSynchronousCommand wcm:action="add">
                    <Order>%1</Order>
                    <Path>cmd /c %2</Path>
                </RunSynchronousCommand>
)").arg(order++).arg(cmd);
        }
        xml += "            </RunSynchronous>\n";
    }

    xml += R"(        </component>
    </settings>
    <settings pass="oobeSystem">
        <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
)";
    if (m_winTweaks & LocalAccount) {
        xml += R"(            <UserAccounts>
                <LocalAccounts>
                    <LocalAccount wcm:action="add">
                        <Password>
                            <Value>rufus</Value>
                            <PlainText>true</PlainText>
                        </Password>
                        <DisplayName>RufusUser</DisplayName>
                        <Name>RufusUser</Name>
                        <Group>Administrators</Group>
                    </LocalAccount>
                </LocalAccounts>
            </UserAccounts>
            <AutoLogon>
                <Enabled>true</Enabled>
                <Username>RufusUser</Username>
                <Password>
                    <Value>rufus</Value>
                    <PlainText>true</PlainText>
                </Password>
            </AutoLogon>
)";
    }
    if (m_winTweaks & BypassMsAccount) {
        xml += R"(            <OOBE>
                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
                <SkipMachineOOBE>true</SkipMachineOOBE>
                <SkipUserOOBE>true</SkipUserOOBE>
                <ProtectYourPC>3</ProtectYourPC>
            </OOBE>
)";
    }
    xml += R"(        </component>
    </settings>
</unattend>
)";

    QFile f(usbMnt + "/autounattend.xml");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(xml.toUtf8());
        f.close();
        emit logMessage(tr("autounattend.xml written successfully."));
    } else {
        emit logMessage(tr("WARNING: Could not write autounattend.xml"));
    }

    // Stage 2: wim registry injection (fallback for custom Windows ISOs)
    QString wimPath;
    QStringList candidates = {usbMnt + "/sources/install.wim",
                              usbMnt + "/sources/install.esd"};
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) {
            wimPath = c;
            break;
        }
    }

    if (wimPath.isEmpty()) {
        emit logMessage(tr("No install.wim found — autounattend.xml will be used, "
                         "but may not work with custom Windows builds."));
        return true;
    }

    // Check for wimlib-imagex
    QProcess which;
    which.start("which", {"wimlib-imagex"});
    which.waitForFinished(2000);
    if (which.exitCode() != 0) {
        emit logMessage(tr("WARNING: wimlib-imagex not found. "
                         "Install 'wimlib' for registry-based Windows tweaks."));
        return true;
    }

    emit logMessage(tr("Injecting bypass registry keys... (may take a while)"));
    QString mntWim = "/tmp/rufus-wim";
    QDir().mkpath(mntWim);

    if (!runCmd("wimlib-imagex", {"mountrw", wimPath, "1", mntWim}, "WIM", 300)) {
        emit logMessage(tr("WARNING: Failed to mount install.wim \u2014 "
                         "skipping registry injection."));
        return true;
    }

    const QString softwareHivePath = mntWim + "/Windows/System32/config/SOFTWARE";
    const QString systemHivePath = mntWim + "/Windows/System32/config/SYSTEM";

    QString labConfigReg = "Windows Registry Editor Version 5.00\n\n";
    labConfigReg += "[HKEY_LOCAL_MACHINE\\SYSTEM\\Setup\\LabConfig]\n";
    if (m_winTweaks & BypassTpm)
        labConfigReg += "\"BypassTPMCheck\"=dword:00000001\n";
    if (m_winTweaks & BypassRam)
        labConfigReg += "\"BypassRAMCheck\"=dword:00000001\n";
    if (m_winTweaks & BypassSecureBoot)
        labConfigReg += "\"BypassSecureBootCheck\"=dword:00000001\n";

    QString oobeReg = "Windows Registry Editor Version 5.00\n\n";
    oobeReg += "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE]\n";
    if (m_winTweaks & BypassMsAccount)
        oobeReg += "\"BypassNRO\"=dword:00000001\n";

    // Check for hivexregedit (Linux registry hive editor)
    which.start("which", {"hivexregedit"});
    which.waitForFinished(2000);
    if (which.exitCode() == 0) {
        auto mergeReg = [&](const QString &hivePath, const QString &prefix,
                            const QString &content, const QString &name) {
            QString regFile = mntWim + "/" + name;
            QFile rf(regFile);
            if (rf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                rf.write(content.toUtf8());
                rf.close();
                runCmd("hivexregedit", {"--merge", "--prefix", prefix, hivePath, regFile}, "WIM", 60);
                QFile::remove(regFile);
            }
        };

        if (m_winTweaks & (BypassTpm | BypassRam | BypassSecureBoot))
            mergeReg(systemHivePath, "HKEY_LOCAL_MACHINE\\SYSTEM", labConfigReg, "_labconfig.reg");
        if (m_winTweaks & BypassMsAccount)
            mergeReg(softwareHivePath, "HKEY_LOCAL_MACHINE\\SOFTWARE", oobeReg, "_oobe.reg");
    } else {
        emit logMessage(tr("WARNING: hivexregedit not found. Registry injection skipped; autounattend.xml will still be used."));
    }

    runCmd("wimlib-imagex", {"unmount", mntWim, "--commit"}, "WIM", 300);
    emit logMessage(tr("Registry injection complete."));
    return true;
}

bool BurnWorker::createPersistentPartition()
{
    emit logMessage(tr("Creating persistent storage partition..."));

    QString partPath = partitionPath(2);

    qint64 sizeBytes = m_persistentSize * (m_persistentUnits == "GB" ? 1073741824LL : 1048576LL);
    QString startStr = QString("-%1MiB").arg(static_cast<int>(sizeBytes / (1024.0 * 1024.0)));

    bool ok = runCmd("parted", {"-s", m_devicePath, "mkpart", "primary",
                                 "ext4", startStr, "100%"}, "PARTED");
    if (!ok) return false;

    rescanPartitions();
    if (!waitForPartition(2))
        return false;

    return runCmd("mkfs.ext4", {"-F", "-L", "casper-rw", partPath}, "MKFS");
}

bool BurnWorker::validateUefiBoot()
{
    emit logMessage(tr("Validating UEFI boot structure..."));

    QString partPath = partitionPath(1);

    QString mntDir = "/tmp/rufus-uefi-check";
    runCmd("mkdir", {"-p", mntDir}, "UEFI");
    bool mounted = runCmd("mount", {partPath, mntDir}, "UEFI");
    if (!mounted) {
        emit logMessage(tr("UEFI validation: could not mount partition."));
        return false;
    }

    bool efiFound = false;
    QFileInfo efiFile(QString("%1/EFI/BOOT/BOOTx64.EFI").arg(mntDir));
    QFileInfo efiFileIa32(QString("%1/EFI/BOOT/BOOTIA32.EFI").arg(mntDir));
    if (efiFile.exists() || efiFileIa32.exists()) {
        emit logMessage(tr("UEFI validation PASSED: EFI boot loader found."));
        efiFound = true;
    } else {
        emit logMessage(tr("WARNING: No EFI boot loader found. "
                        "The drive may not boot on UEFI systems."));
    }

    runCmd("umount", {mntDir}, "UEFI");
    return efiFound;
}

bool BurnWorker::checkIsoUefi(const QString &isoPath)
{
    QProcess proc;
    proc.start("isoinfo", {"-l", "-i", isoPath});
    proc.waitForFinished(5000);
    QString output = QString::fromUtf8(proc.readAllStandardOutput());

    if (output.contains("EFI", Qt::CaseInsensitive) ||
        output.contains("eltorito", Qt::CaseInsensitive))
        return true;

    proc.start("fdisk", {"-l", isoPath});
    proc.waitForFinished(5000);
    output = QString::fromUtf8(proc.readAllStandardOutput());

    return output.contains("EFI", Qt::CaseInsensitive) ||
           output.contains("UEFI", Qt::CaseInsensitive);
}

void BurnWorker::burn()
{
    m_canceled = false;

    emit logMessage(tr("================================================"));
    emit logMessage(tr("Starting formatting/writing process for device: %1").arg(m_devicePath));
    emit logMessage(tr("Target partition scheme: %1").arg(m_partitionScheme));
    emit logMessage(tr("Target filesystem: %1").arg(m_filesystem));
    emit logMessage(tr("Volume label: %1").arg(m_volumeLabel));
    if (m_writeIso)
        emit logMessage(tr("Source ISO image: %1").arg(m_isoPath));
    if (m_persistent)
        emit logMessage(tr("Persistent storage: %1 %2").arg(m_persistentSize).arg(m_persistentUnits));
    emit logMessage(tr("================================================"));

    emit progressUpdated(0.02, tr("Unmounting existing partitions..."), tr("Busy"));
    unmountDevice();
    if (m_canceled) { emit finished(tr("Canceled.")); return; }

    emit progressUpdated(0.05, tr("Wiping partition tables..."), tr("Busy"));
    if (!wipeDevice()) {
        if (m_canceled) { emit finished(tr("Canceled.")); return; }
        emit finished(tr("Failed to wipe device. Check the log for details."));
        return;
    }
    if (m_canceled) { emit finished(tr("Canceled.")); return; }

    if (m_badBlocks > 0) {
        if (!scanBadBlocks()) {
            if (m_canceled) { emit finished(tr("Canceled.")); return; }
            emit finished(tr("Bad block scan failed or aborted."));
            return;
        }
    }
    if (m_canceled) { emit finished(tr("Canceled.")); return; }

    if (!m_writeIso) {
        emit progressUpdated(0.20, tr("Creating partitions..."), tr("Busy"));
        if (!createPartitions()) {
            if (m_canceled) { emit finished(tr("Canceled.")); return; }
            emit finished(tr("Failed to create partitions. Check the log for details."));
            return;
        }
        if (m_canceled) { emit finished(tr("Canceled.")); return; }

        emit progressUpdated(0.50, tr("Formatting filesystem..."), tr("Busy"));
        if (!formatPartition()) {
            if (m_canceled) { emit finished(tr("Canceled.")); return; }
            emit finished(tr("Failed to format partition. Check the log for details."));
            return;
        }
    } else if (m_isoMode) {
        emit progressUpdated(0.20, tr("Creating partitions..."), tr("Busy"));
        if (!createPartitions()) {
            if (m_canceled) { emit finished(tr("Canceled.")); return; }
            emit finished(tr("Failed to create partitions."));
            return;
        }
        if (m_canceled) { emit finished(tr("Canceled.")); return; }

        if (!formatPartition()) {
            if (m_canceled) { emit finished(tr("Canceled.")); return; }
            emit finished(tr("Failed to format partition."));
            return;
        }
        if (m_canceled) { emit finished(tr("Canceled.")); return; }

        if (!extractIsoImage()) {
            emit finished(m_canceled ? tr("Canceled.") : tr("Failed to extract ISO image."));
            return;
        }
    } else {
        emit progressUpdated(0.10, tr("Writing ISO image..."), tr("Busy"));
        if (!writeIsoImage()) {
            emit finished(m_canceled ? tr("Canceled.") : tr("Failed to write ISO image."));
            return;
        }
    }
    if (m_canceled) { emit finished(tr("Canceled.")); return; }

    if (m_persistent && (m_isoMode || !m_writeIso)) {
        emit progressUpdated(0.80, tr("Creating persistent partition..."), tr("Busy"));
        if (!createPersistentPartition()) {
            if (!m_canceled)
                emit logMessage(tr("WARNING: Failed to create persistent partition."));
        }
    }
    if (m_canceled) { emit finished(tr("Canceled.")); return; }

    emit progressUpdated(0.93, tr("Synchronizing filesystem cache..."), tr("Syncing"));
    emit logMessage(tr("Flushing cache buffers... Please do not unplug the drive!"));
    QProcess::execute("sync");
    emit logMessage(tr("Synchronized. USB drive is now safe to unplug."));

    emit progressUpdated(1.0, tr("Complete!"), tr("Done"));
    emit finished(tr("Rufus Qt has successfully formatted and written to the USB device %1.\n"
                      "Your bootable drive is ready!").arg(m_devicePath));
}

// ── HashWorker ─────────────────────────────────────────────────────────

HashWorker::HashWorker(const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_filePath(filePath)
{
}

void HashWorker::compute()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(tr("Error opening image file for checksum."));
        return;
    }

    QCryptographicHash md5(QCryptographicHash::Md5);
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    QCryptographicHash sha256(QCryptographicHash::Sha256);

    QByteArray buffer;
    buffer.resize(65536);
    qint64 totalBytes = 0;

    while (!file.atEnd()) {
        QByteArray chunk = file.read(buffer.size());
        if (chunk.isEmpty()) break;

        md5.addData(QByteArrayView(chunk));
        sha1.addData(QByteArrayView(chunk));
        sha256.addData(QByteArrayView(chunk));
        totalBytes += chunk.size();
    }
    file.close();

    QString result = QString(
        tr("File: %1\nSize: %2 bytes (%3 GB)\n\n"
           "MD5:    %4\nSHA-1:  %5\nSHA-256:%6\n"))
        .arg(QFileInfo(m_filePath).fileName())
        .arg(totalBytes)
        .arg(static_cast<double>(totalBytes) / (1024 * 1024 * 1024), 0, 'f', 2)
        .arg(QString(md5.result().toHex()))
        .arg(QString(sha1.result().toHex()))
        .arg(QString(sha256.result().toHex()));

    emit finished(result);
}
