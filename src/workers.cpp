#include "workers.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDataStream>
#include <QRegularExpression>

// ── BurnWorker ─────────────────────────────────────────────────────────

BurnWorker::BurnWorker(const QString &devicePath, const QString &isoPath,
                       const QString &filesystem, const QString &partitionScheme,
                       const QString &volumeLabel, bool writeIso, qint64 isoSize,
                       int badBlocks, bool persistent, int persistentSize,
                       const QString &persistentUnits,
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
{
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
    runCmd("umount", {m_devicePath + "*"}, "UMOUNT");
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

    if (m_partitionScheme == "GPT") {
        QString partPath = m_devicePath.contains("nvme") ?
                    m_devicePath + "p1" : m_devicePath + "1";
        runCmd("parted", {"-s", m_devicePath, "set", "1", "esp", "off"}, "PARTED");
        if (labelType == "gpt") {
            runCmd("parted", {"-s", m_devicePath, "set", "1", "msftdata", "on"}, "PARTED");
        }
    }

    QThread::msleep(2000);
    return true;
}

bool BurnWorker::formatPartition()
{
    QString partitionPath;
    if (m_devicePath.contains("nvme"))
        partitionPath = m_devicePath + "p1";
    else
        partitionPath = m_devicePath + "1";

    emit logMessage(tr("Formatting partition %1 to %2 with label '%3'...")
        .arg(partitionPath, m_filesystem, m_volumeLabel));

    QThread::msleep(500);

    if (m_filesystem == "FAT32") {
        return runCmd("mkfs.vfat", {"-F", "32", "-n", m_volumeLabel, partitionPath}, "MKFS");
    } else if (m_filesystem == "NTFS") {
        return runCmd("mkfs.ntfs", {"-f", "-L", m_volumeLabel, partitionPath}, "MKFS");
    } else if (m_filesystem == "exFAT") {
        return runCmd("mkfs.exfat", {"-n", m_volumeLabel, partitionPath}, "MKFS");
    } else {
        return runCmd("mkfs.ext4", {"-F", "-L", m_volumeLabel, partitionPath}, "MKFS");
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

bool BurnWorker::createPersistentPartition()
{
    emit logMessage(tr("Creating persistent storage partition..."));

    QString partPath = m_devicePath.contains("nvme") ?
                m_devicePath + "p2" : m_devicePath + "2";

    qint64 sizeBytes = m_persistentSize * (m_persistentUnits == "GB" ? 1073741824LL : 1048576LL);
    QString startStr = QString("-%1MiB").arg(static_cast<int>(sizeBytes / (1024.0 * 1024.0)));

    bool ok = runCmd("parted", {"-s", m_devicePath, "mkpart", "primary",
                                 "ext4", startStr, "100%"}, "PARTED");
    if (!ok) return false;

    QThread::msleep(1500);

    return runCmd("mkfs.ext4", {"-F", "-L", "casper-rw", partPath}, "MKFS");
}

bool BurnWorker::validateUefiBoot()
{
    emit logMessage(tr("Validating UEFI boot structure..."));

    QString partPath = m_devicePath.contains("nvme") ?
                m_devicePath + "p1" : m_devicePath + "1";

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
    } else {
        emit progressUpdated(0.10, tr("Writing ISO image..."), tr("Busy"));
        if (!writeIsoImage()) {
            emit finished(m_canceled ? tr("Canceled.") : tr("Failed to write ISO image."));
            return;
        }
    }
    if (m_canceled) { emit finished(tr("Canceled.")); return; }

    if (m_persistent && !m_writeIso) {
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
