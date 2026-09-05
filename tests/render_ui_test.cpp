#include "EngineController.hpp"
#include "ExportDialog.hpp"
#include "ExportPrefs.hpp"
#include "PreviewLoader.hpp"
#include <QApplication>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("VLT-render-tests");
    QCoreApplication::setApplicationName("render-ui");
    QTemporaryDir temporary;
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
    dialog.close();

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
