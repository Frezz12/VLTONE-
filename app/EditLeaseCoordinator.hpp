#pragma once
#include "CloudProjectClient.hpp"
#include "CollaborationService.hpp"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <functional>

namespace collab {
// Control-thread lease cache. A preview is allowed only after the server grant;
// pending requests never block the event loop. Server expiry is the backstop.
class EditLeaseCoordinator final : public QObject {
public:
    EditLeaseCoordinator(CloudProjectClient* client, CollaborationService* service, QObject* parent)
        : QObject(parent), m_client(client), m_service(service) {
        m_clock.start();
        connect(client, &CloudProjectClient::editLeaseReceived, this, [this](quint64 request, const QJsonObject& value) {
            const auto found = m_requests.find(request);
            if (found == m_requests.end()) return;
            const QString key = found.value();
            m_requests.erase(found);
            auto entry = m_entries.find(key);
            if (entry == m_entries.end()) return;
            if (!contextCurrent() || value.value(QStringLiteral("holderMemberId")).toString() != m_service->localParticipantId()) {
                fail(key);
                return;
            }
            const auto fields = value.value(QStringLiteral("fieldKeys")).toArray();
            if (fields.size() != 1 || fields.at(0).toString() != key) { fail(key); return; }
            entry->id = value.value(QStringLiteral("leaseId")).toString();
            entry->pending = false;
            entry->granted = m_clock.elapsed();
            const auto serverExpiry = QDateTime::fromString(value.value(QStringLiteral("expiresAt")).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
            entry->expires = entry->granted + std::clamp<qint64>(serverExpiry - m_service->estimatedServerTimeMs() - 1000, 0, 12000);
            if (entry->expires <= entry->granted) { fail(key); return; }
            auto callbacks = std::move(entry->ready);
            for (auto& callback : callbacks) callback(true);
        });
        connect(client, &CloudProjectClient::requestFailed, this, [this](quint64 request, CloudRequestKind kind, const CloudClientError&) {
            if (kind != CloudRequestKind::AcquireEditLease && kind != CloudRequestKind::RenewEditLease) return;
            const auto found = m_requests.find(request);
            if (found == m_requests.end()) return;
            const auto key = found.value();
            m_requests.erase(found);
            fail(key);
        });
        connect(service, &CollaborationService::stateChanged, this, [this] {
            if (!contextCurrent()) clear();
        });
        connect(service, &CollaborationService::sessionControlChanged, this, [this] {
            if (!contextCurrent()) clear();
        });
        auto* timer = new QTimer(this);
        timer->setInterval(500);
        connect(timer, &QTimer::timeout, this, [this] {
            if (!contextCurrent()) { clear(); return; }
            const auto now = m_clock.elapsed();
            for (const auto& key : m_entries.keys()) {
                auto entry = m_entries.find(key);
                if (entry->keepAlive) {
                    if (!entry->keepAlive()) { release(key); continue; }
                    entry->used = now;
                }
                if (!entry->id.isEmpty() && now >= entry->expires) { fail(key); continue; }
                if (entry->pending) continue;
                if (now - entry->used > 1500) { release(key); continue; }
                if (!entry->id.isEmpty() && now - entry->granted >= 5000) {
                    entry->pending = true;
                    m_requests.insert(m_client->renewEditLease(m_project, m_session, entry->id, m_policy), key);
                }
            }
        });
        timer->start();
    }
    bool ensure(const QString& key, std::function<void(bool)> ready = {}) {
        if (key.isEmpty() || key.size() > 512 || !m_client) { if (ready) ready(false); return false; }
        if (!m_service || m_service->projectId().isEmpty() || m_service->commandSchemaVersion() < 6) {
            if (ready) ready(true);
            return true;
        }
        if (!m_service->canSubmitOperations()) { if (ready) ready(false); return false; }
        if (!contextCurrent()) clear();
        m_project = m_service->projectId();
        m_session = m_service->sessionId();
        m_policy = m_service->sessionControl().sessionVersion;
        if (!m_entries.contains(key) && m_entries.size() >= 128) { if (ready) ready(false); return false; }
        auto& entry = m_entries[key];
        const auto now = m_clock.elapsed();
        entry.used = now;
        if (!entry.id.isEmpty() && now < entry.expires) { if (ready) ready(true); return true; }
        if (now < entry.retryAfter) { if (ready) ready(false); return false; }
        if (ready) {
            if (entry.ready.size() >= 64) { ready(false); return false; }
            entry.ready.push_back(std::move(ready));
        }
        if (entry.pending) return false;
        entry.pending = true;
        m_requests.insert(m_client->acquireEditLease(m_project, m_session, {key}, m_policy), key);
        return false;
    }
    void release(const QString& key) {
        for (const auto request : m_requests.keys()) {
            if (m_requests.value(request) != key) continue;
            m_requests.remove(request);
            if (m_client) m_client->cancel(request);
        }
        auto found = m_entries.find(key);
        if (found == m_entries.end()) return;
        const auto entry = found.value();
        m_entries.erase(found);
        if (m_client && !entry.id.isEmpty()) m_client->releaseEditLease(m_project, m_session, entry.id);
        for (const auto& callback : entry.ready) callback(false);
    }
    void retainWhile(const QString& key, std::function<bool()> active) {
        auto found = m_entries.find(key);
        if (found != m_entries.end()) found->keepAlive = std::move(active);
    }
    void clear() {
        for (const auto request : m_requests.keys()) if (m_client) m_client->cancel(request);
        m_requests.clear();
        for (const auto& key : m_entries.keys()) release(key);
        m_project.clear(); m_session.clear(); m_policy = 0;
    }
    ~EditLeaseCoordinator() override { clear(); }
    std::function<void()> denied;
    std::function<void(const QString&)> reservationLost;
private:
    bool contextCurrent() const {
        return m_service && m_service->canSubmitOperations() && m_project == m_service->projectId() &&
            m_session == m_service->sessionId() && m_policy == m_service->sessionControl().sessionVersion;
    }
    void fail(const QString& key) {
        auto found = m_entries.find(key);
        if (found == m_entries.end()) return;
        const bool held = !found->id.isEmpty();
        for (const auto request : m_requests.keys()) if (m_requests.value(request) == key) {
            m_requests.remove(request);
            if (m_client) m_client->cancel(request);
        }
        auto callbacks = std::move(found->ready);
        found->id.clear(); found->pending = false; found->expires = 0;
        found->retryAfter = m_clock.elapsed() + 1000;
        for (auto& callback : callbacks) callback(false);
        if (held && reservationLost) reservationLost(key);
        if (denied) denied();
    }
    struct Entry {
        QString id;
        bool pending = false;
        qint64 used = 0, granted = 0, expires = 0, retryAfter = 0;
        std::vector<std::function<void(bool)>> ready;
        std::function<bool()> keepAlive;
    };
    QPointer<CloudProjectClient> m_client;
    QPointer<CollaborationService> m_service;
    QHash<QString, Entry> m_entries;
    QHash<quint64, QString> m_requests;
    QString m_project, m_session;
    quint64 m_policy = 0;
    QElapsedTimer m_clock;
};
}
