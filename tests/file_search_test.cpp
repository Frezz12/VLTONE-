#include "FileSearchWorker.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <cstdio>

namespace {
template<class F> bool waitFor(F predicate, int timeout = 10000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return predicate();
}
bool touch(const QString& path) { QFile file(path); return file.open(QIODevice::WriteOnly); }
}

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir fixture;
    CHECK(fixture.isValid());
    QDir root(fixture.path());
    CHECK(root.mkpath("Samples/deep/Template.vltt"));
    CHECK(root.mkpath("Samples-other"));
    for (const QString& file : {QStringLiteral("Samples/Kick 808.WAV"), QStringLiteral("Samples/Kick.wav.ASD"),
         QStringLiteral("Samples/deep/Bass.flac"), QStringLiteral("Samples/deep/Template.vltt/Project.vlt"),
         QStringLiteral("Samples-other/Kick.wav")}) CHECK(touch(root.filePath(file)));
    for (int i = 0; i < 12000; ++i) CHECK(touch(root.filePath(QStringLiteral("Samples/deep/Hit_%1.wav").arg(i))));

    FileSearchWorker worker;
    QStringList paths;
    bool finished = false, truncated = false;
    QObject::connect(&worker, &FileSearchWorker::results, &app, [&](const auto& found, bool capped, bool done) {
        if (done) { paths = found; truncated = capped; finished = true; }
    });
    QElapsedTimer cold; cold.start();
    worker.setRoots({fixture.path(), root.filePath("Samples")});
    worker.search({root.filePath("Samples")}, "kick");
    worker.cancel(); // A keystroke must not cancel the index build.
    CHECK(waitFor([&] { return worker.indexReadyForTest(); }));
    const auto coldMs = cold.elapsed();
    CHECK(worker.indexBuildCountForTest() == 1);
    const auto query = [&](const QString& text, const QStringList& scope, const QStringList& ignored = {}) {
        finished = false;
        worker.search(scope, text, ignored);
        return waitFor([&] { return finished; });
    };
    const QStringList samples{root.filePath("Samples")};
    QElapsedTimer warm; warm.start();
    CHECK(query("808 kick", samples));
    CHECK(paths == QStringList{root.filePath("Samples/Kick 808.WAV")});
    CHECK(query("*.FLAC", samples));
    CHECK(paths == QStringList{root.filePath("Samples/deep/Bass.flac")});
    CHECK(query("kick", samples, {"asd"}));
    CHECK(paths == QStringList{root.filePath("Samples/Kick 808.WAV")});
    CHECK(query(".asd", samples));
    CHECK(paths == QStringList{root.filePath("Samples/Kick.wav.ASD")});
    CHECK(query("Project.vlt", samples)); CHECK(paths.isEmpty());
    CHECK(query(".vltt", samples));
    CHECK(paths == QStringList{root.filePath("Samples/deep/Template.vltt")});
    CHECK(query("bass", {root.filePath("Samples/deep")})); CHECK(paths.size() == 1);
    CHECK(query("kick", {root.filePath("Samples/Kick 808.WAV")})); CHECK(paths.size() == 1);
    CHECK(query("Hit_", samples)); CHECK(paths.size() == 2000 && truncated);
    worker.search(samples, "Hit_");
    CHECK(query("808 kick", samples)); CHECK(paths.size() == 1 && !truncated);
    CHECK(worker.indexBuildCountForTest() == 1);
    const auto warmMs = warm.elapsed();

    // Let the queued watch installation finish before changing a directory.
    QCoreApplication::processEvents();
    const auto added = root.filePath("Samples/deep/New Needle.wav");
    CHECK(query("new needle", samples)); CHECK(paths.isEmpty());
    finished = false;
    CHECK(touch(added));
    CHECK(waitFor([&] { return finished && paths == QStringList{added}; }));
    finished = false;
    CHECK(QFile::rename(added, root.filePath("Samples/deep/Renamed Needle.wav")));
    CHECK(waitFor([&] { return finished && paths.isEmpty(); }));
    CHECK(query("renamed needle", samples)); CHECK(paths.size() == 1);
    finished = false;
    CHECK(QFile::remove(paths.first()));
    CHECK(waitFor([&] { return finished && paths.isEmpty(); }));
    CHECK(query("anything", {})); CHECK(paths.isEmpty() && !truncated);

    // Destroying a worker with pending indexing/filtering cannot call into it.
    { FileSearchWorker temporary; temporary.setRoots(samples); temporary.search(samples, "Hit"); }
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    std::fprintf(stderr, "PASS: 12,005 files; cold index %lld ms; 10 cached queries %lld ms, one disk walk; scope, cancellation, packages, cap, create/rename/delete\n", coldMs, warmMs);
    return 0;
}
