#include "FileSearchWorker.hpp"
#include "ProjectTemplates.hpp"

#include <QDirIterator>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace {

/// The caps. Generous enough that an ordinary sample folder is searched whole,
/// tight enough that pointing the browser at a home directory cannot hang.
constexpr int kMaxMatches = 2000;
constexpr int kMaxVisited = 200000;
constexpr int kMaxDepth = 12;

int depthUnder(const QString& root, const QString& path) {
    return int(path.mid(root.size()).count(QLatin1Char('/')));
}

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

void FileSearchWorker::search(const QStringList& roots, const QString& query) {
    const quint64 generation =
        m_generation->fetch_add(1, std::memory_order_release) + 1;
    if (query.trimmed().isEmpty() || roots.isEmpty()) return;

    // Captured by value: the task must not touch the worker except through the
    // queued callback below, and the counter outlives both.
    auto counter = m_generation;
    QPointer<FileSearchWorker> self(this);

    QThreadPool::globalInstance()->start([roots, query, generation, counter, self] {
        QStringList found;
        int visited = 0;
        bool truncated = false;

        for (const QString& root : roots) {
            if (counter->load(std::memory_order_acquire) != generation) return;
            QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
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
                const QFileInfo info = it.fileInfo();
                if (info.isDir()) {
                    if (ui::projecttemplates::isTemplatePackage(path) &&
                        matches(info.fileName(), query)) {
                        found << info.absoluteFilePath();
                        if (found.size() >= kMaxMatches) {
                            truncated = true;
                            break;
                        }
                    }
                    continue;
                }
                // QDirIterator cannot prune a package directory. It may still
                // walk through one, but none of the package's implementation
                // files are user-facing search results.
                if (QDir::fromNativeSeparators(path).contains(
                        QLatin1String(".vltt/"), Qt::CaseInsensitive)) {
                    continue;
                }
                if (depthUnder(root, path) > kMaxDepth) continue;
                if (!matches(info.fileName(), query)) continue;
                found << info.absoluteFilePath();
                if (found.size() >= kMaxMatches) {
                    truncated = true;
                    break;
                }
            }
            if (truncated) break;
        }

        if (counter->load(std::memory_order_acquire) != generation) return;
        // Back to the UI thread, and only if the worker is still there.
        QMetaObject::invokeMethod(
            self, [self, found, truncated, generation, counter] {
                if (!self) return;
                if (counter->load(std::memory_order_acquire) != generation) return;
                emit self->results(found, truncated);
            },
            Qt::QueuedConnection);
    });
}
