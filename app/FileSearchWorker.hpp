#pragma once

#include <QObject>
#include <QStringList>
#include <QElapsedTimer>
#include <atomic>
#include <memory>

class QFileSystemWatcher;
class QTimer;

/// A bounded filename index, built independently of the current search.
/// Typing only filters immutable snapshots on a worker thread. Cancelling a
/// query never throws away the disk walk already in progress.
class FileSearchWorker : public QObject {
    Q_OBJECT
public:
    explicit FileSearchWorker(QObject* parent = nullptr);
    ~FileSearchWorker() override;
    /// Warm the index before the user starts typing. Unchanged roots are free.
    void setRoots(const QStringList& roots);
    void refresh();
    void search(const QStringList& roots, const QString& query,
                const QStringList& ignoredExtensions = {});
    void cancel();
    /// Case-insensitive words (in any order), or an exact .wav / *.wav suffix.
    static bool matches(const QString& fileName, const QString& query);
    quint64 indexBuildCountForTest() const { return m_indexBuilds; }
    bool indexReadyForTest() const;
signals:
    /// Cumulative results, including matches available during initial indexing.
    void results(const QStringList& paths, bool truncated, bool finished);
private:
    struct Index;
    void runQuery();
    void acceptIndex(std::shared_ptr<const Index> index);
    QStringList m_roots, m_queryRoots, m_ignoredExtensions;
    QString m_query;
    std::shared_ptr<const Index> m_index;
    std::shared_ptr<std::atomic<quint64>> m_generation, m_scanGeneration;
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QElapsedTimer m_indexAge;
    bool m_watchIncomplete = false;
    quint64 m_indexBuilds = 0;
};
