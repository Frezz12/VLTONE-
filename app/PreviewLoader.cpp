#include "PreviewLoader.hpp"
#include "SampleLoader.hpp"
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <mutex>

struct PreviewLoader::State {
    std::mutex mutex;
    QString latest;
    std::atomic<quint64> generation{0};
    bool running = false;
};
PreviewLoader::PreviewLoader(QObject* parent)
    : QObject(parent), m_state(std::make_shared<State>()) {}
PreviewLoader::~PreviewLoader() { cancel(); }
void PreviewLoader::cancel() {
    const std::lock_guard lock(m_state->mutex);
    ++m_state->generation;
    m_state->latest.clear();
}
void PreviewLoader::request(const QString& path) {
    const auto state = m_state;
    {
        const std::lock_guard lock(state->mutex);
        ++state->generation;
        state->latest = path;
        if (state->running || path.isEmpty()) return;
        state->running = true;
    }
    QPointer<PreviewLoader> self(this);
    // Exactly one job per loader. Arrow-key bursts replace the pending path;
    // the current decoder checks its generation between small blocks.
    QThreadPool::globalInstance()->start([state, self] {
        for (;;) {
            QString path;
            quint64 generation;
            {
                const std::lock_guard lock(state->mutex);
                if (state->latest.isEmpty()) { state->running = false; return; }
                path = std::exchange(state->latest, {});
                generation = state->generation.load();
            }
            const auto current = [&] { return state->generation.load() == generation; };
            std::shared_ptr<const daw::engine::SampleBuffer> audio;
            daw::WaveformPeaks peaks;
            QString reason;
            try {
                const auto result = daw::loadSampleBuffer(path.toStdString(), audio, current);
                if (!current()) continue;
                if (result) daw::buildPeaks(*audio, peaks, current);
                else reason = QString::fromStdString(result.message());
            } catch (const std::exception& error) {
                reason = QString::fromUtf8(error.what());
                audio.reset();
            }
            if (!current()) continue;
            // Resolve QPointer on the UI thread; never dereference a QObject
            // whose destructor could be running concurrently with this worker.
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [self, state, generation, path, audio = std::move(audio),
                 peaks = std::move(peaks), reason]() mutable {
                    if (!self || state->generation.load() != generation) return;
                    if (!reason.isEmpty()) emit self->failed(path, reason);
                    else emit self->loaded(path, std::move(audio), std::move(peaks));
                }, Qt::QueuedConnection);
        }
    });
}
