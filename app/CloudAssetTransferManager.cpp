#include "CloudAssetTransferManager.hpp"

#include "AccountService.hpp"
#include "CloudSnapshotAssetManifest.hpp"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace collab {

struct CloudAssetTransferManager::Credentials {
    QString apiOrigin;
    QByteArray bearerToken;
    QString userId;
    QString deviceId;
    bool authenticated = false;
    bool offline = false;
};

namespace {

constexpr int kDefaultTimeoutMs = 30'000;
constexpr int kMinimumTimeoutMs = 100;
constexpr int kMaximumTimeoutMs = 5 * 60 * 1000;
constexpr qsizetype kMaximumApiResponseBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumProviderResponseBytes = 64 * 1024;
constexpr quint64 kMaximumBlobBytes = 64ULL * 1024 * 1024 * 1024;
constexpr qint64 kMinimumMultipartPartBytes = 5LL * 1024 * 1024;
constexpr qint64 kMaximumMultipartPartBytes = 5LL * 1024 * 1024 * 1024;
constexpr int kMaximumMultipartParts = 1000;
constexpr int kMaximumPartPage = 200;
constexpr int kMaximumRedirects = 3;
constexpr int kQtTransferTimeoutGraceMs = 1000;
constexpr qsizetype kIoBlockBytes = 1024 * 1024;
constexpr quint64 kMaximumExactJsonInteger = 9'007'199'254'740'991ULL;

CloudTransferError transferFailure(CloudTransferErrorCode code,
                                   const QString& message,
                                   int status = 0,
                                   bool retryable = false) {
    CloudTransferError error;
    error.code = code;
    error.httpStatus = status;
    error.safeMessage = message;
    error.retryable = retryable;
    return error;
}

QString statusMessage(int status) {
    return status > 0
        ? QStringLiteral("Cloud transfer request failed (HTTP %1)").arg(status)
        : QStringLiteral("Cloud transfer request failed");
}

bool exactKeys(const QJsonObject& object,
               std::initializer_list<const char*> required,
               std::initializer_list<const char*> optional = {}) {
    QSet<QString> allowed;
    for (const char* key : required) {
        const QString name = QString::fromLatin1(key);
        allowed.insert(name);
        if (!object.contains(name)) return false;
    }
    for (const char* key : optional)
        allowed.insert(QString::fromLatin1(key));
    for (auto iterator = object.constBegin(); iterator != object.constEnd();
         ++iterator) {
        if (!allowed.contains(iterator.key())) return false;
    }
    return true;
}

bool normalizeUuid(const QString& source, QString* output = nullptr) {
    const QUuid parsed(source);
    if (parsed.isNull()) return false;
    const QString normalized =
        parsed.toString(QUuid::WithoutBraces).toLower();
    if (source.trimmed().toLower() != normalized) return false;
    if (output) *output = normalized;
    return true;
}

bool normalizeOptionalUuid(const QJsonObject& object, const char* key,
                           QString* output) {
    const QString name = QString::fromLatin1(key);
    if (!object.contains(name) || object.value(name).isNull()) {
        output->clear();
        return true;
    }
    if (!object.value(name).isString()) return false;
    return normalizeUuid(object.value(name).toString(), output);
}

bool parseSnapshotAssetIds(const QJsonValue& value, QStringList* output) {
    if (!output || !value.isArray()) return false;
    const QJsonArray values = value.toArray();
    if (values.size() > kMaximumSnapshotAssetIds) return false;
    QStringList parsed;
    parsed.reserve(values.size());
    for (const QJsonValue& item : values) {
        if (!item.isString()) return false;
        parsed.push_back(item.toString());
    }
    if (!isCanonicalCloudSnapshotAssetManifest(parsed)) return false;
    *output = std::move(parsed);
    return true;
}

bool validSha256(const QString& value) {
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') ||
              (code >= 'a' && code <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<quint64> exactUnsigned(const QJsonValue& value,
                                     quint64 maximum =
                                         kMaximumExactJsonInteger) {
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0 ||
        std::floor(number) != number || number > double(maximum)) {
        return std::nullopt;
    }
    return quint64(number);
}

std::optional<int> exactInteger(const QJsonValue& value, int minimum,
                                int maximum) {
    const auto parsed = exactUnsigned(value, quint64(maximum));
    if (!parsed || *parsed < quint64(minimum)) return std::nullopt;
    return int(*parsed);
}

bool parseDateTime(const QJsonValue& value, QDateTime* output,
                   bool nullable = false) {
    if (nullable && value.isNull()) {
        *output = {};
        return true;
    }
    if (!value.isString() || value.toString().size() > 64) return false;
    const QDateTime parsed =
        QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid() || parsed.timeSpec() == Qt::LocalTime)
        return false;
    *output = parsed.toUTC();
    return true;
}

int effectivePort(const QUrl& url) {
    const int explicitPort = url.port(-1);
    if (explicitPort >= 0) return explicitPort;
    return url.scheme().compare(QStringLiteral("https"),
                                Qt::CaseInsensitive) == 0
        ? 443
        : 80;
}

bool sameOrigin(const QUrl& left, const QUrl& right) {
    return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0 &&
           left.host().compare(right.host(), Qt::CaseInsensitive) == 0 &&
           effectivePort(left) == effectivePort(right);
}

bool safeOrigin(const QUrl& origin) {
    return origin.isValid() && !origin.host().isEmpty() &&
           (origin.scheme() == QLatin1String("https") ||
            origin.scheme() == QLatin1String("http")) &&
           origin.userInfo().isEmpty() && !origin.hasQuery() &&
           !origin.hasFragment();
}

bool validContentType(const QString& value) {
    return !value.isEmpty() && value.size() <= 160 &&
           !value.contains(QLatin1Char('\r')) &&
           !value.contains(QLatin1Char('\n')) &&
           !value.contains(QChar::Null);
}

QString safeDisplayName(const QString& source) {
    QString value = source.trimmed();
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    value = value.section(QLatin1Char('/'), -1);
    if (value.isEmpty() || value == QLatin1String(".") ||
        value.size() > 255 || value.contains(QChar::Null) ||
        value.contains(QLatin1Char('\r')) ||
        value.contains(QLatin1Char('\n'))) {
        return {};
    }
    return value;
}

QString assetKindName(CloudAssetKind kind) {
    switch (kind) {
        case CloudAssetKind::Audio: return QStringLiteral("audio");
        case CloudAssetKind::Sample: return QStringLiteral("sample");
        case CloudAssetKind::PluginState:
            return QStringLiteral("plugin_state");
        case CloudAssetKind::Other: return QStringLiteral("other");
    }
    return {};
}

daw::AssetKind documentAssetKind(CloudAssetKind kind) {
    switch (kind) {
        case CloudAssetKind::Audio: return daw::AssetKind::Audio;
        case CloudAssetKind::Sample:
            return daw::AssetKind::PluginResource;
        case CloudAssetKind::PluginState:
            return daw::AssetKind::PluginState;
        case CloudAssetKind::Other: return daw::AssetKind::Unknown;
    }
    return daw::AssetKind::Unknown;
}

struct DelegatedRequest {
    QByteArray method;
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> headers;
    QDateTime expiresAt;
};

struct UploadedPart {
    int number = 0;
    quint64 bytes = 0;
    QByteArray entityTag;
};

struct PreparedPart {
    int number = 0;
    quint64 bytes = 0;
    DelegatedRequest request;
};

struct UploadPreparation {
    QString uploadId;
    QString assetId;
    std::optional<quint64> snapshotSequence;
    bool completed = false;
    bool multipart = false;
    bool alreadyAvailable = false;
    std::optional<DelegatedRequest> request;
    quint64 partSize = 0;
    int partCount = 0;
    QVector<UploadedPart> uploadedParts;
    QVector<PreparedPart> parts;
    std::optional<int> nextPartNumberStart;
    QDateTime expiresAt;
};

struct DownloadPreparation {
    DelegatedRequest request;
    QString sha256;
    quint64 bytes = 0;
    QString contentType;
};

struct ParsedProjectAsset {
    QString projectId;
    QString assetId;
    QString blobId;
    QString kind;
    QString displayName;
    QString createdBy;
    QDateTime createdAt;
};

struct ParsedSnapshot {
    QString id;
    QString projectId;
    quint64 sequence = 0;
    QString blobId;
    int schemaVersion = 0;
    QString createdBy;
    QDateTime createdAt;
    QStringList assetIds;
};

bool validHeaderName(const QByteArray& name) {
    if (name.isEmpty() || name.size() > 128) return false;
    static const QByteArray separators("()<>@,;:\"/[]?={} \t");
    for (const char byte : name) {
        const uchar code = uchar(byte);
        if (code <= 32 || code >= 127 || separators.contains(byte))
            return false;
    }
    const QByteArray lower = name.toLower();
    return lower != QByteArrayLiteral("authorization") &&
           lower != QByteArrayLiteral("proxy-authorization") &&
           lower != QByteArrayLiteral("cookie") &&
           lower != QByteArrayLiteral("set-cookie") &&
           lower != QByteArrayLiteral("host");
}

std::optional<DelegatedRequest> parseDelegatedRequest(
    const QJsonObject& object, const QByteArray& requiredMethod,
    CloudTransferError* error) {
    if (!exactKeys(object, {"method", "url", "headers", "expiresAt"}) ||
        !object.value(QStringLiteral("method")).isString() ||
        !object.value(QStringLiteral("url")).isString() ||
        !object.value(QStringLiteral("headers")).isObject()) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Server returned an invalid delegated request"));
        return std::nullopt;
    }
    DelegatedRequest result;
    result.method = object.value(QStringLiteral("method")).toString().toLatin1();
    if (result.method != requiredMethod) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::DelegatedRequestRejected,
                QStringLiteral("Delegated request method was rejected"));
        return std::nullopt;
    }
    const QString rawUrl = object.value(QStringLiteral("url")).toString();
    if (rawUrl.isEmpty() || rawUrl.size() > 8192) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::DelegatedRequestRejected,
                QStringLiteral("Delegated request URL was rejected"));
        return std::nullopt;
    }
    result.url = QUrl(rawUrl, QUrl::StrictMode);
    if (!result.url.isValid() || result.url.isRelative() ||
        result.url.host().isEmpty() || !result.url.userInfo().isEmpty() ||
        result.url.hasFragment() ||
        (result.url.scheme() != QLatin1String("https") &&
         result.url.scheme() != QLatin1String("http"))) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::DelegatedRequestRejected,
                QStringLiteral("Delegated request URL was rejected"));
        return std::nullopt;
    }
    const QJsonObject headers =
        object.value(QStringLiteral("headers")).toObject();
    if (headers.size() > 32) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::DelegatedRequestRejected,
                QStringLiteral("Delegated request headers were rejected"));
        return std::nullopt;
    }
    qsizetype aggregate = 0;
    QSet<QByteArray> normalizedNames;
    for (auto iterator = headers.constBegin(); iterator != headers.constEnd();
         ++iterator) {
        if (!iterator.value().isString()) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::DelegatedRequestRejected,
                    QStringLiteral("Delegated request headers were rejected"));
            return std::nullopt;
        }
        const QByteArray name = iterator.key().toLatin1();
        const QByteArray value = iterator.value().toString().toUtf8();
        const QByteArray lower = name.toLower();
        aggregate += name.size() + value.size();
        if (!validHeaderName(name) || normalizedNames.contains(lower) ||
            value.size() > 8192 || aggregate > 32 * 1024 ||
            value.contains('\r') || value.contains('\n') ||
            value.contains('\0')) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::DelegatedRequestRejected,
                    QStringLiteral("Delegated request headers were rejected"));
            return std::nullopt;
        }
        normalizedNames.insert(lower);
        result.headers.append({name, value});
    }
    if (!parseDateTime(object.value(QStringLiteral("expiresAt")),
                       &result.expiresAt) ||
        result.expiresAt <= QDateTime::currentDateTimeUtc()) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::DelegatedRequestRejected,
                QStringLiteral("Delegated request has expired"));
        return std::nullopt;
    }
    return result;
}

std::optional<QJsonObject> parseJsonObject(const QByteArray& body,
                                           CloudTransferError* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidJson,
                                     QStringLiteral("Server returned invalid JSON"));
        return std::nullopt;
    }
    return document.object();
}

CloudTransferError parseApiError(const QByteArray& body, int status) {
    CloudTransferError error = transferFailure(
        CloudTransferErrorCode::UnexpectedStatus, statusMessage(status),
        status, status == 408 || status == 429 || status >= 500);
    if (body.isEmpty() || body.size() > kMaximumApiResponseBytes)
        return error;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return error;
    }
    const QJsonObject object = document.object();
    if (!exactKeys(object, {"code", "message", "request_id"},
                   {"field_errors"}) ||
        !object.value(QStringLiteral("code")).isString() ||
        !object.value(QStringLiteral("message")).isString() ||
        !object.value(QStringLiteral("request_id")).isString()) {
        return error;
    }
    static const QRegularExpression codePattern(
        QStringLiteral("^[a-z][a-z0-9_.-]{0,63}$"));
    static const QRegularExpression requestPattern(
        QStringLiteral("^[A-Za-z0-9_.:-]{0,128}$"));
    const QString apiCode = object.value(QStringLiteral("code")).toString();
    const QString requestId =
        object.value(QStringLiteral("request_id")).toString();
    if (codePattern.match(apiCode).hasMatch()) error.apiCode = apiCode;
    if (requestPattern.match(requestId).hasMatch())
        error.serverRequestId = requestId;
    if (apiCode == QLatin1String("upload_expired")) {
        error.code = CloudTransferErrorCode::UploadExpired;
        error.retryable = false;
    } else if (apiCode == QLatin1String("upload_state_conflict") ||
               apiCode == QLatin1String("multipart_upload_unavailable")) {
        error.code = CloudTransferErrorCode::UploadStateConflict;
        error.retryable = false;
    }
    // Remote message text may echo a filename or delegated URL. It is parsed
    // for shape only and is never surfaced or logged.
    if (object.value(QStringLiteral("message")).toString().size() > 240)
        error.apiCode.clear();
    return error;
}

std::optional<UploadPreparation> parseUploadPreparation(
    const QJsonObject& object, const QString& expectedUploadId,
    const QString& expectedAssetId,
    const std::optional<quint64>& expectedSnapshotSequence,
    CloudTransferError* error) {
    if (!exactKeys(object,
                   {"uploadId", "status", "uploadMode",
                    "alreadyAvailable", "expiresAt"},
                   {"assetId", "snapshotSeq", "request",
                    "multipartPartSize", "multipartPartCount",
                    "uploadedParts", "parts", "nextPartNumberStart"}) ||
        !object.value(QStringLiteral("uploadId")).isString() ||
        !object.value(QStringLiteral("status")).isString() ||
        !object.value(QStringLiteral("uploadMode")).isString() ||
        !object.value(QStringLiteral("alreadyAvailable")).isBool()) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Server returned invalid upload preparation"));
        return std::nullopt;
    }

    UploadPreparation result;
    if (!normalizeUuid(object.value(QStringLiteral("uploadId")).toString(),
                       &result.uploadId) ||
        result.uploadId != expectedUploadId ||
        !parseDateTime(object.value(QStringLiteral("expiresAt")),
                       &result.expiresAt)) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Upload preparation identity mismatch"));
        return std::nullopt;
    }
    const QString status = object.value(QStringLiteral("status")).toString();
    const QString mode = object.value(QStringLiteral("uploadMode")).toString();
    if ((status != QLatin1String("uploading") &&
         status != QLatin1String("completed")) ||
        (mode != QLatin1String("single") &&
         mode != QLatin1String("multipart"))) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Upload preparation state is invalid"));
        return std::nullopt;
    }
    result.completed = status == QLatin1String("completed");
    result.multipart = mode == QLatin1String("multipart");
    result.alreadyAvailable =
        object.value(QStringLiteral("alreadyAvailable")).toBool();
    if (result.completed != result.alreadyAvailable ||
        (!result.completed &&
         result.expiresAt <= QDateTime::currentDateTimeUtc())) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Upload preparation availability is invalid"));
        return std::nullopt;
    }

    if (expectedAssetId.isEmpty()) {
        if (object.contains(QStringLiteral("assetId"))) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::InvalidResponse,
                    QStringLiteral("Snapshot preparation contained an asset"));
            return std::nullopt;
        }
        const auto snapshot = exactUnsigned(
            object.value(QStringLiteral("snapshotSeq")));
        if (!snapshot || !expectedSnapshotSequence ||
            *snapshot != *expectedSnapshotSequence) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::InvalidResponse,
                    QStringLiteral("Snapshot preparation sequence mismatch"));
            return std::nullopt;
        }
        result.snapshotSequence = *snapshot;
    } else {
        if (!object.value(QStringLiteral("assetId")).isString() ||
            !normalizeUuid(object.value(QStringLiteral("assetId")).toString(),
                           &result.assetId) ||
            result.assetId != expectedAssetId ||
            object.contains(QStringLiteral("snapshotSeq"))) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::InvalidResponse,
                    QStringLiteral("Asset preparation identity mismatch"));
            return std::nullopt;
        }
    }

    if (result.completed) {
        if (object.contains(QStringLiteral("request")) ||
            object.contains(QStringLiteral("parts")) ||
            object.contains(QStringLiteral("nextPartNumberStart"))) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::InvalidResponse,
                    QStringLiteral("Completed upload contained delegated work"));
            return std::nullopt;
        }
        return result;
    }

    if (!result.multipart) {
        if (!object.value(QStringLiteral("request")).isObject() ||
            object.contains(QStringLiteral("multipartPartSize")) ||
            object.contains(QStringLiteral("multipartPartCount")) ||
            object.contains(QStringLiteral("uploadedParts")) ||
            object.contains(QStringLiteral("parts")) ||
            object.contains(QStringLiteral("nextPartNumberStart"))) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::InvalidResponse,
                    QStringLiteral("Single upload preparation is invalid"));
            return std::nullopt;
        }
        auto request = parseDelegatedRequest(
            object.value(QStringLiteral("request")).toObject(),
            QByteArrayLiteral("PUT"), error);
        if (!request) return std::nullopt;
        result.request = std::move(*request);
        return result;
    }

    const auto partSize = exactUnsigned(
        object.value(QStringLiteral("multipartPartSize")),
        quint64(kMaximumMultipartPartBytes));
    const auto partCount = exactInteger(
        object.value(QStringLiteral("multipartPartCount")), 2,
        kMaximumMultipartParts);
    if (!partSize || *partSize < quint64(kMinimumMultipartPartBytes) ||
        !partCount || object.contains(QStringLiteral("request"))) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Multipart layout is invalid"));
        return std::nullopt;
    }
    result.partSize = *partSize;
    result.partCount = *partCount;

    if (object.contains(QStringLiteral("uploadedParts"))) {
        if (!object.value(QStringLiteral("uploadedParts")).isArray()) {
            if (error)
                *error = transferFailure(
                    CloudTransferErrorCode::InvalidResponse,
                    QStringLiteral("Multipart observed state is invalid"));
            return std::nullopt;
        }
        const QJsonArray array =
            object.value(QStringLiteral("uploadedParts")).toArray();
        if (array.size() > result.partCount) return std::nullopt;
        int previous = 0;
        for (const QJsonValue& value : array) {
            if (!value.isObject()) return std::nullopt;
            const QJsonObject part = value.toObject();
            if (!exactKeys(part, {"partNumber", "byteSize", "eTag"}) ||
                !part.value(QStringLiteral("eTag")).isString()) {
                return std::nullopt;
            }
            const auto number = exactInteger(
                part.value(QStringLiteral("partNumber")), 1,
                result.partCount);
            const auto bytes = exactUnsigned(
                part.value(QStringLiteral("byteSize")),
                quint64(kMaximumMultipartPartBytes));
            const QByteArray entityTag =
                part.value(QStringLiteral("eTag")).toString().toUtf8();
            if (!number || !bytes || *number <= previous ||
                entityTag.isEmpty() || entityTag.size() > 256 ||
                entityTag.contains('\r') || entityTag.contains('\n') ||
                entityTag.contains('\0')) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::InvalidResponse,
                        QStringLiteral("Multipart observed part is invalid"));
                return std::nullopt;
            }
            previous = *number;
            result.uploadedParts.append({*number, *bytes, entityTag});
        }
    }

    if (object.contains(QStringLiteral("parts"))) {
        if (!object.value(QStringLiteral("parts")).isArray()) return std::nullopt;
        const QJsonArray array = object.value(QStringLiteral("parts")).toArray();
        if (array.size() > kMaximumPartPage) return std::nullopt;
        int previous = 0;
        for (const QJsonValue& value : array) {
            if (!value.isObject()) return std::nullopt;
            const QJsonObject part = value.toObject();
            if (!exactKeys(part, {"partNumber", "byteSize", "request"}) ||
                !part.value(QStringLiteral("request")).isObject()) {
                return std::nullopt;
            }
            const auto number = exactInteger(
                part.value(QStringLiteral("partNumber")), 1,
                result.partCount);
            const auto bytes = exactUnsigned(
                part.value(QStringLiteral("byteSize")),
                quint64(kMaximumMultipartPartBytes));
            if (!number || !bytes || *number <= previous) return std::nullopt;
            auto request = parseDelegatedRequest(
                part.value(QStringLiteral("request")).toObject(),
                QByteArrayLiteral("PUT"), error);
            if (!request) return std::nullopt;
            previous = *number;
            result.parts.append({*number, *bytes, std::move(*request)});
        }
    }
    if (object.contains(QStringLiteral("nextPartNumberStart"))) {
        const auto next = exactInteger(
            object.value(QStringLiteral("nextPartNumberStart")), 1,
            result.partCount);
        if (!next) return std::nullopt;
        result.nextPartNumberStart = *next;
    }
    return result;
}

std::optional<DownloadPreparation> parseDownloadPreparation(
    const QJsonObject& object, CloudTransferError* error) {
    if (!exactKeys(object, {"request", "sha256", "byteSize", "contentType"}) ||
        !object.value(QStringLiteral("request")).isObject() ||
        !object.value(QStringLiteral("sha256")).isString() ||
        !object.value(QStringLiteral("contentType")).isString()) {
        if (error)
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Server returned invalid download preparation"));
        return std::nullopt;
    }
    DownloadPreparation result;
    auto request = parseDelegatedRequest(
        object.value(QStringLiteral("request")).toObject(),
        QByteArrayLiteral("GET"), error);
    const auto bytes = exactUnsigned(
        object.value(QStringLiteral("byteSize")), kMaximumBlobBytes);
    result.sha256 = object.value(QStringLiteral("sha256")).toString();
    result.contentType = object.value(QStringLiteral("contentType")).toString();
    if (!request || !bytes || *bytes == 0 || !validSha256(result.sha256) ||
        !validContentType(result.contentType)) {
        if (error && error->safeMessage.isEmpty())
            *error = transferFailure(
                CloudTransferErrorCode::InvalidResponse,
                QStringLiteral("Download metadata is invalid"));
        return std::nullopt;
    }
    result.request = std::move(*request);
    result.bytes = *bytes;
    return result;
}

std::optional<CloudBlobDescriptor> parseBlob(const QJsonObject& object,
                                             CloudTransferError* error) {
    if (!exactKeys(object,
                   {"id", "sha256", "bytes", "content_type", "kind",
                    "status", "created_at"},
                   {"created_by", "verified_at"}) ||
        !object.value(QStringLiteral("id")).isString() ||
        !object.value(QStringLiteral("sha256")).isString() ||
        !object.value(QStringLiteral("content_type")).isString() ||
        !object.value(QStringLiteral("kind")).isString() ||
        !object.value(QStringLiteral("status")).isString()) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidResponse,
                                     QStringLiteral("Verified blob is invalid"));
        return std::nullopt;
    }
    CloudBlobDescriptor result;
    const auto bytes = exactUnsigned(object.value(QStringLiteral("bytes")),
                                     kMaximumBlobBytes);
    result.sha256 = object.value(QStringLiteral("sha256")).toString();
    result.contentType = object.value(QStringLiteral("content_type")).toString();
    result.kind = object.value(QStringLiteral("kind")).toString();
    const QSet<QString> kinds = {QStringLiteral("audio"),
                                 QStringLiteral("sample"),
                                 QStringLiteral("plugin_state"),
                                 QStringLiteral("project_snapshot"),
                                 QStringLiteral("other")};
    if (!normalizeUuid(object.value(QStringLiteral("id")).toString(),
                       &result.id) ||
        !bytes || *bytes == 0 || !validSha256(result.sha256) ||
        !validContentType(result.contentType) || !kinds.contains(result.kind) ||
        object.value(QStringLiteral("status")).toString() !=
            QLatin1String("ready") ||
        !normalizeOptionalUuid(object, "created_by", &result.createdBy) ||
        !parseDateTime(object.value(QStringLiteral("created_at")),
                       &result.createdAt)) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidResponse,
                                     QStringLiteral("Verified blob mismatch"));
        return std::nullopt;
    }
    if (object.contains(QStringLiteral("verified_at")) &&
        !parseDateTime(object.value(QStringLiteral("verified_at")),
                       &result.verifiedAt, true)) {
        return std::nullopt;
    }
    result.byteSize = *bytes;
    return result;
}

std::optional<ParsedProjectAsset> parseProjectAsset(
    const QJsonObject& object, CloudTransferError* error) {
    if (!exactKeys(object,
                   {"project_id", "asset_id", "blob_id", "kind",
                    "display_name", "created_at"},
                   {"created_by"}) ||
        !object.value(QStringLiteral("kind")).isString() ||
        !object.value(QStringLiteral("display_name")).isString()) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidResponse,
                                     QStringLiteral("Project asset is invalid"));
        return std::nullopt;
    }
    ParsedProjectAsset result;
    result.kind = object.value(QStringLiteral("kind")).toString();
    result.displayName = object.value(QStringLiteral("display_name")).toString();
    if (!normalizeUuid(object.value(QStringLiteral("project_id")).toString(),
                       &result.projectId) ||
        !normalizeUuid(object.value(QStringLiteral("asset_id")).toString(),
                       &result.assetId) ||
        !normalizeUuid(object.value(QStringLiteral("blob_id")).toString(),
                       &result.blobId) ||
        assetKindName(result.kind == QLatin1String("audio")
                          ? CloudAssetKind::Audio
                          : result.kind == QLatin1String("sample")
                                ? CloudAssetKind::Sample
                                : result.kind == QLatin1String("plugin_state")
                                      ? CloudAssetKind::PluginState
                                      : CloudAssetKind::Other) != result.kind ||
        result.displayName.size() > 255 ||
        !normalizeOptionalUuid(object, "created_by", &result.createdBy) ||
        !parseDateTime(object.value(QStringLiteral("created_at")),
                       &result.createdAt)) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidResponse,
                                     QStringLiteral("Project asset mismatch"));
        return std::nullopt;
    }
    return result;
}

std::optional<ParsedSnapshot> parseSnapshot(const QJsonObject& object,
                                            CloudTransferError* error) {
    if (!exactKeys(object,
                   {"id", "project_id", "seq", "blob_id",
                    "schema_version", "asset_ids", "created_at"},
                   {"created_by"})) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidResponse,
                                     QStringLiteral("Project snapshot is invalid"));
        return std::nullopt;
    }
    ParsedSnapshot result;
    const auto sequence = exactUnsigned(object.value(QStringLiteral("seq")));
    const auto schema = exactInteger(
        object.value(QStringLiteral("schema_version")), 1, 1024);
    if (!normalizeUuid(object.value(QStringLiteral("id")).toString(),
                       &result.id) ||
        !normalizeUuid(object.value(QStringLiteral("project_id")).toString(),
                       &result.projectId) ||
        !normalizeUuid(object.value(QStringLiteral("blob_id")).toString(),
                       &result.blobId) ||
        !sequence || !schema ||
        !parseSnapshotAssetIds(object.value(QStringLiteral("asset_ids")),
                               &result.assetIds) ||
        !normalizeOptionalUuid(object, "created_by", &result.createdBy) ||
        !parseDateTime(object.value(QStringLiteral("created_at")),
                       &result.createdAt)) {
        if (error)
            *error = transferFailure(CloudTransferErrorCode::InvalidResponse,
                                     QStringLiteral("Project snapshot mismatch"));
        return std::nullopt;
    }
    result.sequence = *sequence;
    result.schemaVersion = *schema;
    return result;
}

class FileSliceDevice final : public QIODevice {
public:
    FileSliceDevice(QString path, quint64 offset, quint64 length,
                    QObject* parent = nullptr)
        : QIODevice(parent), m_file(std::move(path)), m_offset(offset),
          m_length(length) {}

    bool open(OpenMode mode) override {
        if (mode != QIODevice::ReadOnly || m_length == 0 ||
            m_offset > quint64(std::numeric_limits<qint64>::max()) ||
            m_length > quint64(std::numeric_limits<qint64>::max()) ||
            !m_file.open(QIODevice::ReadOnly) ||
            !m_file.seek(qint64(m_offset))) {
            return false;
        }
        m_position = 0;
        return QIODevice::open(mode);
    }

    bool isSequential() const override { return false; }
    qint64 size() const override { return qint64(m_length); }
    qint64 pos() const override { return qint64(m_position); }
    bool seek(qint64 position) override {
        if (position < 0 || quint64(position) > m_length ||
            !m_file.seek(qint64(m_offset) + position)) {
            return false;
        }
        m_position = quint64(position);
        return QIODevice::seek(position);
    }

protected:
    qint64 readData(char* data, qint64 maximum) override {
        if (maximum <= 0 || m_position >= m_length) return 0;
        const quint64 remaining = m_length - m_position;
        const qint64 requested =
            qint64(std::min<quint64>(remaining, quint64(maximum)));
        const qint64 count = m_file.read(data, requested);
        if (count > 0) m_position += quint64(count);
        return count;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QFile m_file;
    quint64 m_offset = 0;
    quint64 m_length = 0;
    quint64 m_position = 0;
};

} // namespace

struct CloudAssetTransferManager::Impl {
    class Worker final : public QObject {
    public:
        enum class Stage : quint8 {
            None,
            PrepareUpload,
            PutSingle,
            PutPart,
            CompleteUpload,
            PrepareDownload,
            GetDownload,
            AbortUpload,
        };

        struct Job {
            quint64 id = 0;
            CloudTransferKind kind = CloudTransferKind::AssetUpload;
            std::shared_ptr<std::atomic_bool> cancelled;
            bool failed = false;
            bool terminal = false;

            CloudAssetUploadInput assetUpload;
            CloudSnapshotUploadInput snapshotUpload;
            CloudSnapshotDownloadInput snapshotDownload;
            daw::AssetRef expectedAsset;
            QString projectId;
            QString uploadId;
            QString assetId;
            QString snapshotId;
            QString sourcePath;
            QString contentType;
            QString displayName;
            QString sha256;
            quint64 byteSize = 0;
            quint64 sequence = 0;
            int schemaVersion = 0;
            QStringList assetIds;
            CloudAssetKind assetKind = CloudAssetKind::Other;
            QString destinationPath;

            bool hashPinned = false;
            QString pinnedSha256;
            quint64 pinnedByteSize = 0;

            bool multipartPlanKnown = false;
            quint64 multipartPartSize = 0;
            int multipartPartCount = 0;
            std::map<int, QByteArray> multipartManifest;
            QVector<PreparedPart> pendingParts;
            std::optional<int> nextPartCursor;
            QSet<int> requestedPartCursors;
            int prepareCursor = 0;
            int currentPart = 0;
            quint64 currentOffset = 0;
            quint64 currentLength = 0;
            std::optional<DelegatedRequest> activeDelegated;

            QPointer<QNetworkReply> reply;
            QPointer<QTimer> timeout;
            std::unique_ptr<FileSliceDevice> uploadDevice;
            std::unique_ptr<QSaveFile> downloadFile;
            std::unique_ptr<QCryptographicHash> downloadDigest;
            quint64 downloadedBytes = 0;
            int requestGeneration = 0;
            int redirectCount = 0;
            Stage stage = Stage::None;
            qsizetype maximumResponseBytes = kMaximumApiResponseBytes;
        };

        Worker(CloudAssetTransferManager* owner, AssetCache* cache,
               NetworkFactory networkFactory)
            : m_owner(owner), m_cache(cache),
              m_networkFactory(std::move(networkFactory)) {}

        ~Worker() override { shutdown(); }

        void initialize() {
            if (m_network) return;
            m_network = m_networkFactory ? m_networkFactory() : nullptr;
            if (m_network && !m_network->parent()) m_network->setParent(this);
        }

        void updateCredentials(Credentials credentials) {
            m_credentials = std::move(credentials);
            if (!m_credentials.authenticated || m_credentials.offline ||
                m_credentials.bearerToken.isEmpty()) {
                const auto ids = jobIds();
                for (quint64 id : ids) {
                    auto iterator = m_jobs.find(id);
                    if (iterator == m_jobs.end() || iterator->second->terminal)
                        continue;
                    if (iterator->second->cancelled)
                        iterator->second->cancelled->store(true);
                    fail(*iterator->second,
                         transferFailure(
                             m_credentials.offline
                                 ? CloudTransferErrorCode::Offline
                                 : CloudTransferErrorCode::Unauthenticated,
                             m_credentials.offline
                                 ? QStringLiteral("Cloud transfers are offline")
                                 : QStringLiteral("Sign in to transfer project assets")));
                }
            }
        }

        void setTimeoutMs(int timeoutMs) {
            m_timeoutMs = std::clamp(timeoutMs, kMinimumTimeoutMs,
                                     kMaximumTimeoutMs);
        }

        void startAssetUpload(quint64 id, CloudAssetUploadInput input,
                              std::shared_ptr<std::atomic_bool> cancelled) {
            auto job = std::make_unique<Job>();
            job->id = id;
            job->kind = CloudTransferKind::AssetUpload;
            job->cancelled = std::move(cancelled);
            job->assetUpload = std::move(input);
            Job* raw = job.get();
            m_jobs[id] = std::move(job);
            if (!normalizeAssetInput(*raw)) return;
            hashAndPrepare(*raw);
        }

        void startSnapshotUpload(
            quint64 id, CloudSnapshotUploadInput input,
            std::shared_ptr<std::atomic_bool> cancelled) {
            auto job = std::make_unique<Job>();
            job->id = id;
            job->kind = CloudTransferKind::SnapshotUpload;
            job->cancelled = std::move(cancelled);
            job->snapshotUpload = std::move(input);
            Job* raw = job.get();
            m_jobs[id] = std::move(job);
            if (!normalizeSnapshotInput(*raw)) return;
            hashAndPrepare(*raw);
        }

        void startAssetDownload(quint64 id, QString projectId,
                                daw::AssetRef expected,
                                std::shared_ptr<std::atomic_bool> cancelled) {
            auto job = std::make_unique<Job>();
            job->id = id;
            job->kind = CloudTransferKind::AssetDownload;
            job->cancelled = std::move(cancelled);
            job->projectId = std::move(projectId);
            job->expectedAsset = std::move(expected);
            Job* raw = job.get();
            m_jobs[id] = std::move(job);
            if (!normalizeAssetDownload(*raw)) return;
            prepareDownload(*raw);
        }

        void startSnapshotDownload(
            quint64 id, CloudSnapshotDownloadInput input,
            std::shared_ptr<std::atomic_bool> cancelled) {
            auto job = std::make_unique<Job>();
            job->id = id;
            job->kind = CloudTransferKind::SnapshotDownload;
            job->cancelled = std::move(cancelled);
            job->snapshotDownload = std::move(input);
            Job* raw = job.get();
            m_jobs[id] = std::move(job);
            if (!normalizeSnapshotDownload(*raw)) return;
            prepareDownload(*raw);
        }

        void startAbort(quint64 id, QString projectId, QString uploadId,
                        std::shared_ptr<std::atomic_bool> cancelled) {
            auto job = std::make_unique<Job>();
            job->id = id;
            job->kind = CloudTransferKind::AbortUpload;
            job->cancelled = std::move(cancelled);
            job->projectId = std::move(projectId);
            job->uploadId = std::move(uploadId);
            Job* raw = job.get();
            m_jobs[id] = std::move(job);
            if (!normalizeUuid(raw->projectId, &raw->projectId) ||
                !normalizeUuid(raw->uploadId, &raw->uploadId)) {
                fail(*raw, transferFailure(
                               CloudTransferErrorCode::InvalidInput,
                               QStringLiteral("Upload identity is invalid")));
                return;
            }
            issueAbort(*raw);
        }

        void retry(quint64 id) {
            auto iterator = m_jobs.find(id);
            if (iterator == m_jobs.end()) return;
            Job& job = *iterator->second;
            if (!job.failed || job.terminal) return;
            cleanupNetwork(job);
            job.failed = false;
            if (job.cancelled) job.cancelled->store(false);
            job.stage = Stage::None;
            switch (job.kind) {
                case CloudTransferKind::AssetUpload:
                case CloudTransferKind::SnapshotUpload:
                    // Re-hash before reusing an idempotency key. A changed file
                    // is rejected locally instead of producing a conflicting
                    // prepare request.
                    hashAndPrepare(job);
                    break;
                case CloudTransferKind::AssetDownload:
                case CloudTransferKind::SnapshotDownload:
                    prepareDownload(job);
                    break;
                case CloudTransferKind::AbortUpload:
                    issueAbort(job);
                    break;
            }
        }

        void cancel(quint64 id) {
            auto iterator = m_jobs.find(id);
            if (iterator == m_jobs.end()) return;
            Job& job = *iterator->second;
            if (job.terminal) return;
            if (job.cancelled) job.cancelled->store(true);
            fail(job, transferFailure(CloudTransferErrorCode::Cancelled,
                                      QStringLiteral("Cloud transfer was cancelled")));
        }

        void shutdown() {
            const auto ids = jobIds();
            for (quint64 id : ids) {
                auto iterator = m_jobs.find(id);
                if (iterator == m_jobs.end()) continue;
                if (iterator->second->cancelled)
                    iterator->second->cancelled->store(true);
                cleanupNetwork(*iterator->second);
            }
            m_jobs.clear();
            if (m_network && m_network->parent() == this) {
                delete m_network;
            }
            m_network = nullptr;
        }

    private:
        QList<quint64> jobIds() const {
            QList<quint64> result;
            result.reserve(qsizetype(m_jobs.size()));
            for (const auto& [id, unused] : m_jobs) {
                Q_UNUSED(unused);
                result.append(id);
            }
            return result;
        }

        bool wasCancelled(const Job& job) const {
            return job.cancelled && job.cancelled->load();
        }

        template <typename Callback>
        void post(Callback callback) {
            const QPointer<CloudAssetTransferManager> guard = m_owner;
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard,
                [guard, callback = std::move(callback)]() mutable {
                    if (guard) callback(*guard);
                },
                Qt::QueuedConnection);
        }

        void publishState(const Job& job, CloudTransferState state) {
            const quint64 id = job.id;
            const CloudTransferKind kind = job.kind;
            post([id, kind, state](CloudAssetTransferManager& owner) {
                emit owner.transferStateChanged(id, kind, state);
            });
        }

        void publishProgress(const Job& job, quint64 completed,
                             quint64 total) {
            const quint64 id = job.id;
            const CloudTransferKind kind = job.kind;
            post([id, kind, completed, total](CloudAssetTransferManager& owner) {
                emit owner.transferProgress(id, kind, completed, total);
            });
        }

        void cleanupNetwork(Job& job) {
            if (job.timeout) {
                job.timeout->stop();
                job.timeout.clear();
            }
            if (job.reply) {
                QNetworkReply* reply = job.reply;
                job.reply.clear();
                disconnect(reply, nullptr, this, nullptr);
                if (reply->isRunning()) reply->abort();
                reply->deleteLater();
            }
            if (job.downloadFile) {
                job.downloadFile->cancelWriting();
                job.downloadFile.reset();
            }
            job.downloadDigest.reset();
            job.uploadDevice.reset();
            ++job.requestGeneration;
        }

        void fail(Job& job, CloudTransferError error) {
            if (job.terminal || job.failed) return;
            cleanupNetwork(job);
            job.failed = true;
            const bool cancelled =
                error.code == CloudTransferErrorCode::Cancelled;
            if (cancelled) job.terminal = true;
            publishState(job, cancelled ? CloudTransferState::Cancelled
                                        : CloudTransferState::Failed);
            const quint64 id = job.id;
            const CloudTransferKind kind = job.kind;
            post([id, kind, error = std::move(error)](
                     CloudAssetTransferManager& owner) {
                emit owner.transferFailed(id, kind, error);
            });
            // Failed jobs remain available for retry. Cancellation is
            // terminal, though: erase it only after the current callback has
            // unwound so no caller keeps a dangling Job reference.
            if (cancelled) {
                QMetaObject::invokeMethod(
                    this, [this, id] { finishAndErase(id); },
                    Qt::QueuedConnection);
            }
        }

        void finishAndErase(quint64 id) {
            auto iterator = m_jobs.find(id);
            if (iterator == m_jobs.end()) return;
            iterator->second->terminal = true;
            cleanupNetwork(*iterator->second);
            m_jobs.erase(iterator);
        }

        bool normalizeAssetInput(Job& job) {
            CloudAssetUploadInput& input = job.assetUpload;
            if (!normalizeUuid(input.projectId, &job.projectId) ||
                !normalizeUuid(input.uploadId, &job.uploadId) ||
                !normalizeUuid(input.assetId, &job.assetId) ||
                input.sourcePath.isEmpty()) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Asset upload identity is invalid")));
                return false;
            }
            job.sourcePath = input.sourcePath;
            job.sha256 = input.sha256.trimmed();
            job.byteSize = input.byteSize;
            job.assetKind = input.kind;
            job.contentType = input.contentType.trimmed();
            job.displayName = safeDisplayName(input.displayName);
            if ((!job.sha256.isEmpty() && !validSha256(job.sha256)) ||
                job.byteSize > kMaximumBlobBytes ||
                assetKindName(job.assetKind).isEmpty() ||
                !validContentType(job.contentType) ||
                job.displayName.isEmpty()) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Asset upload metadata is invalid")));
                return false;
            }
            return true;
        }

        bool normalizeSnapshotInput(Job& job) {
            CloudSnapshotUploadInput& input = job.snapshotUpload;
            if (!normalizeUuid(input.projectId, &job.projectId) ||
                !normalizeUuid(input.uploadId, &job.uploadId) ||
                input.sourcePath.isEmpty() ||
                input.sequence > kMaximumExactJsonInteger ||
                input.schemaVersion != 7 ||
                !isCanonicalCloudSnapshotAssetManifest(input.assetIds)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Snapshot upload identity is invalid")));
                return false;
            }
            job.sourcePath = input.sourcePath;
            job.sequence = input.sequence;
            job.schemaVersion = input.schemaVersion;
            job.assetIds = input.assetIds;
            job.sha256 = input.sha256.trimmed();
            job.byteSize = input.byteSize;
            job.contentType = input.contentType.trimmed();
            if ((!job.sha256.isEmpty() && !validSha256(job.sha256)) ||
                job.byteSize > kMaximumBlobBytes ||
                !validContentType(job.contentType)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Snapshot upload metadata is invalid")));
                return false;
            }
            return true;
        }

        bool normalizeAssetDownload(Job& job) {
            QString assetId = QString::fromStdString(
                job.expectedAsset.assetId);
            const QString sha256 = QString::fromStdString(
                job.expectedAsset.sha256);
            if (!normalizeUuid(job.projectId, &job.projectId) ||
                !normalizeUuid(assetId, &job.assetId) ||
                !validSha256(sha256) || job.expectedAsset.byteSize == 0 ||
                job.expectedAsset.byteSize > kMaximumBlobBytes || !m_cache) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Asset download metadata is invalid")));
                return false;
            }
            job.sha256 = sha256;
            job.byteSize = job.expectedAsset.byteSize;
            job.contentType =
                QString::fromStdString(job.expectedAsset.mimeType).trimmed();
            if (!job.contentType.isEmpty() &&
                !validContentType(job.contentType)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Asset content type is invalid")));
                return false;
            }
            job.destinationPath = m_cache->pathForHash(job.sha256);
            if (job.destinationPath.isEmpty()) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Asset cache destination is invalid")));
                return false;
            }
            return true;
        }

        bool normalizeSnapshotDownload(Job& job) {
            const CloudSnapshotDownloadInput& input = job.snapshotDownload;
            if (!normalizeUuid(input.projectId, &job.projectId) ||
                !normalizeUuid(input.snapshotId, &job.snapshotId) ||
                (!input.sha256.isEmpty() && !validSha256(input.sha256)) ||
                input.byteSize > kMaximumBlobBytes ||
                input.destinationPath.isEmpty() ||
                !QFileInfo(input.destinationPath).isAbsolute()) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Snapshot download metadata is invalid")));
                return false;
            }
            // Bootstrap identifies a snapshot by its authorized id. Its blob
            // digest/size are intentionally not duplicated in bootstrap
            // metadata, so an empty hash/zero size means "discover through
            // the authenticated preparation endpoint". Non-empty values are
            // still strict caller-supplied preconditions.
            job.sha256 = input.sha256.trimmed();
            job.byteSize = input.byteSize;
            job.destinationPath = input.destinationPath;
            return true;
        }

        void hashAndPrepare(Job& job) {
            if (wasCancelled(job)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::Cancelled,
                              QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            publishState(job, CloudTransferState::Hashing);
            QFile input(job.sourcePath);
            if (!input.open(QIODevice::ReadOnly)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::FileUnavailable,
                              QStringLiteral("Upload source is unavailable")));
                return;
            }
            const qint64 declaredSize = input.size();
            if (declaredSize <= 0 || quint64(declaredSize) > kMaximumBlobBytes) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidInput,
                              QStringLiteral("Upload source size is invalid")));
                return;
            }
            QCryptographicHash digest(QCryptographicHash::Sha256);
            QByteArray block(kIoBlockBytes, Qt::Uninitialized);
            quint64 total = 0;
            quint64 lastPublished = 0;
            while (true) {
                if (wasCancelled(job)) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::Cancelled,
                                  QStringLiteral("Cloud transfer was cancelled")));
                    return;
                }
                const qint64 count = input.read(block.data(), block.size());
                if (count < 0) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::FileReadFailure,
                                  QStringLiteral("Could not read upload source")));
                    return;
                }
                if (count == 0) break;
                digest.addData(QByteArrayView(block.constData(), count));
                total += quint64(count);
                if (total - lastPublished >= 16ULL * 1024 * 1024) {
                    publishProgress(job, total, quint64(declaredSize));
                    lastPublished = total;
                }
            }
            const QString computed =
                QString::fromLatin1(digest.result().toHex());
            if (total != quint64(declaredSize) ||
                (!job.sha256.isEmpty() && job.sha256 != computed) ||
                (job.byteSize != 0 && job.byteSize != total)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::IntegrityMismatch,
                              QStringLiteral("Upload source checksum or size mismatch")));
                return;
            }
            if (job.hashPinned &&
                (job.pinnedSha256 != computed ||
                 job.pinnedByteSize != total)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::IntegrityMismatch,
                              QStringLiteral("Upload source changed before retry")));
                return;
            }
            job.hashPinned = true;
            job.pinnedSha256 = computed;
            job.pinnedByteSize = total;
            job.sha256 = computed;
            job.byteSize = total;
            job.requestedPartCursors.clear();
            job.pendingParts.clear();
            job.nextPartCursor.reset();
            publishProgress(job, total, total);
            prepareUpload(job, 0);
        }

        std::optional<Credentials> checkedCredentials(
            CloudTransferError* error) const {
            const Credentials& credentials = m_credentials;
            if (!credentials.authenticated || credentials.bearerToken.isEmpty()) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::Unauthenticated,
                        QStringLiteral("Sign in to transfer project assets"));
                return std::nullopt;
            }
            if (credentials.offline) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::Offline,
                        QStringLiteral("Cloud transfers are offline"));
                return std::nullopt;
            }
            const QUrl origin(credentials.apiOrigin);
            if (credentials.bearerToken.size() > 16 * 1024 ||
                credentials.bearerToken.contains('\r') ||
                credentials.bearerToken.contains('\n') ||
                !normalizeUuid(credentials.userId) ||
                !normalizeUuid(credentials.deviceId) || !safeOrigin(origin)) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::UnsafeOrigin,
                        QStringLiteral("Cloud transfer credentials are invalid"));
                return std::nullopt;
            }
            return credentials;
        }

        std::optional<QNetworkRequest> authorizedRequest(
            const Credentials& credentials, const QString& relativePath,
            bool jsonBody, CloudTransferError* error) const {
            QUrl origin(credentials.apiOrigin);
            if (relativePath.isEmpty() ||
                relativePath.startsWith(QLatin1Char('/')) ||
                relativePath.contains(QStringLiteral(".."))) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::UnsafeOrigin,
                        QStringLiteral("Cloud transfer path is invalid"));
                return std::nullopt;
            }
            QString path = origin.path();
            while (path.endsWith(QLatin1Char('/'))) path.chop(1);
            path += QLatin1Char('/') + relativePath;
            QUrl endpoint = origin;
            endpoint.setPath(path);
            endpoint.setQuery(QString{});
            endpoint.setUserInfo({});
            endpoint.setFragment({});
            if (!sameOrigin(origin, endpoint)) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::UnsafeOrigin,
                        QStringLiteral("Cloud transfer origin mismatch"));
                return std::nullopt;
            }
            QNetworkRequest request(endpoint);
            request.setRawHeader("Accept", "application/json");
            if (jsonBody)
                request.setRawHeader("Content-Type", "application/json");
            request.setRawHeader(
                "Authorization",
                QByteArrayLiteral("Bearer ") + credentials.bearerToken);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                                 QNetworkRequest::Manual);
            request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                                 QNetworkRequest::Manual);
            // The explicit reply timer below owns the public Timeout result.
            // Keep Qt's transport watchdog as a later fail-safe so two timers
            // cannot race and misclassify a deadline as NetworkFailure.
            request.setTransferTimeout(m_timeoutMs +
                                       kQtTransferTimeoutGraceMs);
            return request;
        }

        void prepareUpload(Job& job, int partNumberStart) {
            if (wasCancelled(job)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::Cancelled,
                              QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            const int effectiveCursor = partNumberStart == 0 ? 1
                                                              : partNumberStart;
            if (partNumberStart != 0 &&
                job.requestedPartCursors.contains(effectiveCursor)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidResponse,
                              QStringLiteral("Multipart preparation cursor repeated")));
                return;
            }
            job.requestedPartCursors.insert(effectiveCursor);
            job.prepareCursor = effectiveCursor;
            publishState(job, CloudTransferState::Preparing);
            QJsonObject body{
                {QStringLiteral("uploadId"), job.uploadId},
                {QStringLiteral("sha256"), job.sha256},
                {QStringLiteral("byteSize"), double(job.byteSize)},
                {QStringLiteral("contentType"), job.contentType},
            };
            QString path;
            if (job.kind == CloudTransferKind::AssetUpload) {
                body.insert(QStringLiteral("assetId"), job.assetId);
                body.insert(QStringLiteral("kind"),
                            assetKindName(job.assetKind));
                body.insert(QStringLiteral("displayName"), job.displayName);
                path = QStringLiteral("desktop/projects/%1/asset-uploads/prepare")
                           .arg(job.projectId);
            } else {
                body.insert(QStringLiteral("seq"), double(job.sequence));
                body.insert(QStringLiteral("schemaVersion"), job.schemaVersion);
                QJsonArray assetIds;
                for (const QString& assetId : job.assetIds)
                    assetIds.push_back(assetId);
                body.insert(QStringLiteral("assetIds"), assetIds);
                path = QStringLiteral(
                           "desktop/projects/%1/snapshot-uploads/prepare")
                           .arg(job.projectId);
            }
            if (partNumberStart != 0)
                body.insert(QStringLiteral("partNumberStart"),
                            partNumberStart);
            issueApi(job, Stage::PrepareUpload, QByteArrayLiteral("POST"),
                     path,
                     QJsonDocument(body).toJson(QJsonDocument::Compact), 200);
        }

        void prepareDownload(Job& job) {
            if (wasCancelled(job)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::Cancelled,
                              QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            publishState(job, CloudTransferState::Preparing);
            QString path;
            if (job.kind == CloudTransferKind::AssetDownload) {
                path = QStringLiteral("desktop/projects/%1/assets/%2/download")
                           .arg(job.projectId, job.assetId);
            } else {
                path = QStringLiteral(
                           "desktop/projects/%1/snapshots/%2/download")
                           .arg(job.projectId, job.snapshotId);
            }
            issueApi(job, Stage::PrepareDownload, QByteArrayLiteral("GET"),
                     path, {}, 200);
        }

        void issueAbort(Job& job) {
            publishState(job, CloudTransferState::Preparing);
            const QString path =
                QStringLiteral("desktop/projects/%1/uploads/%2")
                    .arg(job.projectId, job.uploadId);
            issueApi(job, Stage::AbortUpload, QByteArrayLiteral("DELETE"),
                     path, {}, 204);
        }

        void issueApi(Job& job, Stage stage, const QByteArray& method,
                      const QString& relativePath, const QByteArray& body,
                      int expectedStatus) {
            if (!m_network) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::NetworkFailure,
                              QStringLiteral("Cloud transfer network is unavailable"),
                              0, true));
                return;
            }
            CloudTransferError requestError;
            const auto credentials = checkedCredentials(&requestError);
            if (!credentials) {
                fail(job, requestError);
                return;
            }
            const auto request = authorizedRequest(
                *credentials, relativePath, !body.isEmpty(), &requestError);
            if (!request) {
                fail(job, requestError);
                return;
            }
            cleanupNetwork(job);
            job.failed = false;
            job.stage = stage;
            job.maximumResponseBytes = kMaximumApiResponseBytes;
            const int generation = job.requestGeneration;
            QNetworkReply* reply =
                m_network->sendCustomRequest(*request, method, body);
            if (!reply) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::NetworkFailure,
                              QStringLiteral("Cloud transfer request could not start"),
                              0, true));
                return;
            }
            job.reply = reply;
            auto* timeout = new QTimer(reply);
            timeout->setSingleShot(true);
            timeout->setInterval(m_timeoutMs);
            job.timeout = timeout;
            connect(timeout, &QTimer::timeout, this,
                    [this, id = job.id, generation] {
                        Job* current = matching(id, generation);
                        if (!current) return;
                        fail(*current,
                             transferFailure(
                                 CloudTransferErrorCode::Timeout,
                                 QStringLiteral("Cloud transfer request timed out"),
                                 0, true));
                    });
            connect(reply, &QNetworkReply::readyRead, this,
                    [this, id = job.id, generation,
                     guard = QPointer<QNetworkReply>(reply)] {
                        Job* current = matching(id, generation, guard);
                        if (!current || !guard) return;
                        if (guard->bytesAvailable() >
                            current->maximumResponseBytes) {
                            fail(*current,
                                 transferFailure(
                                     CloudTransferErrorCode::ResponseTooLarge,
                                     QStringLiteral("Cloud response is too large")));
                        }
                    });
            connect(reply, &QNetworkReply::finished, this,
                    [this, id = job.id, generation, expectedStatus,
                     guard = QPointer<QNetworkReply>(reply)] {
                        if (guard)
                            finishApi(id, generation, guard, expectedStatus);
                    });
            timeout->start();
        }

        Job* matching(quint64 id, int generation,
                      QNetworkReply* reply = nullptr) {
            auto iterator = m_jobs.find(id);
            if (iterator == m_jobs.end()) return nullptr;
            Job* job = iterator->second.get();
            if (job->terminal || job->failed ||
                job->requestGeneration != generation ||
                (reply && job->reply != reply)) {
                return nullptr;
            }
            return job;
        }

        void releaseFinishedReply(Job& job, QNetworkReply* reply) {
            if (job.timeout) job.timeout->stop();
            job.timeout.clear();
            if (job.reply == reply) job.reply.clear();
            disconnect(reply, nullptr, this, nullptr);
            reply->deleteLater();
            job.uploadDevice.reset();
        }

        void finishApi(quint64 id, int generation, QNetworkReply* reply,
                       int expectedStatus) {
            Job* job = matching(id, generation, reply);
            if (!job) return;
            const Stage stage = job->stage;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const QNetworkReply::NetworkError networkError = reply->error();
            QByteArray body = reply->read(kMaximumApiResponseBytes + 1);
            releaseFinishedReply(*job, reply);
            if (body.size() > kMaximumApiResponseBytes) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::ResponseTooLarge,
                               QStringLiteral("Cloud response is too large")));
                return;
            }
            if (status >= 300 && status < 400) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::RedirectRejected,
                               QStringLiteral("Cloud API redirect was rejected"),
                               status));
                return;
            }
            if (networkError != QNetworkReply::NoError) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::NetworkFailure,
                               QStringLiteral("Cloud transfer network request failed"),
                               status, true));
                return;
            }
            if (status != expectedStatus) {
                fail(*job, parseApiError(body, status));
                return;
            }
            if (wasCancelled(*job)) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::Cancelled,
                               QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            switch (stage) {
                case Stage::PrepareUpload:
                    handleUploadPreparation(*job, body);
                    break;
                case Stage::CompleteUpload:
                    handleUploadCompletion(*job, body);
                    break;
                case Stage::PrepareDownload:
                    handleDownloadPreparation(*job, body);
                    break;
                case Stage::AbortUpload: {
                    if (!body.trimmed().isEmpty()) {
                        fail(*job, transferFailure(
                                       CloudTransferErrorCode::InvalidResponse,
                                       QStringLiteral("Upload abort response was invalid")));
                        break;
                    }
                    const quint64 transferId = job->id;
                    const QString projectId = job->projectId;
                    const QString uploadId = job->uploadId;
                    publishState(*job, CloudTransferState::Ready);
                    post([transferId, projectId, uploadId](
                             CloudAssetTransferManager& owner) {
                        emit owner.uploadAborted(transferId, projectId, uploadId);
                    });
                    finishAndErase(transferId);
                    break;
                }
                default:
                    fail(*job, transferFailure(
                                   CloudTransferErrorCode::InvalidResponse,
                                   QStringLiteral("Cloud transfer state is invalid")));
                    break;
            }
        }

        void handleUploadPreparation(Job& job, const QByteArray& body) {
            CloudTransferError parseError;
            const auto object = parseJsonObject(body, &parseError);
            if (!object) {
                fail(job, parseError);
                return;
            }
            const std::optional<quint64> snapshotSequence =
                job.kind == CloudTransferKind::SnapshotUpload
                    ? std::optional<quint64>(job.sequence)
                    : std::nullopt;
            auto preparation = parseUploadPreparation(
                *object, job.uploadId,
                job.kind == CloudTransferKind::AssetUpload ? job.assetId
                                                           : QString{},
                snapshotSequence, &parseError);
            if (!preparation) {
                if (parseError.safeMessage.isEmpty())
                    parseError = transferFailure(
                        CloudTransferErrorCode::InvalidResponse,
                        QStringLiteral("Upload preparation is invalid"));
                fail(job, parseError);
                return;
            }
            if (preparation->completed) {
                completeUpload(job);
                return;
            }
            if (!preparation->multipart) {
                if (job.multipartPlanKnown || !preparation->request) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::InvalidResponse,
                                  QStringLiteral("Upload mode changed unexpectedly")));
                    return;
                }
                issueDelegatedPut(job, *preparation->request, 0,
                                  job.byteSize, 0, 0);
                return;
            }
            applyMultipartPreparation(job, *preparation);
        }

        std::optional<quint64> expectedPartBytes(const Job& job,
                                                 int partNumber) const {
            if (!job.multipartPlanKnown || partNumber < 1 ||
                partNumber > job.multipartPartCount ||
                job.multipartPartSize == 0) {
                return std::nullopt;
            }
            const quint64 index = quint64(partNumber - 1);
            if (index > std::numeric_limits<quint64>::max() /
                            job.multipartPartSize) {
                return std::nullopt;
            }
            const quint64 offset = index * job.multipartPartSize;
            if (offset >= job.byteSize) return std::nullopt;
            return std::min(job.multipartPartSize, job.byteSize - offset);
        }

        void applyMultipartPreparation(Job& job,
                                       const UploadPreparation& preparation) {
            if (!job.multipartPlanKnown) {
                const quint64 prefix =
                    quint64(preparation.partCount - 1) * preparation.partSize;
                if (preparation.partCount < 2 || preparation.partSize == 0 ||
                    prefix >= job.byteSize ||
                    job.byteSize - prefix > preparation.partSize) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::InvalidResponse,
                                  QStringLiteral("Multipart layout does not match file size")));
                    return;
                }
                job.multipartPlanKnown = true;
                job.multipartPartSize = preparation.partSize;
                job.multipartPartCount = preparation.partCount;
            } else if (job.multipartPartSize != preparation.partSize ||
                       job.multipartPartCount != preparation.partCount) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::UploadStateConflict,
                              QStringLiteral("Multipart layout changed during retry")));
                return;
            }

            std::map<int, QByteArray> observed;
            for (const UploadedPart& part : preparation.uploadedParts) {
                const auto expected = expectedPartBytes(job, part.number);
                if (!expected || *expected != part.bytes ||
                    observed.contains(part.number)) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::InvalidResponse,
                                  QStringLiteral("Multipart observed state is invalid")));
                    return;
                }
                observed.emplace(part.number, part.entityTag);
            }
            // Prepare always returns the complete provider-observed manifest.
            // A previously acknowledged part disappearing or changing means
            // this upload cannot be resumed safely.
            for (const auto& [number, entityTag] : job.multipartManifest) {
                const auto found = observed.find(number);
                if (found == observed.end() || found->second != entityTag) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::UploadStateConflict,
                                  QStringLiteral("Multipart provider state changed")));
                    return;
                }
            }
            job.multipartManifest = observed;

            QSet<int> missingNumbers;
            for (const PreparedPart& part : preparation.parts) {
                const auto expected = expectedPartBytes(job, part.number);
                if (!expected || *expected != part.bytes ||
                    part.number < job.prepareCursor ||
                    job.multipartManifest.contains(part.number) ||
                    missingNumbers.contains(part.number)) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::InvalidResponse,
                                  QStringLiteral("Multipart missing-part page is invalid")));
                    return;
                }
                missingNumbers.insert(part.number);
            }
            if (preparation.nextPartNumberStart &&
                *preparation.nextPartNumberStart <= job.prepareCursor) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidResponse,
                              QStringLiteral("Multipart cursor did not advance")));
                return;
            }
            job.pendingParts = preparation.parts;
            job.nextPartCursor = preparation.nextPartNumberStart;
            publishProgress(job, completedMultipartBytes(job), job.byteSize);
            continueMultipart(job);
        }

        quint64 completedMultipartBytes(const Job& job) const {
            quint64 result = 0;
            for (const auto& [number, unused] : job.multipartManifest) {
                Q_UNUSED(unused);
                const auto bytes = expectedPartBytes(job, number);
                if (bytes && result <= std::numeric_limits<quint64>::max() - *bytes)
                    result += *bytes;
            }
            return result;
        }

        void continueMultipart(Job& job) {
            if (!job.pendingParts.isEmpty()) {
                const PreparedPart part = job.pendingParts.takeFirst();
                const quint64 offset =
                    quint64(part.number - 1) * job.multipartPartSize;
                issueDelegatedPut(job, part.request, offset, part.bytes,
                                  part.number, 0);
                return;
            }
            if (job.nextPartCursor) {
                const int cursor = *job.nextPartCursor;
                job.nextPartCursor.reset();
                prepareUpload(job, cursor);
                return;
            }
            if (int(job.multipartManifest.size()) != job.multipartPartCount) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidResponse,
                              QStringLiteral("Multipart preparation omitted required parts")));
                return;
            }
            for (int number = 1; number <= job.multipartPartCount; ++number) {
                if (!job.multipartManifest.contains(number)) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::InvalidResponse,
                                  QStringLiteral("Multipart manifest has a sequence gap")));
                    return;
                }
            }
            completeUpload(job);
        }

        bool validateDelegatedForPut(const Job& job,
                                     const DelegatedRequest& delegated,
                                     quint64 expectedBytes,
                                     CloudTransferError* error) const {
            const QUrl apiOrigin(m_credentials.apiOrigin);
            if (delegated.method != QByteArrayLiteral("PUT") ||
                delegated.expiresAt <= QDateTime::currentDateTimeUtc() ||
                !safeOrigin(apiOrigin) ||
                (apiOrigin.scheme() == QLatin1String("https") &&
                 delegated.url.scheme() != QLatin1String("https"))) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::DelegatedRequestRejected,
                        QStringLiteral("Delegated upload request was rejected"));
                return false;
            }
            std::optional<quint64> contentLength;
            for (const auto& [name, value] : delegated.headers) {
                if (name.compare(QByteArrayLiteral("content-length"),
                                 Qt::CaseInsensitive) == 0) {
                    bool ok = false;
                    const qulonglong parsed = value.toULongLong(&ok);
                    if (!ok || QByteArray::number(parsed) != value.trimmed())
                        return false;
                    contentLength = quint64(parsed);
                }
                if (job.currentPart == 0 &&
                    name.compare(QByteArrayLiteral("content-type"),
                                 Qt::CaseInsensitive) == 0 &&
                    QString::fromUtf8(value) != job.contentType) {
                    return false;
                }
            }
            if (!contentLength || *contentLength != expectedBytes) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::DelegatedRequestRejected,
                        QStringLiteral("Delegated upload length was rejected"));
                return false;
            }
            return true;
        }

        std::optional<QNetworkRequest> delegatedNetworkRequest(
            const DelegatedRequest& delegated, CloudTransferError* error) const {
            if (!delegated.url.isValid() || delegated.url.host().isEmpty() ||
                !delegated.url.userInfo().isEmpty() ||
                delegated.url.hasFragment()) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::DelegatedRequestRejected,
                        QStringLiteral("Delegated request URL was rejected"));
                return std::nullopt;
            }
            QNetworkRequest request(delegated.url);
            for (const auto& [name, value] : delegated.headers)
                request.setRawHeader(name, value);
            // No Authorization, API Accept header, cookie jar or inferred
            // object-store credential is copied onto delegated requests.
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                                 QNetworkRequest::Manual);
            request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                                 QNetworkRequest::Manual);
            request.setTransferTimeout(m_timeoutMs +
                                       kQtTransferTimeoutGraceMs);
            return request;
        }

        void issueDelegatedPut(Job& job, DelegatedRequest delegated,
                               quint64 offset, quint64 length, int partNumber,
                               int redirectCount) {
            if (wasCancelled(job)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::Cancelled,
                              QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            job.currentPart = partNumber;
            job.currentOffset = offset;
            job.currentLength = length;
            CloudTransferError requestError;
            if (!validateDelegatedForPut(job, delegated, length,
                                         &requestError)) {
                if (requestError.safeMessage.isEmpty())
                    requestError = transferFailure(
                        CloudTransferErrorCode::DelegatedRequestRejected,
                        QStringLiteral("Delegated upload headers were rejected"));
                fail(job, requestError);
                return;
            }
            const auto request = delegatedNetworkRequest(delegated,
                                                          &requestError);
            if (!request || !m_network) {
                fail(job, requestError.safeMessage.isEmpty()
                              ? transferFailure(
                                    CloudTransferErrorCode::NetworkFailure,
                                    QStringLiteral("Upload network is unavailable"),
                                    0, true)
                              : requestError);
                return;
            }
            cleanupNetwork(job);
            job.failed = false;
            job.stage = partNumber == 0 ? Stage::PutSingle : Stage::PutPart;
            job.activeDelegated = delegated;
            job.redirectCount = redirectCount;
            job.maximumResponseBytes = kMaximumProviderResponseBytes;
            job.uploadDevice = std::make_unique<FileSliceDevice>(
                job.sourcePath, offset, length);
            if (!job.uploadDevice->open(QIODevice::ReadOnly)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::FileUnavailable,
                              QStringLiteral("Upload source is unavailable")));
                return;
            }
            const int generation = job.requestGeneration;
            QNetworkReply* reply = m_network->sendCustomRequest(
                *request, QByteArrayLiteral("PUT"), job.uploadDevice.get());
            if (!reply) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::NetworkFailure,
                              QStringLiteral("Delegated upload could not start"),
                              0, true));
                return;
            }
            job.reply = reply;
            auto* timeout = new QTimer(reply);
            timeout->setSingleShot(true);
            timeout->setInterval(m_timeoutMs);
            job.timeout = timeout;
            publishState(job, CloudTransferState::Uploading);
            connect(timeout, &QTimer::timeout, this,
                    [this, id = job.id, generation] {
                        Job* current = matching(id, generation);
                        if (current)
                            fail(*current,
                                 transferFailure(
                                     CloudTransferErrorCode::Timeout,
                                     QStringLiteral("Delegated upload timed out"),
                                     0, true));
                    });
            connect(reply, &QNetworkReply::readyRead, this,
                    [this, id = job.id, generation,
                     guard = QPointer<QNetworkReply>(reply)] {
                        Job* current = matching(id, generation, guard);
                        if (current && guard &&
                            guard->bytesAvailable() >
                                kMaximumProviderResponseBytes) {
                            fail(*current,
                                 transferFailure(
                                     CloudTransferErrorCode::ResponseTooLarge,
                                     QStringLiteral("Storage response is too large")));
                        }
                    });
            connect(reply, &QNetworkReply::uploadProgress, this,
                    [this, id = job.id, generation](qint64 sent, qint64) {
                        Job* current = matching(id, generation);
                        if (!current || sent < 0) return;
                        const quint64 base =
                            current->currentPart == 0
                                ? 0
                                : completedMultipartBytes(*current);
                        publishProgress(
                            *current,
                            std::min(current->byteSize,
                                     base + quint64(sent)),
                            current->byteSize);
                    });
            connect(reply, &QNetworkReply::finished, this,
                    [this, id = job.id, generation,
                     guard = QPointer<QNetworkReply>(reply)] {
                        if (guard) finishDelegatedPut(id, generation, guard);
                    });
            timeout->start();
        }

        bool safeRedirect(const QUrl& current, const QUrl& target) const {
            return target.isValid() && !target.isRelative() &&
                   target.userInfo().isEmpty() && !target.hasFragment() &&
                   sameOrigin(current, target) &&
                   !(current.scheme() == QLatin1String("https") &&
                     target.scheme() != QLatin1String("https"));
        }

        void finishDelegatedPut(quint64 id, int generation,
                                QNetworkReply* reply) {
            Job* job = matching(id, generation, reply);
            if (!job) return;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const QNetworkReply::NetworkError networkError = reply->error();
            const QVariant redirectValue =
                reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
            const QByteArray entityTag = reply->rawHeader("ETag");
            const QUrl currentUrl = job->activeDelegated
                                        ? job->activeDelegated->url
                                        : QUrl{};
            const Stage stage = job->stage;
            const quint64 offset = job->currentOffset;
            const quint64 length = job->currentLength;
            const int partNumber = job->currentPart;
            const int redirectCount = job->redirectCount;
            DelegatedRequest delegated = job->activeDelegated.value_or(
                DelegatedRequest{});
            const QByteArray response =
                reply->read(kMaximumProviderResponseBytes + 1);
            releaseFinishedReply(*job, reply);
            if (response.size() > kMaximumProviderResponseBytes) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::ResponseTooLarge,
                               QStringLiteral("Storage response is too large")));
                return;
            }
            if (status >= 300 && status < 400) {
                QUrl target = redirectValue.toUrl();
                if (target.isRelative()) target = currentUrl.resolved(target);
                if ((status != 307 && status != 308) ||
                    redirectCount >= kMaximumRedirects ||
                    !safeRedirect(currentUrl, target)) {
                    fail(*job, transferFailure(
                                   CloudTransferErrorCode::RedirectRejected,
                                   QStringLiteral("Storage redirect was rejected"),
                                   status));
                    return;
                }
                delegated.url = target;
                issueDelegatedPut(*job, std::move(delegated), offset, length,
                                  partNumber, redirectCount + 1);
                return;
            }
            if (networkError != QNetworkReply::NoError) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::NetworkFailure,
                               QStringLiteral("Delegated upload failed"), status,
                               true));
                return;
            }
            if (status != 200) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::UnexpectedStatus,
                               QStringLiteral("Storage rejected upload bytes"),
                               status, status >= 500));
                return;
            }
            if (wasCancelled(*job)) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::Cancelled,
                               QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            if (stage == Stage::PutSingle) {
                publishProgress(*job, job->byteSize, job->byteSize);
                completeUpload(*job);
                return;
            }
            if (stage != Stage::PutPart || partNumber < 1 ||
                entityTag.isEmpty() || entityTag.size() > 256 ||
                entityTag.contains('\r') || entityTag.contains('\n') ||
                entityTag.contains('\0')) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::MissingEntityTag,
                               QStringLiteral("Storage did not return a valid ETag")));
                return;
            }
            const auto existing = job->multipartManifest.find(partNumber);
            if (existing != job->multipartManifest.end() &&
                existing->second != entityTag) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::UploadStateConflict,
                               QStringLiteral("Multipart ETag changed unexpectedly")));
                return;
            }
            job->multipartManifest[partNumber] = entityTag;
            publishProgress(*job, completedMultipartBytes(*job), job->byteSize);
            continueMultipart(*job);
        }

        void completeUpload(Job& job) {
            publishState(job, CloudTransferState::Completing);
            QByteArray body;
            if (job.multipartPlanKnown) {
                QJsonArray parts;
                for (int number = 1; number <= job.multipartPartCount;
                     ++number) {
                    const auto iterator = job.multipartManifest.find(number);
                    if (iterator == job.multipartManifest.end()) {
                        fail(job, transferFailure(
                                      CloudTransferErrorCode::InvalidResponse,
                                      QStringLiteral("Multipart manifest is incomplete")));
                        return;
                    }
                    parts.append(QJsonObject{
                        {QStringLiteral("partNumber"), number},
                        {QStringLiteral("eTag"),
                         QString::fromUtf8(iterator->second)},
                    });
                }
                body = QJsonDocument(
                           QJsonObject{{QStringLiteral("parts"), parts}})
                           .toJson(QJsonDocument::Compact);
            }
            const QString path =
                job.kind == CloudTransferKind::AssetUpload
                    ? QStringLiteral(
                          "desktop/projects/%1/asset-uploads/%2/complete")
                          .arg(job.projectId, job.uploadId)
                    : QStringLiteral(
                          "desktop/projects/%1/snapshot-uploads/%2/complete")
                          .arg(job.projectId, job.uploadId);
            issueApi(job, Stage::CompleteUpload, QByteArrayLiteral("POST"),
                     path, body, 200);
        }

        void handleUploadCompletion(Job& job, const QByteArray& body) {
            CloudTransferError parseError;
            const auto root = parseJsonObject(body, &parseError);
            if (!root) {
                fail(job, parseError);
                return;
            }
            if (job.kind == CloudTransferKind::AssetUpload) {
                if (!exactKeys(*root, {"asset", "blob"}) ||
                    !root->value(QStringLiteral("asset")).isObject() ||
                    !root->value(QStringLiteral("blob")).isObject()) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::InvalidResponse,
                                  QStringLiteral("Completed asset response is invalid")));
                    return;
                }
                auto asset = parseProjectAsset(
                    root->value(QStringLiteral("asset")).toObject(),
                    &parseError);
                auto blob = parseBlob(
                    root->value(QStringLiteral("blob")).toObject(),
                    &parseError);
                const QString expectedKind = assetKindName(job.assetKind);
                if (!asset || !blob || asset->projectId != job.projectId ||
                    asset->assetId != job.assetId ||
                    asset->blobId != blob->id || asset->kind != expectedKind ||
                    blob->kind != expectedKind || blob->sha256 != job.sha256 ||
                    blob->byteSize != job.byteSize ||
                    blob->contentType != job.contentType) {
                    fail(job, parseError.safeMessage.isEmpty()
                                  ? transferFailure(
                                        CloudTransferErrorCode::IntegrityMismatch,
                                        QStringLiteral("Completed asset metadata mismatch"))
                                  : parseError);
                    return;
                }
                CloudAssetUploadResult result;
                result.projectId = job.projectId;
                result.uploadId = job.uploadId;
                result.blobId = blob->id;
                result.contentType = blob->contentType;
                result.asset.assetId = job.assetId.toStdString();
                result.asset.sha256 = job.sha256.toStdString();
                result.asset.byteSize = job.byteSize;
                result.asset.kind = documentAssetKind(job.assetKind);
                result.asset.originalName = asset->displayName.toStdString();
                result.asset.mimeType = job.contentType.toStdString();
                const quint64 id = job.id;
                publishState(job, CloudTransferState::Ready);
                post([id, result](CloudAssetTransferManager& owner) {
                    emit owner.assetUploadCompleted(id, result);
                });
                finishAndErase(id);
                return;
            }

            if (!exactKeys(*root, {"snapshot", "blob"}) ||
                !root->value(QStringLiteral("snapshot")).isObject() ||
                !root->value(QStringLiteral("blob")).isObject()) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::InvalidResponse,
                              QStringLiteral("Completed snapshot response is invalid")));
                return;
            }
            auto snapshot = parseSnapshot(
                root->value(QStringLiteral("snapshot")).toObject(),
                &parseError);
            auto blob = parseBlob(
                root->value(QStringLiteral("blob")).toObject(), &parseError);
            if (!snapshot || !blob || snapshot->projectId != job.projectId ||
                snapshot->sequence != job.sequence ||
                snapshot->schemaVersion != job.schemaVersion ||
                snapshot->assetIds != job.assetIds ||
                snapshot->blobId != blob->id ||
                blob->kind != QLatin1String("project_snapshot") ||
                blob->sha256 != job.sha256 ||
                blob->byteSize != job.byteSize ||
                blob->contentType != job.contentType) {
                fail(job, parseError.safeMessage.isEmpty()
                              ? transferFailure(
                                    CloudTransferErrorCode::IntegrityMismatch,
                                    QStringLiteral("Completed snapshot metadata mismatch"))
                              : parseError);
                return;
            }
            CloudSnapshotUploadResult result;
            result.projectId = job.projectId;
            result.uploadId = job.uploadId;
            result.snapshotId = snapshot->id;
            result.blobId = blob->id;
            result.sequence = snapshot->sequence;
            result.schemaVersion = snapshot->schemaVersion;
            result.sha256 = blob->sha256;
            result.byteSize = blob->byteSize;
            result.contentType = blob->contentType;
            result.assetIds = snapshot->assetIds;
            const quint64 id = job.id;
            publishState(job, CloudTransferState::Ready);
            post([id, result](CloudAssetTransferManager& owner) {
                emit owner.snapshotUploadCompleted(id, result);
            });
            finishAndErase(id);
        }

        void handleDownloadPreparation(Job& job, const QByteArray& body) {
            CloudTransferError parseError;
            const auto root = parseJsonObject(body, &parseError);
            if (!root) {
                fail(job, parseError);
                return;
            }
            auto preparation = parseDownloadPreparation(*root, &parseError);
            if (!preparation ||
                (!job.sha256.isEmpty() &&
                 preparation->sha256 != job.sha256) ||
                (job.byteSize != 0 && preparation->bytes != job.byteSize) ||
                (!job.contentType.isEmpty() &&
                 preparation->contentType != job.contentType)) {
                fail(job, parseError.safeMessage.isEmpty()
                              ? transferFailure(
                                    CloudTransferErrorCode::IntegrityMismatch,
                                    QStringLiteral("Download metadata mismatch"))
                              : parseError);
                return;
            }
            // From this point onward these authenticated, server-authoritative
            // values become exact verification preconditions for the
            // delegated object-store response.
            job.sha256 = preparation->sha256;
            job.byteSize = preparation->bytes;
            job.contentType = preparation->contentType;
            if (job.kind == CloudTransferKind::AssetDownload)
                job.expectedAsset.mimeType = job.contentType.toStdString();
            issueDelegatedGet(job, std::move(preparation->request), 0);
        }

        bool validateDelegatedForGet(const DelegatedRequest& delegated,
                                     CloudTransferError* error) const {
            const QUrl apiOrigin(m_credentials.apiOrigin);
            if (delegated.method != QByteArrayLiteral("GET") ||
                delegated.expiresAt <= QDateTime::currentDateTimeUtc() ||
                !safeOrigin(apiOrigin) ||
                (apiOrigin.scheme() == QLatin1String("https") &&
                 delegated.url.scheme() != QLatin1String("https"))) {
                if (error)
                    *error = transferFailure(
                        CloudTransferErrorCode::DelegatedRequestRejected,
                        QStringLiteral("Delegated download request was rejected"));
                return false;
            }
            return true;
        }

        void issueDelegatedGet(Job& job, DelegatedRequest delegated,
                               int redirectCount) {
            if (wasCancelled(job)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::Cancelled,
                              QStringLiteral("Cloud transfer was cancelled")));
                return;
            }
            CloudTransferError requestError;
            if (!validateDelegatedForGet(delegated, &requestError)) {
                fail(job, requestError);
                return;
            }
            const auto request = delegatedNetworkRequest(delegated,
                                                          &requestError);
            if (!request || !m_network) {
                fail(job, requestError.safeMessage.isEmpty()
                              ? transferFailure(
                                    CloudTransferErrorCode::NetworkFailure,
                                    QStringLiteral("Download network is unavailable"),
                                    0, true)
                              : requestError);
                return;
            }
            cleanupNetwork(job);
            job.failed = false;
            const QFileInfo destination(job.destinationPath);
            if (!QDir().mkpath(destination.absolutePath())) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::FileWriteFailure,
                              QStringLiteral("Download destination is unavailable")));
                return;
            }
            job.downloadFile =
                std::make_unique<QSaveFile>(job.destinationPath);
            job.downloadFile->setDirectWriteFallback(false);
            if (!job.downloadFile->open(QIODevice::WriteOnly)) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::FileWriteFailure,
                              QStringLiteral("Download destination is unavailable")));
                return;
            }
            job.downloadDigest =
                std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
            job.downloadedBytes = 0;
            job.stage = Stage::GetDownload;
            job.activeDelegated = delegated;
            job.redirectCount = redirectCount;
            const int generation = job.requestGeneration;
            QNetworkReply* reply = m_network->sendCustomRequest(
                *request, QByteArrayLiteral("GET"), QByteArray{});
            if (!reply) {
                fail(job, transferFailure(
                              CloudTransferErrorCode::NetworkFailure,
                              QStringLiteral("Delegated download could not start"),
                              0, true));
                return;
            }
            job.reply = reply;
            auto* timeout = new QTimer(reply);
            timeout->setSingleShot(true);
            timeout->setInterval(m_timeoutMs);
            job.timeout = timeout;
            publishState(job, CloudTransferState::Downloading);
            connect(timeout, &QTimer::timeout, this,
                    [this, id = job.id, generation] {
                        Job* current = matching(id, generation);
                        if (current)
                            fail(*current,
                                 transferFailure(
                                     CloudTransferErrorCode::Timeout,
                                     QStringLiteral("Delegated download timed out"),
                                     0, true));
                    });
            connect(reply, &QNetworkReply::readyRead, this,
                    [this, id = job.id, generation,
                     guard = QPointer<QNetworkReply>(reply)] {
                        Job* current = matching(id, generation, guard);
                        if (!current || !guard) return;
                        const int status = guard
                                               ->attribute(QNetworkRequest::
                                                               HttpStatusCodeAttribute)
                                               .toInt();
                        if (status == 200) {
                            drainDownload(*current, guard);
                        } else if (guard->bytesAvailable() >
                                   kMaximumProviderResponseBytes) {
                            fail(*current,
                                 transferFailure(
                                     CloudTransferErrorCode::ResponseTooLarge,
                                     QStringLiteral("Storage response is too large")));
                        }
                    });
            connect(reply, &QNetworkReply::finished, this,
                    [this, id = job.id, generation,
                     guard = QPointer<QNetworkReply>(reply)] {
                        if (guard) finishDelegatedGet(id, generation, guard);
                    });
            timeout->start();
        }

        bool drainDownload(Job& job, QNetworkReply* reply) {
            if (!job.downloadFile || !job.downloadDigest) return false;
            QByteArray block(kIoBlockBytes, Qt::Uninitialized);
            while (reply->bytesAvailable() > 0) {
                const qint64 count =
                    reply->read(block.data(), block.size());
                if (count < 0) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::NetworkFailure,
                                  QStringLiteral("Could not read downloaded bytes"),
                                  0, true));
                    return false;
                }
                if (count == 0) break;
                if (quint64(count) > job.byteSize -
                                         std::min(job.byteSize,
                                                  job.downloadedBytes) ||
                    job.downloadFile->write(block.constData(), count) != count) {
                    fail(job, transferFailure(
                                  job.downloadedBytes + quint64(count) >
                                          job.byteSize
                                      ? CloudTransferErrorCode::IntegrityMismatch
                                      : CloudTransferErrorCode::FileWriteFailure,
                                  job.downloadedBytes + quint64(count) >
                                          job.byteSize
                                      ? QStringLiteral("Download exceeded declared size")
                                      : QStringLiteral("Could not write downloaded bytes")));
                    return false;
                }
                job.downloadDigest->addData(
                    QByteArrayView(block.constData(), count));
                job.downloadedBytes += quint64(count);
                publishProgress(job, job.downloadedBytes, job.byteSize);
                if (wasCancelled(job)) {
                    fail(job, transferFailure(
                                  CloudTransferErrorCode::Cancelled,
                                  QStringLiteral("Cloud transfer was cancelled")));
                    return false;
                }
            }
            return true;
        }

        void finishDelegatedGet(quint64 id, int generation,
                                QNetworkReply* reply) {
            Job* job = matching(id, generation, reply);
            if (!job) return;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const QNetworkReply::NetworkError networkError = reply->error();
            const QVariant redirectValue =
                reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
            const QByteArray contentLength = reply->rawHeader("Content-Length");
            const QByteArray responseContentType = reply->rawHeader("Content-Type");
            const QUrl currentUrl = job->activeDelegated
                                        ? job->activeDelegated->url
                                        : QUrl{};
            const int redirectCount = job->redirectCount;
            DelegatedRequest delegated = job->activeDelegated.value_or(
                DelegatedRequest{});

            if (status >= 300 && status < 400) {
                releaseFinishedReply(*job, reply);
                if (job->downloadFile) {
                    job->downloadFile->cancelWriting();
                    job->downloadFile.reset();
                }
                job->downloadDigest.reset();
                QUrl target = redirectValue.toUrl();
                if (target.isRelative()) target = currentUrl.resolved(target);
                const bool statusAllowed = status == 301 || status == 302 ||
                                           status == 303 || status == 307 ||
                                           status == 308;
                if (!statusAllowed || redirectCount >= kMaximumRedirects ||
                    !safeRedirect(currentUrl, target)) {
                    fail(*job, transferFailure(
                                   CloudTransferErrorCode::RedirectRejected,
                                   QStringLiteral("Storage redirect was rejected"),
                                   status));
                    return;
                }
                delegated.url = target;
                issueDelegatedGet(*job, std::move(delegated),
                                  redirectCount + 1);
                return;
            }
            if (status == 200 && !drainDownload(*job, reply)) return;
            releaseFinishedReply(*job, reply);
            if (networkError != QNetworkReply::NoError) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::NetworkFailure,
                               QStringLiteral("Delegated download failed"), status,
                               true));
                return;
            }
            if (status != 200) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::UnexpectedStatus,
                               QStringLiteral("Storage rejected download request"),
                               status, status >= 500));
                return;
            }
            if (!contentLength.isEmpty()) {
                bool ok = false;
                const qulonglong parsed = contentLength.toULongLong(&ok);
                if (!ok || quint64(parsed) != job->byteSize) {
                    fail(*job, transferFailure(
                                   CloudTransferErrorCode::IntegrityMismatch,
                                   QStringLiteral("Download content length mismatch")));
                    return;
                }
            }
            if (!responseContentType.isEmpty() &&
                QString::fromUtf8(responseContentType).section(
                    QLatin1Char(';'), 0, 0).trimmed() !=
                    job->contentType.section(QLatin1Char(';'), 0, 0).trimmed()) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::IntegrityMismatch,
                               QStringLiteral("Download content type mismatch")));
                return;
            }
            publishState(*job, CloudTransferState::Verifying);
            const QString computed =
                job->downloadDigest
                    ? QString::fromLatin1(
                          job->downloadDigest->result().toHex())
                    : QString{};
            if (job->downloadedBytes != job->byteSize ||
                computed != job->sha256 || !job->downloadFile) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::IntegrityMismatch,
                               QStringLiteral("Downloaded bytes failed verification")));
                return;
            }
            if (!job->downloadFile->commit()) {
                fail(*job, transferFailure(
                               CloudTransferErrorCode::FileWriteFailure,
                               QStringLiteral("Could not commit downloaded bytes")));
                return;
            }
            job->downloadFile.reset();
            job->downloadDigest.reset();
            const quint64 transferId = job->id;
            if (job->kind == CloudTransferKind::AssetDownload) {
                if (!m_cache) {
                    fail(*job, transferFailure(
                                   CloudTransferErrorCode::FileWriteFailure,
                                   QStringLiteral("Asset cache is unavailable")));
                    return;
                }
                job->expectedAsset.sha256 = job->sha256.toStdString();
                job->expectedAsset.byteSize = job->byteSize;
                job->expectedAsset.mimeType = job->contentType.toStdString();
                QString cacheError;
                if (!m_cache->registerReady(job->expectedAsset, &cacheError)) {
                    fail(*job, transferFailure(
                                   CloudTransferErrorCode::FileWriteFailure,
                                   QStringLiteral("Could not register verified asset")));
                    return;
                }
                const daw::AssetRef asset = job->expectedAsset;
                const QString path = job->destinationPath;
                publishState(*job, CloudTransferState::Ready);
                post([transferId, asset, path](
                         CloudAssetTransferManager& owner) {
                    emit owner.assetDownloadCompleted(transferId, asset, path);
                });
                finishAndErase(transferId);
                return;
            }
            CloudSnapshotDownloadResult result;
            result.projectId = job->projectId;
            result.snapshotId = job->snapshotId;
            result.sha256 = job->sha256;
            result.byteSize = job->byteSize;
            result.contentType = job->contentType;
            result.localPath = job->destinationPath;
            publishState(*job, CloudTransferState::Ready);
            post([transferId, result](CloudAssetTransferManager& owner) {
                emit owner.snapshotDownloadCompleted(transferId, result);
            });
            finishAndErase(transferId);
        }

        QPointer<CloudAssetTransferManager> m_owner;
        QPointer<AssetCache> m_cache;
        NetworkFactory m_networkFactory;
        QNetworkAccessManager* m_network = nullptr;
        Credentials m_credentials;
        int m_timeoutMs = kDefaultTimeoutMs;
        std::unordered_map<quint64, std::unique_ptr<Job>> m_jobs;
    };

    QPointer<account::Service> account;
    CredentialProvider credentialProvider;
    QPointer<AssetCache> cache;
    QThread* thread = nullptr;
    Worker* worker = nullptr;
    QHash<quint64, std::shared_ptr<std::atomic_bool>> cancellation;
    QSet<quint64> failed;
    quint64 nextTransferId = 1;
    int timeoutMs = kDefaultTimeoutMs;

    quint64 allocateTransferId() {
        while (nextTransferId == 0 ||
               cancellation.contains(nextTransferId)) {
            ++nextTransferId;
        }
        return nextTransferId++;
    }
};

CloudAssetTransferManager::CloudAssetTransferManager(
    account::Service* accountService, AssetCache* cache, QObject* parent)
    : CloudAssetTransferManager(
          [guard = QPointer<account::Service>(accountService)] {
              Credentials credentials;
              if (!guard) return credentials;
              credentials.apiOrigin = guard->apiOrigin();
              credentials.bearerToken = guard->accessToken().toUtf8();
              credentials.userId = guard->snapshot().userId;
              credentials.deviceId = guard->snapshot().deviceId;
              credentials.authenticated = guard->authenticated();
              credentials.offline = guard->snapshot().offline;
              return credentials;
          },
          [] { return new QNetworkAccessManager; }, cache, parent) {
    m_impl->account = accountService;
    if (accountService) {
        connect(accountService, &account::Service::snapshotChanged, this,
                &CloudAssetTransferManager::refreshCredentials);
        connect(accountService, &account::Service::authenticatedChanged, this,
                [this](bool authenticated) {
                    refreshCredentials();
                    if (!authenticated) cancelAll();
                });
        connect(accountService, &account::Service::logoutFinished, this,
                [this] {
                    refreshCredentials();
                    cancelAll();
                });
    }
    refreshCredentials();
}

CloudAssetTransferManager::CloudAssetTransferManager(
    CredentialProvider credentialProvider, NetworkFactory networkFactory,
    AssetCache* cache, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->credentialProvider = std::move(credentialProvider);
    m_impl->cache = cache;
    m_impl->thread = new QThread(this);
    m_impl->thread->setObjectName(QStringLiteral("CloudAssetTransferWorker"));
    m_impl->worker = new Impl::Worker(this, cache, std::move(networkFactory));
    m_impl->worker->moveToThread(m_impl->thread);
    connect(m_impl->thread, &QThread::finished, m_impl->worker,
            &QObject::deleteLater);
    m_impl->thread->start();
    QMetaObject::invokeMethod(m_impl->worker,
                              [worker = m_impl->worker] {
                                  worker->initialize();
                              },
                              Qt::BlockingQueuedConnection);

    const auto terminal = [this](quint64 transferId) {
        m_impl->cancellation.remove(transferId);
        m_impl->failed.remove(transferId);
    };
    connect(this, &CloudAssetTransferManager::assetUploadCompleted, this,
            [terminal](quint64 id, const CloudAssetUploadResult&) {
                terminal(id);
            });
    connect(this, &CloudAssetTransferManager::snapshotUploadCompleted, this,
            [terminal](quint64 id, const CloudSnapshotUploadResult&) {
                terminal(id);
            });
    connect(this, &CloudAssetTransferManager::assetDownloadCompleted, this,
            [terminal](quint64 id, const daw::AssetRef&, const QString&) {
                terminal(id);
            });
    connect(this, &CloudAssetTransferManager::snapshotDownloadCompleted, this,
            [terminal](quint64 id, const CloudSnapshotDownloadResult&) {
                terminal(id);
            });
    connect(this, &CloudAssetTransferManager::uploadAborted, this,
            [terminal](quint64 id, const QString&, const QString&) {
                terminal(id);
            });
    connect(this, &CloudAssetTransferManager::transferFailed, this,
            [this](quint64 id, CloudTransferKind,
                   const CloudTransferError& error) {
                if (error.code == CloudTransferErrorCode::Cancelled) {
                    m_impl->cancellation.remove(id);
                    m_impl->failed.remove(id);
                } else if (m_impl->cancellation.contains(id)) {
                    m_impl->failed.insert(id);
                }
            });
    refreshCredentials();
}

CloudAssetTransferManager::~CloudAssetTransferManager() {
    if (!m_impl) return;
    for (auto iterator = m_impl->cancellation.begin();
         iterator != m_impl->cancellation.end(); ++iterator) {
        iterator.value()->store(true);
    }
    if (m_impl->thread && m_impl->thread->isRunning() && m_impl->worker) {
        QMetaObject::invokeMethod(m_impl->worker,
                                  [worker = m_impl->worker] {
                                      worker->shutdown();
                                  },
                                  Qt::BlockingQueuedConnection);
        m_impl->thread->quit();
        m_impl->thread->wait();
    }
    m_impl->worker = nullptr;
}

int CloudAssetTransferManager::requestTimeoutMs() const {
    return m_impl->timeoutMs;
}

void CloudAssetTransferManager::setRequestTimeoutMs(int timeoutMs) {
    m_impl->timeoutMs = std::clamp(timeoutMs, kMinimumTimeoutMs,
                                  kMaximumTimeoutMs);
    if (!m_impl->worker) return;
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, timeout = m_impl->timeoutMs] {
            worker->setTimeoutMs(timeout);
        },
        Qt::QueuedConnection);
}

void CloudAssetTransferManager::refreshCredentials() {
    if (!m_impl || !m_impl->worker) return;
    const Credentials credentials = m_impl->credentialProvider
                                        ? m_impl->credentialProvider()
                                        : Credentials{};
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, credentials] {
            worker->updateCredentials(credentials);
        },
        Qt::QueuedConnection);
}

quint64 CloudAssetTransferManager::uploadAsset(
    const CloudAssetUploadInput& input) {
    const quint64 id = m_impl->allocateTransferId();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_impl->cancellation.insert(id, cancelled);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, id, input, cancelled] {
            worker->startAssetUpload(id, input, cancelled);
        },
        Qt::QueuedConnection);
    return id;
}

quint64 CloudAssetTransferManager::uploadSnapshot(
    const CloudSnapshotUploadInput& input) {
    const quint64 id = m_impl->allocateTransferId();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_impl->cancellation.insert(id, cancelled);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, id, input, cancelled] {
            worker->startSnapshotUpload(id, input, cancelled);
        },
        Qt::QueuedConnection);
    return id;
}

quint64 CloudAssetTransferManager::downloadAsset(
    const QString& projectId, const daw::AssetRef& expected) {
    const quint64 id = m_impl->allocateTransferId();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_impl->cancellation.insert(id, cancelled);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, id, projectId, expected, cancelled] {
            worker->startAssetDownload(id, projectId, expected, cancelled);
        },
        Qt::QueuedConnection);
    return id;
}

quint64 CloudAssetTransferManager::downloadSnapshot(
    const CloudSnapshotDownloadInput& input) {
    const quint64 id = m_impl->allocateTransferId();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_impl->cancellation.insert(id, cancelled);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, id, input, cancelled] {
            worker->startSnapshotDownload(id, input, cancelled);
        },
        Qt::QueuedConnection);
    return id;
}

quint64 CloudAssetTransferManager::abortUpload(const QString& projectId,
                                               const QString& uploadId) {
    const quint64 id = m_impl->allocateTransferId();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_impl->cancellation.insert(id, cancelled);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, id, projectId, uploadId, cancelled] {
            worker->startAbort(id, projectId, uploadId, cancelled);
        },
        Qt::QueuedConnection);
    return id;
}

bool CloudAssetTransferManager::retry(quint64 transferId) {
    const auto iterator = m_impl->cancellation.find(transferId);
    if (iterator == m_impl->cancellation.end() ||
        !m_impl->failed.contains(transferId)) {
        return false;
    }
    iterator.value()->store(false);
    m_impl->failed.remove(transferId);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, transferId] {
            worker->retry(transferId);
        },
        Qt::QueuedConnection);
    return true;
}

bool CloudAssetTransferManager::cancel(quint64 transferId) {
    const auto iterator = m_impl->cancellation.find(transferId);
    if (iterator == m_impl->cancellation.end()) return false;
    iterator.value()->store(true);
    QMetaObject::invokeMethod(
        m_impl->worker,
        [worker = m_impl->worker, transferId] {
            worker->cancel(transferId);
        },
        Qt::QueuedConnection);
    return true;
}

void CloudAssetTransferManager::cancelAll() {
    const QList<quint64> ids = m_impl->cancellation.keys();
    for (quint64 id : ids) cancel(id);
}

namespace {

class FakeTransferReply final : public QNetworkReply {
public:
    FakeTransferReply(const QNetworkRequest& request, int status,
                      QByteArray body,
                      const QList<QPair<QByteArray, QByteArray>>& headers,
                      const QUrl& redirectTarget, QObject* parent)
        : QNetworkReply(parent), m_body(std::move(body)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (!redirectTarget.isEmpty()) {
            setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                         redirectTarget);
        }
        for (const auto& [name, value] : headers) setRawHeader(name, value);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void abort() override {
        if (m_finished) return;
        m_finished = true;
        setError(QNetworkReply::OperationCanceledError,
                 QStringLiteral("cancelled"));
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override {
        return qint64(m_body.size()) - m_offset +
               QNetworkReply::bytesAvailable();
    }

    void complete() {
        if (m_finished) return;
        m_finished = true;
        setFinished(true);
        if (!m_body.isEmpty()) emit readyRead();
        emit finished();
    }

protected:
    qint64 readData(char* data, qint64 maximum) override {
        if (m_offset >= m_body.size()) return -1;
        const qint64 count = std::min<qint64>(
            maximum, qint64(m_body.size()) - m_offset);
        std::memcpy(data, m_body.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
    bool m_finished = false;
};

struct FakeTransferNetworkState {
    struct Script {
        Script() = default;
        Script(int responseStatus, QByteArray responseBody,
               QList<QPair<QByteArray, QByteArray>> responseHeaders = {},
               QUrl responseRedirect = {}, bool holdReply = false)
            : status(responseStatus), body(std::move(responseBody)),
              headers(std::move(responseHeaders)),
              redirectTarget(std::move(responseRedirect)),
              deferred(holdReply) {}

        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        QUrl redirectTarget;
        bool deferred = false;
    };

    struct Captured {
        QNetworkRequest request;
        QByteArray method;
        QByteArray body;
    };

    void enqueue(Script script) {
        QMutexLocker locker(&mutex);
        scripts.append(std::move(script));
    }

    int captureCount() const {
        QMutexLocker locker(&mutex);
        return captured.size();
    }

    QVector<Captured> captures() const {
        QMutexLocker locker(&mutex);
        return captured;
    }

    mutable QMutex mutex;
    QVector<Script> scripts;
    QVector<Captured> captured;
};

class FakeTransferNetwork final : public QNetworkAccessManager {
public:
    explicit FakeTransferNetwork(
        std::shared_ptr<FakeTransferNetworkState> state)
        : m_state(std::move(state)) {}

protected:
    QNetworkReply* createRequest(Operation operation,
                                 const QNetworkRequest& request,
                                 QIODevice* outgoingData) override {
        QByteArray method = request
                                .attribute(QNetworkRequest::CustomVerbAttribute)
                                .toByteArray();
        if (method.isEmpty()) {
            switch (operation) {
                case GetOperation: method = QByteArrayLiteral("GET"); break;
                case PostOperation: method = QByteArrayLiteral("POST"); break;
                case PutOperation: method = QByteArrayLiteral("PUT"); break;
                case DeleteOperation:
                    method = QByteArrayLiteral("DELETE");
                    break;
                default: method = QByteArrayLiteral("CUSTOM"); break;
            }
        }
        QByteArray body;
        if (outgoingData) body = outgoingData->readAll();

        FakeTransferNetworkState::Script script;
        {
            QMutexLocker locker(&m_state->mutex);
            m_state->captured.append({request, method, body});
            if (!m_state->scripts.isEmpty()) {
                script = m_state->scripts.takeFirst();
            } else {
                script.status = 500;
                script.body = QByteArrayLiteral(
                    R"({"code":"missing_fixture","message":"Missing fixture","request_id":"test"})");
                script.headers.append(
                    {QByteArrayLiteral("Content-Type"),
                     QByteArrayLiteral("application/json")});
            }
        }

        auto* reply = new FakeTransferReply(
            request, script.status, std::move(script.body), script.headers,
            script.redirectTarget, this);
        if (!script.deferred) {
            QTimer::singleShot(0, reply, [reply] { reply->complete(); });
        }
        return reply;
    }

private:
    std::shared_ptr<FakeTransferNetworkState> m_state;
};

QByteArray compactTransferJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject testDelegatedRequest(const QString& method,
                                 const QString& url,
                                 const QJsonObject& headers = {}) {
    return {
        {QStringLiteral("method"), method},
        {QStringLiteral("url"), url},
        {QStringLiteral("headers"), headers},
        {QStringLiteral("expiresAt"),
         QStringLiteral("2099-01-01T00:00:00Z")},
    };
}

QJsonObject testCompletedAsset(const QString& projectId,
                               const QString& assetId,
                               const QString& blobId,
                               const QString& sha256, quint64 bytes,
                               const QString& displayName,
                               const QString& kind = QStringLiteral("audio"),
                               const QString& contentType =
                                   QStringLiteral("audio/wav")) {
    return {
        {QStringLiteral("asset"),
         QJsonObject{
             {QStringLiteral("project_id"), projectId},
             {QStringLiteral("asset_id"), assetId},
             {QStringLiteral("blob_id"), blobId},
             {QStringLiteral("kind"), kind},
             {QStringLiteral("display_name"), displayName},
             {QStringLiteral("created_at"),
              QStringLiteral("2026-08-30T10:00:00Z")},
         }},
        {QStringLiteral("blob"),
         QJsonObject{
             {QStringLiteral("id"), blobId},
             {QStringLiteral("sha256"), sha256},
             {QStringLiteral("bytes"), double(bytes)},
             {QStringLiteral("content_type"), contentType},
             {QStringLiteral("kind"), kind},
             {QStringLiteral("status"), QStringLiteral("ready")},
             {QStringLiteral("created_at"),
              QStringLiteral("2026-08-30T10:00:00Z")},
             {QStringLiteral("verified_at"),
              QStringLiteral("2026-08-30T10:00:01Z")},
         }},
    };
}

QJsonObject testCompletedSnapshot(
    const QString& projectId, const QString& snapshotId,
    const QString& blobId, const QString& sha256, quint64 bytes,
    quint64 sequence, const QStringList& assetIds) {
    QJsonArray manifest;
    for (const QString& assetId : assetIds) manifest.push_back(assetId);
    return {
        {QStringLiteral("snapshot"),
         QJsonObject{
             {QStringLiteral("id"), snapshotId},
             {QStringLiteral("project_id"), projectId},
             {QStringLiteral("seq"), double(sequence)},
             {QStringLiteral("blob_id"), blobId},
             {QStringLiteral("schema_version"), 7},
             {QStringLiteral("asset_ids"), manifest},
             {QStringLiteral("created_at"),
              QStringLiteral("2026-08-30T10:00:00Z")},
         }},
        {QStringLiteral("blob"),
         QJsonObject{
             {QStringLiteral("id"), blobId},
             {QStringLiteral("sha256"), sha256},
             {QStringLiteral("bytes"), double(bytes)},
             {QStringLiteral("content_type"),
              QStringLiteral("application/vnd.vlt.project+json")},
             {QStringLiteral("kind"), QStringLiteral("project_snapshot")},
             {QStringLiteral("status"), QStringLiteral("ready")},
             {QStringLiteral("created_at"),
              QStringLiteral("2026-08-30T10:00:00Z")},
             {QStringLiteral("verified_at"),
              QStringLiteral("2026-08-30T10:00:01Z")},
         }},
    };
}

QJsonObject testDownloadPreparation(const QByteArray& bytes,
                                    const QString& url,
                                    const QString& contentType,
                                    const QJsonObject& headers = {}) {
    return {
        {QStringLiteral("request"),
         testDelegatedRequest(QStringLiteral("GET"), url, headers)},
        {QStringLiteral("sha256"),
         QString::fromLatin1(
             QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                 .toHex())},
        {QStringLiteral("byteSize"), double(bytes.size())},
        {QStringLiteral("contentType"), contentType},
    };
}

bool waitForTransfer(const std::function<bool()>& predicate,
                     int timeoutMs = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return predicate();
}

bool hasBearer(const QNetworkRequest& request,
               const QByteArray& bearerToken) {
    return request.rawHeader("Authorization") ==
           QByteArrayLiteral("Bearer ") + bearerToken;
}

} // namespace

bool checkCloudAssetTransferManagerForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return fail(QStringLiteral("Could not create transfer fixture"));

    const QString projectId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString userId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString deviceId =
        QStringLiteral("44444444-4444-4444-8444-444444444444");
    const QByteArray bearerToken = QByteArrayLiteral("transfer-test-secret");
    CloudAssetTransferManager::Credentials credentials;
    credentials.apiOrigin = QStringLiteral("https://api.vlt.test/v1");
    credentials.bearerToken = bearerToken;
    credentials.userId = userId;
    credentials.deviceId = deviceId;
    credentials.authenticated = true;

    auto networkState = std::make_shared<FakeTransferNetworkState>();
    AssetCache cache(QDir(temporary.path()).filePath(QStringLiteral("cache")));
    CloudAssetTransferManager manager(
        [credentials] { return credentials; },
        [networkState] { return new FakeTransferNetwork(networkState); },
        &cache, nullptr);

    int failureCount = 0;
    quint64 lastFailedId = 0;
    CloudTransferError lastFailure;
    QObject::connect(
        &manager, &CloudAssetTransferManager::transferFailed,
        [&](quint64 id, CloudTransferKind, const CloudTransferError& value) {
            ++failureCount;
            lastFailedId = id;
            lastFailure = value;
        });

    // A regular upload hashes the source on the worker, sends credentials only
    // to the API, streams exactly the delegated body, and accepts completion
    // only when the verified blob identity matches.
    const QByteArray singleBytes("VLT single upload fixture\n");
    const QString singlePath =
        QDir(temporary.path()).filePath(QStringLiteral("single.wav"));
    QFile singleFile(singlePath);
    if (!singleFile.open(QIODevice::WriteOnly) ||
        singleFile.write(singleBytes) != singleBytes.size()) {
        return fail(QStringLiteral("Could not write single-upload fixture"));
    }
    singleFile.close();
    const QString singleSha = QString::fromLatin1(
        QCryptographicHash::hash(singleBytes, QCryptographicHash::Sha256)
            .toHex());
    const QString singleUploadId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString singleAssetId =
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const QString singleBlobId =
        QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    networkState->enqueue({
        200,
        compactTransferJson(QJsonObject{
            {QStringLiteral("uploadId"), singleUploadId},
            {QStringLiteral("assetId"), singleAssetId},
            {QStringLiteral("status"), QStringLiteral("uploading")},
            {QStringLiteral("uploadMode"), QStringLiteral("single")},
            {QStringLiteral("alreadyAvailable"), false},
            {QStringLiteral("request"),
             testDelegatedRequest(
                 QStringLiteral("PUT"),
                 QStringLiteral("https://storage.vlt.test/single?sig=one"),
                 QJsonObject{
                     {QStringLiteral("Content-Length"),
                      QString::number(singleBytes.size())},
                     {QStringLiteral("Content-Type"),
                      QStringLiteral("audio/wav")},
                     {QStringLiteral("x-vlt-transfer"),
                      QStringLiteral("single")},
                 })},
            {QStringLiteral("expiresAt"),
             QStringLiteral("2099-01-01T00:00:00Z")},
        }),
    });
    networkState->enqueue({200, {}});
    networkState->enqueue({
        200,
        compactTransferJson(testCompletedAsset(
            projectId, singleAssetId, singleBlobId, singleSha,
            quint64(singleBytes.size()), QStringLiteral("single.wav"))),
        {{QByteArrayLiteral("Content-Type"),
          QByteArrayLiteral("application/json")}},
    });
    int assetUploadSignals = 0;
    CloudAssetUploadResult singleResult;
    QObject::connect(
        &manager, &CloudAssetTransferManager::assetUploadCompleted,
        [&](quint64, const CloudAssetUploadResult& value) {
            ++assetUploadSignals;
            singleResult = value;
        });
    CloudAssetUploadInput singleInput;
    singleInput.projectId = projectId;
    singleInput.uploadId = singleUploadId;
    singleInput.assetId = singleAssetId;
    singleInput.sourcePath = singlePath;
    singleInput.kind = CloudAssetKind::Audio;
    singleInput.contentType = QStringLiteral("audio/wav");
    singleInput.displayName = QStringLiteral("single.wav");
    manager.uploadAsset(singleInput);
    if (!waitForTransfer(
            [&] { return assetUploadSignals == 1 || failureCount != 0; }) ||
        assetUploadSignals != 1 || failureCount != 0 ||
        QString::fromStdString(singleResult.asset.sha256) != singleSha) {
        return fail(QStringLiteral("Single asset upload did not complete"));
    }
    QVector<FakeTransferNetworkState::Captured> captures =
        networkState->captures();
    if (captures.size() != 3 || captures[0].method != QByteArrayLiteral("POST") ||
        captures[1].method != QByteArrayLiteral("PUT") ||
        captures[2].method != QByteArrayLiteral("POST") ||
        !hasBearer(captures[0].request, bearerToken) ||
        !hasBearer(captures[2].request, bearerToken) ||
        !captures[1].request.rawHeader("Authorization").isEmpty() ||
        !captures[1].request.rawHeader("Accept").isEmpty() ||
        captures[1].request.rawHeader("x-vlt-transfer") !=
            QByteArrayLiteral("single") ||
        captures[1].body != singleBytes) {
        return fail(QStringLiteral("Delegated upload leaked API credentials"));
    }

    // Resuming multipart trusts only provider-observed completed parts. The
    // client uploads the missing part and returns the complete ordered ETag
    // manifest to the authenticated completion endpoint.
    const qsizetype multipartPrefix = kMinimumMultipartPartBytes;
    QByteArray multipartBytes(multipartPrefix + 3, 'm');
    multipartBytes.replace(multipartPrefix, 3, QByteArrayLiteral("end"));
    const QString multipartPath =
        QDir(temporary.path()).filePath(QStringLiteral("multipart.wav"));
    QFile multipartFile(multipartPath);
    if (!multipartFile.open(QIODevice::WriteOnly) ||
        multipartFile.write(multipartBytes) != multipartBytes.size()) {
        return fail(QStringLiteral("Could not write multipart fixture"));
    }
    multipartFile.close();
    const QString multipartSha = QString::fromLatin1(
        QCryptographicHash::hash(multipartBytes, QCryptographicHash::Sha256)
            .toHex());
    const QString multipartUploadId =
        QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const QString multipartAssetId =
        QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff");
    const QString multipartBlobId =
        QStringLiteral("12121212-1212-4212-8212-121212121212");
    const int multipartCaptureBase = networkState->captureCount();
    networkState->enqueue({
        200,
        compactTransferJson(QJsonObject{
            {QStringLiteral("uploadId"), multipartUploadId},
            {QStringLiteral("assetId"), multipartAssetId},
            {QStringLiteral("status"), QStringLiteral("uploading")},
            {QStringLiteral("uploadMode"), QStringLiteral("multipart")},
            {QStringLiteral("alreadyAvailable"), false},
            {QStringLiteral("multipartPartSize"), double(multipartPrefix)},
            {QStringLiteral("multipartPartCount"), 2},
            {QStringLiteral("uploadedParts"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("partNumber"), 1},
                 {QStringLiteral("byteSize"), double(multipartPrefix)},
                 {QStringLiteral("eTag"), QStringLiteral("already-one")},
             }}},
            {QStringLiteral("parts"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("partNumber"), 2},
                 {QStringLiteral("byteSize"), 3},
                 {QStringLiteral("request"),
                  testDelegatedRequest(
                      QStringLiteral("PUT"),
                      QStringLiteral(
                          "https://storage.vlt.test/multipart?partNumber=2"),
                      QJsonObject{
                          {QStringLiteral("Content-Length"),
                           QStringLiteral("3")},
                          {QStringLiteral("x-vlt-transfer"),
                           QStringLiteral("part-two")},
                      })},
             }}},
            {QStringLiteral("expiresAt"),
             QStringLiteral("2099-01-01T00:00:00Z")},
        }),
    });
    networkState->enqueue({
        200, {}, {{QByteArrayLiteral("ETag"),
                   QByteArrayLiteral("part-two-etag")}},
    });
    networkState->enqueue({
        200,
        compactTransferJson(testCompletedAsset(
            projectId, multipartAssetId, multipartBlobId, multipartSha,
            quint64(multipartBytes.size()), QStringLiteral("multipart.wav"))),
        {{QByteArrayLiteral("Content-Type"),
          QByteArrayLiteral("application/json")}},
    });
    CloudAssetUploadInput multipartInput;
    multipartInput.projectId = projectId;
    multipartInput.uploadId = multipartUploadId;
    multipartInput.assetId = multipartAssetId;
    multipartInput.sourcePath = multipartPath;
    multipartInput.kind = CloudAssetKind::Audio;
    multipartInput.contentType = QStringLiteral("audio/wav");
    multipartInput.displayName = QStringLiteral("multipart.wav");
    manager.uploadAsset(multipartInput);
    if (!waitForTransfer(
            [&] { return assetUploadSignals == 2 || failureCount != 0; }) ||
        assetUploadSignals != 2 || failureCount != 0) {
        return fail(QStringLiteral("Resumable multipart upload did not complete"));
    }
    captures = networkState->captures();
    if (captures.size() != multipartCaptureBase + 3 ||
        captures[multipartCaptureBase + 1].body != QByteArrayLiteral("end") ||
        !captures[multipartCaptureBase + 1]
             .request.rawHeader("Authorization")
             .isEmpty()) {
        return fail(QStringLiteral("Multipart resume resent completed bytes"));
    }
    QJsonParseError completionParseError;
    const QJsonDocument completionDocument = QJsonDocument::fromJson(
        captures[multipartCaptureBase + 2].body, &completionParseError);
    const QJsonArray completionParts = completionDocument.isObject()
                                           ? completionDocument.object()
                                                 .value(QStringLiteral("parts"))
                                                 .toArray()
                                           : QJsonArray{};
    if (completionParseError.error != QJsonParseError::NoError ||
        completionParts.size() != 2 ||
        completionParts[0]
                .toObject()
                .value(QStringLiteral("eTag"))
                .toString() != QLatin1String("already-one") ||
        completionParts[1]
                .toObject()
                .value(QStringLiteral("eTag"))
                .toString() != QLatin1String("part-two-etag")) {
        return fail(QStringLiteral("Multipart completion manifest was invalid"));
    }

    // Snapshot upload identity includes the exact canonical asset set. It is
    // sent on every prepare/retry and the completed descriptor must echo the
    // same sorted, unique manifest.
    const QByteArray uploadSnapshotBytes(
        R"({"schemaVersion":1,"projectFormatVersion":7,"project":{}})");
    const QString uploadSnapshotPath =
        QDir(temporary.path()).filePath(QStringLiteral("upload.snapshot"));
    QFile uploadSnapshotFile(uploadSnapshotPath);
    if (!uploadSnapshotFile.open(QIODevice::WriteOnly) ||
        uploadSnapshotFile.write(uploadSnapshotBytes) !=
            uploadSnapshotBytes.size()) {
        return fail(QStringLiteral("Could not write snapshot-upload fixture"));
    }
    uploadSnapshotFile.close();
    const QString uploadSnapshotSha = QString::fromLatin1(
        QCryptographicHash::hash(uploadSnapshotBytes,
                                 QCryptographicHash::Sha256)
            .toHex());
    const QString snapshotUploadId =
        QStringLiteral("13131313-1313-4313-8313-131313131313");
    const QString uploadedSnapshotId =
        QStringLiteral("14141414-1414-4414-8414-141414141414");
    const QString snapshotBlobId =
        QStringLiteral("15151515-1515-4515-8515-151515151515");
    const QStringList snapshotManifest{singleAssetId, multipartAssetId};
    const int snapshotUploadCaptureBase = networkState->captureCount();
    networkState->enqueue({
        200,
        compactTransferJson(QJsonObject{
            {QStringLiteral("uploadId"), snapshotUploadId},
            {QStringLiteral("snapshotSeq"), 2.0},
            {QStringLiteral("status"), QStringLiteral("uploading")},
            {QStringLiteral("uploadMode"), QStringLiteral("single")},
            {QStringLiteral("alreadyAvailable"), false},
            {QStringLiteral("request"),
             testDelegatedRequest(
                 QStringLiteral("PUT"),
                 QStringLiteral("https://storage.vlt.test/snapshot-upload"),
                 QJsonObject{
                     {QStringLiteral("Content-Length"),
                      QString::number(uploadSnapshotBytes.size())},
                     {QStringLiteral("Content-Type"),
                      QStringLiteral(
                          "application/vnd.vlt.project+json")}})},
            {QStringLiteral("expiresAt"),
             QStringLiteral("2099-01-01T00:00:00Z")},
        }),
    });
    networkState->enqueue({200, {}});
    networkState->enqueue({
        200,
        compactTransferJson(testCompletedSnapshot(
            projectId, uploadedSnapshotId, snapshotBlobId,
            uploadSnapshotSha, quint64(uploadSnapshotBytes.size()), 2,
            snapshotManifest)),
        {{QByteArrayLiteral("Content-Type"),
          QByteArrayLiteral("application/json")}},
    });
    int snapshotUploadSignals = 0;
    CloudSnapshotUploadResult snapshotUploadResult;
    QObject::connect(
        &manager, &CloudAssetTransferManager::snapshotUploadCompleted,
        [&](quint64, const CloudSnapshotUploadResult& value) {
            ++snapshotUploadSignals;
            snapshotUploadResult = value;
        });
    CloudSnapshotUploadInput uploadSnapshotInput;
    uploadSnapshotInput.projectId = projectId;
    uploadSnapshotInput.uploadId = snapshotUploadId;
    uploadSnapshotInput.sourcePath = uploadSnapshotPath;
    uploadSnapshotInput.sequence = 2;
    uploadSnapshotInput.schemaVersion = 7;
    uploadSnapshotInput.assetIds = snapshotManifest;
    manager.uploadSnapshot(uploadSnapshotInput);
    if (!waitForTransfer(
            [&] { return snapshotUploadSignals == 1 || failureCount != 0; }) ||
        snapshotUploadSignals != 1 || failureCount != 0 ||
        snapshotUploadResult.assetIds != snapshotManifest) {
        return fail(QStringLiteral("Snapshot asset manifest upload failed"));
    }
    captures = networkState->captures();
    QJsonParseError snapshotPrepareError;
    const QJsonDocument snapshotPrepare = QJsonDocument::fromJson(
        captures.at(snapshotUploadCaptureBase).body, &snapshotPrepareError);
    if (captures.size() != snapshotUploadCaptureBase + 3 ||
        snapshotPrepareError.error != QJsonParseError::NoError ||
        !snapshotPrepare.isObject() ||
        snapshotPrepare.object().value(QStringLiteral("assetIds")).toArray() !=
            QJsonArray{singleAssetId, multipartAssetId}) {
        return fail(QStringLiteral(
            "Snapshot prepare omitted its canonical asset manifest"));
    }

    // Asset downloads are verified before QSaveFile publishes the blob, then
    // atomically registered in AssetCache by assetId and content hash.
    const QByteArray assetBytes("downloaded VLT audio\n");
    const QString downloadedAssetId =
        QStringLiteral("23232323-2323-4232-8232-232323232323");
    const QString assetSha = QString::fromLatin1(
        QCryptographicHash::hash(assetBytes, QCryptographicHash::Sha256)
            .toHex());
    networkState->enqueue({
        200,
        compactTransferJson(testDownloadPreparation(
            assetBytes,
            QStringLiteral("https://storage.vlt.test/asset?sig=download"),
            QStringLiteral("audio/wav"),
            QJsonObject{{QStringLiteral("x-vlt-transfer"),
                         QStringLiteral("asset-download")}})),
    });
    networkState->enqueue({
        200,
        assetBytes,
        {{QByteArrayLiteral("Content-Length"),
          QByteArray::number(assetBytes.size())},
         {QByteArrayLiteral("Content-Type"),
          QByteArrayLiteral("audio/wav")}},
    });
    int assetDownloadSignals = 0;
    QString downloadedAssetPath;
    QObject::connect(
        &manager, &CloudAssetTransferManager::assetDownloadCompleted,
        [&](quint64, const daw::AssetRef&, const QString& path) {
            ++assetDownloadSignals;
            downloadedAssetPath = path;
        });
    daw::AssetRef expectedAsset;
    expectedAsset.assetId = downloadedAssetId.toStdString();
    expectedAsset.sha256 = assetSha.toStdString();
    expectedAsset.byteSize = quint64(assetBytes.size());
    expectedAsset.kind = daw::AssetKind::Audio;
    expectedAsset.mimeType = "audio/wav";
    manager.downloadAsset(projectId, expectedAsset);
    if (!waitForTransfer(
            [&] { return assetDownloadSignals == 1 || failureCount != 0; }) ||
        assetDownloadSignals != 1 || failureCount != 0 ||
        cache.resolve(expectedAsset) != downloadedAssetPath) {
        return fail(QStringLiteral("Verified asset was not registered atomically"));
    }
    QFile cachedAsset(downloadedAssetPath);
    if (!cachedAsset.open(QIODevice::ReadOnly) ||
        cachedAsset.readAll() != assetBytes) {
        return fail(QStringLiteral("Asset cache contains incorrect bytes"));
    }

    // Bootstrap exposes only snapshot id/sequence. Empty expected SHA/size is
    // accepted, while authenticated preparation metadata becomes authoritative
    // and is enforced against the delegated bytes before atomic publication.
    const QByteArray snapshotBytes(
        R"({"schemaVersion":1,"projectFormatVersion":7,"project":{}})");
    const QString snapshotId =
        QStringLiteral("34343434-3434-4343-8343-343434343434");
    const QString snapshotSha = QString::fromLatin1(
        QCryptographicHash::hash(snapshotBytes, QCryptographicHash::Sha256)
            .toHex());
    const QString snapshotPath =
        QDir(temporary.path()).filePath(QStringLiteral("snapshot.json"));
    const int snapshotCaptureBase = networkState->captureCount();
    networkState->enqueue({
        200,
        compactTransferJson(testDownloadPreparation(
            snapshotBytes,
            QStringLiteral("https://storage.vlt.test/snapshot?sig=download"),
            QStringLiteral("application/vnd.vlt.project+json"),
            QJsonObject{{QStringLiteral("x-vlt-transfer"),
                         QStringLiteral("snapshot-download")}})),
    });
    networkState->enqueue({
        200,
        snapshotBytes,
        {{QByteArrayLiteral("Content-Length"),
          QByteArray::number(snapshotBytes.size())},
         {QByteArrayLiteral("Content-Type"),
          QByteArrayLiteral("application/vnd.vlt.project+json")}},
    });
    int snapshotDownloadSignals = 0;
    CloudSnapshotDownloadResult snapshotResult;
    QObject::connect(
        &manager, &CloudAssetTransferManager::snapshotDownloadCompleted,
        [&](quint64, const CloudSnapshotDownloadResult& value) {
            ++snapshotDownloadSignals;
            snapshotResult = value;
        });
    CloudSnapshotDownloadInput snapshotInput;
    snapshotInput.projectId = projectId;
    snapshotInput.snapshotId = snapshotId;
    snapshotInput.destinationPath = snapshotPath;
    manager.downloadSnapshot(snapshotInput);
    if (!waitForTransfer([&] {
            return snapshotDownloadSignals == 1 || failureCount != 0;
        }) ||
        snapshotDownloadSignals != 1 || failureCount != 0 ||
        snapshotResult.sha256 != snapshotSha ||
        snapshotResult.byteSize != quint64(snapshotBytes.size())) {
        return fail(QStringLiteral(
            "Snapshot metadata discovery or verification failed"));
    }
    QFile downloadedSnapshot(snapshotPath);
    captures = networkState->captures();
    if (!downloadedSnapshot.open(QIODevice::ReadOnly) ||
        downloadedSnapshot.readAll() != snapshotBytes ||
        captures.size() != snapshotCaptureBase + 2 ||
        !hasBearer(captures[snapshotCaptureBase].request, bearerToken) ||
        !captures[snapshotCaptureBase + 1]
             .request.rawHeader("Authorization")
             .isEmpty() ||
        captures[snapshotCaptureBase + 1]
                .request.rawHeader("x-vlt-transfer") !=
            QByteArrayLiteral("snapshot-download")) {
        return fail(QStringLiteral("Snapshot delegated request was not isolated"));
    }

    // When bootstrap (or another caller) does provide a digest, it remains a
    // strict precondition. A mismatch is rejected before object-store access.
    const int mismatchCaptureBase = networkState->captureCount();
    networkState->enqueue({
        200,
        compactTransferJson(testDownloadPreparation(
            snapshotBytes,
            QStringLiteral("https://storage.vlt.test/unused"),
            QStringLiteral("application/vnd.vlt.project+json"))),
    });
    CloudSnapshotDownloadInput mismatchInput = snapshotInput;
    mismatchInput.snapshotId =
        QStringLiteral("45454545-4545-4454-8454-454545454545");
    mismatchInput.sha256 = QString(64, QLatin1Char('0'));
    mismatchInput.byteSize = quint64(snapshotBytes.size());
    mismatchInput.destinationPath =
        QDir(temporary.path()).filePath(QStringLiteral("mismatch.json"));
    const quint64 mismatchId = manager.downloadSnapshot(mismatchInput);
    if (!waitForTransfer([&] { return lastFailedId == mismatchId; }) ||
        lastFailure.code != CloudTransferErrorCode::IntegrityMismatch ||
        networkState->captureCount() != mismatchCaptureBase + 1 ||
        QFileInfo::exists(mismatchInput.destinationPath)) {
        return fail(QStringLiteral("Caller snapshot precondition was ignored"));
    }

    // Delegated redirects are manual and may not change origin. This prevents
    // prescribed headers or signed query data from being sent to another host.
    const int redirectCaptureBase = networkState->captureCount();
    networkState->enqueue({
        200,
        compactTransferJson(testDownloadPreparation(
            snapshotBytes,
            QStringLiteral("https://storage.vlt.test/redirect-source"),
            QStringLiteral("application/vnd.vlt.project+json"))),
    });
    networkState->enqueue({
        307, {}, {}, QStringLiteral("https://evil.test/capture"), false,
    });
    CloudSnapshotDownloadInput redirectInput = snapshotInput;
    redirectInput.snapshotId =
        QStringLiteral("56565656-5656-4565-8565-565656565656");
    redirectInput.destinationPath =
        QDir(temporary.path()).filePath(QStringLiteral("redirect.json"));
    const quint64 redirectId = manager.downloadSnapshot(redirectInput);
    if (!waitForTransfer([&] { return lastFailedId == redirectId; }) ||
        lastFailure.code != CloudTransferErrorCode::RedirectRejected ||
        networkState->captureCount() != redirectCaptureBase + 2) {
        return fail(QStringLiteral("Cross-origin storage redirect was accepted"));
    }

    // Both the explicit cancel path and the request deadline abort the reply
    // without committing a destination file.
    manager.setRequestTimeoutMs(1000);
    const int cancelCaptureBase = networkState->captureCount();
    networkState->enqueue({200, {}, {}, {}, true});
    CloudSnapshotDownloadInput cancelInput = snapshotInput;
    cancelInput.snapshotId =
        QStringLiteral("67676767-6767-4676-8676-676767676767");
    cancelInput.destinationPath =
        QDir(temporary.path()).filePath(QStringLiteral("cancelled.json"));
    const quint64 cancelId = manager.downloadSnapshot(cancelInput);
    if (!waitForTransfer(
            [&] { return networkState->captureCount() == cancelCaptureBase + 1; }) ||
        !manager.cancel(cancelId) ||
        !waitForTransfer([&] { return lastFailedId == cancelId; }) ||
        lastFailure.code != CloudTransferErrorCode::Cancelled ||
        QFileInfo::exists(cancelInput.destinationPath)) {
        return fail(QStringLiteral("Transfer cancellation did not abort safely"));
    }

    manager.setRequestTimeoutMs(100);
    networkState->enqueue({200, {}, {}, {}, true});
    CloudSnapshotDownloadInput timeoutInput = snapshotInput;
    timeoutInput.snapshotId =
        QStringLiteral("78787878-7878-4787-8787-787878787878");
    timeoutInput.destinationPath =
        QDir(temporary.path()).filePath(QStringLiteral("timeout.json"));
    const quint64 timeoutId = manager.downloadSnapshot(timeoutInput);
    if (!waitForTransfer([&] { return lastFailedId == timeoutId; }) ||
        lastFailure.code != CloudTransferErrorCode::Timeout ||
        QFileInfo::exists(timeoutInput.destinationPath)) {
        return fail(QStringLiteral("Transfer deadline was not enforced"));
    }
    return true;
}

} // namespace collab
