#include "FileSearchWorker.hpp"
#include "ProjectTemplates.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>

namespace {
constexpr int kMaxMatches = 2000;
constexpr int kMaxVisited = 200000;
constexpr int kMaxDepth = 32;
constexpr int kMaxWatches = 4096;
#if defined(Q_OS_WIN)
constexpr auto kPathCase = Qt::CaseInsensitive;
#else
constexpr auto kPathCase = Qt::CaseSensitive;
#endif

QStringList normalizedRoots(const QStringList& roots) {
    QStringList result;
    for (const auto& root : roots) {
        if (root.trimmed().isEmpty()) continue;
        const QString clean = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        if (!result.contains(clean, kPathCase)) result.append(clean);
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool withinRoot(const QString& path, const QString& root) {
    return path.compare(root, kPathCase) == 0 ||
        path.startsWith(root.endsWith('/') ? root : root + '/', kPathCase);
}

struct Query {
    QString extension;
    QStringList words;
    explicit Query(const QString& query) {
        const QString wanted = query.trimmed().toCaseFolded();
        if (wanted.startsWith(QLatin1String("*."))) extension = wanted.mid(2);
        else if (wanted.startsWith('.')) extension = wanted.mid(1);
        if (extension.isEmpty()) words = wanted.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    }
    bool matches(const QString& name, const QString& suffix) const {
        if (!extension.isEmpty()) return suffix == extension;
        return !words.isEmpty() && std::all_of(words.cbegin(), words.cend(),
            [&name](const QString& word) { return name.contains(word); });
    }
};
} // namespace

struct FileSearchWorker::Index {
    struct Entry {
        QString path, name, suffix;
        bool directory = false;
    };
    QVector<Entry> entries;
    QStringList directories;
    bool finished = false;
    bool truncated = false;
    bool watchIncomplete = false;
};

FileSearchWorker::FileSearchWorker(QObject* parent)
    : QObject(parent),
      m_generation(std::make_shared<std::atomic<quint64>>(0)),
      m_scanGeneration(std::make_shared<std::atomic<quint64>>(0)),
      m_watcher(new QFileSystemWatcher(this)), m_refreshTimer(new QTimer(this)) {
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(400);
    connect(m_refreshTimer, &QTimer::timeout, this, &FileSearchWorker::refresh);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
        [this] { m_refreshTimer->start(); });
}

FileSearchWorker::~FileSearchWorker() {
    cancel();
    m_scanGeneration->fetch_add(1, std::memory_order_release);
}

bool FileSearchWorker::matches(const QString& fileName, const QString& query) {
    return Query(query).matches(fileName.toCaseFolded(), QFileInfo(fileName).suffix().toCaseFolded());
}

bool FileSearchWorker::indexReadyForTest() const {
    return m_index && m_index->finished;
}

void FileSearchWorker::cancel() {
    m_query.clear();
    m_generation->fetch_add(1, std::memory_order_release);
}

void FileSearchWorker::setRoots(const QStringList& roots) {
    const auto normalized = normalizedRoots(roots);
    if (normalized == m_roots && m_index) return;
    m_roots = normalized;
    const auto watched = m_watcher->directories();
    if (!watched.isEmpty()) m_watcher->removePaths(watched);
    refresh();
}

void FileSearchWorker::refresh() {
    m_refreshTimer->stop();
    // A stale fallback index may trigger one rebuild, never one per keypress.
    m_indexAge.invalidate();
    ++m_indexBuilds;
    const quint64 generation = m_scanGeneration->fetch_add(1, std::memory_order_release) + 1;
    m_index = std::make_shared<Index>();
    runQuery();
    auto counter = m_scanGeneration;
    QPointer<FileSearchWorker> self(this);
    const auto roots = m_roots;
    QThreadPool::globalInstance()->start([roots, generation, counter, self] {
        Index index;
        QQueue<QPair<QString, int>> pending;
        QSet<QString> scanned, recorded, watches;
        QElapsedTimer delivery;
        delivery.start();
        int visited = 0;
        const auto stale = [&] { return counter->load(std::memory_order_acquire) != generation; };
        const auto publish = [&] {
            auto snapshot = std::make_shared<const Index>(index);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, snapshot, generation, counter] {
                if (self && counter->load(std::memory_order_acquire) == generation)
                    self->acceptIndex(snapshot);
            }, Qt::QueuedConnection);
            delivery.restart();
        };
        const auto identity = [](const QString& path) {
            return kPathCase == Qt::CaseInsensitive ? path.toCaseFolded() : path;
        };
        const auto watch = [&](const QString& path) {
            if (watches.contains(identity(path))) return;
            if (watches.size() >= kMaxWatches) { index.watchIncomplete = true; return; }
            watches.insert(identity(path));
            index.directories.append(path);
        };
        const auto record = [&](const QFileInfo& info) {
            const QString path = info.absoluteFilePath();
            const QString key = identity(path);
            if (recorded.contains(key)) return;
            recorded.insert(key);
            const QString suffix = info.suffix().toCaseFolded();
            const bool directory = info.isDir() && suffix != QLatin1String(ui::projecttemplates::kExtension);
            index.entries.append({path, info.fileName().toCaseFolded(), suffix, directory});
        };
        for (const QString& root : roots) {
            if (stale()) return;
            const QFileInfo info(root);
            watch(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
            if (!info.exists()) continue;
            record(info);
            if (info.isDir() && info.suffix().compare(QLatin1String(ui::projecttemplates::kExtension), Qt::CaseInsensitive) != 0)
                pending.enqueue({info.absoluteFilePath(), 0});
        }
        while (!pending.isEmpty() && visited < kMaxVisited) {
            if (stale()) return;
            const auto [directory, depth] = pending.dequeue();
            const QString key = identity(directory);
            if (scanned.contains(key)) continue;
            scanned.insert(key);
            watch(directory);
            QDirIterator it(directory, QDir::AllEntries | QDir::NoDotAndDotDot);
            while (it.hasNext()) {
                it.next();
                if (++visited > kMaxVisited) { index.truncated = true; break; }
                if ((visited & 0xFF) == 0 && stale()) return;
                const QFileInfo info = it.fileInfo();
                record(info);
                if (info.isDir() && info.suffix().compare(QLatin1String(ui::projecttemplates::kExtension), Qt::CaseInsensitive) != 0 && !info.isSymLink()) {
                    if (depth < kMaxDepth) pending.enqueue({info.absoluteFilePath(), depth + 1});
                    else index.truncated = true;
                }
                // Publish useful names early, then batch snapshots.
                if (index.entries.size() == 1 || delivery.elapsed() >= 120) publish();
            }
        }
        if (stale()) return;
        if (!pending.isEmpty()) index.truncated = true;
        index.finished = true;
        publish();
    });
}

void FileSearchWorker::acceptIndex(std::shared_ptr<const Index> index) {
    m_index = std::move(index);
    if (m_index->finished) {
        m_indexAge.restart();
        m_watchIncomplete = m_index->watchIncomplete;
        const auto old = m_watcher->directories();
        const QSet<QString> oldSet(old.cbegin(), old.cend());
        const QSet<QString> newSet(m_index->directories.cbegin(), m_index->directories.cend());
        QStringList remove, add;
        for (const auto& path : old) if (!newSet.contains(path)) remove.append(path);
        for (const auto& path : m_index->directories) if (!oldSet.contains(path)) add.append(path);
        if (!remove.isEmpty()) m_watcher->removePaths(remove);
        const quint64 generation = m_scanGeneration->load(std::memory_order_acquire);
        // Installing thousands of watches must not stall a keystroke.
        for (qsizetype i = 0; i < add.size(); i += 64) {
            const auto batch = add.mid(i, 64);
            QTimer::singleShot(int(i / 64), this, [this, batch, generation] {
                if (m_scanGeneration->load(std::memory_order_acquire) == generation &&
                    !m_watcher->addPaths(batch).isEmpty()) m_watchIncomplete = true;
            });
        }
    }
    runQuery();
}

void FileSearchWorker::search(const QStringList& roots, const QString& query,
                              const QStringList& ignoredExtensions) {
    cancel();
    m_query = query.trimmed();
    if (m_query.isEmpty()) return;
    m_queryRoots = normalizedRoots(roots);
    m_ignoredExtensions = ignoredExtensions;
    auto indexedRoots = m_roots;
    for (const auto& root : m_queryRoots) {
        if (std::none_of(m_roots.cbegin(), m_roots.cend(), [&root](const auto& indexed) {
                return withinRoot(root, indexed);
            })) indexedRoots.append(root);
    }
    if (!m_queryRoots.isEmpty() && (!m_index || indexedRoots != m_roots))
        setRoots(indexedRoots);
    // If the OS could not watch the entire library, periodically revalidate it.
    else if (m_watchIncomplete && m_indexAge.isValid() && m_indexAge.elapsed() > 30000)
        refresh();
    runQuery();
}

void FileSearchWorker::runQuery() {
    const quint64 generation = m_generation->fetch_add(1, std::memory_order_release) + 1;
    if (m_query.isEmpty()) return;
    if (m_queryRoots.isEmpty()) { emit results({}, false, true); return; }
    if (!m_index) return;
    const auto index = m_index;
    const auto roots = m_queryRoots;
    const auto ignored = m_ignoredExtensions;
    const Query query(m_query);
    auto counter = m_generation;
    QPointer<FileSearchWorker> self(this);
    QThreadPool::globalInstance()->start([index, roots, ignored, query, generation, counter, self] {
        QStringList found;
        bool truncated = index->truncated;
        int visited = 0;
        for (const auto& entry : index->entries) {
            if ((++visited & 0xFF) == 0 && counter->load(std::memory_order_acquire) != generation) return;
            if (ignored.contains(entry.suffix) || !query.matches(entry.name, entry.suffix)) continue;
            const bool included = std::any_of(roots.cbegin(), roots.cend(), [&entry](const auto& root) {
                return entry.directory ? entry.path.compare(root, kPathCase) == 0 : withinRoot(entry.path, root);
            });
            if (!included) continue;
            if (found.size() == kMaxMatches) { truncated = true; break; }
            found.append(entry.path);
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, counter, generation, found, truncated, finished = index->finished] {
            if (self && counter->load(std::memory_order_acquire) == generation)
                emit self->results(found, truncated, finished);
        }, Qt::QueuedConnection);
    });
}
