#include "AnimatedImageDecoder.hpp"
#include <QImageReader>
#include <QThreadPool>
#include <QPointer>
#include <QCoreApplication>
#include <QElapsedTimer>
namespace ui::graphics {
struct AnimatedImageDecoder::State {
    QString path;
    std::unique_ptr<QImageReader> reader; // accessed exclusively by the decode job
};
AnimatedImageDecoder::AnimatedImageDecoder(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &AnimatedImageDecoder::requestFrame);
}
void AnimatedImageDecoder::setSource(const QString& path) {
    ++m_generation;
    m_timer.stop();
    m_haveFrame = false;
    m_state = std::make_shared<State>();
    m_state->path = path;
    requestFrame();
}
void AnimatedImageDecoder::setPlaying(bool playing) {
    m_playing = playing;
    if (!playing) m_timer.stop();
    else if (!m_busy && !m_timer.isActive()) requestFrame();
}
void AnimatedImageDecoder::requestFrame() {
    if (m_busy || !m_state || m_state->path.isEmpty() || (m_haveFrame && !m_playing)) return;
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(2); pool.setThreadPriority(QThread::LowPriority); pool.setExpiryTimeout(5000); return true; }();
    Q_UNUSED(configured);
    m_busy = true;
    const auto state = m_state;
    const auto generation = m_generation;
    QPointer<AnimatedImageDecoder> guard(this);
    pool.start([state, generation, guard] {
        QElapsedTimer elapsed; elapsed.start();
        if (!state->reader || !state->reader->canRead()) {
            state->reader = std::make_unique<QImageReader>(state->path);
            // Keep a malformed/high-resolution animation from exhausting the
            // process. Qt's reader allocation limit provides the second bound.
            const QSize original = state->reader->size();
            if (qint64(original.width()) * original.height() > 16 * 1024 * 1024)
                state->reader->setScaledSize(original.scaled(4096, 4096, Qt::KeepAspectRatio));
        }
        const QImage image = state->reader->read();
        const int delay = std::max(1, std::max(10, state->reader->nextImageDelay()) - int(elapsed.elapsed()));
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, generation, image, delay] {
            if (!guard) return;
            guard->m_busy = false;
            if (generation != guard->m_generation) { guard->requestFrame(); return; }
            if (image.isNull()) return;
            guard->m_haveFrame = true;
            emit guard->frameReady(image);
            if (guard && guard->m_playing) guard->m_timer.start(delay);
        }, Qt::QueuedConnection);
    });
}
}
