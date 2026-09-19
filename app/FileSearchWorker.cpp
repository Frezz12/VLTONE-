#include "FileSearchWorker.hpp"
#include "ProjectTemplates.hpp"
#include "BrowserPrefs.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QThreadPool>

namespace {

/// The caps. Generous enough that an ordinary sample folder is searched whole,
/// tight enough that pointing the browser at a home directory cannot hang.
constexpr int kMaxMatches = 2000;
constexpr int kMaxVisited = 200000;
constexpr int kMaxDepth = 12;

} // namespace

FileSearchWorker::FileSearchWorker(QObject* parent)
    : QObject(parent),
      m_generation(std::make_shared<std::atomic<quint64>>(0)) {}

FileSearchWorker::~FileSearchWorker() {
    // The running task holds the counter by shared_ptr and checks it, so it
    // finds itself stale and drops its results rather than touching a dead
    // object. Nothing to wait for.
    cancel();
}

bool FileSearchWorker::matches(const QString& fileName, const QString& query) {
    QString wanted = query.trimmed().toLower();
    if (wanted.isEmpty()) return false;

    // ".wav" or "*.wav" means "by extension"; anything else is a name search.
    QString extension;
    if (wanted.startsWith(QLatin1String("*."))) extension = wanted.mid(2);
    else if (wanted.startsWith(QLatin1Char('.'))) extension = wanted.mid(1);

    const QString name = fileName.toLower();
    if (!extension.isEmpty()) {
        return QFileInfo(name).suffix() == extension;
    }
    // A bare word matches the name *or* the extension, so typing "wav" finds
    // WAV files without having to remember the dot.
    return name.contains(wanted) || QFileInfo(name).suffix() == wanted;
}

void FileSearchWorker::cancel() {
    m_generation->fetch_add(1, std::memory_order_release);
}

void FileSearchWorker::search(const QStringList& roots, const QString& query,
                              const QStringList& ignoredExtensions) {
    const quint64 generation =
        m_generation->fetch_add(1, std::memory_order_release) + 1;
    if (query.trimmed().isEmpty()) return;

    // Captured by value: the task must not touch the worker except through the
    // queued callback below, and the counter outlives both.
    auto counter = m_generation;
    QPointer<FileSearchWorker> self(this);

    QThreadPool::globalInstance()->start([roots, query, ignoredExtensions, generation, counter, self] {
        QStringList found;
        int visited = 0;
        bool truncated = false;
        QElapsedTimer delivery;
        delivery.start();
        qsizetype publishedCount = 0;
        const auto publish = [&](bool finished) {
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [self, found, truncated, finished, generation, counter] {
                    if (self && counter->load(std::memory_order_acquire) == generation)
                        emit self->results(found, truncated, finished);
                }, Qt::QueuedConnection);
            publishedCount = found.size();
            delivery.restart();
        };
        QQueue<QPair<QString, int>> pending;
        for (const QString& root : roots) pending.enqueue({root, 0});
        QSet<QString> scanned;

        // Breadth first: one large library must not postpone matches directly
        // inside the other roots. Prune packages/deep folders before entering.
        while (!pending.isEmpty() && visited < kMaxVisited && found.size() < kMaxMatches) {
            if (counter->load(std::memory_order_acquire) != generation) return;
            const auto [directory, depth] = pending.dequeue();
            QString identity = QDir(directory).absolutePath();
#if defined(Q_OS_WIN)
            identity = identity.toCaseFolded();
#endif
            if (scanned.contains(identity)) continue;
            scanned.insert(identity);
            QDirIterator it(directory, QDir::AllEntries | QDir::NoDotAndDotDot);
            while (it.hasNext()) {
                const QString path = it.next();
                if (++visited > kMaxVisited) {
                    truncated = true;
                    break;
                }
                // Checked per entry rather than per folder: a new keystroke
                // should stop this walk now, not when it reaches the next
                // directory.
                if ((visited & 0xFF) == 0 &&
                    counter->load(std::memory_order_acquire) != generation) {
                    return;
                }
                if (found.size() != publishedCount && delivery.elapsed() >= 100)
                    publish(false);
                const QFileInfo info = it.fileInfo();
                const bool package = ui::projecttemplates::isTemplatePackage(path);
                if (info.isDir() && !package) {
                    if (!info.isSymLink()) {
                        if (depth < kMaxDepth) pending.enqueue({path, depth + 1});
                        else truncated = true;
                    }
                    continue;
                }
                if (ui::browserprefs::isIgnoredFile(info.fileName(), ignoredExtensions)) continue;
                if (!matches(info.fileName(), query)) continue;
                found << info.absoluteFilePath();
                if (found.size() == 1 || delivery.elapsed() >= 100) {
                    publish(false);
                }
                if (found.size() >= kMaxMatches) {
                    truncated = true;
                    break;
                }
            }
        }

        if (counter->load(std::memory_order_acquire) != generation) return;
        if (!pending.isEmpty()) truncated = true;
        publish(true);
    });
}
