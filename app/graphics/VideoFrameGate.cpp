#include "VideoFrameGate.hpp"
#include <algorithm>
namespace ui::graphics {
struct VideoFrameGate::Mailbox {
    std::mutex mutex;
    QVideoFrame latest;
    VideoFrameGate* owner = nullptr; // accessed only under mutex
    bool queued = false;
};
VideoFrameGate::VideoFrameGate(QObject* parent) : QObject(parent), m_mailbox(std::make_shared<Mailbox>()) {
    m_mailbox->owner = this;
    m_time.start();
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &VideoFrameGate::deliver);
    m_connection = connect(&m_input, &QVideoSink::videoFrameChanged, this, [mailbox = m_mailbox](const QVideoFrame& frame) {
        std::lock_guard lock(mailbox->mutex);
        if (!mailbox->owner) return;
        mailbox->latest = frame;
        if (!mailbox->queued) {
            mailbox->queued = true;
            QMetaObject::invokeMethod(mailbox->owner, &VideoFrameGate::deliver, Qt::QueuedConnection);
        }
    }, Qt::DirectConnection);
}
VideoFrameGate::~VideoFrameGate() {
    disconnect(m_connection);
    // An already-running direct connection owns only this shared mailbox.
    // After this fence it cannot access the QObject or enqueue another call.
    std::lock_guard lock(m_mailbox->mutex);
    m_mailbox->owner = nullptr;
}
void VideoFrameGate::setTarget(QVideoSink* target) {
    if (target == m_target) return;
    m_target = target;
    m_input.setRhi(target ? target->rhi() : nullptr);
    emit targetChanged();
}
void VideoFrameGate::setFrameLimit(int limit) {
    limit = std::max(0, limit);
    if (m_limit == limit) return;
    m_limit = limit; m_lastNs = 0; emit frameLimitChanged();
}
void VideoFrameGate::deliver() {
    const qint64 now = m_time.nsecsElapsed();
    const qint64 period = m_limit ? 1000000000LL / m_limit : 0;
    if (m_lastNs && period && now - m_lastNs < period) {
        m_timer.start(int((period - (now - m_lastNs) + 999999) / 1000000));
        return;
    }
    QVideoFrame frame;
    {
        std::lock_guard lock(m_mailbox->mutex);
        frame = std::move(m_mailbox->latest);
        m_mailbox->queued = false;
    }
    if (!m_target) return;
    // VideoOutput establishes the window's RHI after scene-graph creation and
    // again after device loss. Forward the same RHI to the decoder sink.
    if (m_input.rhi() != m_target->rhi()) m_input.setRhi(m_target->rhi());
    m_target->setVideoFrame(frame);
    m_lastNs = frame.isValid() ? now : 0;
    if (frame.isValid()) emit framePresented();
}
}
