#include <QtTest/QtTest>
#include <QApplication>
#include <QComboBox>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QLabel>
#include <QLineF>
#include <QPalette>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QTextStream>
#include <QPushButton>
#include <QTableWidget>
#include "ThemeManager.h"
#include "BoardWorkspace.h"
#include "PeripheralManager.h"
#include "DisplayPanel.h"
#include "ProjectDocument.h"

class ThemeManagerTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        ThemeManager::instance()->apply(qobject_cast<QApplication *>(QCoreApplication::instance()));
    }

    void paletteAndSystemFallback() {
        auto *theme = ThemeManager::instance();
        QSignalSpy changed(theme, &ThemeManager::themeChanged);
        theme->setMode(ThemeManager::Mode::Dark);
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#161616"));
        QVERIFY(theme->tokens().dark);
        theme->setMode(ThemeManager::Mode::Light);
        QCOMPARE(qApp->palette().color(QPalette::Window), QColor("#f4f4f4"));
        QVERIFY(!theme->tokens().dark);
        QCOMPARE(changed.count(), 2);
        QVERIFY(!qApp->styleSheet().contains('@'));
        theme->setMode(ThemeManager::Mode::System);
        QVERIFY(!theme->appearanceReason().isEmpty());
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        QVERIFY(!theme->systemAppearanceSupported());
        QVERIFY(theme->appearanceReason().contains("cannot detect"));
#endif
    }

    void persistsAcrossProcessRestart() {
        ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
        QCOMPARE(QSettings().value("appearance/mode").toString(), QString("dark"));
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), {"--probe-saved-dark"});
        QVERIFY(reader.waitForFinished(5000));
        QCOMPARE(reader.exitStatus(), QProcess::NormalExit);
        QCOMPARE(reader.exitCode(), 0);
        QVERIFY(reader.readAllStandardOutput().contains("persisted-dark"));
    }

    void canvasKeepsItemsDocumentSelectionAndViewport() {
        const QString root = QStringLiteral(SIMULATOR_SOURCE_DIR);
        PeripheralManager manager;
        manager.setWorkspaceRoot(root);
        QVERIFY(manager.loadConfig(root + "/peripherals/peripherals.example.json"));
        const auto allRunning = [&manager]() {
            for (const auto &value : manager.devicesSnapshot())
                if (value.toObject().value("status").toString() != "running") return false;
            return !manager.devicesSnapshot().isEmpty();
        };
        QTRY_VERIFY_WITH_TIMEOUT(allRunning(), 5000);
        BoardWorkspace workspace;
        workspace.setManager(&manager);
        workspace.resize(1300, 800);
        workspace.show();
        QCoreApplication::processEvents();
        auto *view = workspace.findChild<QGraphicsView *>("circuitCanvas");
        auto *selected = workspace.findChild<QComboBox *>("selectedComponent");
        auto *inspect = workspace.findChild<QPushButton *>("inspectComponent");
        auto *pins = workspace.findChild<QTableWidget *>("connectionTable");
        QVERIFY(view);
        QVERIFY(selected);
        QVERIFY(inspect);
        QVERIFY(pins);
        selected->setCurrentIndex(1);
        const auto rowsFit = [pins]() {
            for (int row = 0; row < pins->rowCount(); ++row) {
                auto *combo = qobject_cast<QComboBox *>(pins->cellWidget(row, 1));
                if (!combo || pins->rowHeight(row) - 1 < combo->sizeHint().height()
                    || combo->height() < combo->minimumSizeHint().height()) return false;
            }
            return pins->rowCount() > 0;
        };
        QVERIFY(rowsFit());
        QCOMPARE(view->scene()->selectedItems().size(), 1);
        auto *card = view->scene()->selectedItems().first();
        card->moveBy(12, 24);
        const auto beforeInspect = workspace.circuitDocument();
        view->setFocus();
        const qreal fittedScale = view->transform().m11();
        QTest::keyClick(view, Qt::Key_Plus);
        // Zoom changes scrollbar visibility through posted layout events.
        // Let that user action settle before measuring a theme-only change.
        QCoreApplication::processEvents();
        QVERIFY(view->transform().m11() > fittedScale);
        QTest::mouseClick(inspect, Qt::LeftButton);
        QCoreApplication::processEvents();
        QVERIFY(view->transform().m11() >= 1.0);
        QVERIFY(QLineF(card->sceneBoundingRect().center(),
                       view->mapToScene(view->viewport()->rect().center())).length() < 3);
        QCOMPARE(workspace.circuitDocument(), beforeInspect);
        // The keyboard route returns to the same readable inspection view.
        view->setFocus();
        QTest::keyClick(view, Qt::Key_0);
        QCoreApplication::processEvents();
        QVERIFY(view->transform().m11() < 1.0);
        QTest::keyClick(view, Qt::Key_I);
        QCoreApplication::processEvents();
        QVERIFY(view->transform().m11() >= 1.0);
        const auto transform = view->transform();
        const auto center = view->mapToScene(view->viewport()->rect().center());
        const auto location = card->pos();
        const auto document = workspace.circuitDocument();
        const auto items = view->scene()->items();
        QSignalSpy runtimeChanged(&manager, &PeripheralManager::deviceSetChanged);
        ThemeManager::instance()->setMode(ThemeManager::Mode::Light);
        QCoreApplication::processEvents();
        QCOMPARE(view->scene()->backgroundBrush().color(), QColor("#f4f4f4"));
        QVERIFY(rowsFit());
        ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
        QCoreApplication::processEvents();
        QCOMPARE(view->scene()->backgroundBrush().color(), QColor("#161616"));
        QVERIFY(rowsFit());
        QCOMPARE(workspace.circuitDocument(), document);
        QCOMPARE(view->scene()->items(), items);
        QCOMPARE(view->scene()->selectedItems().first(), card);
        QCOMPARE(card->pos(), location);
        QCOMPARE(selected->currentIndex(), 1);
        QCOMPARE(view->transform(), transform);
        const auto after = view->mapToScene(view->viewport()->rect().center());
        QVERIFY(QLineF(center, after).length() < 3);
        QCOMPARE(runtimeChanged.count(), 0);
        workspace.resize(1220, 780);
        QCoreApplication::processEvents();
        QCOMPARE(view->transform(), transform);
        QVERIFY(QLineF(card->sceneBoundingRect().center(),
                       view->mapToScene(view->viewport()->rect().center())).length() < 3);
        const auto selectedId = selected->currentData().toString();
        QTest::keyClick(view, Qt::Key_Right);
        const auto moved = workspace.projectModel()->geometry().value("components").toObject().value(selectedId).toObject();
        QCOMPARE(QPointF(moved.value("x").toDouble(), moved.value("y").toDouble()), location + QPointF(6, 0));
        QCoreApplication::processEvents();
        card = view->scene()->selectedItems().first();
        QCOMPARE(card->pos(), location + QPointF(6, 0));
        // Canvas selection also identifies the component in the inspector.
        for (auto *item : view->scene()->items()) {
            if (item == card || !(item->flags() & QGraphicsItem::ItemIsSelectable)) continue;
            view->scene()->clearSelection();
            item->setSelected(true);
            QVERIFY(selected->currentIndex() != 1);
            break;
        }
    }

    void displayPixelsAndControlsSurviveThemeChange() {
        DisplayPanel display("screen", "ssd1306", QJsonObject{});
        QLabel *screen = nullptr;
        for (auto *label : display.findChildren<QLabel *>())
            if (label->property("role").toString() == "display") screen = label;
        QVERIFY(screen);
        const QImage blank = screen->pixmap().toImage();
        display.updateState(QJsonObject{{"display_on", true}, {"contrast", 210},
            {"frame_update", QJsonObject{{"encoding", "u8"}, {"layout", "page-major"},
                {"data", QJsonArray{0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa}}}}});
        QTRY_VERIFY(screen->pixmap().toImage() != blank);
        const QImage pixels = screen->pixmap().toImage();
        QVERIFY(!pixels.isNull());
        QSignalSpy writes(&display, &DevicePanelBase::parameterChangeRequested);
        display.updateStatus("running", QString());
        QLabel *status = nullptr;
        for (auto *label : display.findChildren<QLabel *>())
            if (label->property("tone").toString() == "success") status = label;
        QVERIFY(status);
        ThemeManager::instance()->setMode(ThemeManager::Mode::Light);
        status->ensurePolished();
        QCOMPARE(status->palette().color(QPalette::WindowText), ThemeManager::instance()->tokens().supportSuccess);
        QCOMPARE(screen->pixmap().toImage(), pixels);
        ThemeManager::instance()->setMode(ThemeManager::Mode::Dark);
        status->ensurePolished();
        QCOMPARE(status->palette().color(QPalette::WindowText), ThemeManager::instance()->tokens().supportSuccess);
        QCOMPARE(screen->pixmap().toImage(), pixels);
        QCOMPARE(writes.count(), 0);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName("ESP32S3VMThemeTests");
    app.setApplicationName("ThemeManagerTest");
    app.setStyle("Fusion");
    QTemporaryDir settings;
    if (qEnvironmentVariableIsEmpty("ESP32S3_THEME_TEST_SETTINGS"))
        qputenv("ESP32S3_THEME_TEST_SETTINGS", settings.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       qEnvironmentVariable("ESP32S3_THEME_TEST_SETTINGS"));
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--probe-saved-dark") {
        if (ThemeManager::instance()->mode() != ThemeManager::Mode::Dark) return 1;
        QTextStream(stdout) << "persisted-dark\n";
        return 0;
    }
    ThemeManagerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ThemeManagerTest.moc"
