#include "MidiPreviewLoader.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include <atomic>
#include <mutex>
#include <utility>

struct MidiPreviewLoader::State {
    std::mutex mutex;
    QString latest;
    std::atomic<quint64> generation{0};
    bool running = false;
};

MidiPreviewLoader::MidiPreviewLoader(QObject* parent)
    : QObject(parent), m_state(std::make_shared<State>()) {}

MidiPreviewLoader::~MidiPreviewLoader() { cancel(); }

void MidiPreviewLoader::cancel() {
    const std::lock_guard lock(m_state->mutex);
    ++m_state->generation;
    m_state->latest.clear();
}

void MidiPreviewLoader::request(const QString& path) {
    const auto state = m_state;
    {
        const std::lock_guard lock(state->mutex);
        ++state->generation;
        state->latest = path;
        if (state->running || path.isEmpty()) return;
        state->running = true;
    }

    QPointer<MidiPreviewLoader> self(this);
    QThreadPool::globalInstance()->start([state, self] {
        for (;;) {
            QString path;
            quint64 generation = 0;
            {
                const std::lock_guard lock(state->mutex);
                if (state->latest.isEmpty()) {
                    state->running = false;
                    return;
                }
                path = std::exchange(state->latest, {});
                generation = state->generation.load();
            }

            auto file = std::make_shared<daw::midifile::File>();
            std::string error;
            const bool ok = daw::midifile::parse(path.toStdString(), *file, error);
            if (state->generation.load() != generation) continue;

            const QString reason = QString::fromStdString(error);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, state, generation, path, file = std::move(file), ok,
                 reason]() mutable {
                    if (!self || state->generation.load() != generation) return;
                    if (ok) emit self->loaded(path, std::move(file));
                    else emit self->failed(path, reason);
                },
                Qt::QueuedConnection);
        }
    });
}
