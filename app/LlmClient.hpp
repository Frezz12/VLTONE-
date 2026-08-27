#pragma once

#include "ai/AiWire.hpp"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QNetworkReply;

namespace ui {

/// Everything a provider needs beyond the conversation itself.
struct LlmConfig {
    enum class Transport { Managed, Direct };
    Transport transport = Transport::Managed;
    QString accessToken;
    QString connectionId;
    QString displayName;
    QString endpoint;
    QString apiKey;
    QString model;
    int maxTokens = 8192;
    /// Show the answer as it is written rather than when it is finished.
    bool stream = true;
};

/// One request to a language model.
///
/// Only the HTTP lives here: the URL, the headers, the socket, and feeding
/// bytes to the decoder. Both providers' request and response shapes are pure
/// JSON and live in `daw::ai::wire`, where a test can drive them without a
/// network — which is the only way streaming could be checked at all.
class LlmClient : public QObject {
    Q_OBJECT
public:
    using Provider = daw::ai::wire::Provider;
    using Reply = std::function<void(daw::ai::ModelReply)>;
    /// Prose as it arrives, when streaming. Never called for a whole answer.
    using PartialSink = std::function<void(const QString& text)>;
    using UsageSink = std::function<void(daw::ai::AiSession::Usage)>;

    LlmClient(Provider provider, QObject* parent = nullptr);
    ~LlmClient() override;

    void setConfig(LlmConfig config) { m_config = std::move(config); }
    const LlmConfig& config() const { return m_config; }
    Provider provider() const { return m_provider; }

    void setPartialSink(PartialSink sink) { m_partialSink = std::move(sink); }
    void setUsageSink(UsageSink sink) { m_usageSink = std::move(sink); }

    /// Human-readable, for the panel's header and error messages.
    QString displayName() const;
    /// Used when the user has not named one.
    QString defaultModel() const;
    /// Send the conversation. `onReply` runs on the GUI thread, exactly once,
    /// unless `cancel()` got there first.
    ///
    /// Virtual so a headless check can stand in a client that answers from a
    /// script instead of the network — that is what lets `--selftest` drive the
    /// whole panel → session → document path without a key or a connection.
    virtual void send(const QString& system,
                      const std::vector<daw::ai::Message>& messages,
                      Reply onReply);

    /// Abandon the request in flight. The callback will not run.
    virtual void cancel();

    virtual bool busy() const;

protected:
    LlmConfig m_config;

private:
    void requestManagedLease(const QString& system,
                             const std::vector<daw::ai::Message>& messages,
                             bool stream);
    void sendToProvider(const QString& endpoint, const QString& apiKey,
                        const nlohmann::json& body, bool stream);
    void settleManaged(qint64 actualTokens, const QString& outcome,
                       std::function<void()> completed = {});
    qint64 usageTokens(const daw::ai::AiSession::Usage& usage) const;
    /// Deliver, taking the callback first so a callback that starts the next
    /// request cannot be clobbered by this one finishing.
    void answer(daw::ai::ModelReply reply);

    Provider m_provider;
    QNetworkAccessManager m_net;
    QPointer<QNetworkReply> m_inFlight;
    Reply m_onReply;
    PartialSink m_partialSink;
    UsageSink m_usageSink;
    std::unique_ptr<daw::ai::wire::StreamDecoder> m_decoder;
    QString m_reservationId;
    qint64 m_reservedTokens = 0;
};

} // namespace ui
