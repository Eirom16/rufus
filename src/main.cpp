#include <QApplication>
#include <QProcess>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
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
