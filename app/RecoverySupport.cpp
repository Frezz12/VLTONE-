#include "RecoverySupport.hpp"

#include "EngineController.hpp"
#include "ProjectSerializer.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QStandardPaths>

#include <algorithm>
#include <memory>
#include <cstdlib>
#include <utility>

namespace ui::recovery {

namespace {

QString formatWhen(std::int64_t unixMs) {
    if (unixMs <= 0) return QObject::tr("an unknown time");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(unixMs),
                              QLocale::ShortFormat);
}

} // namespace

QString rootDir() {
    if (const char* override = std::getenv("DAW_RECOVERY_ROOT"))
        return QString::fromUtf8(override);
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("recovery"));
}

QString guardPath() {
    const QDir directory(QCoreApplication::applicationDirPath());
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("daw_guard.exe");
#else
    const QString name = QStringLiteral("daw_guard");
#endif
    const QString path = directory.filePath(name);
    return QFile::exists(path) ? path : QString();
}

QString describeSession(const daw::recovery::SessionInfo& info) {
    if (!info.crashReason.empty())
        return QString::fromStdString(info.crashReason);
    switch (info.outcome) {
        case daw::recovery::Outcome::Hung:
            return QObject::tr("the program stopped responding");
        case daw::recovery::Outcome::Crashed:
            return QObject::tr("the program closed unexpectedly");
        case daw::recovery::Outcome::Running:
            break;
    }
    // No verdict at all means the watchdog never got to write one — the machine
    // lost power, or the process was killed outright.
    return QObject::tr("the program did not shut down normally");
}

QMessageBox* buildRecoveryPrompt(QWidget* parent,
                                 const daw::recovery::SessionInfo& session) {
    QString name = session.projectName.empty()
                       ? QObject::tr("an unsaved project")
                       : QString::fromStdString(session.projectName);
    if (name == QLatin1String("Untitled")) name = QObject::tr("Untitled");

    auto* box = new QMessageBox(parent);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QObject::tr("Recover unsaved work"));
    box->setText(
        QObject::tr("VLT Studio Pro closed unexpectedly while you were working on %1.")
            .arg(name));
    // Every limit stated up front. Recovery that quietly restores less than the
    // user assumes is worse than recovery that says what it has.
    box->setInformativeText(
        QObject::tr(
            "Last saved automatically at %1 — %2.\n\n"
            "Notes, clips, the mix and plugin settings come back as of that "
            "moment. Audio is read "
            "from your original files, not from copies inside the project.")
            .arg(formatWhen(session.journalUnixMs))
            .arg(describeSession(session)));

    QPushButton* restore =
        box->addButton(QObject::tr("Restore"), QMessageBox::AcceptRole);
    box->addButton(QObject::tr("Discard"), QMessageBox::DestructiveRole);
    box->addButton(QObject::tr("Decide Later"), QMessageBox::RejectRole);
    box->setDefaultButton(restore);
    return box;
}

bool applySession(const daw::recovery::SessionInfo& session,
                  daw::EngineController& controller, QString* error) {
    daw::ProjectModel model;
    const std::string journal =
        QDir(QString::fromStdString(session.directory))
            .filePath(QString::fromUtf8(daw::recovery::kJournalFile))
            .toStdString();
    // Clip paths in a journal are absolute and ignore mediaDir. Fresh plugin
    // chunks live beside the journal; a slot whose plugin could not be sampled
    // may still fall back to its state from the last manual package.
    auto result = daw::ProjectSerializer::loadDocument(
        model, journal,
        session.projectPath.empty()
            ? std::string{}
            : daw::ProjectSerializer::mediaPath(session.projectPath));
    if (!result) {
        if (error) *error = QString::fromStdString(result.message());
        return false;
    }
    result = controller.restoreRecoveryProject(
        std::move(model), session.directory, session.projectPath);
    if (!result) {
        if (error) *error = QString::fromStdString(result.message());
        return false;
    }
    return true;
}

Choice offerRecovery(QWidget* parent, daw::EngineController& controller) {
    Choice choice;
    auto sessions = daw::recovery::staleSessions(rootDir().toStdString());
    if (sessions.empty()) return choice;

    // Newest first: if several are waiting, the most recent work is the one the
    // user is most likely to want back.
    std::sort(sessions.begin(), sessions.end(),
              [](const auto& a, const auto& b) {
                  return a.journalUnixMs > b.journalUnixMs;
              });

    for (const auto& session : sessions) {
        std::unique_ptr<QMessageBox> box(buildRecoveryPrompt(parent, session));
        box->exec();

        QAbstractButton* clicked = box->clickedButton();
        const QMessageBox::ButtonRole role =
            clicked ? box->buttonRole(clicked) : QMessageBox::RejectRole;
        if (role == QMessageBox::DestructiveRole) {
            daw::recovery::discardSession(session.directory);
            continue;
        }
        if (role != QMessageBox::AcceptRole) continue;   // decide later

        QString error;
        if (!applySession(session, controller, &error)) {
            QMessageBox::warning(
                parent, QObject::tr("Recovery failed"),
                QObject::tr("The recovered file could not be read: %1").arg(error));
            continue;
        }

        choice.restored = true;
        choice.projectPath = QString::fromStdString(session.projectPath);
        daw::recovery::discardSession(session.directory);
        // One recovery per launch: restoring a second would throw away the
        // first. Anything left over is offered again next time.
        break;
    }
    return choice;
}

} // namespace ui::recovery
