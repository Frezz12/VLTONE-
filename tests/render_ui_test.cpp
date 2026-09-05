#include "EngineController.hpp"
#include "ExportDialog.hpp"
#include "ExportPrefs.hpp"
#include "PreviewLoader.hpp"
#include <QApplication>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include "Theme.hpp"
#include <filesystem>
#include <iostream>
#include <cmath>

class FolderCapture : public QObject {
    Q_OBJECT
public:
    QUrl opened;
    int calls = 0;
public slots:
    void open(const QUrl& url) { opened = url; ++calls; }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("VLT-render-tests");
    QCoreApplication::setApplicationName("render-ui");
    QTemporaryDir temporary;
    if (qEnvironmentVariableIsSet("DAW_RENDER_TEST_ARTIFACTS")) {
        temporary.setAutoRemove(false);
        std::cout << "Fixtures: " << temporary.path().toStdString() << '\n';
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
    };
    const auto source = temporary.filePath("source.wav");
    audio::platform::AudioFileWriter writer;
    check(bool(writer.open(source.toStdString(), 48000, 2, 480000)), "fixture opens");
    float samples[1024];
    std::fill_n(samples, 1024, 0.25f);
    const float* channels[] = {samples, samples};
    for (int i = 0; i < 469; ++i) writer.write(channels, 1024);
    writer.close();

    daw::EngineController controller;
    check(bool(controller.initialize(48000, 64, false)), "controller prepares");
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Test");
    controller.importAudio(source.toStdString(), track, 0);
    daw::rendering::Spec spec;
    spec.outputDir = temporary.filePath("export").toStdString();
    ui::exportprefs::save(spec);
    ui::exportprefs::setLastFolder(QString::fromStdString(spec.outputDir));
    QWidget parent;
    ExportDialog dialog(controller, nullptr, &parent);
    check(QDir(dialog.findChild<QLineEdit*>("ExportFolder")->text()) ==
          QDir(QString::fromStdString(spec.outputDir)), "unsaved project uses fallback folder");
    parent.show();
    dialog.show();
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    auto* status = dialog.findChild<QLabel*>("ExportStatus");
    bool cancelled = false, enabled = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &dialog, [&] {
        if (!status || !status->text().startsWith("Rendering ")) return;
        timer.stop();
        enabled = buttons->button(QDialogButtonBox::Cancel)->isEnabled();
        buttons->button(QDialogButtonBox::Cancel)->click();
        cancelled = true;
    });
    timer.start(0);
    buttons->button(QDialogButtonBox::Ok)->click();
    timer.stop();
    check(cancelled && enabled, "Cancel remains enabled during the actual DSP pass");
    check(status && status->text().startsWith("Cancelled"), "dialog reports cancellation");
    check(std::filesystem::is_empty(spec.outputDir), "cancelled UI export publishes no partial files");
    check(dialog.findChild<QPushButton*>("ExportOpenFolder")->isHidden(),
          "cancelled export offers no completed file");
    dialog.close();

    // Exercise the actual cover -> spec -> writer -> completed-dialog path.
    audio::platform::WriteSpec mp3;
    mp3.container = audio::platform::Container::Mp3;
    mp3.encoding = audio::platform::Encoding::Mp3;
    if (audio::platform::isWriteSpecSupported(mp3, 2, 48000)) {
        const QString project = temporary.filePath(QString::fromUtf8("Проект"));
        QDir().mkpath(project);
        const QString cover = temporary.filePath("cover.png");
        QImage artwork(80, 80, QImage::Format_RGB32);
        artwork.fill(QColor(35, 126, 184));
        check(artwork.save(cover), "cover fixture saves");
        controller.setProjectMetadata("Test Artist", cover.toStdString());
        spec.file = mp3;
        spec.file.bitrateKbps = 128;
        ui::exportprefs::save(spec);
        ExportDialog completed(controller, nullptr, &parent, project);
        completed.show();
        auto* folder = completed.findChild<QLineEdit*>("ExportFolder");
        check(QDir(folder->text()) == QDir(project), "saved project overrides last export folder");
        completed.findChild<QLineEdit*>("ExportName")->setText("Covered");
        const QString fullTitle = QString::fromUtf8("Ночной город — полная версия без обрезки названия");
        completed.findChild<QLineEdit*>("ExportTitle")->setText(fullTitle);
        completed.findChild<QLineEdit*>("ExportAlbum")->setText("Test Album");
        auto* coverButton = completed.findChild<QPushButton*>("ExportCover");
        check(!coverButton->icon().isNull(), "project cover appears in preview");
        auto* container = completed.findChild<QComboBox*>("ExportContainer");
        container->setCurrentIndex(container->findData(int(audio::platform::Container::Wav)));
        check(!coverButton->isEnabled(), "artwork is unavailable for WAV");
        container->setCurrentIndex(container->findData(int(audio::platform::Container::Mp3)));
        check(coverButton->isEnabled() && !coverButton->icon().isNull(), "switching back to MP3 retains cover");
        if (qEnvironmentVariableIsSet("DAW_RENDER_TEST_ARTIFACTS")) {
            completed.resize(1020, 860);
            QApplication::processEvents();
            completed.grab().save(temporary.filePath("render-with-cover.png"));
        }
        FolderCapture opened;
        QDesktopServices::setUrlHandler("file", &opened, "open");
        completed.findChild<QCheckBox*>("ExportOpenAfterRender")->setChecked(true);
        auto* completedButtons = completed.findChild<QDialogButtonBox*>();
        completedButtons->button(QDialogButtonBox::Ok)->click();
        check(completed.findChild<QLabel*>("ExportStatus")->text().startsWith("Render complete"),
              "successful render shows completion");
        check(opened.calls == 1 && QDir(opened.opened.toLocalFile()) == QDir(project),
              "auto-open uses the rendered project's folder");
        auto* openFolder = completed.findChild<QPushButton*>("ExportOpenFolder");
        check(!openFolder->isHidden(), "completed render exposes folder button");
        openFolder->click();
        check(opened.calls == 2, "folder button opens the completed location");
        QDesktopServices::unsetUrlHandler("file");

        QFile mp3File(project + "/Covered.mp3");
        check(mp3File.open(QIODevice::ReadOnly), "MP3 exists in the project");
        const QByteArray bytes = mp3File.readAll();
        QFile imageFile(cover);
        imageFile.open(QIODevice::ReadOnly);
        const QByteArray original = imageFile.readAll();
        const int apic = bytes.indexOf("APIC");
        check(bytes.startsWith("ID3") && apic >= 10, "MP3 has an APIC frame in ID3");
        if (apic >= 10) {
            const QByteArray payload = QByteArray(1, '\0') + QByteArray("image/png")
                + QByteArray("\0\3\0", 3) + original;
            check(bytes.mid(apic + 10, payload.size()) == payload, "front cover embeds exact PNG bytes");
        }
        check(bytes.contains("TIT2") && bytes.contains("TPE1") && bytes.contains("TALB")
              && bytes.contains(fullTitle.toUtf8()) && bytes.contains("Test Artist") && bytes.contains("Test Album"),
              "cover preserves title, artist and album metadata");
        audio::platform::DecodedAudio coveredAudio;
        check(bool(audio::platform::decodeAudioFile((project + "/Covered.mp3").toStdString(), coveredAudio)),
              "covered MP3 decodes");
        completed.findChild<QPushButton*>("ExportRemoveCover")->click();
        completed.findChild<QCheckBox*>("ExportOpenAfterRender")->setChecked(false);
        completed.findChild<QLineEdit*>("ExportName")->setText("Plain");
        completedButtons->button(QDialogButtonBox::Ok)->click();
        audio::platform::DecodedAudio plainAudio;
        check(bool(audio::platform::decodeAudioFile((project + "/Plain.mp3").toStdString(), plainAudio))
              && coveredAudio.interleaved == plainAudio.interleaved,
              "adding artwork leaves decoded audio unchanged");
        QFile plainFile(project + "/Plain.mp3");
        plainFile.open(QIODevice::ReadOnly);
        check(!plainFile.readAll().contains("APIC"), "removing cover omits artwork from the next render");
        // Contrast is measured from the rendered widget palette, after QSS,
        // rather than from the theme's nominal colors.
        const auto contrast = [](const QColor& a, const QColor& b) {
            const auto luminance = [](const QColor& c) {
                const auto linear = [](double x) { return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
                return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
            };
            const double x = luminance(a), y = luminance(b);
            return (std::max(x, y) + .05) / (std::min(x, y) + .05);
        };
        for (const QString& theme : {QStringLiteral("logic"), QStringLiteral("logic-light")}) {
            ThemeManager::instance().setThemeId(theme, false);
            completed.resize(640, 760);
            QApplication::processEvents();
            const auto palette = completedButtons->button(QDialogButtonBox::Ok)->palette();
            check(contrast(palette.color(QPalette::ButtonText), palette.color(QPalette::Button)) >= 4.5,
                  "render button meets text contrast in both themes");
            for (auto* action : {completedButtons->button(QDialogButtonBox::Ok), openFolder})
                check(completed.rect().contains(QRect(action->mapTo(&completed, QPoint()), action->size())),
                      "completed actions remain inside a narrow window");
            auto* subtitle = completed.findChild<QLabel*>("ExportSubtitle");
            check(contrast(subtitle->palette().color(QPalette::WindowText), th().background) >= 4.5,
                  "secondary text meets contrast in both themes");
            auto* renderScroll = completed.findChild<QScrollArea*>();
            check(renderScroll->widget()->width() <= renderScroll->viewport()->width(),
                  "narrow render form does not overflow horizontally");
            if (qEnvironmentVariableIsSet("DAW_RENDER_TEST_ARTIFACTS"))
                completed.grab().save(temporary.filePath("render-narrow-" + theme + ".png"));
        }
        completedButtons->button(QDialogButtonBox::Cancel)->click();
        check(completed.result() == QDialog::Accepted, "closing completed render reports success");

        const QString jpeg = temporary.filePath("cover.jpg");
        check(artwork.save(jpeg), "JPEG fixture saves");
        QFile jpegFile(jpeg);
        jpegFile.open(QIODevice::ReadOnly);
        const QByteArray jpegData = jpegFile.readAll();
        audio::platform::AudioFileWriter jpegWriter;
        audio::platform::FileTags jpegTags;
        jpegTags.coverArt.assign(jpegData.begin(), jpegData.end());
        const QString jpegMp3 = temporary.filePath("jpeg.mp3");
        check(bool(jpegWriter.open(jpegMp3.toStdString(), mp3, 48000, 2)), "JPEG MP3 writer opens");
        check(bool(jpegWriter.setTags(jpegTags)) && bool(jpegWriter.write(channels, 1024))
              && bool(jpegWriter.close()), "JPEG cover exports");
        QFile jpegOutput(jpegMp3);
        jpegOutput.open(QIODevice::ReadOnly);
        const auto jpegBytes = jpegOutput.readAll();
        check(jpegBytes.contains(QByteArray("image/jpeg\0\3\0", 13) + jpegData), "JPEG embeds as front cover");
        audio::platform::AudioFileWriter invalidWriter;
        check(bool(invalidWriter.open(temporary.filePath("invalid.mp3").toStdString(), mp3, 48000, 2)),
              "invalid cover writer opens");
        jpegTags.coverArt = {1, 2, 3};
        check(!invalidWriter.setTags(jpegTags), "invalid cover data is rejected");
        invalidWriter.close();
    }

    PreviewLoader loader;
    QEventLoop loop;
    int loaded = 0, failed = 0;
    QObject::connect(&loader, &PreviewLoader::loaded, &loop,
        [&](const QString& path, auto audio, auto peaks) {
            ++loaded;
            check(path == source && audio && peaks.isValid(), "only the newest preview is delivered");
            loop.quit();
        });
    QObject::connect(&loader, &PreviewLoader::failed, &loop, [&](auto, auto) { ++failed; });
    for (int i = 0; i < 100; ++i)
        loader.request(i % 2 ? source : temporary.filePath("missing.wav"));
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(loaded == 1 && failed == 0, "preview requests coalesce without obsolete failures");
    loader.cancel();
    check(QThreadPool::globalInstance()->waitForDone(10000), "preview worker finishes");
    return failures ? 1 : 0;
}

#include "render_ui_test.moc"
