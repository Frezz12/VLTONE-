#include "CloudProjectAssetHydrator.hpp"

#include "AssetCache.hpp"
#include "CloudAssetTransferManager.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <climits>
#include <utility>

namespace collab {
namespace {

void appendAsset(QList<daw::AssetRef>& output,
                 QSet<QString>& seenIdentities,
                 const daw::AssetRef& asset) {
    if (asset.empty()) return;
    const QString id = QString::fromStdString(asset.assetId).toLower();
    const QString identity = QStringLiteral("%1|%2|%3|%4")
                                 .arg(id,
                                      QString::fromStdString(asset.sha256)
                                          .trimmed().toLower())
                                 .arg(asset.byteSize)
                                 .arg(int(asset.kind));
    // Identical references are ordinary dedupe. A second content identity for
    // the same id remains in output so hydrate() reports the conflict.
    if (seenIdentities.contains(identity)) return;
    seenIdentities.insert(identity);
    output.push_back(asset);
}

void appendInsertAssets(QList<daw::AssetRef>& output,
                        QSet<QString>& seenIdentities,
                        const daw::InsertModel& insert) {
    appendAsset(output, seenIdentities, insert.stateAsset);
    appendAsset(output, seenIdentities, insert.rightStateAsset);
    for (const daw::PluginAssetBinding& binding : insert.assetBindings)
        appendAsset(output, seenIdentities, binding.asset);
}

void appendInsertList(QList<daw::AssetRef>& output,
                      QSet<QString>& seenIdentities,
                      const std::vector<daw::InsertModel>& inserts) {
    for (const daw::InsertModel& insert : inserts)
        appendInsertAssets(output, seenIdentities, insert);
}

bool lowercaseSha256(const QString& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return (code >= '0' && code <= '9') ||
               (code >= 'a' && code <= 'f');
    });
}

QString assetIdentity(const daw::AssetRef& asset) {
    return QStringLiteral("%1|%2|%3")
        .arg(QString::fromStdString(asset.sha256).trimmed().toLower())
        .arg(asset.byteSize)
        .arg(int(asset.kind));
}

} // namespace

QList<daw::AssetRef> collectCloudProjectAssets(
    const daw::ProjectModel& project) {
    QList<daw::AssetRef> result;
    QSet<QString> seenIdentities;

    appendInsertList(result, seenIdentities, project.masterInserts);
    for (const daw::TrackModel& track : project.tracks) {
        // Arrangement media is intentionally discovered before plugin state:
        // a collaborator can hear and navigate the song while a larger opaque
        // state blob is still arriving.
        for (const daw::ClipModel& clip : track.clips) {
            appendAsset(result, seenIdentities, clip.asset);
            for (const daw::TakeModel& take : clip.takes)
                appendAsset(result, seenIdentities, take.asset);
            appendInsertList(result, seenIdentities, clip.inserts);
        }
        appendInsertAssets(result, seenIdentities, track.instrument);
        appendInsertList(result, seenIdentities, track.samplerFx.inserts);
        appendInsertList(result, seenIdentities, track.inserts);
    }
    return result;
}

CloudProjectAssetHydrator::CloudProjectAssetHydrator(
    CloudAssetTransferManager* transfers, AssetCache* cache, QObject* parent)
    : QObject(parent), m_transfers(transfers), m_cache(cache) {
    if (m_transfers) {
        connect(m_transfers,
                &CloudAssetTransferManager::assetDownloadCompleted,
                this, &CloudProjectAssetHydrator::handleCompleted);
        connect(m_transfers, &CloudAssetTransferManager::transferFailed,
                this, &CloudProjectAssetHydrator::handleFailed);
    }
}

QString CloudProjectAssetHydrator::normalizedProjectId(const QString& value) {
    const QUuid uuid(value);
    return uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
}

QString CloudProjectAssetHydrator::normalizedAssetId(
    const daw::AssetRef& asset) {
    const QUuid uuid(QString::fromStdString(asset.assetId));
    return uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
}

QString CloudProjectAssetHydrator::normalizedHash(
    const daw::AssetRef& asset) {
    return QString::fromStdString(asset.sha256).trimmed().toLower();
}

bool CloudProjectAssetHydrator::completeAsset(const daw::AssetRef& asset) {
    return !normalizedAssetId(asset).isEmpty() &&
           lowercaseSha256(normalizedHash(asset)) && asset.byteSize > 0 &&
           asset.kind != daw::AssetKind::Unknown;
}

void CloudProjectAssetHydrator::setState(CloudHydrationState state) {
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(m_state);
}

qsizetype CloudProjectAssetHydrator::pendingCount() const noexcept {
    return m_queue.size() + m_active.size() + m_scheduledRetryCount;
}

void CloudProjectAssetHydrator::cancel() {
    ++m_generation;
    if (m_transfers) {
        const QList<quint64> ids = m_active.keys();
        for (quint64 id : ids) m_transfers->cancel(id);
    }
    m_queue.clear();
    m_active.clear();
    m_activeHashes.clear();
    m_knownIdentities.clear();
    m_failed.clear();
    m_projectId.clear();
    m_totalCount = 0;
    m_completedCount = 0;
    m_failedCount = 0;
    m_scheduledRetryCount = 0;
    m_settledEmitted = false;
    setState(CloudHydrationState::Idle);
}

void CloudProjectAssetHydrator::hydrate(
    const QString& requestedProjectId,
    const QList<daw::AssetRef>& assets,
    const QStringList& priorityAssetIds) {
    const QString normalizedProject = normalizedProjectId(requestedProjectId);
    if (normalizedProject.isEmpty()) {
        cancel();
        m_totalCount = 1;
        m_failedCount = 1;
        setState(CloudHydrationState::Degraded);
        emit assetUnavailable({}, QStringLiteral("Invalid cloud project id"),
                              false);
        emit hydrationSettled({}, 1);
        return;
    }
    cancel();
    m_projectId = normalizedProject;
    ++m_generation;

    appendAssets(assets, priorityAssetIds);
}

void CloudProjectAssetHydrator::ensureMissing(
    const QString& requestedProjectId,
    const QList<daw::AssetRef>& assets,
    const QStringList& priorityAssetIds) {
    const QString normalizedProject = normalizedProjectId(requestedProjectId);
    if (normalizedProject.isEmpty() || normalizedProject != m_projectId) {
        hydrate(requestedProjectId, assets, priorityAssetIds);
        return;
    }
    appendAssets(assets, priorityAssetIds);
}

void CloudProjectAssetHydrator::retryFailed() {
    if (m_projectId.isEmpty() || m_failed.isEmpty()) return;
    QList<Item> retrying;
    retrying.reserve(m_failed.size());
    for (Item item : std::as_const(m_failed)) {
        if (item.generation != m_generation) continue;
        item.attempts = 0;
        retrying.push_back(std::move(item));
    }
    if (retrying.isEmpty()) return;
    m_settledEmitted = false;
    m_failed.clear();
    m_failedCount = std::max<qsizetype>(
        0, m_failedCount - retrying.size());
    for (Item& item : retrying) m_queue.push_back(std::move(item));
    emit progressChanged(m_completedCount, m_totalCount);
    pump();
    finishIfSettled();
}

void CloudProjectAssetHydrator::appendAssets(
    const QList<daw::AssetRef>& assets,
    const QStringList& priorityAssetIds) {
    if (m_projectId.isEmpty()) return;
    m_settledEmitted = false;

    QHash<QString, int> priority;
    for (int index = 0; index < priorityAssetIds.size(); ++index) {
        const QUuid uuid(priorityAssetIds.at(index));
        if (!uuid.isNull()) {
            priority.insert(uuid.toString(QUuid::WithoutBraces).toLower(),
                            index);
        }
    }

    QList<Item> discovered;
    const qsizetype limit =
        std::min<qsizetype>(assets.size(), kMaximumAssetsPerProject);
    for (qsizetype index = 0; index < limit; ++index) {
        daw::AssetRef asset = assets.at(index);
        const QString assetId = normalizedAssetId(asset);
        const QString hash = normalizedHash(asset);
        if (!completeAsset(asset)) {
            ++m_totalCount;
            ++m_failedCount;
            emit assetUnavailable(assetId,
                                  QStringLiteral("Invalid cloud asset reference"),
                                  false);
            continue;
        }
        asset.assetId = assetId.toStdString();
        asset.sha256 = hash.toStdString();

        const QString identity = assetIdentity(asset);
        const auto known = m_knownIdentities.constFind(assetId);
        if (known != m_knownIdentities.cend()) {
            if (known.value() != identity) {
                ++m_totalCount;
                ++m_failedCount;
                emit assetUnavailable(
                    assetId,
                    QStringLiteral("Cloud asset id refers to different bytes"),
                    false);
            }
            continue;
        }
        m_knownIdentities.insert(assetId, identity);
        if (m_cache && m_cache->contains(asset)) continue;
        discovered.push_back(Item{std::move(asset), 0, m_generation});
    }
    if (assets.size() > kMaximumAssetsPerProject) {
        ++m_totalCount;
        ++m_failedCount;
        emit assetUnavailable(
            {}, QStringLiteral("Cloud project references too many assets"),
            false);
    }

    std::stable_sort(discovered.begin(), discovered.end(),
                     [&](const Item& left, const Item& right) {
        const QString leftId = normalizedAssetId(left.asset);
        const QString rightId = normalizedAssetId(right.asset);
        const int leftPriority = priority.value(leftId, INT_MAX);
        const int rightPriority = priority.value(rightId, INT_MAX);
        return leftPriority < rightPriority;
    });
    m_totalCount += discovered.size();
    for (Item& item : discovered) m_queue.push_back(std::move(item));
    emit progressChanged(m_completedCount, m_totalCount);
    pump();
    finishIfSettled();
}

void CloudProjectAssetHydrator::pump() {
    if ((!m_transfers || !m_cache) && !m_queue.isEmpty()) {
        while (!m_queue.isEmpty()) {
            const Item item = m_queue.takeFirst();
            const QString assetId = normalizedAssetId(item.asset);
            if (item.generation != m_generation) {
                m_totalCount = std::max<qsizetype>(0, m_totalCount - 1);
                continue;
            }
            m_failed.insert(assetId, item);
            ++m_failedCount;
            emit assetUnavailable(
                assetId,
                QStringLiteral("Cloud asset downloads are unavailable"),
                true);
        }
    }
    while (m_transfers && m_cache && !m_queue.isEmpty() &&
           m_active.size() < kMaximumConcurrentDownloads) {
        auto candidate = std::find_if(
            m_queue.begin(), m_queue.end(), [this](const Item& item) {
                return !m_activeHashes.contains(normalizedHash(item.asset));
            });
        // Another logical asset may pin the same bytes. Wait for that one;
        // after it registers the content-addressed blob this item is a cache
        // hit instead of a second network transfer.
        if (candidate == m_queue.end()) break;
        Item item = m_queue.takeAt(candidate - m_queue.begin());
        if (item.generation != m_generation) {
            // A stale item never reaches completed or failed, so leaving it in
            // the total would freeze progress short of its own end.
            m_totalCount = std::max<qsizetype>(0, m_totalCount - 1);
            continue;
        }
        if (m_cache->contains(item.asset)) {
            m_knownIdentities.remove(normalizedAssetId(item.asset));
            m_failed.remove(normalizedAssetId(item.asset));
            ++m_completedCount;
            emit progressChanged(m_completedCount, m_totalCount);
            continue;
        }
        ++item.attempts;
        const quint64 transferId =
            m_transfers->downloadAsset(m_projectId, item.asset);
        if (transferId == 0) {
            m_failed.insert(normalizedAssetId(item.asset), item);
            ++m_failedCount;
            emit assetUnavailable(
                normalizedAssetId(item.asset),
                QStringLiteral("Cloud asset download could not be started"),
                true);
            continue;
        }
        m_activeHashes.insert(normalizedHash(item.asset));
        m_active.insert(transferId, std::move(item));
    }
    if (pendingCount() > 0)
        setState(CloudHydrationState::Downloading);
    else
        finishIfSettled();
}

void CloudProjectAssetHydrator::handleCompleted(
    quint64 transferId, const daw::AssetRef& asset, const QString& localPath) {
    const auto found = m_active.find(transferId);
    if (found == m_active.end()) return;
    const Item expected = found.value();
    m_active.erase(found);
    m_activeHashes.remove(normalizedHash(expected.asset));
    if (expected.generation != m_generation) return;
    const QString assetId = normalizedAssetId(expected.asset);
    const bool accepted = normalizedAssetId(asset) == assetId &&
                          normalizedHash(asset) ==
                              normalizedHash(expected.asset) &&
                          !localPath.isEmpty() && m_cache &&
                          m_cache->contains(expected.asset);
    m_knownIdentities.remove(assetId);
    if (accepted) {
        if (m_failed.remove(assetId) != 0)
            m_failedCount = std::max<qsizetype>(0, m_failedCount - 1);
        ++m_completedCount;
    } else {
        m_knownIdentities.insert(assetId, assetIdentity(expected.asset));
        // The same asset can fail more than once across retries. Counting each
        // arrival would drift the tally away from what retryFailed() can act on.
        if (!m_failed.contains(assetId)) ++m_failedCount;
        m_failed.insert(assetId, expected);
        emit assetUnavailable(assetId,
                              QStringLiteral("Downloaded asset is unavailable"),
                              true);
    }
    emit progressChanged(m_completedCount, m_totalCount);
    pump();
    finishIfSettled();
}

void CloudProjectAssetHydrator::handleFailed(
    quint64 transferId, CloudTransferKind kind,
    const CloudTransferError& error) {
    if (kind != CloudTransferKind::AssetDownload) return;
    const auto found = m_active.find(transferId);
    if (found == m_active.end()) return;
    Item item = found.value();
    m_active.erase(found);
    m_activeHashes.remove(normalizedHash(item.asset));
    if (item.generation != m_generation) return;
    const QString assetId = normalizedAssetId(item.asset);
    if (error.retryable && item.attempts < kMaximumAttempts) {
        const quint64 taskGeneration = m_generation;
        const int delayMs = 250 * (1 << (item.attempts - 1));
        ++m_scheduledRetryCount;
        QTimer::singleShot(delayMs, this,
                           [this, taskGeneration, item = std::move(item)]() mutable {
            if (taskGeneration != m_generation) return;
            m_scheduledRetryCount = std::max<qsizetype>(
                0, m_scheduledRetryCount - 1);
            if (m_projectId.isEmpty()) {
                finishIfSettled();
                return;
            }
            m_queue.prepend(std::move(item));
            pump();
        });
    } else {
        if (error.retryable) {
            if (!m_failed.contains(assetId)) ++m_failedCount;
            m_failed.insert(assetId, item);
        } else {
            m_knownIdentities.remove(assetId);
            if (m_failed.remove(assetId) == 0) ++m_failedCount;
        }
        emit assetUnavailable(
            assetId,
            error.safeMessage.isEmpty()
                ? QStringLiteral("Cloud asset download failed")
                : error.safeMessage,
            error.retryable);
    }
    emit progressChanged(m_completedCount, m_totalCount);
    pump();
    finishIfSettled();
}

void CloudProjectAssetHydrator::finishIfSettled() {
    if (pendingCount() > 0) return;
    if (m_projectId.isEmpty()) {
        setState(CloudHydrationState::Idle);
        return;
    }
    setState(m_failedCount > 0 ? CloudHydrationState::Degraded
                               : CloudHydrationState::Ready);
    if (m_settledEmitted) return;
    m_settledEmitted = true;
    emit hydrationSettled(m_projectId, m_failedCount);
}

bool checkCloudProjectAssetHydratorForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const auto asset = [](const char* id, const QByteArray& bytes,
                          daw::AssetKind kind) {
        daw::AssetRef result;
        result.assetId = id;
        result.sha256 =
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                .toHex().toStdString();
        result.byteSize = quint64(bytes.size());
        result.kind = kind;
        return result;
    };

    const daw::AssetRef audio = asset(
        "11111111-1111-4111-8111-111111111111", QByteArray("audio"),
        daw::AssetKind::Audio);
    const daw::AssetRef sample = asset(
        "22222222-2222-4222-8222-222222222222", QByteArray("sample"),
        daw::AssetKind::Audio);
    daw::ProjectModel project;
    daw::TrackModel track;
    track.id = "33333333-3333-4333-8333-333333333333";
    daw::ClipModel clip;
    clip.id = "44444444-4444-4444-8444-444444444444";
    clip.kind = daw::ClipKind::Audio;
    clip.asset = audio;
    daw::TakeModel take;
    take.id = "55555555-5555-4555-8555-555555555555";
    take.asset = audio;
    clip.takes.push_back(take);
    track.clips.push_back(clip);
    track.instrument.id = "66666666-6666-4666-8666-666666666666";
    track.instrument.uid = "daw.sampler";
    track.instrument.assetBindings.push_back({"sample", sample, true});
    project.tracks.push_back(track);

    const QList<daw::AssetRef> collected = collectCloudProjectAssets(project);
    if (collected.size() != 2 || collected.at(0) != audio ||
        collected.at(1) != sample) {
        return fail(QStringLiteral("Cloud asset collection did not deduplicate"));
    }
    daw::AssetRef conflicting = audio;
    conflicting.sha256 =
        QCryptographicHash::hash(QByteArray("different"),
                                 QCryptographicHash::Sha256)
            .toHex().toStdString();
    project.masterInserts.push_back({});
    project.masterInserts.back().assetBindings.push_back(
        {"conflict", conflicting, true});
    if (collectCloudProjectAssets(project).size() != 3) {
        return fail(QStringLiteral("Conflicting asset identity was hidden"));
    }

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return fail(QStringLiteral("Could not create hydration fixture"));
    AssetCache cache(QDir(temporary.path()).filePath(QStringLiteral("cache")));
    CloudProjectAssetHydrator hydrator(nullptr, &cache);
    bool invalidReported = false;
    qsizetype reportedTotal = 0;
    QObject::connect(&hydrator,
                     &CloudProjectAssetHydrator::assetUnavailable,
                     &hydrator,
                     [&](const QString&, const QString&, bool) {
        invalidReported = true;
    });
    QObject::connect(&hydrator,
                     &CloudProjectAssetHydrator::progressChanged,
                     &hydrator,
                     [&](qsizetype, qsizetype total) {
        reportedTotal = total;
    });
    daw::AssetRef invalid = audio;
    invalid.sha256 = "ABC";
    hydrator.hydrate(
        QStringLiteral("77777777-7777-4777-8777-777777777777"),
        {invalid});
    if (!invalidReported ||
        hydrator.state() != CloudHydrationState::Degraded) {
        return fail(QStringLiteral("Invalid cloud asset was not rejected"));
    }

    hydrator.cancel();
    invalidReported = false;
    hydrator.hydrate(
        QStringLiteral("77777777-7777-4777-8777-777777777777"), {audio});
    if (!invalidReported || hydrator.pendingCount() != 0 ||
        hydrator.failedCount() != 1 ||
        hydrator.state() != CloudHydrationState::Degraded ||
        reportedTotal != 1) {
        return fail(QStringLiteral(
            "Unavailable download service left hydration pending"));
    }
    invalidReported = false;
    hydrator.ensureMissing(
        QStringLiteral("77777777-7777-4777-8777-777777777777"), {audio});
    hydrator.retryFailed();
    if (!invalidReported || hydrator.pendingCount() != 0 ||
        hydrator.failedCount() != 1 || reportedTotal != 1 ||
        hydrator.state() != CloudHydrationState::Degraded) {
        return fail(QStringLiteral(
            "Retrying failed hydration changed progress totals"));
    }
    hydrator.cancel();
    hydrator.retryFailed();
    if (hydrator.state() != CloudHydrationState::Idle ||
        hydrator.pendingCount() != 0 || hydrator.failedCount() != 0) {
        return fail(QStringLiteral(
            "Cancelled hydration generation was retried"));
    }
    return true;
}

} // namespace collab
