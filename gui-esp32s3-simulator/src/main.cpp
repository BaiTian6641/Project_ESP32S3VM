#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QPixmap>
#include <QDebug>
#include "MainWindow.h"
#include "ThemeManager.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("ESP32S3VM");
    app.setOrganizationName("ESP32S3VM");
    app.setStyle("Fusion");
    app.setFont(QFont("IBM Plex Sans", 10));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"firmware", "Boot a merged 4MB flash image.", "path"});
    parser.addOption({"screenshot", "Save the studio preview and exit.", "path"});
    parser.addOption({"theme", "Appearance: system, light or dark.", "mode"});
    parser.process(app);
    auto *theme = ThemeManager::instance();
    if (parser.isSet("theme")) {
        const QString mode = parser.value("theme").toLower();
        if (mode == "light") theme->setMode(ThemeManager::Mode::Light);
        else if (mode == "dark") theme->setMode(ThemeManager::Mode::Dark);
        else if (mode == "system") theme->setMode(ThemeManager::Mode::System);
        else { qCritical("--theme must be system, light or dark"); return 2; }
    }
    theme->apply(&app);
    MainWindow window;
    window.show();
    if (parser.isSet("firmware")) window.loadFirmware(parser.value("firmware"));
    if (parser.isSet("screenshot")) QTimer::singleShot(1500, &app, [&]() {
        app.exit(window.grab().save(parser.value("screenshot")) ? 0 : 1);
    });
    return app.exec();
}
