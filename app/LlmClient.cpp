#include "LlmClient.hpp"
#include "AccountService.hpp"

#include <QNetworkReply>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <utility>

using json = nlohmann::json;
namespace ai = daw::ai;
namespace wire = daw::ai::wire;

namespace ui {

namespace {

/// Long enough for a model that is thinking, short enough that a dead endpoint
/// does not leave the panel spinning forever.
constexpr int kTimeoutMs = 180'000;
constexpr int kAccountTimeoutMs = 15'000;

std::string toStd(const QString& s) { return s.toStdString(); }

QString fromJsonString(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) return {};
    return QString::fromStdString(found->get<std::string>());
}

qint64 fromJsonInteger(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number_integer()) return 0;
    return found->get<qint64>();
}

QUrl resolvedEndpoint(const QString& raw, LlmClient::Provider provider) {
    QUrl url(raw.trimmed());
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);

    const QString suffix =
        provider == LlmClient::Provider::Anthropic
            ? QStringLiteral("/messages")
            : QStringLiteral("/chat/completions");
    if (!path.endsWith(suffix)) {
        if (path.isEmpty()) path = QStringLiteral("/v1");
        path += suffix;
    }
    url.setPath(path);
    return url;
}

} // namespace

LlmClient::LlmClient(Provider provider, QObject* parent)
    : QObject(parent), m_provider(provider) {
    m_net.setTransferTimeout(kTimeoutMs);
}

LlmClient::~LlmClient() { cancel(); }

bool LlmClient::busy() const { return !m_inFlight.isNull(); }

QString LlmClient::displayName() const {
    if (!m_config.displayName.isEmpty()) return m_config.displayName;
    return m_provider == Provider::Anthropic ? QStringLiteral("Claude")
                                             : QStringLiteral("GPT");
}

QString LlmClient::defaultModel() const {
    return m_provider == Provider::Anthropic
               ? QStringLiteral("claude-sonnet-4-5")
               : QStringLiteral("gpt-4o");
}

void LlmClient::answer(ai::ModelReply reply) {
    // Taken by move first: the callback may start the next request, and that
    // must not be clobbered by this one finishing.
    Reply callback = std::move(m_onReply);
    m_onReply = nullptr;
    m_decoder.reset();
    if (callback) callback(std::move(reply));
}

void LlmClient::send(const QString& system,
                     const std::vector<ai::Message>& messages, Reply onReply) {
    cancel();
    m_onReply = std::move(onReply);

    const bool managed = m_config.transport == LlmConfig::Transport::Managed;
    auto* account = account::Service::instance();
    if (managed && (!account || !account->authenticated() ||
                    m_config.accessToken.isEmpty() ||
                    m_config.connectionId.isEmpty())) {
        ai::ModelReply reply;
        reply.error = "VLT account authorization is unavailable. Sign in again.";
        answer(std::move(reply));
        return;
    }
    const QUrl directUrl(m_config.endpoint);
    if (!managed && (!directUrl.isValid() || directUrl.host().isEmpty() ||
                     (directUrl.scheme() != QLatin1String("http") &&
                      directUrl.scheme() != QLatin1String("https")))) {
        ai::ModelReply reply;
        reply.error = "The custom model endpoint is invalid. Check AI settings.";
        answer(std::move(reply));
        return;
    }

    const bool stream = m_config.stream;
    if (managed) {
        requestManagedLease(system, messages, stream);
        return;
    }

    const QString model =
        m_config.model.isEmpty() ? defaultModel() : m_config.model;
    const json body = wire::requestBody(m_provider, toStd(model),
                                        m_config.maxTokens, toStd(system),
                                        messages, stream,
                                        /*vendorExtensions=*/false,
                                        &m_availableTools);
    sendToProvider(resolvedEndpoint(m_config.endpoint, m_provider).toString(),
                   m_config.apiKey, body, stream);
}

void LlmClient::requestManagedLease(
    const QString& system, const std::vector<ai::Message>& messages,
    bool stream) {
    auto* account = account::Service::instance();
    if (!account) return;

    // The server does not receive the conversation. It needs only a
    // conservative byte count to reserve enough quota before releasing the
    // one-request credential.
    const QString provisionalModel =
        m_config.model.isEmpty() ? defaultModel() : m_config.model;
    const json provisional = wire::requestBody(
        m_provider, toStd(provisionalModel), m_config.maxTokens, toStd(system),
        messages, stream, /*vendorExtensions=*/true, &m_availableTools);
    const QByteArray providerBytes = QByteArray::fromStdString(provisional.dump());
    const json leaseBody = {
        {"input_bytes", std::max<qint64>(1, providerBytes.size())},
        {"max_output_tokens", m_config.maxTokens},
    };

    QNetworkRequest request(
        QUrl(account->apiOrigin() + QStringLiteral("/desktop/ai/models/") +
             m_config.connectionId + QStringLiteral("/lease")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization",
                         "Bearer " + m_config.accessToken.toUtf8());
    request.setTransferTimeout(kAccountTimeoutMs);

    QNetworkReply* http = m_net.post(
        request, QByteArray::fromStdString(leaseBody.dump()));
    m_inFlight = http;
    connect(http, &QNetworkReply::finished, this,
            [this, http, system, messages, stream] {
        const QByteArray payload = http->readAll();
        const int status =
            http->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool failed =
            http->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300;
        const QString transportError = http->errorString();
        http->deleteLater();
        if (m_inFlight != http) return;
        m_inFlight = nullptr;

        const json parsed = json::parse(payload.constData(),
                                        payload.constData() + payload.size(),
                                        nullptr, /*allow_exceptions=*/false);
        if (failed || parsed.is_discarded() || !parsed.is_object()) {
            QString detail;
            if (!parsed.is_discarded())
                detail = QString::fromStdString(
                    wire::errorMessage(m_provider, parsed));
            if (detail.isEmpty()) detail = transportError;
            if (detail.isEmpty()) detail = QStringLiteral("authorization failed");
            if (auto* account = account::Service::instance()) {
                if (status == 404) account->refreshAiModels();
                if (status == 402 || status == 503) account->refreshQuota();
            }
            ai::ModelReply reply;
            reply.error = toStd(displayName() + QStringLiteral(": ") + detail +
                                (status ? QStringLiteral(" (HTTP %1)").arg(status)
                                        : QString()));
            answer(std::move(reply));
            return;
        }

        m_reservationId = fromJsonString(parsed, "reservation_id");
        m_reservedTokens = fromJsonInteger(parsed, "reserved_tokens");
        const QString provider = fromJsonString(parsed, "provider");
        const QString expected = m_provider == Provider::Anthropic
                                     ? QStringLiteral("anthropic")
                                     : QStringLiteral("openai");
        const QString model = fromJsonString(parsed, "model");
        const QString endpoint = fromJsonString(parsed, "endpoint_url");
        const QString apiKey = fromJsonString(parsed, "api_key");
        const QUrl endpointUrl(endpoint);
        if (m_reservationId.isEmpty() || m_reservedTokens <= 0 ||
            provider != expected || model.isEmpty() || apiKey.isEmpty() ||
            !endpointUrl.isValid() || endpointUrl.host().isEmpty() ||
            (endpointUrl.scheme() != QLatin1String("http") &&
             endpointUrl.scheme() != QLatin1String("https"))) {
            settleManaged(m_reservedTokens, QStringLiteral("conservative_no_usage"));
            if (auto* account = account::Service::instance())
                account->refreshAiModels();
            ai::ModelReply reply;
            reply.error = toStd(displayName()) +
                          " received an invalid model authorization response";
            answer(std::move(reply));
            return;
        }

        const json body = wire::requestBody(
            m_provider, toStd(model), m_config.maxTokens, toStd(system),
            messages, stream, /*vendorExtensions=*/true, &m_availableTools);
        sendToProvider(endpoint, apiKey, body, stream);
    });
}

void LlmClient::sendToProvider(const QString& endpoint, const QString& apiKey,
                               const json& body, bool stream) {
    const QUrl url(endpoint);
    if (!url.isValid() || url.host().isEmpty()) {
        settleManaged(m_reservedTokens, QStringLiteral("transport_failure"));
        ai::ModelReply reply;
        reply.error = toStd(displayName()) + " has an invalid provider endpoint";
        answer(std::move(reply));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!apiKey.isEmpty()) {
        if (m_provider == Provider::Anthropic)
            request.setRawHeader("x-api-key", apiKey.toUtf8());
        else
            request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    }
    if (m_provider == Provider::Anthropic)
        request.setRawHeader("anthropic-version", "2023-06-01");

    QNetworkReply* http =
        m_net.post(request, QByteArray::fromStdString(body.dump()));
    m_inFlight = http;
    m_decoder = stream ? std::make_unique<wire::StreamDecoder>(m_provider)
                       : nullptr;

    if (stream) {
        connect(http, &QNetworkReply::readyRead, this, [this, http] {
            if (m_inFlight != http || !m_decoder) return;
            const int status =
                http->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray contentType = http->header(
                QNetworkRequest::ContentTypeHeader).toByteArray();
            if (status >= 400 ||
                (!contentType.isEmpty() &&
                 !contentType.toLower().contains("text/event-stream")))
                return;
            const QByteArray chunk = http->readAll();
            m_decoder->feed(
                std::string_view(chunk.constData(), std::size_t(chunk.size())));
            if (m_partialSink) {
                const std::string text = m_decoder->takeText();
                if (!text.empty()) m_partialSink(QString::fromStdString(text));
            }
        });
    }

    connect(http, &QNetworkReply::finished, this, [this, http] {
        const QByteArray payload = http->readAll();
        const int status =
            http->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool failed =
            http->error() != QNetworkReply::NoError || status >= 400;
        const QString transportError = http->errorString();
        const QByteArray contentType =
            http->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        http->deleteLater();
        if (m_inFlight != http) return;
        m_inFlight = nullptr;

        const bool eventStream =
            contentType.toLower().contains("text/event-stream");
        const json parsed = json::parse(payload.constData(),
                                        payload.constData() + payload.size(),
                                        nullptr, /*allow_exceptions=*/false);

        ai::ModelReply reply;
        ai::AiSession::Usage usage;
        if (failed) {
            QString detail;
            if (!parsed.is_discarded())
                detail = QString::fromStdString(
                    wire::errorMessage(m_provider, parsed));
            if (detail.isEmpty() && m_decoder && !m_decoder->reply().error.empty())
                detail = QString::fromStdString(m_decoder->reply().error);
            if (detail.isEmpty()) detail = transportError;
            if (detail.isEmpty()) detail = QStringLiteral("request failed");
            reply.error = toStd(displayName() + QStringLiteral(": ") + detail +
                                (status ? QStringLiteral(" (HTTP %1)").arg(status)
                                        : QString()));
        } else if (m_decoder && eventStream) {
            m_decoder->feed(std::string_view(payload.constData(),
                                             std::size_t(payload.size())));
            if (m_partialSink) {
                const std::string text = m_decoder->takeText();
                if (!text.empty()) m_partialSink(QString::fromStdString(text));
            }
            reply = m_decoder->reply();
            usage = m_decoder->usage();
            if (m_usageSink) m_usageSink(usage);
        } else if (parsed.is_discarded()) {
            reply.error = toStd(displayName()) + " sent something that is not JSON";
        } else {
            reply = wire::parseReply(m_provider, parsed);
            usage = wire::parseUsage(m_provider, parsed);
            if (m_usageSink) m_usageSink(usage);
        }

        auto deliver = [this, reply = std::move(reply)]() mutable {
            answer(std::move(reply));
        };
        if (!m_reservationId.isEmpty()) {
            if (failed && status >= 400) {
                settleManaged(0, QStringLiteral("provider_rejected"),
                              std::move(deliver));
            } else if (failed) {
                settleManaged(m_reservedTokens,
                              QStringLiteral("transport_failure"),
                              std::move(deliver));
            } else if (const qint64 actual = usageTokens(usage); actual > 0) {
                settleManaged(actual, QStringLiteral("provider_usage"),
                              std::move(deliver));
            } else {
                settleManaged(m_reservedTokens,
                              QStringLiteral("conservative_no_usage"),
                              std::move(deliver));
            }
            return;
        }
        deliver();
    });
}

qint64 LlmClient::usageTokens(const ai::AiSession::Usage& usage) const {
    std::uint64_t total = usage.inputTokens + usage.outputTokens;
    if (m_provider == Provider::Anthropic)
        total += usage.cachedTokens + usage.cacheCreationTokens;
    return qint64(std::min<std::uint64_t>(
        total, std::uint64_t(std::numeric_limits<qint64>::max())));
}

void LlmClient::settleManaged(qint64 actualTokens, const QString& outcome,
                              std::function<void()> completed) {
    const QString reservationId = std::exchange(m_reservationId, {});
    const qint64 reservedTokens = std::exchange(m_reservedTokens, 0);
    if (reservationId.isEmpty()) {
        if (completed) completed();
        return;
    }

    auto* account = account::Service::instance();
    if (!account || !account->authenticated() || account->accessToken().isEmpty()) {
        if (completed) completed();
        return; // The server keeps the reservation held: fail closed.
    }

    const json body = {
        {"actual_tokens", std::clamp<qint64>(
                              actualTokens, 0,
                              std::max<qint64>(0, reservedTokens))},
        {"outcome", toStd(outcome)},
    };
    QNetworkRequest request(
        QUrl(account->apiOrigin() + QStringLiteral("/desktop/ai/reservations/") +
             reservationId + QStringLiteral("/settle")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization",
                         "Bearer " + account->accessToken().toUtf8());
    request.setTransferTimeout(kAccountTimeoutMs);
    QNetworkReply* settlement = m_net.post(
        request, QByteArray::fromStdString(body.dump()));
    const bool blocksAnswer = bool(completed);
    if (blocksAnswer) m_inFlight = settlement;
    connect(settlement, &QNetworkReply::finished, this,
            [this, settlement, blocksAnswer,
             completed = std::move(completed)]() mutable {
        settlement->deleteLater();
        if (blocksAnswer && m_inFlight != settlement) return;
        if (blocksAnswer) m_inFlight = nullptr;
        if (auto* account = account::Service::instance())
            account->refreshQuota();
        if (completed) completed();
    });
}

void LlmClient::cancel() {
    m_onReply = nullptr;
    m_decoder.reset();
    if (!m_reservationId.isEmpty())
        settleManaged(m_reservedTokens, QStringLiteral("cancelled"));
    if (QNetworkReply* http = m_inFlight) {
        m_inFlight = nullptr;
        http->abort();
    }
}

} // namespace ui
