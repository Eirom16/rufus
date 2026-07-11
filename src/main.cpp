#include <QApplication>
#include <QProcess>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QTranslator>
#include <QSettings>
#include <unistd.h>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Rufus Qt");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Rufus");
    app.setWindowIcon(QIcon::fromTheme("drive-removable-media-usb",
        QIcon(":/icons/rufus-128.png")));

    // Load translation (user override > system locale)
    QTranslator translator;
    QSettings s("Rufus", "rufus-qt");
    QString langOverride = s.value("language", "").toString();
    if (!langOverride.isEmpty()) {
        if (translator.load("rufus-qt_" + langOverride, ":/i18n"))
            app.installTranslator(&translator);
    } else {
        if (translator.load(QLocale(), "rufus-qt", "_", ":/i18n"))
            app.installTranslator(&translator);
    }

    // Force Breeze style on KDE if available (looks native)
    if (QApplication::style()->name() != "breeze")
        QApplication::setStyle("breeze");

    // Check for root privileges
    if (geteuid() != 0) {
        QString selfPath = QFileInfo("/proc/self/exe").symLinkTarget();
        if (selfPath.isEmpty())
            selfPath = QCoreApplication::applicationFilePath();

        if (qEnvironmentVariableIsSet("PKEXEC_UID")) {
            QMessageBox::critical(nullptr,
                MainWindow::tr("Privilege Escalation Failed"),
                MainWindow::tr("Rufus Qt needs root privileges to write raw block devices.\n"
                "Please run the application as root (e.g. 'sudo rufus-qt')."));
            return 1;
        }

        QProcess pkexec;
        pkexec.setProgram("pkexec");
        pkexec.setArguments({selfPath});
        if (pkexec.startDetached())
            return 0;

        QMessageBox::critical(nullptr,
            MainWindow::tr("Privilege Escalation Required"),
            MainWindow::tr("Rufus Qt needs root privileges to write raw block devices.\n"
            "Please run the application as root (e.g. 'sudo rufus-qt' "
            "or 'pkexec rufus-qt')."));
        return 1;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
