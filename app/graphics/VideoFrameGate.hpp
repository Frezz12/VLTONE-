#pragma once
#include <QObject>
#include <QPointer>
#include <QVideoSink>
#include <QVideoFrame>
#include <QTimer>
#include <QElapsedTimer>
#include <mutex>
#include <memory>

namespace ui::graphics {
// The decoder can outrun the display. Keep one latest hardware frame and give
// VideoOutput only frames that will be displayed. No image conversion/readback.
class VideoFrameGate : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVideoSink* videoSink READ videoSink CONSTANT)
    Q_PROPERTY(QVideoSink* target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(int frameLimit READ frameLimit WRITE setFrameLimit NOTIFY frameLimitChanged)
public:
    explicit VideoFrameGate(QObject* parent = nullptr);
    ~VideoFrameGate() override;
    Q_INVOKABLE QVideoSink* videoSink() { return &m_input; }
    QVideoSink* target() const { return m_target; }
    void setTarget(QVideoSink*);
    int frameLimit() const { return m_limit; }
    void setFrameLimit(int);
signals:
    void targetChanged();
    void frameLimitChanged();
    void framePresented();
private:
    void deliver();
    QVideoSink m_input;
    QPointer<QVideoSink> m_target;
    struct Mailbox;
    std::shared_ptr<Mailbox> m_mailbox;
    QTimer m_timer;
    QElapsedTimer m_time;
    qint64 m_lastNs = 0;
    int m_limit = 0;
    QMetaObject::Connection m_connection;
};
}
