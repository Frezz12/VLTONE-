#include "MainWindow.hpp"
#include "AccountService.hpp"
#include "AssetCache.hpp"
#include "CollaborationService.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationTransport.hpp"
#include "CollaborationTypes.hpp"
#include "CloudProjectClient.hpp"
#include "CloudProjectInviteDialog.hpp"
#include "CloudAssetTransferManager.hpp"
#include "CloudRecordingAssetCoordinator.hpp"
#include "CloudRecordingRecoveryUpload.hpp"
#include "CloudProjectAssetHydrator.hpp"
#include "CloudProjectCache.hpp"
#include "CloudProjectPublisher.hpp"
#include "CloudProjectSyncCoordinator.hpp"
#include "CloudSessionLifecycleController.hpp"
#include "EngineProjectProjectionAdapter.hpp"
#include "PresenceInputRouter.hpp"
#include "RecordingLeaseCoordinator.hpp"
#include "PianoRollWindow.hpp"
#include "AutomationEditorWindow.hpp"
#include "AudioPreferences.hpp"
#include "LocalizationManager.hpp"
#include "PromptService.hpp"
#include "StartupWindow.hpp"
#include "UpdateChecker.hpp"
#include "TelemetryClient.hpp"
#include "SettingsWindow.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QFile>
#include <QFont>
#include <QToolTip>
#include <QEvent>
#include <QEventLoop>
#include <QFileOpenEvent>
#include <QFileInfo>
#include <QWidget>
#include <QMouseEvent>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>

#include <cstdio>
#include <QMessageBox>
#include <QTimer>
#include <QThread>
#include <cstring>
#include <optional>
#include <cstdlib>
#include <atomic>
#include <functional>
#include <memory>

// A `--selftest` flag constructs the whole UI, pumps the event loop briefly,
// and exits 0. Combined with QT_QPA_PLATFORM=offscreen it gives a headless
// smoke test that the interface builds and wires up without crashing.
// `--theme <id>` forces a preset (handy for screenshots of each theme).
//
// `DAW_DEBUG_HOVER=1` installs a global event filter that logs every ToolTip
// event and any widget under the cursor — used to find the source of a popup
// that shows the time/date near the mouse.
class HoverDebugFilter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* ev) override {
        if (ev->type() == QEvent::ToolTip) {
            auto* me = static_cast<QMouseEvent*>(ev);
            QWidget* w = QApplication::widgetAt(me->globalPosition().toPoint());
            qInfo() << "[hover-debug] ToolTip event at" << me->globalPosition().toPoint()
                    << "under widget:" << (w ? w->objectName() : QString("<none>"))
                    << (w ? w->metaObject()->className() : QString())
                    << "| widget tooltip:" << (w ? w->toolTip() : QString());
        } else if (ev->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(ev);
            QWidget* w = QApplication::widgetAt(me->globalPosition().toPoint());
            if (w && !w->toolTip().isEmpty())
                qInfo() << "[hover-debug] hover widget:" << w->objectName()
                        << w->metaObject()->className()
                        << "tooltip:" << w->toolTip();
        }
        return false;
    }
};

/// macOS opens a document in an already-running application with a FileOpen
/// event rather than a new argv. Queue early events until MainWindow exists,
/// then route every delivery through the same guarded project-open path as the
/// File menu.
class ProjectOpenFilter : public QObject {
public:
    using Handler = std::function<void(const QString&)>;

    void setHandler(Handler handler) {
        m_handler = std::move(handler);
        const QStringList pending = std::move(m_pending);
        m_pending.clear();
        for (const QString& path : pending) deliver(path);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::FileOpen)
            return QObject::eventFilter(watched, event);
        auto* open = static_cast<QFileOpenEvent*>(event);
        QString path = open->file();
        if (path.isEmpty() && open->url().isLocalFile())
            path = open->url().toLocalFile();
        if (!path.isEmpty()) deliver(path);
        return true;
    }

private:
    void deliver(const QString& path) {
        if (m_handler) m_handler(path);
        else m_pending.push_back(path);
    }

    Handler m_handler;
    QStringList m_pending;
};

namespace {
std::atomic<bool> g_selftestQtFailure{false};
QtMessageHandler g_previousMessageHandler = nullptr;

void selftestMessageHandler(QtMsgType type, const QMessageLogContext& context,
                            const QString& message) {
    if ((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) &&
        (message.contains(QStringLiteral("QPainter::begin")) ||
         message.contains(QStringLiteral("QPainter::set")) ||
         message.contains(QStringLiteral("Painter not active")) ||
         message.contains(QStringLiteral("QString::arg")))) {
        g_selftestQtFailure.store(true, std::memory_order_relaxed);
    }
    if (g_previousMessageHandler) {
        g_previousMessageHandler(type, context, message);
    } else {
        const QByteArray text = message.toLocal8Bit();
        std::fprintf(stderr, "%s\n", text.constData());
    }
}

/// The smallest real Standard MIDI File: one track, two quarter notes at 96
/// ticks per quarter. Written by hand so the selftest's drop check needs no
/// fixture on disk and no writer in the shipping code.
bool writeDemoMidiFile(const QString& path) {
    const unsigned char bytes[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 20,
        0x00, 0x90, 60, 100,          // note on C
        0x60, 0x80, 60, 0x40,         // …96 ticks later, off
        0x00, 0x90, 64, 100,          // note on E
        0x60, 0x80, 64, 0x40,         // …and off
        0x00, 0xFF, 0x2F, 0x00,       // end of track
    };
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    return file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes)) ==
           qint64(sizeof(bytes));
}
} // namespace

/// Points QSettings at a private store for the rest of the process.
///
/// Seeding copies the user's real settings across first, so a headless run sees
/// exactly what they see and still cannot write back. An owned store is deleted
/// when the process ends; one named by `DAW_PREF_DIR` belongs to the caller.
class PreferenceSandbox {
public:
    void redirect(const QString& dir, bool seed, bool owned) {
        QDir().mkpath(dir);
        std::optional<QSettings> source;
        if (seed) source.emplace();          // the user's native store
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
        if (source) {
            QSettings target;
            for (const QString& key : source->allKeys()) {
                target.setValue(key, source->value(key));
            }
            target.sync();
        }
        m_dir = owned ? dir : QString();
    }

    ~PreferenceSandbox() {
        if (m_dir.isEmpty()) return;
        QDir(m_dir).removeRecursively();
    }

private:
    QString m_dir;
};

int main(int argc, char** argv) {
    bool selftest = false;
    bool collaborationSelftest = false;
    bool updateSelftest = false;
    // Deliberately faults after writing a known project into the recovery
    // journal, so the recovery path can be verified against a real crash
    // rather than a simulated one. Needs DAW_RECOVERY_ROOT.
    bool crashtest = false;
    /// The other half of --crashtest: recovers what the fault left behind and
    /// reports what came back.
    bool recovercheck = false;
    const char* screenshotPath = nullptr;
    const char* themeId = nullptr;
    const char* languageLocale = nullptr;
    QString projectArgument;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;
        else if (std::strcmp(argv[i], "--collaboration-selftest") == 0)
            collaborationSelftest = true;
        else if (std::strcmp(argv[i], "--update-selftest") == 0)
            updateSelftest = true;
        else if (std::strcmp(argv[i], "--crashtest") == 0) crashtest = true;
        else if (std::strcmp(argv[i], "--recovercheck") == 0) recovercheck = true;
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotPath = argv[++i];
        else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc)
            themeId = argv[++i];
        else if (std::strcmp(argv[i], "--language") == 0 && i + 1 < argc)
            languageLocale = argv[++i];
        else if (argv[i][0] != '-' && projectArgument.isEmpty())
            projectArgument = QString::fromLocal8Bit(argv[i]);
    }
    if (updateSelftest) {
        const QUrl windowsUrl = UpdateChecker::latestReleaseUrlForTest(
            QStringLiteral("https://example.invalid/api/v1"),
            QStringLiteral("windows"), QStringLiteral("ru"));
        const QUrl macosUrl = UpdateChecker::latestReleaseUrlForTest(
            QStringLiteral("https://example.invalid/api/v1"),
            QStringLiteral("macos"), QStringLiteral("en"));
        const QUrl linuxUrl = UpdateChecker::latestReleaseUrlForTest(
            QStringLiteral("https://example.invalid/api/v1"),
            QStringLiteral("linux"), QStringLiteral("en"));
        if (!UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1.2"), QStringLiteral("0.1.1")) ||
            UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1.1"), QStringLiteral("0.1.1")) ||
            UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1.0"), QStringLiteral("0.1.1")) ||
            UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1-beta"), QStringLiteral("0.1.1")) ||
            windowsUrl.query() != QStringLiteral("platform=windows&locale=ru") ||
            macosUrl.query() != QStringLiteral("platform=macos&locale=en") ||
            linuxUrl.query() != QStringLiteral("platform=linux&locale=en")) {
            std::fprintf(stderr, "update selftest failed\n");
            return 36;
        }
        return 0;
    }
    const bool headless = selftest || collaborationSelftest || screenshotPath ||
                          crashtest || recovercheck;
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS")) {
        QByteArray chromiumFlags;
#ifdef Q_OS_MACOS
        // This Qt WebEngine build advertises WebGPU on macOS, but Chromium
        // cannot create the provider used by sites such as Twitch. They fall
        // back successfully, after emitting GPU/process errors. Disable only
        // WebGPU; ordinary accelerated compositing and video remain enabled.
        chromiumFlags = QByteArrayLiteral("--disable-features=WebGPU");
#endif
        if (headless) {
            // Chromium's GPU process has no display in the offscreen selftest
            // and screenshot path.
            if (!chromiumFlags.isEmpty()) chromiumFlags.append(' ');
            chromiumFlags.append(QByteArrayLiteral("--disable-gpu"));
        }
        if (!chromiumFlags.isEmpty())
            qputenv("QTWEBENGINE_CHROMIUM_FLAGS", chromiumFlags);
    }

    // A plugin's own GUI is a foreign NSView/HWND embedded in one of our
    // widgets, which forces that widget — and every widget above it — to be
    // native. By default Qt also turns every *sibling* of each of those
    // widgets native, and creates those views afterwards, so they end up
    // stacked above the editor that asked for a native window in the first
    // place: the plugin's view never reaches the screen and its title bar
    // never sees a click. Siblings stay ordinary widgets instead.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication app(argc, argv);

#if defined(Q_OS_WIN)
    // The narrow CRT argv follows the active ANSI code page. Qt reconstructs
    // arguments from GetCommandLineW on Windows, so use its list for the file
    // association / positional project path instead of losing characters
    // before the project serializer ever sees them.
    projectArgument.clear();
    const QStringList nativeArguments = QCoreApplication::arguments();
    for (int i = 1; i < nativeArguments.size(); ++i) {
        const QString& argument = nativeArguments.at(i);
        if ((argument == QLatin1String("--screenshot") ||
             argument == QLatin1String("--theme") ||
             argument == QLatin1String("--language")) &&
            i + 1 < nativeArguments.size()) {
            ++i;
            continue;
        }
        if (!argument.startsWith(QLatin1Char('-')) &&
            projectArgument.isEmpty()) {
            projectArgument = argument;
        }
    }
#endif

    // Qt calls qFatal() — an abort, with a crash report — the first time a
    // window is created with no screen attached. That happens for real: a
    // launch while the display is asleep or the session is not owned by this
    // user gets a platform plugin with an empty screen list. Saying so and
    // leaving is not a fix for the situation, but it is the difference
    // between a message and a crash log.
    if (!QGuiApplication::primaryScreen()) {
        std::fprintf(stderr,
                     "%s: no display is available (no screen attached to this "
                     "session). Nothing can be drawn, so the program is "
                     "stopping instead of crashing.\n",
                     VLT_STUDIO_PRO_NAME);
        return 1;
    }

    ProjectOpenFilter projectOpenFilter;
    app.installEventFilter(&projectOpenFilter);
    QApplication::setApplicationName(QStringLiteral(VLT_STUDIO_PRO_NAME));
    QApplication::setApplicationDisplayName(QStringLiteral(VLT_STUDIO_PRO_NAME));
    QApplication::setOrganizationName(QStringLiteral("VLT Studio"));
    // Recorded in every recovery session, so a leftover file says which build
    // wrote it — the first thing worth knowing about a crash report.
    QApplication::setApplicationVersion(QStringLiteral(VLT_STUDIO_PRO_VERSION));
#ifdef Q_OS_MACOS
    // The generic family exposed by the offscreen Cocoa path is "Sans Serif",
    // which is not a real macOS family and forces Qt to populate its complete
    // alias table on first use. Select the native system face explicitly.
    QApplication::setFont(QFont(QStringLiteral("Helvetica Neue")));
#endif
    if (selftest) {
        g_previousMessageHandler = qInstallMessageHandler(selftestMessageHandler);
    }

    // A headless run must not be able to change the user's settings. It reads
    // them — a screenshot is only worth anything in the palette and with the
    // folders the user actually has — but every write lands in a throwaway
    // copy that is deleted on the way out.
    //
    // This is not hypothetical tidiness. A test run left the user's theme
    // switched: on macOS the preferences live behind cfprefsd, so an app
    // instance that read the file, was outlived by an edit, and then wrote one
    // unrelated key flushed its whole stale cache back over it. The only fix
    // that holds is for the automated run to own a different file.
    //
    // `DAW_PREF_DIR` names the store explicitly (and is then used as-is, empty
    // or not); without it a headless run gets its own seeded copy.
    PreferenceSandbox prefs;
    QString headlessPrefsDir;
    if (const char* dir = std::getenv("DAW_PREF_DIR")) {
        headlessPrefsDir = QString::fromUtf8(dir);
        prefs.redirect(headlessPrefsDir, /*seed=*/false, /*owned=*/false);
    } else if (headless) {
        headlessPrefsDir =
            QDir::tempPath() +
            QStringLiteral("/daw-prefs-%1")
                .arg(QCoreApplication::applicationPid());
        prefs.redirect(headlessPrefsDir, /*seed=*/true, /*owned=*/true);
    }
    if (headless) {
        if (headlessPrefsDir.isEmpty()) {
            headlessPrefsDir =
                QDir::tempPath() +
                QStringLiteral("/daw-web-%1")
                    .arg(QCoreApplication::applicationPid());
        }
        app.setProperty("dawHeadlessDataRoot",
                        QDir(headlessPrefsDir).filePath("web-runtime"));
        if (!qEnvironmentVariableIsSet("DAW_PRESET_ROOT")) {
            qputenv("DAW_PRESET_ROOT",
                    QDir(headlessPrefsDir).filePath("presets").toUtf8());
        }
    }

    if (std::getenv("DAW_DEBUG_HOVER")) {
        static HoverDebugFilter filter;
        QApplication::instance()->installEventFilter(&filter);
    }

    ui::LocalizationManager::instance().initialize();
    if (languageLocale) {
        QString languageError;
        if (!ui::LocalizationManager::instance().activateLanguage(
                QString::fromLatin1(languageLocale), false, &languageError)) {
            std::fprintf(stderr, "language override failed: %s\n",
                         languageError.toUtf8().constData());
            if (headless) return 34;
        }
    }
    if (selftest) {
        QString localizationError;
        if (!ui::LocalizationManager::instance().checkJsonPackForTest(
                &localizationError)) {
            std::fprintf(stderr, "localization pack check failed: %s\n",
                         localizationError.toUtf8().constData());
            return 33;
        }
    }

    ThemeManager::instance().apply();
    if (themeId)
        ThemeManager::instance().setThemeId(QString::fromUtf8(themeId),
                                            /*persist=*/false);
    if (selftest) {
        QString fontError;
        if (!ThemeManager::instance().checkFontForTest(&fontError)) {
            std::fprintf(stderr, "custom font check failed: %s\n",
                         fontError.toUtf8().constData());
            return 35;
        }
    }
    if (selftest && !ui::checkAudioPreferencesRoundTripForTest()) {
        std::fprintf(stderr, "audio preferences did not round-trip\n");
        return 32;
    }
    if (selftest || collaborationSelftest) {
        QString collaborationError;
        if (!collab::checkCollaborationProtocolForTest(&collaborationError)) {
            std::fprintf(stderr, "collaboration protocol check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 37;
        }
        if (!collab::checkCollaborationPresenceSafetyForTest(
                &collaborationError)) {
            std::fprintf(stderr, "collaboration privacy check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 39;
        }
        if (!collab::PresenceInputRouter::checkSafetyForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "collaboration input privacy check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 41;
        }
        if (!collab::checkCollaborationPresenceGeometryForTest(
                &collaborationError) ||
            !PianoRollView::checkCollaborationPresenceForTest(
                &collaborationError) ||
            !AutomationCurveView::checkCollaborationPresenceForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "collaboration surface geometry check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 40;
        }
        QString assetCacheError;
        if (!collab::checkAssetCacheForTest(&assetCacheError)) {
            std::fprintf(stderr, "asset cache check failed: %s\n",
                         assetCacheError.toUtf8().constData());
            return 38;
        }
        if (!collab::checkEngineProjectProjectionForTest(
                &collaborationError)) {
            std::fprintf(stderr, "engine projection check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 49;
        }
        if (!collab::checkCollaborationTransportForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "collaboration transport check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 42;
        }
        if (!collab::checkCollaborationCommandBridgeForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "collaboration command bridge check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 43;
        }
        if (!collab::checkCloudProjectClientForTest(&collaborationError)) {
            std::fprintf(stderr,
                         "cloud project client check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 44;
        }
        if (!collab::checkCloudProjectInviteDialogForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "cloud project invite dialog check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 51;
        }
        if (!collab::checkRecordingLeaseCoordinatorForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "recording lease coordinator check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 52;
        }
        if (!collab::checkCloudRecordingAssetCoordinatorForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "cloud recording asset coordinator check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 53;
        }
        if (!collab::checkCloudRecordingRecoveryUploadForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "cloud recording recovery upload check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 54;
        }
        if (!collab::checkCloudAssetTransferManagerForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "cloud asset transfer manager check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 45;
        }
        if (!collab::checkCloudProjectSyncCoordinatorForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "cloud project sync coordinator check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 46;
        }
        if (!collab::checkCloudSessionLifecycleControllerForTest(
                &collaborationError)) {
            std::fprintf(stderr,
                         "cloud session lifecycle check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 50;
        }
        if (!collab::checkCloudProjectCacheForTest(&collaborationError)) {
            std::fprintf(stderr, "cloud project cache check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 47;
        }
        if (!collab::checkCloudProjectAssetHydratorForTest(
                &collaborationError)) {
            std::fprintf(stderr, "cloud project hydrator check failed: %s\n",
                         collaborationError.toUtf8().constData());
            return 48;
        }
    }
    if (collaborationSelftest)
        return 0;

    // Authentication is a hard startup gate. The branded startup window first
    // restores a saved entitlement silently; the login form is revealed only
    // when that restore really cannot continue.
    account::Service accountService;
    // The assistant's instructions are served, not compiled in: this restores
    // the cached pack straight away and asks for a newer one once there is a
    // session to ask with. See app/PromptService.hpp.
    ui::PromptService promptService;
    QObject::connect(&accountService, &account::Service::authenticatedChanged,
                     &promptService, [&promptService](bool authenticated) {
                         if (authenticated) promptService.refresh();
                     });
    promptService.start();
    if (selftest) {
        const QUrl apiBase(accountService.apiOrigin());
        if (!apiBase.isValid() || apiBase.host().isEmpty() ||
            (apiBase.scheme() != QLatin1String("http") &&
             apiBase.scheme() != QLatin1String("https")) ||
            !apiBase.path().endsWith(QLatin1String("/v1"))) {
            std::fprintf(stderr, "the account API base URL is invalid: %s\n",
                         accountService.apiOrigin().toUtf8().constData());
            return 30;
        }
        if (!UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1.2"), QStringLiteral("0.1.1")) ||
            UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1.1"), QStringLiteral("0.1.1")) ||
            UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1.0"), QStringLiteral("0.1.1")) ||
            UpdateChecker::isNewerVersionForTest(QStringLiteral("0.1-beta"), QStringLiteral("0.1.1"))) {
            std::fprintf(stderr, "update version comparison failed\n");
            return 36;
        }
    }
    std::unique_ptr<StartupWindow> startup;
    if (headless) {
        // Automated UI checks use an in-process account state. This is reached
        // only by the existing test/crash/screenshot harnesses; there is no
        // shipping command-line switch that bypasses authentication.
        accountService.installHeadlessTestSession();
        if (selftest) {
            StartupWindow startupCheck(&accountService);
            if (!startupCheck.checkForTest()) {
                std::fprintf(stderr, "the startup window hierarchy is incomplete\n");
                return 31;
            }
        }
    } else {
        startup = std::make_unique<StartupWindow>(&accountService);
        if (!startup->runAuthentication()) return 0;
        startup->showSystemLoading();
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    // Headless visual QA for the startup hierarchy. It intentionally stops
    // before MainWindow so the captured frame is exactly what a user sees
    // while new plugins are being inspected.
    if (screenshotPath && std::getenv("DAW_SHOT_STARTUP")) {
        StartupWindow startupShot(&accountService);
        startupShot.showPluginScan(
            3, 8, QStringLiteral("/Library/Audio/Plug-Ins/VST3/Example.vst3"));
        startupShot.show();
        QTimer::singleShot(250, &app, [&startupShot, screenshotPath] {
            startupShot.grab().save(QString::fromUtf8(screenshotPath));
            QApplication::quit();
        });
        return app.exec();
    }

    // In headless (selftest/screenshot) mode don't grab a real audio device.
#ifdef DAW_ENABLE_COLLABORATION
    collab::CollaborationService collaborationService(&accountService);
    collab::CollaborationTransport collaborationTransport(
        &accountService, &collaborationService);
    collab::AssetCache collaborationAssetCache;
    collab::CloudProjectClient cloudProjectClient(&accountService);
    collab::RecordingLeaseCoordinator recordingLeases(&cloudProjectClient);
    QObject::connect(
        &accountService, &account::Service::authenticatedChanged,
        &recordingLeases, [&recordingLeases](bool authenticated) {
            if (!authenticated) recordingLeases.handleLogout();
        });
    collab::CloudAssetTransferManager cloudAssetTransfers(
        &accountService, &collaborationAssetCache);
    collab::CloudProjectAssetHydrator cloudAssetHydrator(
        &cloudAssetTransfers, &collaborationAssetCache);
    collab::CloudProjectPublisher cloudProjectPublisher(
        &cloudProjectClient, &cloudAssetTransfers);
    daw::collab::CommandGateway collaborationCommands;
    collab::CollaborationCommandBridge collaborationCommandBridge(
        &collaborationService, &collaborationCommands);
    collab::CloudProjectSyncCoordinator cloudProjectSync(
        &cloudProjectClient, &cloudAssetTransfers,
        &collaborationCommandBridge, &collaborationService);
    collab::CloudSessionLifecycleController cloudSessionLifecycle(
        &cloudProjectClient, &cloudProjectSync, &collaborationService);
    QObject::connect(
        &cloudProjectSync,
        &collab::CloudProjectSyncCoordinator::projectAssetsDiscovered,
        &cloudAssetHydrator,
        [&cloudAssetHydrator](const QString& projectId,
                              const QList<daw::AssetRef>& assets) {
            cloudAssetHydrator.hydrate(projectId, assets);
        });
    MainWindow window(/*openDevice=*/!headless, nullptr,
                      &collaborationService);
    window.setCloudPublicationServices(&cloudProjectPublisher,
                                       &cloudProjectSync,
                                       &cloudSessionLifecycle,
                                       &collaborationCommandBridge,
                                       &cloudProjectClient,
                                       &recordingLeases);
    collab::EngineProjectProjectionAdapter collaborationProjection(
        window.collaborationEngineController(), &collaborationAssetCache);
    collaborationCommands.setAdapter(&collaborationProjection);
    struct ProjectionAdapterDetach final {
        daw::collab::CommandGateway* gateway = nullptr;
        ~ProjectionAdapterDetach() {
            if (gateway) gateway->setAdapter(nullptr);
        }
    } projectionAdapterDetach{&collaborationCommands};
    window.collaborationEngineController()->attachSharedMutationSink(
        collaborationCommandBridge);
    struct SharedMutationSinkDetach final {
        daw::EngineController* controller = nullptr;
        const daw::collab::SharedMutationSink* sink = nullptr;
        ~SharedMutationSinkDetach() {
            if (controller && sink)
                controller->detachSharedMutationSink(*sink);
        }
    } sharedMutationSinkDetach{window.collaborationEngineController(),
                               &collaborationCommandBridge};
    QObject::connect(
        &collaborationProjection,
        &collab::EngineProjectProjectionAdapter::projectionSucceeded,
        &window, &MainWindow::refreshAfterCollaborationProjection);
    QObject::connect(
        &collaborationProjection,
        &collab::EngineProjectProjectionAdapter::projectionFailed,
        &collaborationCommandBridge,
        &collab::CollaborationCommandBridge::handleProjectionFailure,
        Qt::QueuedConnection);
#else
    MainWindow window(/*openDevice=*/!headless);
#endif
    if (startup) {
        daw::PluginManager& plugins = window.pluginManagerForStartup();
        startup->showPluginScan(0, 0, QString());
        plugins.startScan(/*rescanAll=*/false);
        while (plugins.isScanning()) {
            startup->showPluginScan(
                plugins.scanned(), plugins.scanTotal(),
                QString::fromStdString(plugins.currentScanPath()));
            QApplication::processEvents(QEventLoop::AllEvents, 25);
            if (startup->cancelled()) {
                plugins.cancelScan();
                plugins.waitForScan();
                return 0;
            }
            QThread::msleep(12);
        }
        plugins.waitForScan();
        window.applyStartupPluginScanResults();
        startup->showReady(int(plugins.plugins().size()));
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // A screenshot gets a taller canvas: the mixer overlay claims the bottom of
    // the arrangement, and at 900 px there is not enough left to show an
    // expanded comp editor. Nothing about the layout changes, only the room.
    // DAW_SHOT_SIZE=WxH grabs at a chosen window size: most layout bugs only
    // show up on a laptop screen, where the arrangement is short.
    int shotWidth = 1440;
    int shotHeight = screenshotPath ? 1200 : 900;
    if (const char* size = std::getenv("DAW_SHOT_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 400 && h > 300) {
            shotWidth = w;
            shotHeight = h;
        }
    }
    window.resize(shotWidth, shotHeight);
    window.show();
    const QString localizationWarning =
        ui::LocalizationManager::instance().takeStartupWarning();
    if (!headless && !localizationWarning.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, localizationWarning] {
            QMessageBox::warning(&window, QObject::tr("Language unavailable"),
                                 localizationWarning);
        });
    }
    if (startup) {
        startup->accept();
        startup.reset();
    }

    // No reporter in headless checks. Live sessions enqueue immediately and
    // let the separate process own retries and post-crash delivery.
    std::unique_ptr<TelemetryClient> telemetry;
    if (!headless) telemetry = std::make_unique<TelemetryClient>(&window);

    std::unique_ptr<UpdateChecker> updateChecker;
    if (!headless) {
        updateChecker = std::make_unique<UpdateChecker>(&accountService);
        QTimer::singleShot(0, updateChecker.get(),
                           [&window, checker = updateChecker.get()] {
                               checker->start(&window);
                           });
    }

    projectOpenFilter.setHandler(
        [&window](const QString& path) { window.openProjectPath(path); });
    if (!headless && !projectArgument.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, projectArgument] {
            window.openProjectPath(projectArgument);
        });
    }

    // DAW_SHOT_PLUGIN_EDITOR names a scanned plugin (substring match): it is
    // loaded onto the first track and its editor opened. The only way to check
    // a natively embedded plugin GUI without a person sitting at the screen,
    // and with --selftest it is a crash check for open-and-tear-down.
    const char* shotEditor = std::getenv("DAW_SHOT_PLUGIN_EDITOR");
    bool shootEditor = false;

    if (screenshotPath) {
        window.populateDemo();
        if (std::getenv("DAW_SHOT_REDUCE_MOTION"))
            QSettings().setValue(QStringLiteral("ui/reduceMotion"), true);
        // Engaged, not rolling: the state the recording options belong to.
        if (std::getenv("DAW_SHOT_RECORD")) window.engageRecord(true);
        // DAW_SHOT_TRACKS adds this many extra tracks, so a grab shows what a
        // real session's worth of them looks like — and whether the arrangement
        // scrolls instead of growing the window.
        // DAW_SHOT_MIXER=off hides the mixer; a number sets its height.
        if (const char* mixer = std::getenv("DAW_SHOT_MIXER")) {
            if (std::strcmp(mixer, "off") == 0) window.setMixerShownForShot(false);
            else window.setMixerHeightForShot(std::atoi(mixer));
        }
        if (const char* many = std::getenv("DAW_SHOT_TRACKS"))
            window.addDemoTracks(std::atoi(many));
        // DAW_SHOT_NEST=n buries a track n folders deep, which is the only way
        // to see a header row give up its pan and then its fader for width.
        if (const char* nest = std::getenv("DAW_SHOT_NEST"))
            window.nestDemoTracksForShot(std::atoi(nest));
        // DAW_SHOT_SELECT names tracks to select, comma-separated — the only
        // way a grab can show a multi-track selection, or the colour a
        // non-blue track is washed with.
        if (const char* select = std::getenv("DAW_SHOT_SELECT"))
            window.selectTracksForShot(QString::fromUtf8(select));
        // DAW_SHOT_AUTOMATION gives the demo's first audio track a volume
        // curve — the only way a grab can show an automation lane, since none
        // exists until something asks for one.
        if (std::getenv("DAW_SHOT_AUTOMATION")) window.stageAutomationForShot();
        // DAW_SHOT_AUTOMATION_EDITOR does the same and then opens the curve's
        // own editor, which is a separate window and so is grabbed instead of
        // the shell.
        const bool shootAutomation =
            std::getenv("DAW_SHOT_AUTOMATION_EDITOR") != nullptr;
        if (shootAutomation)
            window.openAutomationEditorForShot(
                QString::fromUtf8(std::getenv("DAW_SHOT_AUTOMATION_EDITOR")));
        // DAW_SHOT_CYCLE="from,to" arms a cycle region over those seconds.
        if (const char* cycle = std::getenv("DAW_SHOT_CYCLE")) {
            const QStringList parts = QString::fromUtf8(cycle).split(',');
            if (parts.size() >= 2) {
                // A third field of "off" shows the region defined but not yet
                // armed — the state a drag leaves it in, before C.
                window.setCycleForShot(parts.at(0).toDouble(),
                                       parts.at(1).toDouble(),
                                       parts.size() < 3 ||
                                           parts.at(2) != QLatin1String("off"));
            }
        }
        // …and the same after the window has settled, which is the state a user
        // adds a track from. Layout bugs that only bite the second time round
        // are invisible to the eager path above.
        if (const char* late = std::getenv("DAW_SHOT_TRACKS_LATE")) {
            const int count = std::atoi(late);
            QTimer::singleShot(300, &window,
                               [&window, count] { window.addDemoTracks(count); });
        }
        // DAW_SHOT_SCROLL scrolls the arrangement down by that many pixels
        // once the window has settled — a grab cannot turn a wheel.
        if (const char* by = std::getenv("DAW_SHOT_SCROLL")) {
            const int px = std::atoi(by);
            QTimer::singleShot(200, &window, [&window, px] {
                window.scrollArrangement(px);
            });
        }
        // DAW_SHOT_TAKE stages a take that is actually rolling — "layers" to
        // punch into the demo's layered clip, anything else onto empty
        // timeline — so the clip the arrangement draws while recording can be
        // photographed without an input device.
        if (const char* shotTake = std::getenv("DAW_SHOT_TAKE"))
            window.stageRecordingShot(QString::fromUtf8(shotTake));
        // DAW_SHOT_SLOTS fills insert and instrument slots with real scanned
        // plugins, so a grab shows the strip as it looks with a chain on it
        // rather than a rack of empty slots.
        if (std::getenv("DAW_SHOT_SLOTS")) window.loadDemoPluginSlots();
        // The piano roll is an internal editor, so the shell grab includes its
        // custom frame and the arrangement visible around it.
        const bool shootRoll = std::getenv("DAW_SHOT_PIANOROLL") != nullptr;
        if (shootRoll) window.openFirstMidiClip();
        // DAW_SHOT_PIANOROLL=selected also selects every note, which is the
        // only way a still can show the context panel — it exists exactly when
        // something is selected.
        if (shootRoll && QString::fromUtf8(std::getenv("DAW_SHOT_PIANOROLL")) ==
                             QStringLiteral("selected")) {
            window.selectAllNotesForShot();
        }
        const bool shootClip = std::getenv("DAW_SHOT_CLIP_EDITOR") != nullptr;
        if (shootClip) window.openFirstAudioClip();
        const char* shotPattern = std::getenv("DAW_SHOT_PATTERN");
        const bool patternTimeline = shotPattern != nullptr &&
            (std::strcmp(shotPattern, "timeline") == 0 ||
             std::strcmp(shotPattern, "expanded-timeline") == 0);
        const bool shootPattern = shotPattern != nullptr && !patternTimeline;
        if (shotPattern) {
            window.openDemoPattern(shootPattern);
            if (!shootPattern) window.scrollArrangement(100000);
        }
        // DAW_SHOT_SAMPLER opens the built-in sampler's editor on the demo's
        // instrument track. Its value, when not empty, is a sample to load, so
        // the grab shows a real waveform rather than the empty state. Grabbed
        // like the piano roll — it is pure Qt, so `grab()` works offscreen and
        // no screen capture is needed.
        // DAW_SHOT_EXPORT opens the render dialog. Shown rather than exec'd:
        // a modal dialog would block the timer that grabs it.
        const char* shotExport = std::getenv("DAW_SHOT_EXPORT");
        const bool shootExport = shotExport != nullptr;
        if (shootExport) {
            window.openExportDialogForShot(std::strcmp(shotExport, "stems") == 0);
        }
        const char* shotSampler = std::getenv("DAW_SHOT_SAMPLER");
        const bool shootSampler = shotSampler != nullptr;
        if (shootSampler) window.openDemoSampler(QString::fromUtf8(shotSampler));
        // DAW_SHOT_GRAVITY opens the built-in spatial pitch-delay. It is pure
        // Qt, so preferred and minimum layouts can both be captured offscreen.
        // DAW_SHOT_GRAVITY_DRAWER expands the advanced controls and
        // DAW_SHOT_GRAVITY_DUAL separates the linked A/B attractors.
        const bool shootGravity = std::getenv("DAW_SHOT_GRAVITY") != nullptr;
        if (shootGravity) window.openDemoGravity();
        if (shootGravity && std::getenv("DAW_SHOT_GRAVITY_MIN")) {
            QTimer::singleShot(300, &window,
                               [&window] { window.resizeGravityForShot(); });
        }
        // DAW_SHOT_EQUALIZER opens the built-in EQ with a factory curve.
        const bool shootEqualizer = std::getenv("DAW_SHOT_EQUALIZER") != nullptr;
        if (shootEqualizer) window.openDemoEqualizer();
        if (shootEqualizer && std::getenv("DAW_SHOT_EQUALIZER_MIN")) {
            QTimer::singleShot(300, &window,
                               [&window] { window.resizeEqualizerForShot(); });
        }
        // Both hosts embed the same panel and therefore share the same minimum
        // size check; this also makes CLIP screenshots prove layout parity.
        if ((shootSampler || shootClip) &&
            std::getenv("DAW_SHOT_SAMPLER_MIN")) {
            QTimer::singleShot(0, &window, [&window] {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    if (widget != &window && widget->isVisible() &&
                        widget->width() > 600) {
                        widget->resize(860, 520);
                    }
                }
            });
        }
        // DAW_SHOT_BROWSER names a folder for the browser to show; with
        // DAW_SHOT_BROWSER_FILE a file inside it is selected, so the grab has a
        // real waveform in the preview strip rather than the empty state.
        // DAW_SHOT_BROWSER_RIGHT puts the panel on the other side — the two
        // layouts are the check for the tool strip's zone order.
        if (const char* shotBrowser = std::getenv("DAW_SHOT_BROWSER")) {
            if (std::getenv("DAW_SHOT_BROWSER_RIGHT"))
                window.setBrowserOnLeft(false, /*persist=*/false);
            const char* shotFile = std::getenv("DAW_SHOT_BROWSER_FILE");
            window.openDemoBrowser(QString::fromUtf8(shotBrowser),
                                   QString::fromUtf8(shotFile ? shotFile : ""));
        }
        // DAW_SHOT_BROWSER_PLUGINS opens the browser on its plugin folders
        // instead of a sample folder — the listing has no file on disk behind
        // it, so nothing else can show it.
        if (std::getenv("DAW_SHOT_BROWSER_PLUGINS")) {
            window.openDemoBrowserPlugins();
        }
        // DAW_SHOT_AI opens the assistant with a finished transcript in it,
        // so a grab shows the panel doing its job rather than empty.
        // DAW_SHOT_AI=empty shows the panel as a user first meets it, with no
        // key configured; any other value runs a scripted turn first.
        // DAW_SHOT_AI=music shows the second mode instead: the switch thrown
        // to Music, with a finished generation on a new track.
        if (const char* shotAi = std::getenv("DAW_SHOT_AI")) {
            if (std::strcmp(shotAi, "music") == 0) window.openDemoAiMusic();
            else window.openDemoAi(std::strcmp(shotAi, "empty") != 0);
        }
        // DAW_SHOT_WEB opens the integrated browser. Combining it with
        // DAW_SHOT_AI photographs the independent workspace | Web | AI layout.
        if (std::getenv("DAW_SHOT_WEB")) window.openWebBrowserForShot();
        // DAW_SHOT_SETTINGS is a settings tab index, so each page can be
        // grabbed on its own.
        const char* shotSettings = std::getenv("DAW_SHOT_SETTINGS");
        const bool shootSettings = shotSettings != nullptr;
        if (shootSettings) window.openSettings(std::atoi(shotSettings));
        // DAW_SHOT_PLUGINS is the tab index to show (0 Plugins, 1 Search Paths,
        // 2 Blacklist), so each tab can be grabbed on its own.
        // DAW_SHOT_CONTEXT is a comma-separated list of clip indices, left to
        // right, to select — so the context panel can be grabbed riding above
        // one clip or centred over a group.
        if (const char* shotContext = std::getenv("DAW_SHOT_CONTEXT")) {
            // The demo is already populated above; populating again here (as
            // this used to) gave the grab two of every track.
            window.selectDemoClipsForShot(QString::fromUtf8(shotContext));
        }
        // DAW_SHOT_CONTEXT_THEN switches to a second selection once the event
        // loop is running, so the grab lands inside the swap rather than after
        // it. With DAW_SHOT_DELAY this photographs the animation frame by frame.
        if (const char* then = std::getenv("DAW_SHOT_CONTEXT_THEN")) {
            const QString second = QString::fromUtf8(then);
            // Late enough that the first selection has finished arriving, so
            // the swap being photographed starts from a settled plate — which
            // is what it starts from in front of a user.
            QTimer::singleShot(600, &window, [&window, second] {
                window.selectDemoClipsForShot(second, /*settle=*/false);
            });
        }
        // DAW_SHOT_FLYOUT opens the context panel's level ("volume") or pan
        // flyout on the first track — the only way to photograph a control
        // that exists solely under the pointer.
        if (const char* flyout = std::getenv("DAW_SHOT_FLYOUT")) {
            // After the event loop has run once: the panel builds its content
            // in response to the selection change, so its controls do not exist
            // yet at this point in main().
            const QString which = QString::fromUtf8(flyout);
            QTimer::singleShot(250, &window, [&window, which] {
                const bool opened = window.openDemoTrackFlyout(which);
                std::fprintf(stderr, "DAW_SHOT_FLYOUT %s: %s\n",
                             which.toUtf8().constData(), opened ? "open" : "not found");
            });
        }
        // DAW_SHOT_MENU=plugins pops the plugin picker up on its own, so the
        // search field, the groups and the format column can be looked at.
        if (const char* shotMenu = std::getenv("DAW_SHOT_MENU")) {
            // "plugins", "instruments", or either with ":query" to land in the
            // menu with the search already filtering.
            const QStringList parts = QString::fromUtf8(shotMenu).split(':');
            const bool instruments = parts.first() == QLatin1String("instruments");
            const QString query = parts.size() > 1 ? parts.at(1) : QString();
            QTimer::singleShot(250, &window, [&window, instruments, query] {
                window.openDemoPluginMenu(instruments, query);
            });
        }
        // Expand the selected track's inline glass plugin finder. Kept as a
        // first-class screenshot state because it temporarily grows the tool
        // strip and exercises a layout the ordinary shell grab cannot reach.
        if (const char* pluginSearch = std::getenv("DAW_SHOT_PLUGIN_SEARCH")) {
            window.openDemoPluginSearch(std::strcmp(pluginSearch, "closed") != 0);
        }
        // DAW_SHOT_RECOVERY grabs the prompt a user meets after a crash,
        // over a made-up session — no real recovery data is involved.
        const bool shootRecovery = std::getenv("DAW_SHOT_RECOVERY") != nullptr;
        if (shootRecovery) window.openDemoRecoveryPrompt();
        const char* shotPlugins = std::getenv("DAW_SHOT_PLUGINS");
        const bool shootPlugins = shotPlugins != nullptr;
        if (shootPlugins) window.openPluginManager(std::atoi(shotPlugins));
        if (shotEditor) {
            shootEditor = window.openDemoPluginEditor(QString::fromUtf8(shotEditor));
        }
        // DAW_PROBE_SWAP="<first>,<second>" runs the Replace sequence by hand
        // and reports what became of the editor window and the live plugin at
        // each step. A diagnostic, not a check: the plugins it needs exist only
        // on the machine the user runs it on.
        if (const char* swap = std::getenv("DAW_PROBE_SWAP")) {
            const QStringList names = QString::fromUtf8(swap).split(',');
            if (names.size() == 2)
                window.probePluginEditorSwap(names[0].trimmed(), names[1].trimmed());
        }
        // A plugin's editor is a foreign NSView/HWND: `QWidget::grab()` walks
        // the Qt tree and would return an empty rectangle for it, so that case
        // captures the screen instead, and waits long enough for the plugin to
        // have drawn itself. A flyout is a separate top-level window of our
        // own and misses a widget grab for the same reason.
        // DAW_SHOT_DELAY overrides how long the app runs before the grab, so a
        // running animation can be caught at a chosen moment.
        int grabDelay = shootEditor ? 2500 : 500;
        if (const char* delay = std::getenv("DAW_SHOT_DELAY")) {
            grabDelay = std::max(0, std::atoi(delay));
        }
        // The flyout is a Qt::Tool window, which macOS hides while the app is
        // not frontmost — a screen grab of an unfocused run does not contain
        // it however visible Qt thinks it is. So it is grabbed as a widget, on
        // its own, which is all there is to look at anyway.
        const bool shootFlyout = std::getenv("DAW_SHOT_FLYOUT") != nullptr ||
                                 std::getenv("DAW_SHOT_MENU") != nullptr ||
                                 std::getenv("DAW_SHOT_FOLDER_DIALOG") != nullptr;
        // DAW_SHOT_FOLDER_DIALOG opens "which kind of folder?" over the demo.
        // It runs its own nested event loop, so it is opened from a timer of
        // its own and the grab below still fires inside it.
        if (std::getenv("DAW_SHOT_FOLDER_DIALOG")) {
            QTimer::singleShot(0, &window, [&window] { window.onNewFolder(); });
        }
        QTimer::singleShot(grabDelay, &app, [&] {
            if (shootEditor) {
                if (QScreen* screen = QGuiApplication::primaryScreen()) {
                    screen->grabWindow(0).save(QString::fromUtf8(screenshotPath));
                }
                QApplication::quit();
                return;
            }
            QWidget* target = &window;
            if (shootFlyout) {
                // The popup, whatever kind it is: a flyout or a menu, both are
                // top-level windows of ours that a widget grab of the shell
                // would miss.
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (w != &window && w->isVisible() && w->width() < 700) target = w;
                }
            }
            if (shootRoll || shootClip || shootPattern || shootPlugins ||
                shootSampler || shootGravity || shootEqualizer || shootSettings || shootAutomation ||
                shootExport) {
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (w != &window && w->isVisible() && w->width() > 600) target = w;
                }
            }
            // A message box is far narrower than the 600 px the windows above
            // are found by, so it gets its own pass with no width floor.
            if (shootRecovery) {
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (w != &window && w->isVisible() &&
                        qobject_cast<QMessageBox*>(w)) {
                        target = w;
                    }
                }
            }
            target->grab().save(QString::fromUtf8(screenshotPath));
            QApplication::quit();
        });
    } else if (recovercheck) {
        const int tracks = window.checkRecovery();
        if (tracks < 0) {
            std::fprintf(stderr, "nothing was recovered\n");
            window.endRecoverySessionForTest();
            return 10;
        }
        std::printf("recovered %d tracks\n", tracks);
        window.endRecoverySessionForTest();
        return 0;
    } else if (crashtest) {
        if (!std::getenv("DAW_RECOVERY_ROOT")) {
            std::fprintf(stderr, "--crashtest needs DAW_RECOVERY_ROOT\n");
            return 8;
        }
        window.populateDemo();
        if (!window.flushRecoveryForTest()) {
            std::fprintf(stderr, "the recovery journal is not running\n");
            return 8;
        }
        // A genuine fault: no destructor runs, no handler tidies up, and the
        // session directory is left in exactly the state a real crash leaves.
        std::fflush(nullptr);
        volatile int* boom = nullptr;
        *boom = 1;
        return 9;   // unreachable; keeps the compiler from eliding the store
    } else if (selftest) {
        // Keep Gravity's widget/undo/preset invariants independently runnable:
        // the full UI selftest also exercises platform codecs, file watching
        // and WebEngine, which may be unavailable on a sanitizer machine.
        if (qEnvironmentVariableIsSet("DAW_SELFTEST_GRAVITY_ONLY")) {
            window.populateDemo();
            if (!window.checkGravityPanelForTest()) {
                std::fprintf(stderr, "Gravity panel UI invariants failed\n");
                return 30;
            }
            QTimer::singleShot(0, &app, [] { QApplication::quit(); });
        } else if (qEnvironmentVariableIsSet("DAW_SELFTEST_EQUALIZER_ONLY")) {
            window.populateDemo();
            if (!window.checkEqualizerPanelForTest()) {
                std::fprintf(stderr, "Equalizer panel UI invariants failed\n");
                return 31;
            }
            QTimer::singleShot(0, &app, [] { QApplication::quit(); });
        } else {
        // Build every secondary window too: they are where the tables, the
        // timers and the theme hooks live, and a crash in one of them would
        // otherwise only show up in front of the user.
        window.openPluginManager();
        // Every settings page is built here too — a page that throws on
        // construction would otherwise only be found by opening it by hand.
        window.openSettings(SettingsWindow::kBrowserTab);
        if (!window.checkSettingsViewportForTest()) {
            std::fprintf(stderr,
                         "the settings window is not screen-bounded and scrollable\n");
            return 16;
        }
        // The sampler's panel is built here too: it is a window full of bound
        // controls and timers, and a crash in it would otherwise only show up
        // in front of the user.
        window.populateDemo();
        if (!window.checkPluginAutoOpenForTest()) {
            std::fprintf(stderr,
                         "a newly added plugin did not request its editor\n");
            return 28;
        }
        if (!window.checkPluginSearchFocusForTest()) {
            std::fprintf(stderr,
                         "the plugin picker did not keep keyboard focus in search\n");
            return 29;
        }
        window.openDemoSampler();
        if (!window.checkGravityPanelForTest()) {
            std::fprintf(stderr, "Gravity panel UI invariants failed\n");
            return 30;
        }
        if (!window.checkEqualizerPanelForTest()) {
            std::fprintf(stderr, "Equalizer panel UI invariants failed\n");
            return 31;
        }
        // The typing keyboard is a key filter over the whole application, and
        // the only way to know it still plays a note is to send it one.
        if (!window.checkTypingKeyboard()) {
            std::fprintf(stderr, "the typing keyboard did not play a note\n");
            return 3;
        }
        if (!window.checkLayoutIndependentShortcuts()) {
            std::fprintf(stderr,
                         "a shortcut did not follow its physical key across layouts\n");
            return 15;
        }
        // The render dialog probes libsndfile to build its format menus, so a
        // build whose codecs went missing shows up here rather than as an
        // sf_open failure the first time someone exports.
        if (!window.checkExportDialogForTest()) {
            std::fprintf(stderr,
                         "the render dialog did not build a usable channel or format list\n");
            return 19;
        }
        if (!window.checkAuxiliaryWindowPolicyForTest()) {
            std::fprintf(stderr,
                         "the internal piano-roll frame did not follow workspace policy\n");
            return 18;
        }
        if (!window.checkMidiInput()) {
            std::fprintf(stderr,
                         "hardware MIDI routing or Piano Roll highlighting failed\n");
            return 32;
        }
        if (!window.checkPianoRollForTest()) {
            std::fprintf(stderr, "piano-roll gesture invariants failed\n");
            return 19;
        }
        // A level changed on the context panel has to move the mixer's fader
        // with it — the panel and the strip are two views of one value.
        if (!window.checkContextSyncForTest()) {
            std::fprintf(stderr, "the context panel did not reach the mixer\n");
            return 11;
        }
        // Adding a track must not shrink the ones already there — the header
        // column is a second view of the same lanes and has to keep step.
        if (!window.checkTrackRowHeightsForTest()) {
            std::fprintf(stderr, "the track headers do not match their lanes\n");
            return 12;
        }
        // Selection is a set now — shift ranges, ctrl toggles — and folders are
        // made out of it. Both are pure gesture handling, invisible to the
        // headless controller tests.
        if (!window.checkTrackSelectionForTest()) {
            std::fprintf(stderr, "track selection or folder packing failed\n");
            return 20;
        }
        if (!window.checkAutomationForTest()) {
            std::fprintf(stderr, "automation editing failed\n");
            return 22;
        }
        if (!window.checkAutomationEditorForTest()) {
            std::fprintf(stderr, "the automation editor failed\n");
            return 23;
        }
        if (!window.checkKnobAutomationForTest()) {
            std::fprintf(stderr, "knob-to-automation failed\n");
            return 24;
        }
        if (!window.checkParameterDockForTest()) {
            std::fprintf(stderr, "the plugin parameter dock failed\n");
            return 25;
        }
        if (!window.checkCycleRegionForTest()) {
            std::fprintf(stderr, "the cycle region failed\n");
            return 21;
        }
        if (!window.checkTimelinePanForTest()) {
            std::fprintf(stderr, "middle-button timeline panning failed\n");
            return 13;
        }
        if (!window.checkTimelineClipGesturesForTest()) {
            std::fprintf(stderr, "timeline clip gestures failed\n");
            return 36;
        }
        if (!window.checkLiveTempoForTest()) {
            std::fprintf(stderr, "the tempo grid did not update live\n");
            return 14;
        }
        if (!window.checkTempoScrubForTest()) {
            std::fprintf(stderr, "the tempo scrub interaction failed\n");
            return 17;
        }
        // Dropping files on the arrangement: the routing decides what kind of
        // track a file needs, and nothing but a real drop exercises it.
        {
            const QString dir = QDir::tempPath() + "/daw-selftest-drop";
            QDir().mkpath(dir);
            const QString midi = dir + "/two-notes.mid";
            if (!writeDemoMidiFile(midi)) {
                std::fprintf(stderr, "could not write the test MIDI file\n");
                return 4;
            }
            if (!window.checkFileDrop({midi}, 1)) {
                std::fprintf(stderr, "dropping a MIDI file made no clip\n");
                return 4;
            }
            // The browser: its tree, its worker-thread decode, the audition it
            // starts, and moving the panel across the window.
            const QString tone = QDir::temp().filePath("daw_demo_tone.wav");
            if (!window.checkBrowser(QDir::tempPath(), tone)) {
                std::fprintf(stderr, "the browser did not audition a file\n");
                return 5;
            }
            if (!window.checkWebBrowserForTest(tone)) {
                std::fprintf(stderr,
                             "the Web browser audio import/undo flow failed\n");
                return 27;
            }
        }
        // The assistant: a scripted stand-in for a provider drives a whole
        // turn, so the panel -> session -> dispatch -> document -> undo path is
        // checked on every build with no key and no network.
        if (!window.checkAiAssistant()) {
            std::fprintf(stderr, "the AI assistant did not complete a turn\n");
            return 6;
        }
        // And its music mode: a stand-in for the music server answers with a
        // real file, so the whole generate -> save -> import -> undo path is
        // checked too.
        // Last of the window checks: it has to leave a clip selected and the
        // roll in front to prove what it proves, and the context panel caches a
        // page per context — so anything reading that panel runs before it.
        if (!window.checkEditChordRoutingForTest()) {
            std::fprintf(stderr, "edit chords do not reach the piano roll\n");
            return 26;
        }
        if (!window.checkAiMusic()) {
            std::fprintf(stderr, "the AI music mode did not deliver a clip\n");
            return 7;
        }
        if (shotEditor) {
            window.populateDemo();
            if (!window.openDemoPluginEditor(QString::fromUtf8(shotEditor))) {
                std::fprintf(stderr, "no scanned plugin matches '%s'\n", shotEditor);
                return 2;
            }
        }
        QTimer::singleShot(shotEditor ? 1200 : 400, &app, [] { QApplication::quit(); });
        }
    }
    const int result = app.exec();
    if (selftest && g_selftestQtFailure.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "selftest observed a critical Qt rendering warning\n");
        return 7;
    }
    return result;
}
