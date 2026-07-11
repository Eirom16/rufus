#include <QApplication>
#include <QProcess>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
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

    // Force Breeze style on KDE if available (looks native)
    if (QApplication::style()->name() != "breeze")
        QApplication::setStyle("breeze");

    // Check for root privileges
    if (geteuid() != 0) {
        // Try pkexec auto-elevation
        QString selfPath = QFileInfo("/proc/self/exe").symLinkTarget();
        if (selfPath.isEmpty())
            selfPath = QCoreApplication::applicationFilePath();

        // Check if we're already under pkexec
        if (qEnvironmentVariableIsSet("PKEXEC_UID")) {
            QMessageBox::critical(nullptr, "Privilege Escalation Failed",
                "Rufus Qt needs root privileges to write raw block devices.\n"
                "Please run the application as root (e.g. 'sudo rufus-qt').");
            return 1;
        }

        QProcess pkexec;
        pkexec.setProgram("pkexec");
        pkexec.setArguments({selfPath});
        if (pkexec.startDetached())
            return 0;

        QMessageBox::critical(nullptr, "Privilege Escalation Required",
            "Rufus Qt needs root privileges to write raw block devices.\n"
            "Please run the application as root (e.g. 'sudo rufus-qt' "
            "or 'pkexec rufus-qt').");
        return 1;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
