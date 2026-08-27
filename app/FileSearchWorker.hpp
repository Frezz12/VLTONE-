#pragma once

#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

/// Searches the browser's folders by name or extension, off the UI thread.
///
/// A sample library is tens of thousands of files on a slow disk, so the walk
/// cannot run where the interface does. It is also bounded in three directions
/// — matches, entries visited and depth — because an unbounded recursive search
/// of a home folder is a hang however many threads it uses.
///
/// Requests are generation-counted: typing produces a request per keystroke,
/// and everything but the newest is dropped when it lands.
class FileSearchWorker : public QObject {
    Q_OBJECT
public:
    explicit FileSearchWorker(QObject* parent = nullptr);
    ~FileSearchWorker() override;

    /// Start a search. An empty query cancels whatever is running and emits
    /// nothing — the caller shows the tree again.
    void search(const QStringList& roots, const QString& query);
    /// Abandon the running search; its results will be dropped when they land.
    void cancel();

    /// Does this name match the query? A leading "." or "*." makes it an
    /// extension test, so both `wav` and `.wav` find WAV files, and any other
    /// text matches anywhere in the name. Public because the panel uses the
    /// same rule to describe what it searched for.
    static bool matches(const QString& fileName, const QString& query);

signals:
    /// One finished search. `truncated` says the cap was hit, so the panel can
    /// admit the list is partial instead of implying it is everything.
    void results(const QStringList& paths, bool truncated);

private:
    /// Bumped per request; a result carrying an older number is stale.
    std::shared_ptr<std::atomic<quint64>> m_generation;
};
