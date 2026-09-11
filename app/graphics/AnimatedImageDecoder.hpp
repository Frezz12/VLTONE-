#pragma once
#include <QObject>
#include <QTimer>
#include <QImage>
#include <memory>
namespace ui::graphics {
// One in-flight decode and one retained image per source. No QMovie timer or
// image decoding runs in the GUI thread, and no catch-up queue accumulates.
class AnimatedImageDecoder final : public QObject {
    Q_OBJECT
public:
    explicit AnimatedImageDecoder(QObject* parent = nullptr);
    void setSource(const QString& path);
    void setPlaying(bool playing);
signals:
    void frameReady(const QImage& image);
private:
    void requestFrame();
    struct State;
    std::shared_ptr<State> m_state;
    QTimer m_timer;
    quint64 m_generation = 0;
    bool m_busy = false, m_playing = false, m_haveFrame = false;
};
}
