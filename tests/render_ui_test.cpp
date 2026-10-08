#include "EngineController.hpp"
#include "ExportDialog.hpp"
#include "BounceInPlaceDialog.hpp"
#include "ExportPrefs.hpp"
#include "PreviewLoader.hpp"
#include <QApplication>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QStyleOptionButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTabWidget>
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
    const auto settleWindow = [] {
        // Native window managers can asynchronously constrain a requested size.
        QEventLoop loop;
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();
    };
    QSettings().setValue(QStringLiteral("ui/themeId"),
                         QStringLiteral("logic"));
    ThemeManager& themes = ThemeManager::instance();
    themes.apply();
    QStringList presetIds;
    for (const Theme& theme : themes.presets()) presetIds.push_back(theme.id);
    check(presetIds == QStringList({QStringLiteral("dark"),
                                    QStringLiteral("studio-gray"),
                                    QStringLiteral("light"),
                                    QStringLiteral("solarized-light"),
                                    QStringLiteral("gruvbox")}),
          "supported built-in themes remain available");
    check(themes.themeId() == QStringLiteral("dark") &&
              QSettings().value(QStringLiteral("ui/themeId")).toString() ==
                  QStringLiteral("dark"),
          "a retired saved theme migrates to Dark");
    const auto source = temporary.filePath("source.wav");
    audio::platform::AudioFileWriter writer;
    check(bool(writer.open(source.toStdString(), 48000, 2, 480000)), "fixture opens");
    float samples[1024];
    std::fill_n(samples, 1024, 0.25f);
    const float* channels[] = {samples, samples};
    for (int i = 0; i < 469; ++i) writer.write(channels, 1024);
    writer.close();

    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    check(bool(controller.initialize(48000, 64, false)), "controller prepares");
    {
        daw::EngineController::BounceRequest selection;
        selection.tracks = {"first", "second", "third", "fourth"};
        selection.endSeconds = 1;
        BounceInPlaceDialog bounce(controller, selection, QStringLiteral("4 MIDI tracks"));
        auto* destination = bounce.findChild<QComboBox*>(QStringLiteral("BounceDestination"));
        auto* result = bounce.findChild<QLabel*>(QStringLiteral("BounceResultSummary"));
        check(destination && destination->count() == 1 && destination->currentData().toInt() ==
              int(daw::EngineController::BounceDestination::NewTrack) && result && !result->text().isEmpty(),
              "multi-track Bounce dialog clearly creates one mixed audio track");
        selection.tracks.resize(1);
        BounceInPlaceDialog single(controller, selection, QStringLiteral("1 track"));
        check(single.findChild<QComboBox*>(QStringLiteral("BounceDestination"))->count() == 2,
              "single-track Bounce still offers Replace and New Track");
    }
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
    settleWindow();
    check(dialog.findChildren<QTabWidget*>().isEmpty(), "render settings require no tabs");
    check(dialog.size().width() <= dialog.screen()->availableGeometry().width() - 80
          && dialog.size().height() <= dialog.screen()->availableGeometry().height() - 80,
          "initial render window leaves room around it on the current screen");
    dialog.resize(1100, 820);
    settleWindow();
    auto* renderScroll = dialog.findChild<QScrollArea*>();
    const QStringList sections{"ExportFormatSection", "ExportRangeSection",
        "ExportProcessingSection", "ExportOutputsSection", "ExportDetailsSection"};
    for (const QString& name : sections) {
        auto* section = dialog.findChild<QGroupBox*>(name);
        if (section && dialog.height() < 820) {
            renderScroll->ensureWidgetVisible(section, 0, 0);
            settleWindow();
        }
        check(section && section->isVisible()
              && renderScroll->viewport()->rect().contains(
                  QRect(section->mapTo(renderScroll->viewport(), QPoint()), section->size())),
              "render sections fit together or remain reachable on a short screen");
    }
    renderScroll->verticalScrollBar()->setValue(0);
    settleWindow();
    auto* stems = dialog.findChild<QCheckBox*>("ExportStems");
    auto* trackList = dialog.findChild<QListWidget*>("ExportChannels");
    auto* details = dialog.findChild<QGroupBox*>("ExportDetailsSection");
    const QRect detailsBefore = details->geometry();
    check(trackList->isVisible() && !trackList->isEnabled(),
          "stem choices remain visible when exporting only the master");
    stems->setChecked(true);
    settleWindow();
    if (details->geometry() != detailsBefore) {
        const QRect after = details->geometry();
        std::cerr << "Details before " << detailsBefore.x() << ',' << detailsBefore.y()
                  << ' ' << detailsBefore.width() << 'x' << detailsBefore.height()
                  << ", after " << after.x() << ',' << after.y()
                  << ' ' << after.width() << 'x' << after.height() << '\n';
    }
    check(trackList->isEnabled() && details->geometry() == detailsBefore,
          "enabling stems preserves the positions of adjacent settings");
    stems->setChecked(false);
    settleWindow();
    if (qEnvironmentVariableIsSet("DAW_RENDER_TEST_ARTIFACTS")) {
        dialog.grab().save(temporary.filePath("render-overview.png"));
    }
    const QFont originalFont = QApplication::font();
    QFont largeFont = originalFont;
    largeFont.setPixelSize(18);
    QApplication::setFont(largeFont);
    themes.fontChanged();
    dialog.resize(520, 560);
    QApplication::processEvents();
    check(renderScroll->widget()->width() <= renderScroll->viewport()->width(),
          "large text fits a narrow render window without horizontal clipping");
    renderScroll->ensureWidgetVisible(dialog.findChild<QLineEdit*>("ExportComment"));
    QApplication::processEvents();
    auto* commentField = dialog.findChild<QLineEdit*>("ExportComment");
    check(renderScroll->viewport()->rect().contains(
              QRect(commentField->mapTo(renderScroll->viewport(), QPoint()), commentField->size())),
          "large-text metadata remains reachable");
    if (qEnvironmentVariableIsSet("DAW_RENDER_TEST_ARTIFACTS"))
        dialog.grab().save(temporary.filePath("render-large-text.png"));
    QApplication::setFont(originalFont);
    themes.fontChanged();
    dialog.resize(1100, 820);
    QApplication::processEvents();
    auto* formatSection = dialog.findChild<QGroupBox*>("ExportFormatSection");
    auto* outputsSection = dialog.findChild<QGroupBox*>("ExportOutputsSection");
    check(outputsSection->mapTo(renderScroll->widget(), QPoint()).x()
              > formatSection->mapTo(renderScroll->widget(), QPoint()).x() + formatSection->width(),
          "render layout returns to two columns after expanding the window");
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
        const QString exportName = QString::fromUtf8("Ночной микс 日本語 العربية");
        completed.findChild<QLineEdit*>("ExportName")->setText(exportName);
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

        const QString exportedMp3 = project + "/" + exportName + ".mp3";
        QFile mp3File(exportedMp3);
        check(mp3File.open(QIODevice::ReadOnly),
              "MP3 preserves a multilingual export name exactly");
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
        check(bool(audio::platform::decodeAudioFile(exportedMp3.toStdString(), coveredAudio)),
              "multilingual MP3 path decodes");
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
        for (const QString& theme : presetIds) {
            ThemeManager::instance().setThemeId(theme, false);
            completed.resize(520, 560);
            QApplication::processEvents();
            const auto palette = completedButtons->button(QDialogButtonBox::Ok)->palette();
            const QBrush fill = palette.brush(QPalette::Button);
            double buttonContrast = contrast(palette.color(QPalette::ButtonText), fill.color());
            if (const auto* gradient = fill.gradient()) {
                buttonContrast = 21.0;
                for (const auto& stop : gradient->stops())
                    buttonContrast = std::min(buttonContrast,
                        contrast(palette.color(QPalette::ButtonText), stop.second));
            }
            check(buttonContrast >= 4.5,
                  "render button meets text contrast in every theme");

            // Render the shared stylesheet in every interaction state, including
            // gradient ends. Hover/pressed fills cannot be read from QPalette.
            const QStyle::State states[] = {
                QStyle::State_Enabled | QStyle::State_Raised,
                QStyle::State_Enabled | QStyle::State_Raised | QStyle::State_MouseOver,
                QStyle::State_Enabled | QStyle::State_Sunken,
                QStyle::State_Enabled | QStyle::State_On,
                QStyle::State_Enabled | QStyle::State_On | QStyle::State_MouseOver,
                QStyle::State_Enabled | QStyle::State_Raised | QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange
            };
            for (bool primary : {false, true}) {
                QPushButton probe(QStringLiteral("Control"));
                probe.setProperty("accentAction", primary);
                probe.resize(160, 36);
                probe.ensurePolished();
                for (const auto state : states) {
                    QStyleOptionButton option;
                    option.initFrom(&probe);
                    option.state = state;
                    option.text = probe.text();
                    QImage rendered(probe.size(), QImage::Format_ARGB32_Premultiplied);
                    rendered.fill(th().background);
                    QPainter painter(&rendered);
                    probe.style()->drawControl(QStyle::CE_PushButton, &option, &painter, &probe);
                    painter.end();
                    const QColor ink = probe.palette().color(QPalette::ButtonText);
                    const double ratio = std::min(contrast(ink, rendered.pixelColor(80, 2)),
                                                   contrast(ink, rendered.pixelColor(80, 33)));
                    if (ratio < 4.5)
                        std::cerr << "Contrast " << theme.toStdString() << " primary=" << primary
                                  << " state=" << int(state) << " ratio=" << ratio << '\n';
                    check(ratio >= 4.5, "shared button states meet normal-text contrast");
                }
            }
            for (auto* action : {completedButtons->button(QDialogButtonBox::Ok), openFolder})
                check(completed.rect().contains(QRect(action->mapTo(&completed, QPoint()), action->size())),
                      "completed actions remain inside a narrow window");
            auto* summary = completed.findChild<QLabel*>("ExportSummary");
            check(contrast(summary->palette().color(QPalette::WindowText), th().panelBottom()) >= 4.5,
                  "secondary text meets contrast in every theme");
            for (const QColor& background : {th().panelTop(), th().panelBottom(), th().wellTop(), th().wellBottom()})
                check(contrast(th().textSecondary, background) >= 4.5,
                      "secondary labels meet contrast at both ends of panel and field gradients");
            auto* renderScroll = completed.findChild<QScrollArea*>();
            check(renderScroll->widget()->width() <= renderScroll->viewport()->width(),
                  "narrow render form does not overflow horizontally");
            auto* format = completed.findChild<QGroupBox*>("ExportFormatSection");
            auto* outputs = completed.findChild<QGroupBox*>("ExportOutputsSection");
            check(outputs->mapTo(renderScroll->widget(), QPoint()).y()
                      >= format->mapTo(renderScroll->widget(), QPoint()).y() + format->height(),
                  "narrow render window stacks its settings into one column");
            renderScroll->verticalScrollBar()->setValue(renderScroll->verticalScrollBar()->maximum());
            QApplication::processEvents();
            auto* comment = completed.findChild<QLineEdit*>("ExportComment");
            check(renderScroll->viewport()->rect().contains(
                      QRect(comment->mapTo(renderScroll->viewport(), QPoint()), comment->size())),
                  "last metadata field remains reachable by scrolling");
            renderScroll->verticalScrollBar()->setValue(0);
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
