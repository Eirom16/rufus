#include "workers.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDataStream>

// ── BurnWorker ─────────────────────────────────────────────────────────

BurnWorker::BurnWorker(const QString &devicePath, const QString &isoPath,
                       const QString &filesystem, const QString &partitionScheme,
                       const QString &volumeLabel, bool writeIso, qint64 isoSize,
                       QObject *parent)
    : QObject(parent)
    , m_devicePath(devicePath)
    , m_isoPath(isoPath)
    , m_filesystem(filesystem)
    , m_partitionScheme(partitionScheme)
    , m_volumeLabel(volumeLabel)
    , m_writeIso(writeIso)
    , m_isoSize(isoSize)
{
}

bool BurnWorker::runCmd(const QString &program, const QStringList &args,
                         const QString &logPrefix)
{
    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();

    if (!proc.waitForStarted(5000)) {
        emit logMessage(QString("  [%1] Failed to start: %2").arg(logPrefix, program));
        return false;
    }

    while (proc.waitForReadyRead(1000) || proc.state() == QProcess::Running) {
        QByteArray data = proc.readAllStandardOutput();
        if (!data.isEmpty()) {
            for (const QByteArray &line : data.split('\n')) {
                if (!line.trimmed().isEmpty())
                    emit logMessage(QString("  [%1] %2").arg(logPrefix, QString::fromUtf8(line.trimmed())));
            }
        }
    }
    proc.waitForFinished(30000);

    if (proc.exitCode() != 0) {
        emit logMessage(QString("  [%1] Failed with exit code %2: %3")
            .arg(logPrefix).arg(proc.exitCode()).arg(QString::fromUtf8(proc.readAll())));
        return false;
    }
    return true;
}

void BurnWorker::unmountDevice()
{
    emit logMessage(QString("Unmounting any active partitions on %1...").arg(m_devicePath));
    runCmd("umount", {m_devicePath + "*"}, "UMOUNT");
    QThread::msleep(1000);
}

bool BurnWorker::wipeDevice()
{
    emit logMessage(QString("Wiping partition table on %1...").arg(m_devicePath));
    return runCmd("dd", { "if=/dev/zero", "of=" + m_devicePath,
                          "bs=1M", "count=10", "oflag=sync", "conv=notrunc" }, "WIPE");
}

bool BurnWorker::createPartitions()
{
    emit logMessage("Creating partition table using parted...");
    QString labelType = (m_partitionScheme == "GPT") ? "gpt" : "msdos";
    bool ok = runCmd("parted", {"-s", m_devicePath, "mklabel", labelType,
                                 "mkpart", "primary", "1MiB", "100%"}, "PARTED");
    if (!ok) return false;

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

    emit logMessage(QString("Formatting partition %1 to %2 with label '%3'...")
        .arg(partitionPath, m_filesystem, m_volumeLabel));

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
    emit logMessage("Starting ISO burn process...");
    m_timer.start();

    QProcess dd;
    dd.setProgram("dd");
    dd.setArguments({"if=" + m_isoPath, "of=" + m_devicePath,
                     "bs=4M", "status=progress", "conv=fdatasync"});
    dd.setProcessChannelMode(QProcess::MergedChannels);
    dd.start();

    if (!dd.waitForStarted(5000)) {
        emit logMessage("CRITICAL ERROR: Failed to launch dd subprocess!");
        return false;
    }

    // Read dd output line by line for progress
    while (dd.waitForReadyRead(500) || dd.state() == QProcess::Running) {
        QByteArray data = dd.readAllStandardOutput();
        if (data.isEmpty()) continue;

        // dd outputs lines like "1234567890 bytes (1.2 GB, ...)"
        for (const QByteArray &line : data.split('\n')) {
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

                    QString speedStr = QString("Speed: %1 MB/s").arg(speedMb, 0, 'f', 1);
                    QString etaStr;
                    int hours = static_cast<int>(remainingSecs / 3600);
                    int mins = static_cast<int>((remainingSecs - hours * 3600) / 60);
                    int secs = static_cast<int>(remainingSecs - hours * 3600 - mins * 60);

                    if (hours > 0)
                        etaStr = QString("ETA: %1:%2:%3")
                            .arg(hours, 2, 10, QChar('0'))
                            .arg(mins, 2, 10, QChar('0'))
                            .arg(secs, 2, 10, QChar('0'));
                    else
                        etaStr = QString("ETA: %1:%2")
                            .arg(mins, 2, 10, QChar('0'))
                            .arg(secs, 2, 10, QChar('0'));

                    QString statusStr = QString("Writing ISO... %1%")
                        .arg(progress * 100.0, 0, 'f', 1);

                    emit progressUpdated(progress, statusStr,
                        QString("%1 | %2").arg(speedStr, etaStr));
                }
            } else {
                emit logMessage(QString("  [DD] %1").arg(QString::fromUtf8(line.trimmed())));
            }
        }
    }

    dd.waitForFinished(60000);
    return dd.exitCode() == 0;
}

void BurnWorker::burn()
{
    emit logMessage("==================================================");
    emit logMessage(QString("Starting formatting/writing process for device: %1").arg(m_devicePath));
    emit logMessage(QString("Target partition scheme: %1").arg(m_partitionScheme));
    emit logMessage(QString("Target filesystem: %1").arg(m_filesystem));
    emit logMessage(QString("Volume label: %1").arg(m_volumeLabel));
    if (m_writeIso)
        emit logMessage(QString("Source ISO image: %1").arg(m_isoPath));
    emit logMessage("==================================================");

    emit progressUpdated(0.02, "Unmounting existing partitions...", "Busy");
    unmountDevice();

    emit progressUpdated(0.05, "Wiping partition tables...", "Busy");
    if (!wipeDevice()) {
        emit finished("Failed to wipe device. Check the log for details.");
        return;
    }

    if (!m_writeIso) {
        emit progressUpdated(0.20, "Creating partitions...", "Busy");
        if (!createPartitions()) {
            emit finished("Failed to create partitions. Check the log for details.");
            return;
        }

        emit progressUpdated(0.50, "Formatting filesystem...", "Busy");
        if (!formatPartition()) {
            emit finished("Failed to format partition. Check the log for details.");
            return;
        }
    } else {
        emit progressUpdated(0.10, "Writing ISO image...", "Busy");
        if (!writeIsoImage()) {
            emit finished("Failed to write ISO image. Check the log for details.");
            return;
        }
    }

    emit progressUpdated(0.99, "Synchronizing filesystem cache...", "Syncing");
    emit logMessage("Flushing cache buffers... Please do not unplug the drive!");
    QProcess::execute("sync");
    emit logMessage("Synchronized. USB drive is now safe to unplug.");

    emit progressUpdated(1.0, "Complete!", "Done");
    emit finished(QString("Rufus Qt has successfully formatted and written to the USB device %1.\n"
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
        emit finished("Error opening image file for checksum.");
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
        "File: %1\nSize: %2 bytes (%3 GB)\n\n"
        "MD5:    %4\nSHA-1:  %5\nSHA-256:%6\n")
        .arg(QFileInfo(m_filePath).fileName())
        .arg(totalBytes)
        .arg(static_cast<double>(totalBytes) / (1024 * 1024 * 1024), 0, 'f', 2)
        .arg(QString(md5.result().toHex()))
        .arg(QString(sha1.result().toHex()))
        .arg(QString(sha256.result().toHex()));

    emit finished(result);
}
