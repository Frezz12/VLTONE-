#include "PromptService.hpp"

#include "AccountService.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

using json = nlohmann::json;

namespace ui {

namespace {

/// Often enough that an edit in the admin panel reaches a running session the
/// same day, rare enough that it is never a cost worth thinking about.
constexpr int kRefreshHours = 6;
constexpr int kTimeoutMs = 20'000;
/// A pack is a few tens of kilobytes of text. Anything past this is not one.
constexpr qint64 kMaxBodyBytes = 4 * 1024 * 1024;

} // namespace

PromptService* PromptService::s_instance = nullptr;

PromptService* PromptService::instance() { return s_instance; }

PromptService::PromptService(QObject* parent) : QObject(parent) {
    if (!s_instance) s_instance = this;
    m_pack = daw::ai::builtinPrompts();
    m_network = new QNetworkAccessManager(this);
    m_network->setTransferTimeout(kTimeoutMs);
    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshHours * 60 * 60 * 1000);
    connect(m_timer, &QTimer::timeout, this, &PromptService::refresh);
}

PromptService::~PromptService() {
    if (s_instance == this) s_instance = nullptr;
    if (m_inFlight) {
        QNetworkReply* reply = m_inFlight;
        m_inFlight = nullptr;
        reply->abort();
    }
}

bool PromptService::busy() const { return m_inFlight != nullptr; }

QString PromptService::cachePath() const {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/ai-prompts.json");
}

void PromptService::start() {
    loadCache();
    m_timer->start();
    refresh();
}

void PromptService::loadCache() {
    const QString path = cachePath();
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray body = file.readAll();
    file.close();

    const json parsed = json::parse(body.constData(), body.constData() + body.size(),
                                    nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return;

    // The cache file wraps the pack so the ETag rides along with it; an older
    // file holding the bare pack still loads.
    const auto it = parsed.find("pack");
    const json& packJson = it != parsed.end() ? *it : parsed;
    if (it != parsed.end()) {
        const auto tag = parsed.find("etag");
        if (tag != parsed.end() && tag->is_string())
            m_etag = QString::fromStdString(tag->get<std::string>());
    }

    std::string error;
    daw::ai::PromptPack cached = daw::ai::parsePromptPack(packJson, &error);
    if (cached.empty()) {
        // A cache we cannot read is a cache worth losing: it will be replaced
        // by the next successful fetch, and until then the built-ins are right.
        m_etag.clear();
        return;
    }
    adopt(std::move(cached), Source::Cache);
}

void PromptService::saveCache(const QByteArray& body, const QString& etag) {
    const QString path = cachePath();
    if (path.isEmpty()) return;
    QDir().mkpath(QFileInfo(path).absolutePath());

    const json parsed = json::parse(body.constData(), body.constData() + body.size(),
                                    nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) return;
    const json wrapper{{"etag", etag.toStdString()}, {"pack", parsed}};

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    const std::string text = wrapper.dump();
    file.write(text.data(), qint64(text.size()));
    file.close();
}

void PromptService::adopt(daw::ai::PromptPack pack, Source source) {
    const bool changed = pack.version != m_pack.version ||
                         pack.main != m_pack.main ||
                         pack.playbooks.size() != m_pack.playbooks.size();
    m_pack = std::move(pack);
    m_source = source;
    if (changed) emit packChanged();
    emit stateChanged();
}

void PromptService::refresh() {
    if (m_inFlight) return;
    auto* account = account::Service::instance();
    if (!account || !account->authenticated() || account->accessToken().isEmpty()) {
        // Not an error worth showing: the app starts before anybody signs in,
        // and the cache or the built-ins are already in force.
        return;
    }

    QNetworkRequest request{QUrl(account->apiOrigin() +
                                 QStringLiteral("/desktop/ai/prompts"))};
    request.setRawHeader("Authorization",
                         "Bearer " + account->accessToken().toUtf8());
    if (!m_etag.isEmpty()) request.setRawHeader("If-None-Match", m_etag.toUtf8());

    m_inFlight = m_network->get(request);
    emit stateChanged();
    connect(m_inFlight, &QNetworkReply::finished, this, [this] {
        QNetworkReply* reply = m_inFlight;
        if (!reply) return;
        m_inFlight = nullptr;
        reply->deleteLater();
        finish(reply);
    });
}

void PromptService::finish(QNetworkReply* reply) {
    m_lastCheckedAt = QDateTime::currentDateTime();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (status == 304) {
        // Unchanged since the cached copy — the common case, and the reason the
        // ETag is kept at all.
        m_lastError.clear();
        emit stateChanged();
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        // Staying on the cache is the right answer to a server that is down;
        // the message is only for the line in Settings.
        m_lastError = status == 404
                          ? tr("this server has no prompt library yet")
                          : reply->errorString();
        emit stateChanged();
        return;
    }

    const QByteArray body = reply->readAll();
    if (body.size() > kMaxBodyBytes) {
        m_lastError = tr("the prompt library sent back was too large");
        emit stateChanged();
        return;
    }
    const json parsed = json::parse(body.constData(), body.constData() + body.size(),
                                    nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        m_lastError = tr("the prompt library sent back was not JSON");
        emit stateChanged();
        return;
    }

    std::string error;
    daw::ai::PromptPack fetched = daw::ai::parsePromptPack(parsed, &error);
    if (fetched.empty()) {
        m_lastError = QString::fromStdString(error);
        emit stateChanged();
        return;
    }

    m_lastError.clear();
    m_etag = QString::fromUtf8(reply->rawHeader("ETag"));
    saveCache(body, m_etag);
    adopt(std::move(fetched), Source::Server);
}

} // namespace ui
