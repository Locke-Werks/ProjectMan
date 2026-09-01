#include "config.h"
#include "git.h"
#include "main_window.h"
#include "scanner.h"
#include "theme_qt.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ProjectMan"));
    QApplication::setOrganizationName(QStringLiteral("Locke Werks"));
    QApplication::setApplicationVersion(QStringLiteral(PM_VERSION_STRING));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/projectman.ico")));

    // Before any widget exists: the application font is set here.
    pm::gui::theme::loadFonts();
    app.setStyleSheet(pm::gui::theme::styleSheet());

    pm::ConfigStatus status = pm::ConfigStatus::Missing;
    std::string      detail;
    pm::Config       cfg = pm::Config::load(&status, &detail);

    if (status == pm::ConfigStatus::Unreadable) {
        // Defaults in memory, and the file is left exactly as it is. It is the
        // only copy of something a person typed.
        QMessageBox::warning(
            nullptr, QStringLiteral("ProjectMan"),
            QStringLiteral("%1\n\nUsing defaults. The file was not changed.")
                .arg(QString::fromStdString(detail)));
    } else if (status == pm::ConfigStatus::Missing) {
        cfg.save(nullptr);
    }

    if (!cfg.gitExe.empty())
        pm::git::setExePathOverride(cfg.gitExe);

    if (pm::git::exePath().empty()) {
        QMessageBox::critical(nullptr, QStringLiteral("ProjectMan"),
                              QStringLiteral("git.exe was not found on PATH. "
                                             "Set git_exe in the configuration file."));
        return 3;
    }

    pm::gui::MainWindow w(std::move(cfg));
    w.show();
    return app.exec();
}
