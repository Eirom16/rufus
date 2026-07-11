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
                        QObject *parent = nullptr);

public slots:
    void burn();

signals:
    void progressUpdated(double pct, const QString &status, const QString &speedEta);
    void logMessage(const QString &msg);
    void finished(const QString &result);

private:
    bool runCmd(const QString &program, const QStringList &args, const QString &logPrefix);
    void unmountDevice();
    bool wipeDevice();
    bool createPartitions();
    bool formatPartition();
    bool writeIsoImage();

    QString m_devicePath;
    QString m_isoPath;
    QString m_filesystem;
    QString m_partitionScheme;
    QString m_volumeLabel;
    bool m_writeIso;
    qint64 m_isoSize;

    QElapsedTimer m_timer;
    QByteArray m_ddBuf;
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
