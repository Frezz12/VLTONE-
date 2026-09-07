#pragma once

#include "WebVideoSource.hpp"
#include <QImage>
#include <QObject>
#include <QTimer>

namespace ui {

// Owns the background browser independently of every browser tab. The lazy
// profile outlives both the panel's pages and the hidden background players.
class WebVideoBackground final : public QObject {
    Q_OBJECT
public:
    explicit WebVideoBackground(QObject* parent = nullptr,
                                QWebEngineProfile* profile = nullptr);
    ~WebVideoBackground() override;
    QWebEngineProfile* browserProfile();
    void request(const WebVideoSource& source);
    void restore();
    void clear();
    void reloadSettings();
    void setPlaying(bool playing);
    void setMuted(bool muted);
    bool active() const { return m_active != nullptr; }
    bool loading() const { return m_pending != nullptr; }
    bool muted() const { return m_muted; }
    QUrl sourceUrl() const;

signals:
    void frameReady(const QImage& frame, quint64 sourceId);
    void cleared();
    void stateChanged();
    void sourceCommitted(const ui::WebVideoSource& original);
    void error(const QString& message);

private:
    struct Session;
    void tick();
    void inspect(Session* session);
    void prepare(Session* session, const WebVideoSource& candidate);
    void poll(Session* session);
    void fail(Session* session, const QString& reason);
    void savePosition();
    bool owns(Session* session) const;
    QWebEngineProfile* m_profile = nullptr;
    bool m_ownsProfile = false;
    Session* m_pending = nullptr;
    Session* m_active = nullptr;
    QTimer m_timer;
    QTimer m_saveTimer;
    quint64 m_nextId = 0;
    quint64 m_sourceRevision = 0;
    bool m_playing = false;
    bool m_muted = true;
    QUrl m_lastRequestedUrl;
};

} // namespace ui
