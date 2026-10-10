#include "TrackIcons.hpp"
#include "TrackIconPicker.hpp"
#include "Theme.hpp"
#include "ThemePackage.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "collaboration/CommandJson.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImageReader>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QComboBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TrackIconsTest : public QObject {
    Q_OBJECT
private slots:
    void libraryAndImport() {
        const auto entries = ui::trackicons::builtins();
        QVERIFY(entries.size() >= 64);
        QSet<QString> keys;
        QImage atlas(800, ((entries.size() + 7) / 8) * 88, QImage::Format_ARGB32_Premultiplied);
        atlas.fill(QColor(32, 32, 32));
        QPainter painter(&atlas);
        painter.setPen(Qt::white);
        QFont font = painter.font(); font.setPixelSize(10); painter.setFont(font);
        for (int i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            QVERIFY(!keys.contains(entry.id)); keys.insert(entry.id);
            const QIcon icon = ui::trackicons::icon(entry.id, QColor(230, 230, 230));
            QVERIFY2(!icon.isNull(), qPrintable(entry.id));
            const auto pixels = icon.pixmap(48, 48).toImage();
            int ink = 0;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x) ink += qAlpha(pixels.pixel(x, y)) > 16;
            QVERIFY2(ink > 20, qPrintable(entry.id));
            const int x = (i % 8) * 100, y = (i / 8) * 88;
            icon.paint(&painter, QRect(x + 30, y + 6, 40, 40));
            painter.drawText(QRect(x + 2, y + 49, 96, 36), Qt::AlignHCenter | Qt::TextWordWrap, entry.name);
        }
        painter.end();
        if (const QString shots = qEnvironmentVariable("VLT_TRACK_ICON_SCREENSHOTS"); !shots.isEmpty()) {
            QDir().mkpath(shots);
            QVERIFY(atlas.save(QDir(shots).filePath("catalog.png")));
        }
        QTemporaryDir source;
        QImage large(1600, 800, QImage::Format_ARGB32);
        large.fill(QColor(40, 180, 120, 150));
        const QString path = source.filePath("My icon.png");
        QVERIFY(large.save(path));
        const qint64 originalBytes = QFileInfo(path).size();
        QString error;
        const QString id = ui::trackicons::importFile(path, &error);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        const int count = ui::trackicons::customIcons().size();
        QCOMPARE(ui::trackicons::importFile(path, &error), id);
        QCOMPARE(ui::trackicons::customIcons().size(), count);
        const QString saved = ui::trackicons::customPath(id);
        QVERIFY(!saved.isEmpty());
        QVERIFY(QFileInfo(saved).size() < originalBytes);
        QImageReader reader(saved);
        QCOMPARE(reader.size(), QSize(96, 96));
        const QImage optimized = reader.read();
        reader.setDevice(nullptr);
        QCOMPARE(qAlpha(optimized.pixel(0, 0)), 0);
        QVERIFY(qAlpha(optimized.pixel(48, 48)) >= 140);
        QVERIFY(QFile::remove(path));
        QVERIFY(!ui::trackicons::icon(id, Qt::white).isNull());
        QVERIFY(ui::trackicons::icon("custom:" + QString(64, 'a'), Qt::white).isNull());
        QVERIFY(ui::trackicons::icon("builtin:future-instrument", Qt::white).isNull());
        QVERIFY(ui::trackicons::customPath("custom:../../file").isEmpty());
        QFile bad(source.filePath("broken.png"));
        QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("not an image"); bad.close();
        QVERIFY(ui::trackicons::importFile(bad.fileName(), &error).isEmpty());
        QCOMPARE(ui::trackicons::customIcons().size(), count);
        QFile enormous(source.filePath("large.svg"));
        QVERIFY(enormous.open(QIODevice::WriteOnly));
        enormous.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100000\" height=\"100000\"/>"); enormous.close();
        QVERIFY(ui::trackicons::importFile(enormous.fileName(), &error).isEmpty());

        ui::ThemePackageSnapshot snapshot;
        QVERIFY2(ui::ThemePackage::captureCurrent("Icons", snapshot, &error), qPrintable(error));
        const QString package = source.filePath("icons.vlttheme");
        const auto written = ui::ThemePackage::write(snapshot, package);
        QVERIFY2(written.ok, qPrintable(written.error));
        QVERIFY(ui::trackicons::removeCustom(id, &error));
        QVERIFY(ui::trackicons::icon(id, Qt::white).isNull());
        const auto installed = ui::ThemePackage::install(package);
        QVERIFY2(installed.ok, qPrintable(installed.error));
        const auto applied = ui::ThemePackage::apply(installed.filePath, installed.storageId);
        QVERIFY2(applied.ok, qPrintable(applied.error));
        QVERIFY(!ui::trackicons::icon(id, Qt::white).isNull());
        QCOMPARE(ui::trackicons::customIcons().last().id, id);
        QCOMPARE(ui::trackicons::customIcons().last().name, QString("My icon"));
        QVERIFY(!ui::trackicons::restoreCustom("custom:" + QString(64, 'b'), "bad", ui::trackicons::customPath(id), &error));
    }

    void projectAndUndo() {
        daw::EngineController controller{};
        QVERIFY(controller.initialize(48000, 256, false));
        const auto a = controller.addTrack(daw::TrackKind::Audio, "Drums");
        const auto b = controller.addTrack(daw::TrackKind::Midi, "Bass");
        QCOMPARE(controller.setTrackIcons({a, b}, "builtin:drum-kit"), daw::collab::SharedMutationResult::LocalFallback);
        QCOMPARE(controller.project().findTrack(a)->iconId, std::string("builtin:drum-kit"));
        controller.undo();
        QVERIFY(controller.project().findTrack(a)->iconId.empty());
        QVERIFY(controller.project().findTrack(b)->iconId.empty());
        controller.redo();
        QCOMPARE(controller.project().findTrack(b)->iconId, std::string("builtin:drum-kit"));
        const auto copy = controller.duplicateTrack(a);
        QVERIFY(!copy.empty());
        QCOMPARE(controller.project().findTrack(copy)->iconId, std::string("builtin:drum-kit"));
        const std::string absent = "custom:" + std::string(64, 'c');
        controller.setTrackIcons({b}, absent);
        std::string bytes;
        QVERIFY(daw::ProjectSerializer::serializeDocument(controller.project(), bytes));
        daw::ProjectModel loaded;
        QVERIFY(daw::ProjectSerializer::deserializeDocument(loaded, bytes));
        QCOMPARE(loaded.findTrack(b)->iconId, absent);
        QCOMPARE(loaded.findTrack(a)->iconId, std::string("builtin:drum-kit"));
        QVERIFY(ui::trackicons::icon(QString::fromStdString(absent), Qt::white).isNull());
        QCOMPARE(controller.setTrackIcons({a}, "../../bad.png"), daw::collab::SharedMutationResult::Blocked);
        QCOMPARE(controller.project().findTrack(a)->iconId, std::string("builtin:drum-kit"));

        using namespace daw::collab;
        SharedProjectDocument document;
        document.project = loaded;
        ProjectCommand command;
        command.meta.projectId = daw::newUuid();
        command.meta.actorId = daw::newUuid();
        command.meta.clientId = daw::newUuid();
        command.meta.operationId = daw::newUuid();
        command.meta.clientSequence = 1;
        command.body = SetTrackProperty{a, TrackProperty::IconId, std::string("builtin:piano")};
        const auto decoded = deserializeProjectCommand(serializeProjectCommand(command));
        QVERIFY(decoded.has_value());
        auto result = ProjectReducer::apply(document, *decoded);
        QVERIFY2(result.changed(), result.message.c_str());
        QVERIFY(!result.impact.graphRebuild);
        QCOMPARE(document.project.findTrack(a)->iconId, std::string("builtin:piano"));
        QVERIFY(result.inverse != nullptr);
        result.inverse->meta.operationId = daw::newUuid();
        result.inverse->meta.clientSequence = 2;
        QVERIFY(ProjectReducer::apply(document, *result.inverse).changed());
        QCOMPARE(document.project.findTrack(a)->iconId, std::string("builtin:drum-kit"));
    }

    void pickerKeyboardAndActions() {
        auto* picker = new TrackIconPicker("builtin:piano", ui::trackicons::icon("builtin:waveform", Qt::white));
        QSignalSpy selected(picker, &TrackIconPicker::iconSelected);
        picker->popup(QPoint(20, 20));
        QTest::qWait(40);
        auto* list = picker->findChild<QListWidget*>("TrackIconGrid");
        auto* search = picker->findChild<QLineEdit*>("TrackIconSearch");
        QVERIFY(list && search && list->count() >= 65);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QString("builtin:piano"));
        if (const QString shots = qEnvironmentVariable("VLT_TRACK_ICON_SCREENSHOTS"); !shots.isEmpty())
            QVERIFY(picker->grab().save(QDir(shots).filePath("picker-widgets.png")));
        QTest::keyClicks(search, "guitar");
        int visible = 0;
        for (int i = 0; i < list->count(); ++i) visible += !list->item(i)->isHidden();
        QCOMPARE(visible, 9); // Guitar/string category is searchable too.
        QTest::keyClick(search, Qt::Key_Down);
        QVERIFY(list->hasFocus());
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected[0][0].toString(), QString("builtin:acoustic-guitar"));

        picker = new TrackIconPicker("builtin:piano", {});
        QSignalSpy enter(picker, &TrackIconPicker::iconSelected);
        picker->popup(QPoint(20, 20));
        search = picker->findChild<QLineEdit*>("TrackIconSearch");
        QTest::keyClicks(search, "saxophone");
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(enter.size(), 1);
        QCOMPARE(enter[0][0].toString(), QString("builtin:saxophone"));

        picker = new TrackIconPicker("builtin:piano", {});
        QSignalSpy reset(picker, &TrackIconPicker::iconSelected);
        picker->popup(QPoint(20, 20));
        picker->findChild<QPushButton*>("TrackIconDefault")->click();
        QCOMPARE(reset.size(), 1); QVERIFY(reset[0][0].toString().isEmpty());
        picker = new TrackIconPicker({}, {});
        QSignalSpy add(picker, &TrackIconPicker::addIconRequested);
        picker->popup(QPoint(20, 20));
        picker->findChild<QPushButton*>("TrackIconAdd")->click();
        QCOMPARE(add.size(), 1);
        picker = new TrackIconPicker("builtin:piano", {});
        QSignalSpy cancelled(picker, &TrackIconPicker::iconSelected);
        picker->popup(QPoint(20, 20));
        QTest::keyClick(picker, Qt::Key_Escape);
        QCOMPARE(cancelled.size(), 0);
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QString::fromUtf8(DAW_TEST_FONT_PATH));
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    QCoreApplication::setOrganizationName("VLTTrackIconsTest");
    QCoreApplication::setApplicationName("TrackIcons");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());
    app.setProperty("dawHeadlessDataRoot", root.path());
    ThemeManager::instance().setThemeId("dark", false);
    ThemeManager::instance().apply();
    app.setFont(QFont(QStringLiteral("Inter"), 10));
    TrackIconsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "track_icons_test.moc"
