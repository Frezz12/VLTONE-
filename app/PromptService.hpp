#pragma once

#include "ai/Prompts.hpp"

#include <QDateTime>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace ui {

/// Where the assistant's instructions come from.
///
/// The main prompt and the playbooks are edited in the admin panel and served
/// by the backend, so "teach it to write better bass" is a text edit and a save
/// rather than a release. This is the client half of that: fetch, validate,
/// cache, and hand the result to the chat panel.
///
/// Three sources, in this order — whichever is available first wins, and the
/// assistant is never left without instructions:
///   1. the server, on sign-in and every few hours after;
///   2. the last good pack, cached in the application's data folder;
///   3. the text compiled into this build from `prompts/`.
///
/// A pack that fails validation is *discarded*, not adopted: half a prompt is
/// worse than yesterday's whole one.
class PromptService final : public QObject {
    Q_OBJECT
public:
    enum class Source { Builtin, Cache, Server };

    static PromptService* instance();

    explicit PromptService(QObject* parent = nullptr);
    ~PromptService() override;

    /// The instructions in force. Never empty.
    const daw::ai::PromptPack& pack() const { return m_pack; }
    Source source() const { return m_source; }
    QString version() const { return QString::fromStdString(m_pack.version); }
    QDateTime lastCheckedAt() const { return m_lastCheckedAt; }
    /// Why the last fetch did not land, for the line in Settings. Empty when
    /// the last one was fine.
    QString lastError() const { return m_lastError; }
    bool busy() const;

    /// Load the cached pack and ask the server for a newer one. Safe to call
    /// before sign-in: with no token it simply stays on the cache.
    void start();

    /// Ask now — the button in Settings, and what sign-in triggers.
    void refresh();

signals:
    /// The text changed. The panel re-reads `pack()`; nothing else needs to.
    void packChanged();
    void stateChanged();

private:
    void loadCache();
    void saveCache(const QByteArray& body, const QString& etag);
    QString cachePath() const;
    void adopt(daw::ai::PromptPack pack, Source source);
    void finish(QNetworkReply* reply);

    static PromptService* s_instance;
    QNetworkAccessManager* m_network = nullptr;
    QTimer* m_timer = nullptr;
    QNetworkReply* m_inFlight = nullptr;
    daw::ai::PromptPack m_pack;
    Source m_source = Source::Builtin;
    QString m_etag;
    QDateTime m_lastCheckedAt;
    QString m_lastError;
};

} // namespace ui
