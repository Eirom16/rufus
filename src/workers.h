#ifndef WORKERS_H
#define WORKERS_H

#include <QObject>
#include <QString>
#include <QProcess>
#include <QElapsedTimer>
#include <QCryptographicHash>

class BurnWorker : public QObject
{
    Q_OBJECT

public:
    explicit BurnWorker(const QString &devicePath, const QString &isoPath,
                        const QString &filesystem, const QString &partitionScheme,
                        const QString &volumeLabel, bool writeIso, qint64 isoSize,
                        int badBlocks, bool persistent, int persistentSize,
                        const QString &persistentUnits,
                        QObject *parent = nullptr);

    static bool checkIsoUefi(const QString &isoPath);

public slots:
    void burn();
    void cancel();

signals:
    void progressUpdated(double pct, const QString &status, const QString &speedEta);
    void logMessage(const QString &msg);
    void finished(const QString &result);

private:
    bool runCmd(const QString &program, const QStringList &args, const QString &logPrefix,
                int timeoutSecs = 120);
    void unmountDevice();
    bool wipeDevice();
    bool scanBadBlocks();
    bool createPartitions();
    bool formatPartition();
    bool writeIsoImage();
    bool createPersistentPartition();
    bool validateUefiBoot();

    QString m_devicePath;
    QString m_isoPath;
    QString m_filesystem;
    QString m_partitionScheme;
    QString m_volumeLabel;
    bool m_writeIso;
    qint64 m_isoSize;
    int m_badBlocks;
    bool m_persistent;
    int m_persistentSize;
    QString m_persistentUnits;

    QElapsedTimer m_timer;
    bool m_canceled = false;
    QProcess *m_activeProc = nullptr;
};

class HashWorker : public QObject
{
    Q_OBJECT

public:
    explicit HashWorker(const QString &filePath, QObject *parent = nullptr);

public slots:
    void compute();

signals:
    void finished(const QString &result);
    void logMessage(const QString &msg);

private:
    QString m_filePath;
};

#endif // WORKERS_H
