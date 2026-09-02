#include "CloudProjectClient.hpp"

#include "AccountService.hpp"
#include "CloudSnapshotAssetManifest.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/CommandJson.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace collab {
namespace {

constexpr qsizetype kMaxRegularResponseBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaxBootstrapPageBytes = 16 * 1024 * 1024;
constexpr qsizetype kMaxRequestBodyBytes = 32 * 1024;
constexpr int kDefaultTimeoutMs = 15'000;
constexpr int kMinimumTimeoutMs = 1'000;
constexpr int kMaximumTimeoutMs = 120'000;
constexpr int kMaximumProjects = 10'000;
constexpr int kMaximumMembers = 10'000;
constexpr int kMaximumFieldHeads = 65'536;
constexpr int kMaximumBootstrapOperations = 200'000;
constexpr int kMinimumRecordingLeaseTtlSeconds = 5;
constexpr int kMaximumRecordingLeaseTtlSeconds = 120;
constexpr double kLargestExactJsonInteger = 9007199254740991.0;

struct ParseFailure {
    CloudClientErrorCode code = CloudClientErrorCode::InvalidResponse;
    QString message;
};

CloudClientError failure(CloudClientErrorCode code, const QString& message,
                         int status = 0, bool retryable = false) {
    CloudClientError error;
    error.code = code;
    error.httpStatus = status;
    error.safeMessage = message.left(240);
    error.retryable = retryable;
    return error;
}

QString statusMessage(int status) {
    switch (status) {
        case 401: return QStringLiteral("Sign in to access cloud projects");
        case 403: return QStringLiteral("Cloud project permission was denied");
        case 404: return QStringLiteral("Cloud project resource was not found");
        case 409: return QStringLiteral("Cloud project state has changed");
        case 410: return QStringLiteral("Cloud project resource is unavailable");
        case 422: return QStringLiteral("Cloud request was not accepted");
        case 429: return QStringLiteral("Too many cloud requests");
        default:
            return status >= 500
                ? QStringLiteral("Cloud service is temporarily unavailable")
                : QStringLiteral("Cloud request was rejected");
    }
}

bool exactKeys(const QJsonObject& object,
               std::initializer_list<const char*> required,
               std::initializer_list<const char*> optional = {}) {
    for (const char* key : required) {
        if (!object.contains(QString::fromLatin1(key))) return false;
    }
    for (auto iterator = object.constBegin(); iterator != object.constEnd();
         ++iterator) {
        const QByteArray key = iterator.key().toLatin1();
        const bool requiredKey = std::any_of(
            required.begin(), required.end(), [&](const char* allowed) {
                return key == allowed;
            });
        const bool optionalKey = std::any_of(
            optional.begin(), optional.end(), [&](const char* allowed) {
                return key == allowed;
            });
        if (!requiredKey && !optionalKey) return false;
    }
    return true;
}

std::optional<quint64> exactUnsigned(const QJsonValue& value,
                                    bool positive = false) {
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble(-1.0);
    const double minimum = positive ? 1.0 : 0.0;
    if (!std::isfinite(number) || number < minimum ||
        number > kLargestExactJsonInteger || std::floor(number) != number) {
        return std::nullopt;
    }
    return quint64(number);
}

std::optional<int> boundedInteger(const QJsonValue& value, int minimum,
                                  int maximum) {
    const auto number = exactUnsigned(value, minimum > 0);
    if (!number || *number < quint64(std::max(0, minimum)) ||
        *number > quint64(maximum)) {
        return std::nullopt;
    }
    return int(*number);
}

bool normalizedUuid(const QJsonValue& value, QString* result) {
    if (!value.isString()) return false;
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
        "[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$"));
    const QString text = value.toString();
    const QUuid uuid(text);
    if (!pattern.match(text).hasMatch() || uuid.isNull()) return false;
    if (result)
        *result = uuid.toString(QUuid::WithoutBraces).toLower();
    return true;
}

bool optionalUuid(const QJsonObject& object, const char* key,
                  QString* result) {
    const QString name = QString::fromLatin1(key);
    if (!object.contains(name) || object.value(name).isNull()) {
        if (result) result->clear();
        return true;
    }
    return normalizedUuid(object.value(name), result);
}

bool boundedString(const QJsonValue& value, int minimum, int maximum,
                   QString* result) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.size() < minimum || text.size() > maximum) return false;
    if (result) *result = text;
    return true;
}

bool dateTimeValue(const QJsonValue& value, QDateTime* result) {
    if (!value.isString()) return false;
    QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
    if (!parsed.isValid() || parsed.timeSpec() == Qt::LocalTime) return false;
    if (result) *result = parsed.toUTC();
    return true;
}

bool optionalDateTime(const QJsonObject& object, const char* key,
                      QDateTime* result) {
    const QString name = QString::fromLatin1(key);
    if (!object.contains(name) || object.value(name).isNull()) {
        if (result) *result = {};
        return true;
    }
    return dateTimeValue(object.value(name), result);
}

std::optional<CloudProjectStatus> projectStatus(const QString& value) {
    if (value == QLatin1String("uploading"))
        return CloudProjectStatus::Uploading;
    if (value == QLatin1String("active")) return CloudProjectStatus::Active;
    if (value == QLatin1String("read_only"))
        return CloudProjectStatus::ReadOnly;
    if (value == QLatin1String("conflict"))
        return CloudProjectStatus::Conflict;
    if (value == QLatin1String("archived"))
        return CloudProjectStatus::Archived;
    return std::nullopt;
}

std::optional<CloudProjectRole> projectRole(const QString& value) {
    if (value == QLatin1String("owner")) return CloudProjectRole::Owner;
    if (value == QLatin1String("editor")) return CloudProjectRole::Editor;
    if (value == QLatin1String("viewer")) return CloudProjectRole::Viewer;
    return std::nullopt;
}

std::optional<CloudMemberRole> memberRole(const QString& value) {
    if (value == QLatin1String("editor")) return CloudMemberRole::Editor;
    if (value == QLatin1String("viewer")) return CloudMemberRole::Viewer;
    return std::nullopt;
}

QString memberRoleName(CloudMemberRole role) {
    return role == CloudMemberRole::Editor ? QStringLiteral("editor")
                                           : QStringLiteral("viewer");
}

std::optional<CloudSessionMode> sessionMode(const QString& value) {
    if (value == QLatin1String("independent"))
        return CloudSessionMode::Independent;
    if (value == QLatin1String("follow_host"))
        return CloudSessionMode::FollowHost;
    if (value == QLatin1String("synchronized"))
        return CloudSessionMode::Synchronized;
    return std::nullopt;
}

QString sessionModeName(CloudSessionMode mode) {
    switch (mode) {
        case CloudSessionMode::FollowHost: return QStringLiteral("follow_host");
        case CloudSessionMode::Synchronized:
            return QStringLiteral("synchronized");
        default: return QStringLiteral("independent");
    }
}

std::optional<CloudSessionStatus> sessionStatus(const QString& value) {
    if (value == QLatin1String("starting"))
        return CloudSessionStatus::Starting;
    if (value == QLatin1String("active")) return CloudSessionStatus::Active;
    if (value == QLatin1String("ending")) return CloudSessionStatus::Ending;
    if (value == QLatin1String("ended")) return CloudSessionStatus::Ended;
    return std::nullopt;
}

bool validApplicationVersion(const QString& value) {
    static const QRegularExpression semver(QStringLiteral(
        "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
        "(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?$"));
    return value.size() >= 5 && value.size() <= 64 &&
           semver.match(value).hasMatch();
}

std::optional<CloudProject> parseProject(const QJsonObject& object,
                                         ParseFailure* error) {
    if (!exactKeys(object,
                   {"id", "owner_user_id", "title", "status",
                    "format_version", "engine_version",
                    "minimum_app_version", "head_seq", "snapshot_seq",
                    "created_at", "updated_at"},
                   {"archived_at"})) {
        if (error) error->message = QStringLiteral("Invalid project shape");
        return std::nullopt;
    }
    CloudProject project;
    const auto status = projectStatus(
        object.value(QStringLiteral("status")).toString());
    const auto formatVersion = boundedInteger(
        object.value(QStringLiteral("format_version")), 1, 1'000'000);
    const auto head = exactUnsigned(object.value(QStringLiteral("head_seq")));
    const auto snapshot =
        exactUnsigned(object.value(QStringLiteral("snapshot_seq")));
    if (!normalizedUuid(object.value(QStringLiteral("id")), &project.id) ||
        !normalizedUuid(object.value(QStringLiteral("owner_user_id")),
                        &project.ownerUserId) ||
        !boundedString(object.value(QStringLiteral("title")), 1, 160,
                       &project.title) ||
        !status || !formatVersion ||
        !boundedString(object.value(QStringLiteral("engine_version")), 0, 64,
                       &project.engineVersion) ||
        !boundedString(object.value(QStringLiteral("minimum_app_version")),
                       0, 64, &project.minimumAppVersion) ||
        !head || !snapshot || *snapshot > *head ||
        !dateTimeValue(object.value(QStringLiteral("created_at")),
                       &project.createdAt) ||
        !dateTimeValue(object.value(QStringLiteral("updated_at")),
                       &project.updatedAt) ||
        !optionalDateTime(object, "archived_at", &project.archivedAt)) {
        if (error) error->message = QStringLiteral("Invalid project fields");
        return std::nullopt;
    }
    project.status = *status;
    project.formatVersion = *formatVersion;
    project.headSequence = *head;
    project.snapshotSequence = *snapshot;
    return project;
}

std::optional<CloudProjectView> parseProjectView(const QJsonObject& object,
                                                 ParseFailure* error) {
    if (!exactKeys(object, {"project", "role"}) ||
        !object.value(QStringLiteral("project")).isObject() ||
        !object.value(QStringLiteral("role")).isString()) {
        if (error) error->message = QStringLiteral("Invalid project view shape");
        return std::nullopt;
    }
    auto project = parseProject(
        object.value(QStringLiteral("project")).toObject(), error);
    const auto role = projectRole(
        object.value(QStringLiteral("role")).toString());
    if (!project || !role) {
        if (error && error->message.isEmpty())
            error->message = QStringLiteral("Invalid project role");
        return std::nullopt;
    }
    return CloudProjectView{std::move(*project), *role};
}

std::optional<CloudSnapshotDescriptor> parseSnapshot(
    const QJsonObject& object, ParseFailure* error) {
    if (!exactKeys(object,
                   {"id", "project_id", "seq", "blob_id",
                    "schema_version", "asset_ids", "created_at"},
                   {"created_by"})) {
        if (error) error->message = QStringLiteral("Invalid snapshot shape");
        return std::nullopt;
    }
    CloudSnapshotDescriptor snapshot;
    const auto sequence = exactUnsigned(object.value(QStringLiteral("seq")));
    const auto schema = boundedInteger(
        object.value(QStringLiteral("schema_version")), 1, 1'000'000);
    const QJsonValue assetIdsValue =
        object.value(QStringLiteral("asset_ids"));
    QStringList assetIds;
    bool assetIdsValid = assetIdsValue.isArray();
    if (assetIdsValid) {
        const QJsonArray values = assetIdsValue.toArray();
        assetIdsValid = values.size() <= kMaximumSnapshotAssetIds;
        if (assetIdsValid) {
            assetIds.reserve(values.size());
            for (const QJsonValue& value : values) {
                if (!value.isString()) {
                    assetIdsValid = false;
                    break;
                }
                assetIds.push_back(value.toString());
            }
        }
    }
    if (!normalizedUuid(object.value(QStringLiteral("id")), &snapshot.id) ||
        !normalizedUuid(object.value(QStringLiteral("project_id")),
                        &snapshot.projectId) ||
        !sequence ||
        !normalizedUuid(object.value(QStringLiteral("blob_id")),
                        &snapshot.blobId) ||
        !schema || !assetIdsValid ||
        !isCanonicalCloudSnapshotAssetManifest(assetIds) ||
        !optionalUuid(object, "created_by", &snapshot.createdBy) ||
        !dateTimeValue(object.value(QStringLiteral("created_at")),
                       &snapshot.createdAt)) {
        if (error) error->message = QStringLiteral("Invalid snapshot fields");
        return std::nullopt;
    }
    snapshot.sequence = *sequence;
    snapshot.schemaVersion = *schema;
    snapshot.assetIds = std::move(assetIds);
    return snapshot;
}

std::optional<CloudProjectOperation> parseOperation(
    const QJsonObject& object, ParseFailure* error) {
    if (!exactKeys(object,
                   {"project_id", "serverSeq", "opId", "kind",
                    "schemaVersion", "baseServerSeq", "payload",
                    "preconditions", "touchedFields", "created_at"},
                   {"transactionId", "actor_user_id", "actor_device_id"}) ||
        !object.value(QStringLiteral("payload")).isObject() ||
        !object.value(QStringLiteral("preconditions")).isArray() ||
        !object.value(QStringLiteral("touchedFields")).isArray()) {
        if (error) error->message = QStringLiteral("Invalid operation shape");
        return std::nullopt;
    }
    CloudProjectOperation operation;
    QString operationId;
    QString transactionId;
    const auto sequence =
        exactUnsigned(object.value(QStringLiteral("serverSeq")), true);
    const auto baseSequence =
        exactUnsigned(object.value(QStringLiteral("baseServerSeq")));
    if (!normalizedUuid(object.value(QStringLiteral("project_id")),
                        &operation.projectId) ||
        !sequence ||
        !normalizedUuid(object.value(QStringLiteral("opId")), &operationId) ||
        !optionalUuid(object, "transactionId", &transactionId) ||
        !optionalUuid(object, "actor_user_id", &operation.actorUserId) ||
        !optionalUuid(object, "actor_device_id", &operation.actorDeviceId) ||
        !baseSequence || *baseSequence >= *sequence ||
        !dateTimeValue(object.value(QStringLiteral("created_at")),
                       &operation.createdAt)) {
        if (error) error->message = QStringLiteral("Invalid operation fields");
        return std::nullopt;
    }

    QJsonObject locked{
        {QStringLiteral("schemaVersion"),
         object.value(QStringLiteral("schemaVersion"))},
        {QStringLiteral("opId"), operationId},
        {QStringLiteral("transactionId"), transactionId},
        {QStringLiteral("baseServerSeq"), double(*baseSequence)},
        {QStringLiteral("kind"), object.value(QStringLiteral("kind"))},
        {QStringLiteral("payload"), object.value(QStringLiteral("payload"))},
        {QStringLiteral("preconditions"),
         object.value(QStringLiteral("preconditions"))},
        {QStringLiteral("touchedFields"),
         object.value(QStringLiteral("touchedFields"))},
    };
    const QByteArray bytes =
        QJsonDocument(locked).toJson(QJsonDocument::Compact);
    const nlohmann::json json = nlohmann::json::parse(
        bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    std::string commandError;
    auto command = json.is_discarded()
        ? std::optional<daw::collab::ProjectCommand>()
        : daw::collab::projectCommandFromJson(json, &commandError);
    if (!command) {
        if (error) {
            error->message = QStringLiteral("Invalid typed project command");
        }
        return std::nullopt;
    }
    command->meta.projectId = operation.projectId.toStdString();
    command->meta.actorId = operation.actorUserId.toStdString();
    command->meta.clientId = operation.actorDeviceId.toStdString();
    command->meta.serverSequence = *sequence;
    operation.serverSequence = *sequence;
    operation.command = std::move(*command);
    return operation;
}

std::optional<CloudProjectFieldHead> parseFieldHead(
    const QJsonObject& object, ParseFailure* error) {
    if (!exactKeys(object,
                   {"project_id", "field_key", "head_seq", "headOpId",
                    "updated_at"})) {
        if (error) error->message = QStringLiteral("Invalid field-head shape");
        return std::nullopt;
    }
    CloudProjectFieldHead head;
    const auto sequence =
        exactUnsigned(object.value(QStringLiteral("head_seq")), true);
    if (!normalizedUuid(object.value(QStringLiteral("project_id")),
                        &head.projectId) ||
        !boundedString(object.value(QStringLiteral("field_key")), 1, 512,
                       &head.fieldKey) ||
        !sequence ||
        !normalizedUuid(object.value(QStringLiteral("headOpId")),
                        &head.operationId) ||
        !dateTimeValue(object.value(QStringLiteral("updated_at")),
                       &head.updatedAt)) {
        if (error) error->message = QStringLiteral("Invalid field-head fields");
        return std::nullopt;
    }
    head.headSequence = *sequence;
    return head;
}

struct BootstrapPage {
    CloudProject project;
    CloudProjectRole role = CloudProjectRole::Viewer;
    std::optional<CloudSnapshotDescriptor> snapshot;
    QVector<CloudProjectOperation> operations;
    QVector<CloudProjectFieldHead> fieldHeads;
    quint64 headSequence = 0;
    quint64 nextAfterSequence = 0;
    bool hasMore = false;
};

std::optional<BootstrapPage> parseBootstrapPage(const QJsonObject& object,
                                                ParseFailure* error) {
    if (!exactKeys(object,
                   {"project", "role", "operations", "field_heads",
                    "head_seq", "next_after_seq", "has_more"},
                   {"snapshot"}) ||
        !object.value(QStringLiteral("project")).isObject() ||
        !object.value(QStringLiteral("operations")).isArray() ||
        !object.value(QStringLiteral("field_heads")).isArray() ||
        !object.value(QStringLiteral("has_more")).isBool()) {
        if (error) error->message = QStringLiteral("Invalid bootstrap shape");
        return std::nullopt;
    }
    BootstrapPage page;
    auto project = parseProject(
        object.value(QStringLiteral("project")).toObject(), error);
    const auto role = projectRole(object.value(QStringLiteral("role")).toString());
    const auto head = exactUnsigned(object.value(QStringLiteral("head_seq")));
    const auto next =
        exactUnsigned(object.value(QStringLiteral("next_after_seq")));
    if (!project || !role || !head || !next || *head != project->headSequence ||
        *next > *head) {
        if (error && error->message.isEmpty())
            error->message = QStringLiteral("Invalid bootstrap metadata");
        return std::nullopt;
    }
    page.project = std::move(*project);
    page.role = *role;
    page.headSequence = *head;
    page.nextAfterSequence = *next;
    page.hasMore = object.value(QStringLiteral("has_more")).toBool();

    if (object.contains(QStringLiteral("snapshot")) &&
        !object.value(QStringLiteral("snapshot")).isNull()) {
        if (!object.value(QStringLiteral("snapshot")).isObject()) {
            if (error) error->message = QStringLiteral("Invalid snapshot value");
            return std::nullopt;
        }
        auto snapshot = parseSnapshot(
            object.value(QStringLiteral("snapshot")).toObject(), error);
        if (!snapshot || snapshot->projectId != page.project.id ||
            snapshot->sequence > page.project.snapshotSequence ||
            snapshot->sequence > page.headSequence) {
            if (error && error->message.isEmpty())
                error->message = QStringLiteral("Snapshot does not match project");
            return std::nullopt;
        }
        page.snapshot = std::move(*snapshot);
    }

    const QJsonArray operations =
        object.value(QStringLiteral("operations")).toArray();
    if (operations.size() > 2000) {
        if (error) error->message = QStringLiteral("Bootstrap page is too large");
        return std::nullopt;
    }
    page.operations.reserve(operations.size());
    for (const QJsonValue& value : operations) {
        if (!value.isObject()) {
            if (error) error->message = QStringLiteral("Invalid operation value");
            return std::nullopt;
        }
        auto operation = parseOperation(value.toObject(), error);
        if (!operation || operation->projectId != page.project.id ||
            operation->serverSequence > page.headSequence) {
            if (error && error->message.isEmpty())
                error->message = QStringLiteral("Operation does not match project");
            return std::nullopt;
        }
        page.operations.push_back(std::move(*operation));
    }

    const QJsonArray fieldHeads =
        object.value(QStringLiteral("field_heads")).toArray();
    if (fieldHeads.size() > kMaximumFieldHeads) {
        if (error) error->message = QStringLiteral("Too many project field heads");
        return std::nullopt;
    }
    page.fieldHeads.reserve(fieldHeads.size());
    QSet<QString> fieldKeys;
    for (const QJsonValue& value : fieldHeads) {
        if (!value.isObject()) {
            if (error) error->message = QStringLiteral("Invalid field-head value");
            return std::nullopt;
        }
        auto fieldHead = parseFieldHead(value.toObject(), error);
        if (!fieldHead || fieldHead->projectId != page.project.id ||
            fieldHead->headSequence > page.headSequence ||
            fieldKeys.contains(fieldHead->fieldKey)) {
            if (error && error->message.isEmpty())
                error->message = QStringLiteral("Field head does not match project");
            return std::nullopt;
        }
        fieldKeys.insert(fieldHead->fieldKey);
        page.fieldHeads.push_back(std::move(*fieldHead));
    }
    return page;
}

std::optional<CloudProjectMember> parseMember(const QJsonObject& object,
                                              ParseFailure* error) {
    if (!exactKeys(object,
                   {"project_id", "user_id", "role", "color_index",
                    "joined_at", "updated_at"},
                   {"invited_by"})) {
        if (error) error->message = QStringLiteral("Invalid member shape");
        return std::nullopt;
    }
    CloudProjectMember member;
    const auto role = memberRole(object.value(QStringLiteral("role")).toString());
    const auto color = boundedInteger(
        object.value(QStringLiteral("color_index")), 0, 31);
    if (!normalizedUuid(object.value(QStringLiteral("project_id")),
                        &member.projectId) ||
        !normalizedUuid(object.value(QStringLiteral("user_id")),
                        &member.userId) ||
        !role || !color ||
        !optionalUuid(object, "invited_by", &member.invitedBy) ||
        !dateTimeValue(object.value(QStringLiteral("joined_at")),
                       &member.joinedAt) ||
        !dateTimeValue(object.value(QStringLiteral("updated_at")),
                       &member.updatedAt)) {
        if (error) error->message = QStringLiteral("Invalid member fields");
        return std::nullopt;
    }
    member.role = *role;
    member.colorIndex = *color;
    return member;
}

std::optional<CloudProjectInvite> parseInvite(const QJsonObject& object,
                                              ParseFailure* error) {
    if (!exactKeys(object,
                   {"id", "project_id", "role", "expires_at", "created_at"},
                   {"invited_by", "accepted_by", "accepted_at",
                    "revoked_at"})) {
        if (error) error->message = QStringLiteral("Invalid invite shape");
        return std::nullopt;
    }
    CloudProjectInvite invite;
    const auto role = memberRole(object.value(QStringLiteral("role")).toString());
    if (!normalizedUuid(object.value(QStringLiteral("id")), &invite.id) ||
        !normalizedUuid(object.value(QStringLiteral("project_id")),
                        &invite.projectId) ||
        !optionalUuid(object, "invited_by", &invite.invitedBy) || !role ||
        !dateTimeValue(object.value(QStringLiteral("expires_at")),
                       &invite.expiresAt) ||
        !optionalUuid(object, "accepted_by", &invite.acceptedBy) ||
        !optionalDateTime(object, "accepted_at", &invite.acceptedAt) ||
        !optionalDateTime(object, "revoked_at", &invite.revokedAt) ||
        !dateTimeValue(object.value(QStringLiteral("created_at")),
                       &invite.createdAt)) {
        if (error) error->message = QStringLiteral("Invalid invite fields");
        return std::nullopt;
    }
    invite.role = *role;
    return invite;
}

std::optional<CloudLiveSession> parseLiveSession(const QJsonObject& object,
                                                 ParseFailure* error) {
    if (!exactKeys(object,
                   {"id", "project_id", "mode", "status", "version",
                    "created_at", "updated_at"},
                   {"created_by", "host_member_id", "started_at",
                    "ended_at"})) {
        if (error) error->message = QStringLiteral("Invalid session shape");
        return std::nullopt;
    }
    CloudLiveSession session;
    const auto mode = sessionMode(object.value(QStringLiteral("mode")).toString());
    const auto status =
        sessionStatus(object.value(QStringLiteral("status")).toString());
    const auto version =
        exactUnsigned(object.value(QStringLiteral("version")), true);
    if (!normalizedUuid(object.value(QStringLiteral("id")), &session.id) ||
        !normalizedUuid(object.value(QStringLiteral("project_id")),
                        &session.projectId) ||
        !optionalUuid(object, "created_by", &session.createdBy) ||
        !optionalUuid(object, "host_member_id", &session.hostMemberId) ||
        !mode || !status || !version ||
        !dateTimeValue(object.value(QStringLiteral("created_at")),
                       &session.createdAt) ||
        !optionalDateTime(object, "started_at", &session.startedAt) ||
        !dateTimeValue(object.value(QStringLiteral("updated_at")),
                       &session.updatedAt) ||
        !optionalDateTime(object, "ended_at", &session.endedAt)) {
        if (error) error->message = QStringLiteral("Invalid session fields");
        return std::nullopt;
    }
    session.mode = *mode;
    session.status = *status;
    session.version = *version;
    return session;
}

std::optional<CloudSessionMember> parseSessionMember(
    const QJsonObject& object, ParseFailure* error) {
    if (!exactKeys(object,
                   {"id", "session_id", "user_id", "device_id",
                    "joined_at", "last_seen_at"},
                   {"desktop_session_id", "left_at"})) {
        if (error) error->message = QStringLiteral("Invalid session-member shape");
        return std::nullopt;
    }
    CloudSessionMember member;
    if (!normalizedUuid(object.value(QStringLiteral("id")), &member.id) ||
        !normalizedUuid(object.value(QStringLiteral("session_id")),
                        &member.sessionId) ||
        !normalizedUuid(object.value(QStringLiteral("user_id")),
                        &member.userId) ||
        !normalizedUuid(object.value(QStringLiteral("device_id")),
                        &member.deviceId) ||
        !optionalUuid(object, "desktop_session_id",
                      &member.desktopSessionId) ||
        !dateTimeValue(object.value(QStringLiteral("joined_at")),
                       &member.joinedAt) ||
        !dateTimeValue(object.value(QStringLiteral("last_seen_at")),
                       &member.lastSeenAt) ||
        !optionalDateTime(object, "left_at", &member.leftAt)) {
        if (error) error->message = QStringLiteral("Invalid session-member fields");
        return std::nullopt;
    }
    return member;
}

std::optional<CloudSessionState> parseSessionState(const QJsonObject& object,
                                                   ParseFailure* error) {
    if (!exactKeys(object, {"session", "members"}) ||
        !object.value(QStringLiteral("session")).isObject() ||
        !object.value(QStringLiteral("members")).isArray()) {
        if (error) error->message = QStringLiteral("Invalid session-state shape");
        return std::nullopt;
    }
    auto session = parseLiveSession(
        object.value(QStringLiteral("session")).toObject(), error);
    const QJsonArray values = object.value(QStringLiteral("members")).toArray();
    if (!session || values.size() > 8) {
        if (error && error->message.isEmpty())
            error->message = QStringLiteral("Invalid session-state members");
        return std::nullopt;
    }
    CloudSessionState state;
    state.session = std::move(*session);
    QSet<QString> memberIds;
    state.members.reserve(values.size());
    for (const QJsonValue& value : values) {
        if (!value.isObject()) {
            if (error) error->message = QStringLiteral("Invalid session member");
            return std::nullopt;
        }
        auto member = parseSessionMember(value.toObject(), error);
        if (!member || member->sessionId != state.session.id ||
            memberIds.contains(member->id)) {
            if (error && error->message.isEmpty())
                error->message = QStringLiteral("Session member mismatch");
            return std::nullopt;
        }
        memberIds.insert(member->id);
        state.members.push_back(std::move(*member));
    }
    if (!state.session.hostMemberId.isEmpty() &&
        !memberIds.contains(state.session.hostMemberId)) {
        if (error) error->message = QStringLiteral("Session host is not online");
        return std::nullopt;
    }
    return state;
}

std::optional<CloudProjectTrackLease> parseTrackLease(
    const QJsonObject& object, ParseFailure* error) {
    if (!exactKeys(object,
                   {"id", "project_id", "session_id", "track_id",
                    "lease_kind", "holder_member_id", "acquired_at",
                    "renewed_at", "expires_at"})) {
        if (error) error->message = QStringLiteral("Invalid track lease shape");
        return std::nullopt;
    }

    CloudProjectTrackLease lease;
    if (!normalizedUuid(object.value(QStringLiteral("id")), &lease.id) ||
        !normalizedUuid(object.value(QStringLiteral("project_id")),
                        &lease.projectId) ||
        !normalizedUuid(object.value(QStringLiteral("session_id")),
                        &lease.sessionId) ||
        !normalizedUuid(object.value(QStringLiteral("track_id")),
                        &lease.trackId) ||
        !object.value(QStringLiteral("lease_kind")).isString() ||
        object.value(QStringLiteral("lease_kind")).toString() !=
            QLatin1String("record") ||
        !normalizedUuid(object.value(QStringLiteral("holder_member_id")),
                        &lease.holderMemberId) ||
        !dateTimeValue(object.value(QStringLiteral("acquired_at")),
                       &lease.acquiredAt) ||
        !dateTimeValue(object.value(QStringLiteral("renewed_at")),
                       &lease.renewedAt) ||
        !dateTimeValue(object.value(QStringLiteral("expires_at")),
                       &lease.expiresAt) ||
        lease.acquiredAt > lease.renewedAt ||
        lease.renewedAt >= lease.expiresAt) {
        if (error) error->message = QStringLiteral("Invalid track lease fields");
        return std::nullopt;
    }
    lease.kind = CloudProjectLeaseKind::Record;
    return lease;
}

bool projectsEqual(const CloudProject& left, const CloudProject& right) {
    return left.id == right.id && left.ownerUserId == right.ownerUserId &&
           left.title == right.title && left.status == right.status &&
           left.formatVersion == right.formatVersion &&
           left.engineVersion == right.engineVersion &&
           left.minimumAppVersion == right.minimumAppVersion &&
           left.headSequence == right.headSequence &&
           left.snapshotSequence == right.snapshotSequence &&
           left.createdAt == right.createdAt && left.updatedAt == right.updatedAt &&
           left.archivedAt == right.archivedAt;
}

bool snapshotsEqual(const std::optional<CloudSnapshotDescriptor>& left,
                    const std::optional<CloudSnapshotDescriptor>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return left->id == right->id && left->projectId == right->projectId &&
           left->sequence == right->sequence && left->blobId == right->blobId &&
           left->schemaVersion == right->schemaVersion &&
           left->createdBy == right->createdBy &&
           left->createdAt == right->createdAt &&
           left->assetIds == right->assetIds;
}

bool fieldHeadsEqual(const QVector<CloudProjectFieldHead>& left,
                     const QVector<CloudProjectFieldHead>& right) {
    if (left.size() != right.size()) return false;
    for (qsizetype index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.projectId != b.projectId || a.fieldKey != b.fieldKey ||
            a.headSequence != b.headSequence ||
            a.operationId != b.operationId || a.updatedAt != b.updatedAt) {
            return false;
        }
    }
    return true;
}

int effectivePort(const QUrl& url) {
    const int explicitPort = url.port(-1);
    if (explicitPort >= 0) return explicitPort;
    return url.scheme() == QLatin1String("https") ? 443 : 80;
}

bool sameOrigin(const QUrl& left, const QUrl& right) {
    return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0 &&
           left.host().compare(right.host(), Qt::CaseInsensitive) == 0 &&
           effectivePort(left) == effectivePort(right);
}

std::optional<QJsonObject> parseJsonObject(const QByteArray& bytes,
                                           CloudClientError* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = failure(CloudClientErrorCode::InvalidJson,
                             QStringLiteral("Server returned invalid JSON"));
        return std::nullopt;
    }
    return document.object();
}

CloudClientError parseApiError(const QByteArray& body, int status) {
    CloudClientError error = failure(
        CloudClientErrorCode::UnexpectedStatus,
        statusMessage(status), status,
        status == 408 || status == 429 || status >= 500);
    if (body.isEmpty() || body.size() > kMaxRegularResponseBytes) return error;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return error;
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
    // Server text is deliberately validated but never surfaced. It is raw
    // remote payload and could echo an invitation, filename or delegated URL.
    const QString serverMessage =
        object.value(QStringLiteral("message")).toString();
    if (serverMessage.size() > 240)
        return failure(CloudClientErrorCode::UnexpectedStatus,
                       statusMessage(status), status, error.retryable);
    return error;
}

} // namespace

struct CloudProjectClient::Impl {
    using Handler = std::function<void(const QByteArray&)>;

    struct PendingRequest {
        CloudRequestKind kind = CloudRequestKind::GetProject;
        QPointer<QNetworkReply> reply;
        QPointer<QTimer> timeout;
        QSet<int> expectedStatuses;
        qsizetype maximumBytes = kMaxRegularResponseBytes;
        bool expectsJson = true;
        Handler handler;
    };

    struct BootstrapAccumulator {
        quint64 requestId = 0;
        quint64 generation = 0;
        QString projectId;
        quint64 requestedAfter = 0;
        quint64 currentAfter = 0;
        int pageLimit = 500;
        int pageCount = 0;
        bool initialized = false;
        QElapsedTimer elapsed;
        CloudProjectBootstrap result;
        QSet<QString> operationIds;
    };

    CloudProjectClient* q = nullptr;
    CredentialProvider credentialProvider;
    QPointer<QNetworkAccessManager> network;
    QHash<quint64, PendingRequest*> pending;
    quint64 nextRequestId = 1;
    int timeoutMs = kDefaultTimeoutMs;
    quint64 bootstrapGeneration = 0;
    quint64 bootstrapRequestId = 0;

    Impl(CloudProjectClient* owner, CredentialProvider provider,
         QNetworkAccessManager* manager)
        : q(owner),
          credentialProvider(std::move(provider)),
          network(manager) {}

    ~Impl() {
        const QList<quint64> ids = pending.keys();
        for (quint64 id : ids) discard(id);
    }

    quint64 allocate() {
        if (nextRequestId == 0) ++nextRequestId;
        return nextRequestId++;
    }

    void emitFailure(quint64 requestId, CloudRequestKind kind,
                     const CloudClientError& error) {
        if (requestId == bootstrapRequestId &&
            kind == CloudRequestKind::BootstrapProject) {
            bootstrapRequestId = 0;
        }
        emit q->requestFailed(requestId, kind, error);
    }

    quint64 invalid(CloudRequestKind kind, const QString& message) {
        const quint64 requestId = allocate();
        emitFailure(requestId, kind,
                    failure(CloudClientErrorCode::InvalidInput, message));
        return requestId;
    }

    std::optional<QString> uuidInput(const QString& value) const {
        QString normalized;
        return normalizedUuid(QJsonValue(value), &normalized)
            ? std::optional<QString>(normalized)
            : std::nullopt;
    }

    std::optional<Credentials> credentials(CloudClientError* error) const {
        const Credentials value = credentialProvider ? credentialProvider()
                                                     : Credentials{};
        if (!value.authenticated || value.bearerToken.isEmpty()) {
            if (error)
                *error = failure(CloudClientErrorCode::Unauthenticated,
                                 QStringLiteral("Sign in to access cloud projects"));
            return std::nullopt;
        }
        if (value.offline) {
            if (error)
                *error = failure(CloudClientErrorCode::Offline,
                                 QStringLiteral("Cloud projects are offline"));
            return std::nullopt;
        }
        if (value.bearerToken.size() > 16 * 1024 ||
            value.bearerToken.contains('\r') ||
            value.bearerToken.contains('\n') ||
            !uuidInput(value.userId) || !uuidInput(value.deviceId)) {
            if (error)
                *error = failure(CloudClientErrorCode::Unauthenticated,
                                 QStringLiteral("Cloud identity is unavailable"));
            return std::nullopt;
        }
        const QUrl origin(value.apiOrigin);
        if (!origin.isValid() || origin.host().isEmpty() ||
            (origin.scheme() != QLatin1String("https") &&
             origin.scheme() != QLatin1String("http")) ||
            !origin.userInfo().isEmpty() || origin.hasQuery() ||
            origin.hasFragment()) {
            if (error)
                *error = failure(CloudClientErrorCode::UnsafeOrigin,
                                 QStringLiteral("Cloud API origin is invalid"));
            return std::nullopt;
        }
        return value;
    }

    std::optional<QNetworkRequest> authorizedRequest(
        const Credentials& auth, const QString& relativePath,
        const QUrlQuery& query, bool hasJsonBody, CloudClientError* error) const {
        QUrl origin(auth.apiOrigin);
        QString path = origin.path();
        while (path.endsWith(QLatin1Char('/'))) path.chop(1);
        if (relativePath.isEmpty() || relativePath.startsWith(QLatin1Char('/')) ||
            relativePath.contains(QStringLiteral(".."))) {
            if (error)
                *error = failure(CloudClientErrorCode::UnsafeOrigin,
                                 QStringLiteral("Cloud request path is invalid"));
            return std::nullopt;
        }
        path += QLatin1Char('/') + relativePath;
        QUrl endpoint = origin;
        endpoint.setPath(path);
        endpoint.setQuery(query);
        endpoint.setUserInfo({});
        endpoint.setFragment({});
        if (!sameOrigin(origin, endpoint)) {
            if (error)
                *error = failure(CloudClientErrorCode::UnsafeOrigin,
                                 QStringLiteral("Cloud request origin mismatch"));
            return std::nullopt;
        }
        QNetworkRequest request(endpoint);
        request.setRawHeader("Accept", "application/json");
        if (hasJsonBody)
            request.setRawHeader("Content-Type", "application/json");
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") + auth.bearerToken);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(timeoutMs);
        return request;
    }

    bool issue(quint64 requestId, CloudRequestKind kind,
               const QByteArray& method, const QString& relativePath,
               const QUrlQuery& query, const QByteArray& body,
               std::initializer_list<int> expectedStatuses,
               qsizetype maximumBytes, bool expectsJson, Handler handler) {
        if (!network) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::NetworkFailure,
                                QStringLiteral("Cloud network is unavailable"),
                                0, true));
            return false;
        }
        if (body.size() > kMaxRequestBodyBytes) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::InvalidInput,
                                QStringLiteral("Cloud request is too large")));
            return false;
        }
        CloudClientError requestError;
        const auto auth = credentials(&requestError);
        if (!auth) {
            emitFailure(requestId, kind, requestError);
            return false;
        }
        const auto request = authorizedRequest(*auth, relativePath, query,
                                               !body.isEmpty(), &requestError);
        if (!request) {
            emitFailure(requestId, kind, requestError);
            return false;
        }
        QNetworkReply* reply =
            network->sendCustomRequest(*request, method, body);
        if (!reply) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::NetworkFailure,
                                QStringLiteral("Cloud request could not start"),
                                0, true));
            return false;
        }
        auto* item = new PendingRequest;
        item->kind = kind;
        item->reply = reply;
        item->expectedStatuses = QSet<int>(expectedStatuses.begin(),
                                           expectedStatuses.end());
        item->maximumBytes = maximumBytes;
        item->expectsJson = expectsJson;
        item->handler = std::move(handler);
        item->timeout = new QTimer(reply);
        item->timeout->setSingleShot(true);
        item->timeout->setInterval(timeoutMs);
        pending.insert(requestId, item);
        connect(item->timeout, &QTimer::timeout, q,
                [this, requestId, guard = QPointer<QNetworkReply>(reply)] {
                    if (!guard || !matches(requestId, guard)) return;
                    failPending(
                        requestId,
                        failure(CloudClientErrorCode::Timeout,
                                QStringLiteral("Cloud request timed out"), 0,
                                true));
                });
        connect(reply, &QNetworkReply::readyRead, q,
                [this, requestId, guard = QPointer<QNetworkReply>(reply)] {
                    const auto iterator = pending.constFind(requestId);
                    if (!guard || iterator == pending.constEnd() ||
                        (*iterator)->reply != guard) {
                        return;
                    }
                    if (guard->bytesAvailable() >
                        (*iterator)->maximumBytes) {
                        failPending(
                            requestId,
                            failure(CloudClientErrorCode::ResponseTooLarge,
                                    QStringLiteral("Cloud response is too large")));
                    }
                });
        connect(reply, &QNetworkReply::finished, q,
                [this, requestId, guard = QPointer<QNetworkReply>(reply)] {
                    if (guard) finish(requestId, guard);
                });
        item->timeout->start();
        return true;
    }

    bool matches(quint64 requestId, const QNetworkReply* reply) const {
        const auto iterator = pending.constFind(requestId);
        return iterator != pending.constEnd() && (*iterator)->reply == reply;
    }

    void finish(quint64 requestId, QNetworkReply* reply) {
        const auto iterator = pending.find(requestId);
        if (iterator == pending.end() || (*iterator)->reply != reply) return;
        PendingRequest* item = *iterator;
        pending.erase(iterator);
        if (item->timeout) item->timeout->stop();
        const CloudRequestKind kind = item->kind;
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const bool redirected = status >= 300 && status < 400;
        const bool tooLarge = body.size() > item->maximumBytes;
        const bool statusAccepted = item->expectedStatuses.contains(status);
        const bool expectsJson = item->expectsJson;
        const bool networkFailed =
            reply->error() != QNetworkReply::NoError && status == 0;
        const QByteArray contentType =
            reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        Handler handler = std::move(item->handler);
        delete item;
        reply->deleteLater();

        if (redirected) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::RedirectRejected,
                                QStringLiteral("Cloud redirect was rejected"),
                                status));
            return;
        }
        if (tooLarge) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::ResponseTooLarge,
                                QStringLiteral("Cloud response is too large"),
                                status));
            return;
        }
        if (networkFailed) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::NetworkFailure,
                                QStringLiteral("Cloud request failed"), 0,
                                true));
            return;
        }
        if (!statusAccepted) {
            emitFailure(requestId, kind, parseApiError(body, status));
            return;
        }
        if (status == 204) {
            if (!body.trimmed().isEmpty()) {
                emitFailure(
                    requestId, kind,
                    failure(CloudClientErrorCode::InvalidResponse,
                            QStringLiteral("Unexpected response body"), status));
                return;
            }
        }
        if (expectsJson && status != 204 &&
            !contentType.toLower().startsWith("application/json")) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::InvalidResponse,
                                QStringLiteral("Cloud response is not JSON"),
                                status));
            return;
        }
        if (expectsJson && status != 204 && body.isEmpty()) {
            emitFailure(requestId, kind,
                        failure(CloudClientErrorCode::InvalidResponse,
                                QStringLiteral("Cloud response is empty"),
                                status));
            return;
        }
        if (handler) handler(body);
    }

    void failPending(quint64 requestId, const CloudClientError& error,
                     bool notify = true) {
        const auto iterator = pending.find(requestId);
        if (iterator == pending.end()) return;
        PendingRequest* item = *iterator;
        pending.erase(iterator);
        const CloudRequestKind kind = item->kind;
        QPointer<QNetworkReply> reply = item->reply;
        if (item->timeout) item->timeout->stop();
        delete item;
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
        if (notify) emitFailure(requestId, kind, error);
    }

    void discard(quint64 requestId) {
        failPending(requestId,
                    failure(CloudClientErrorCode::Cancelled,
                            QStringLiteral("Cloud request was cancelled")),
                    false);
    }

    bool cancel(quint64 requestId) {
        if (!pending.contains(requestId)) return false;
        if (requestId == bootstrapRequestId) {
            ++bootstrapGeneration;
            bootstrapRequestId = 0;
        }
        failPending(requestId,
                    failure(CloudClientErrorCode::Cancelled,
                            QStringLiteral("Cloud request was cancelled")));
        return true;
    }

    void cancelAll() {
        ++bootstrapGeneration;
        bootstrapRequestId = 0;
        const QList<quint64> ids = pending.keys();
        for (quint64 id : ids) {
            failPending(id,
                        failure(CloudClientErrorCode::Cancelled,
                                QStringLiteral("Cloud request was cancelled")));
        }
    }

    std::optional<QJsonObject> responseObject(
        quint64 requestId, CloudRequestKind kind, const QByteArray& body) {
        CloudClientError jsonError;
        const auto object = parseJsonObject(body, &jsonError);
        if (!object) {
            emitFailure(requestId, kind, jsonError);
            return std::nullopt;
        }
        return object;
    }

    void invalidResponse(quint64 requestId, CloudRequestKind kind,
                         const ParseFailure& parse,
                         CloudClientErrorCode code =
                             CloudClientErrorCode::InvalidResponse) {
        emitFailure(requestId, kind,
                    failure(code,
                            parse.message.isEmpty()
                                ? QStringLiteral("Invalid cloud response")
                                : parse.message));
    }

    quint64 requestProject(CloudRequestKind kind, const QByteArray& method,
                           const QString& path, const QJsonObject* body,
                           int expectedStatus,
                           const QString& expectedProjectId = {}) {
        const quint64 requestId = allocate();
        const QByteArray encoded = body
            ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
            : QByteArray();
        issue(requestId, kind, method, path, {}, encoded, {expectedStatus},
              kMaxRegularResponseBytes, true,
              [this, requestId, kind, expectedProjectId](
                  const QByteArray& response) {
                  const auto object = responseObject(requestId, kind, response);
                  if (!object) return;
                  ParseFailure parse;
                  auto view = parseProjectView(*object, &parse);
                  if (!view ||
                      (!expectedProjectId.isEmpty() &&
                       view->project.id != expectedProjectId)) {
                      if (parse.message.isEmpty())
                          parse.message =
                              QStringLiteral("Project response mismatch");
                      invalidResponse(requestId, kind, parse);
                      return;
                  }
                  emit q->projectReceived(requestId, kind, *view);
              });
        return requestId;
    }

    quint64 requestVoid(CloudRequestKind kind, const QByteArray& method,
                        const QString& path, const QString& resourceId) {
        const quint64 requestId = allocate();
        issue(requestId, kind, method, path, {}, {}, {204},
              kMaxRegularResponseBytes, false,
              [this, requestId, kind, resourceId](const QByteArray&) {
                  emit q->operationCompleted(requestId, kind, resourceId);
              });
        return requestId;
    }

    quint64 requestSession(CloudRequestKind kind, const QByteArray& method,
                           const QString& path, const QJsonObject* body,
                           int expectedStatus, const QString& projectId,
                           const QString& sessionId = {}) {
        const quint64 requestId = allocate();
        const QByteArray encoded = body
            ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
            : QByteArray();
        issue(requestId, kind, method, path, {}, encoded, {expectedStatus},
              kMaxRegularResponseBytes, true,
              [this, requestId, kind, projectId, sessionId](
                  const QByteArray& response) {
                  const auto object = responseObject(requestId, kind, response);
                  if (!object) return;
                  ParseFailure parse;
                  auto state = parseSessionState(*object, &parse);
                  if (!state || state->session.projectId != projectId ||
                      (!sessionId.isEmpty() &&
                       state->session.id != sessionId)) {
                      if (parse.message.isEmpty())
                          parse.message =
                              QStringLiteral("Session response mismatch");
                      invalidResponse(requestId, kind, parse);
                      return;
                  }
                  emit q->sessionStateReceived(requestId, kind, *state);
              });
        return requestId;
    }

    quint64 requestLease(CloudRequestKind kind, const QByteArray& method,
                         const QString& path, const QJsonObject& body,
                         int expectedStatus, const QString& projectId,
                         const QString& sessionId,
                         const QString& expectedLeaseId = {},
                         const QString& expectedTrackId = {}) {
        const quint64 requestId = allocate();
        issue(
            requestId, kind, method, path, {},
            QJsonDocument(body).toJson(QJsonDocument::Compact),
            {expectedStatus}, kMaxRegularResponseBytes, true,
            [this, requestId, kind, projectId, sessionId, expectedLeaseId,
             expectedTrackId](const QByteArray& response) {
                const auto object = responseObject(requestId, kind, response);
                if (!object) return;
                ParseFailure parse;
                auto lease = parseTrackLease(*object, &parse);
                if (!lease || lease->projectId != projectId ||
                    lease->sessionId != sessionId ||
                    (!expectedLeaseId.isEmpty() &&
                     lease->id != expectedLeaseId) ||
                    (!expectedTrackId.isEmpty() &&
                     lease->trackId != expectedTrackId)) {
                    if (parse.message.isEmpty())
                        parse.message =
                            QStringLiteral("Track lease response mismatch");
                    invalidResponse(requestId, kind, parse);
                    return;
                }
                emit q->leaseReceived(requestId, kind, *lease);
            });
        return requestId;
    }

    void issueBootstrapPage(
        const std::shared_ptr<BootstrapAccumulator>& accumulator) {
        if (!accumulator || accumulator->generation != bootstrapGeneration ||
            accumulator->requestId != bootstrapRequestId) {
            return;
        }
        if (accumulator->elapsed.isValid() &&
            accumulator->elapsed.elapsed() >= timeoutMs) {
            emitFailure(
                accumulator->requestId, CloudRequestKind::BootstrapProject,
                failure(CloudClientErrorCode::Timeout,
                        QStringLiteral("Cloud bootstrap timed out"), 0, true));
            return;
        }
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("after_seq"),
                           QString::number(accumulator->currentAfter));
        query.addQueryItem(QStringLiteral("limit"),
                           QString::number(accumulator->pageLimit));
        const QString path = QStringLiteral("desktop/projects/%1/bootstrap")
                                 .arg(accumulator->projectId);
        issue(accumulator->requestId, CloudRequestKind::BootstrapProject,
              QByteArrayLiteral("GET"), path, query, {}, {200},
              kMaxBootstrapPageBytes, true,
              [this, accumulator](const QByteArray& response) {
                  handleBootstrapPage(accumulator, response);
              });
    }

    void handleBootstrapPage(
        const std::shared_ptr<BootstrapAccumulator>& accumulator,
        const QByteArray& response) {
        if (!accumulator || accumulator->generation != bootstrapGeneration ||
            accumulator->requestId != bootstrapRequestId) {
            return;
        }
        const auto object = responseObject(
            accumulator->requestId, CloudRequestKind::BootstrapProject,
            response);
        if (!object) return;
        ParseFailure parse;
        auto page = parseBootstrapPage(*object, &parse);
        if (!page || page->project.id != accumulator->projectId) {
            if (parse.message.isEmpty())
                parse.message = QStringLiteral("Bootstrap project mismatch");
            invalidResponse(accumulator->requestId,
                            CloudRequestKind::BootstrapProject, parse,
                            CloudClientErrorCode::BootstrapMismatch);
            return;
        }
        if (++accumulator->pageCount > 100'000) {
            invalidResponse(
                accumulator->requestId, CloudRequestKind::BootstrapProject,
                {CloudClientErrorCode::ResponseTooLarge,
                 QStringLiteral("Bootstrap has too many pages")},
                CloudClientErrorCode::ResponseTooLarge);
            return;
        }

        if (!accumulator->initialized) {
            if (accumulator->requestedAfter > page->headSequence) {
                invalidResponse(
                    accumulator->requestId,
                    CloudRequestKind::BootstrapProject,
                    {CloudClientErrorCode::BootstrapMismatch,
                     QStringLiteral("Bootstrap starts ahead of project head")},
                    CloudClientErrorCode::BootstrapMismatch);
                return;
            }
            accumulator->initialized = true;
            accumulator->result.project = page->project;
            accumulator->result.role = page->role;
            accumulator->result.snapshot = page->snapshot;
            accumulator->result.fieldHeads = page->fieldHeads;
            accumulator->result.requestedAfterSequence =
                accumulator->requestedAfter;
            accumulator->result.replayBaseSequence = std::max(
                accumulator->requestedAfter,
                page->snapshot ? page->snapshot->sequence : quint64(0));
            accumulator->result.headSequence = page->headSequence;
        } else if (!projectsEqual(accumulator->result.project,
                                  page->project) ||
                   accumulator->result.role != page->role ||
                   !snapshotsEqual(accumulator->result.snapshot,
                                   page->snapshot) ||
                   !fieldHeadsEqual(accumulator->result.fieldHeads,
                                    page->fieldHeads) ||
                   accumulator->result.headSequence != page->headSequence) {
            invalidResponse(
                accumulator->requestId, CloudRequestKind::BootstrapProject,
                {CloudClientErrorCode::BootstrapMismatch,
                 QStringLiteral("Bootstrap changed between pages")},
                CloudClientErrorCode::BootstrapMismatch);
            return;
        }

        quint64 expected = accumulator->result.operations.isEmpty()
            ? accumulator->result.replayBaseSequence + 1
            : accumulator->result.operations.back().serverSequence + 1;
        for (CloudProjectOperation& operation : page->operations) {
            const QString operationId = QString::fromStdString(
                operation.command.meta.operationId);
            if (accumulator->operationIds.contains(operationId)) {
                invalidResponse(
                    accumulator->requestId,
                    CloudRequestKind::BootstrapProject,
                    {CloudClientErrorCode::BootstrapMismatch,
                     QStringLiteral("Bootstrap repeated an operation id")},
                    CloudClientErrorCode::BootstrapMismatch);
                return;
            }
            if (operation.serverSequence != expected) {
                invalidResponse(
                    accumulator->requestId,
                    CloudRequestKind::BootstrapProject,
                    {CloudClientErrorCode::BootstrapGap,
                     operation.serverSequence < expected
                         ? QStringLiteral("Bootstrap repeated a sequence")
                         : QStringLiteral("Bootstrap operation sequence gap")},
                    CloudClientErrorCode::BootstrapGap);
                return;
            }
            accumulator->operationIds.insert(operationId);
            accumulator->result.operations.push_back(std::move(operation));
            ++expected;
            if (accumulator->result.operations.size() >
                kMaximumBootstrapOperations) {
                invalidResponse(
                    accumulator->requestId,
                    CloudRequestKind::BootstrapProject,
                    {CloudClientErrorCode::ResponseTooLarge,
                     QStringLiteral("Bootstrap contains too many operations")},
                    CloudClientErrorCode::ResponseTooLarge);
                return;
            }
        }

        const quint64 pageEnd = page->operations.isEmpty()
            ? std::max(accumulator->currentAfter,
                       page->snapshot ? page->snapshot->sequence : quint64(0))
            : accumulator->result.operations.back().serverSequence;
        if (page->nextAfterSequence != pageEnd ||
            (page->hasMore &&
             (page->operations.isEmpty() ||
              page->nextAfterSequence <= accumulator->currentAfter ||
              page->nextAfterSequence >= page->headSequence)) ||
            (!page->hasMore &&
             page->nextAfterSequence != page->headSequence)) {
            invalidResponse(
                accumulator->requestId, CloudRequestKind::BootstrapProject,
                {CloudClientErrorCode::BootstrapMismatch,
                 QStringLiteral("Bootstrap pagination metadata mismatch")},
                CloudClientErrorCode::BootstrapMismatch);
            return;
        }

        if (page->hasMore) {
            accumulator->currentAfter = page->nextAfterSequence;
            issueBootstrapPage(accumulator);
            return;
        }
        if (accumulator->generation != bootstrapGeneration ||
            accumulator->requestId != bootstrapRequestId) {
            return;
        }
        bootstrapRequestId = 0;
        emit q->bootstrapCompleted(accumulator->requestId,
                                   accumulator->result);
    }
};

CloudProjectClient::CloudProjectClient(account::Service* account,
                                       QNetworkAccessManager* network,
                                       QObject* parent)
    : CloudProjectClient(
          [guard = QPointer<account::Service>(account)] {
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
          network, parent) {
    if (!account) return;
    connect(account, &account::Service::authenticatedChanged, this,
            [this](bool authenticated) {
                if (!authenticated) cancelAll();
            });
    connect(account, &account::Service::snapshotChanged, this,
            [this, guard = QPointer<account::Service>(account)] {
                if (!guard || !guard->authenticated() ||
                    guard->snapshot().offline) {
                    cancelAll();
                }
            });
}

CloudProjectClient::CloudProjectClient(CredentialProvider credentials,
                                       QNetworkAccessManager* network,
                                       QObject* parent)
    : QObject(parent) {
    QNetworkAccessManager* manager = network;
    if (!manager) manager = new QNetworkAccessManager(this);
    m_impl = std::make_unique<Impl>(this, std::move(credentials), manager);
    qRegisterMetaType<CloudRequestKind>();
    qRegisterMetaType<CloudClientError>();
    qRegisterMetaType<CloudProjectView>();
    qRegisterMetaType<CloudProjectBootstrap>();
    qRegisterMetaType<CloudOperationLookup>();
    qRegisterMetaType<CloudProjectMember>();
    qRegisterMetaType<CloudProjectInvite>();
    qRegisterMetaType<CreatedCloudProjectInvite>();
    qRegisterMetaType<CloudOwnershipTransfer>();
    qRegisterMetaType<CloudSessionState>();
    qRegisterMetaType<CloudProjectTrackLease>();
}

CloudProjectClient::~CloudProjectClient() = default;

int CloudProjectClient::requestTimeoutMs() const {
    return m_impl ? m_impl->timeoutMs : kDefaultTimeoutMs;
}

void CloudProjectClient::setRequestTimeoutMs(int timeoutMs) {
    if (!m_impl) return;
    m_impl->timeoutMs =
        std::clamp(timeoutMs, kMinimumTimeoutMs, kMaximumTimeoutMs);
}

QString CloudProjectClient::currentUserId() const {
    if (!m_impl || !m_impl->credentialProvider) return {};
    const Credentials credentials = m_impl->credentialProvider();
    const auto normalized = m_impl->uuidInput(credentials.userId);
    return normalized ? *normalized : QString();
}

quint64 CloudProjectClient::createProject(
    const CreateCloudProjectInput& input) {
    const QString title = input.title.trimmed();
    const QString engineVersion = input.engineVersion.trimmed();
    const QString minimumVersion = input.minimumAppVersion.trimmed();
    if (input.formatVersion != daw::ProjectSerializer::kFormatVersion ||
        title.isEmpty() || title.size() > 160 ||
        engineVersion.isEmpty() || engineVersion.size() > 64 ||
        minimumVersion.isEmpty() || minimumVersion.size() > 64) {
        return m_impl->invalid(CloudRequestKind::CreateProject,
                               QStringLiteral("Invalid cloud project metadata"));
    }
    const QJsonObject body{
        {QStringLiteral("title"), title},
        {QStringLiteral("format_version"), input.formatVersion},
        {QStringLiteral("engine_version"), engineVersion},
        {QStringLiteral("minimum_app_version"), minimumVersion},
    };
    return m_impl->requestProject(
        CloudRequestKind::CreateProject, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects"), &body, 201);
}

quint64 CloudProjectClient::listProjects() {
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::ListProjects, QByteArrayLiteral("GET"),
        QStringLiteral("desktop/projects"), {}, {}, {200},
        kMaxRegularResponseBytes, true,
        [this, requestId](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::ListProjects, response);
            if (!object) return;
            if (!exactKeys(*object, {"projects"}) ||
                !object->value(QStringLiteral("projects")).isArray()) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::ListProjects,
                    {CloudClientErrorCode::InvalidResponse,
                     QStringLiteral("Invalid project list shape")});
                return;
            }
            const QJsonArray values =
                object->value(QStringLiteral("projects")).toArray();
            if (values.size() > kMaximumProjects) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::ListProjects,
                    {CloudClientErrorCode::ResponseTooLarge,
                     QStringLiteral("Too many cloud projects")},
                    CloudClientErrorCode::ResponseTooLarge);
                return;
            }
            QVector<CloudProjectView> projects;
            QSet<QString> ids;
            projects.reserve(values.size());
            ParseFailure parse;
            for (const QJsonValue& value : values) {
                if (!value.isObject()) {
                    parse.message = QStringLiteral("Invalid project list item");
                    break;
                }
                auto project = parseProjectView(value.toObject(), &parse);
                if (!project || ids.contains(project->project.id)) {
                    if (parse.message.isEmpty())
                        parse.message = QStringLiteral("Duplicate cloud project");
                    break;
                }
                ids.insert(project->project.id);
                projects.push_back(std::move(*project));
            }
            if (!parse.message.isEmpty()) {
                m_impl->invalidResponse(requestId,
                                        CloudRequestKind::ListProjects, parse);
                return;
            }
            emit projectsListed(requestId, projects);
        });
    return requestId;
}

quint64 CloudProjectClient::getProject(const QString& projectId) {
    const auto id = m_impl->uuidInput(projectId);
    if (!id)
        return m_impl->invalid(CloudRequestKind::GetProject,
                               QStringLiteral("Invalid project id"));
    return m_impl->requestProject(
        CloudRequestKind::GetProject, QByteArrayLiteral("GET"),
        QStringLiteral("desktop/projects/%1").arg(*id), nullptr, 200, *id);
}

quint64 CloudProjectClient::archiveProject(const QString& projectId) {
    const auto id = m_impl->uuidInput(projectId);
    if (!id)
        return m_impl->invalid(CloudRequestKind::ArchiveProject,
                               QStringLiteral("Invalid project id"));
    return m_impl->requestVoid(
        CloudRequestKind::ArchiveProject, QByteArrayLiteral("DELETE"),
        QStringLiteral("desktop/projects/%1").arg(*id), *id);
}

quint64 CloudProjectClient::publishProject(const QString& projectId) {
    const auto id = m_impl->uuidInput(projectId);
    if (!id)
        return m_impl->invalid(CloudRequestKind::PublishProject,
                               QStringLiteral("Invalid project id"));
    return m_impl->requestProject(
        CloudRequestKind::PublishProject, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/publish").arg(*id), nullptr, 200,
        *id);
}

quint64 CloudProjectClient::bootstrapProject(const QString& projectId,
                                             quint64 afterSequence,
                                             int pageLimit) {
    const auto id = m_impl->uuidInput(projectId);
    if (!id || afterSequence > quint64(kLargestExactJsonInteger) ||
        pageLimit < 1 || pageLimit > 2000) {
        return m_impl->invalid(CloudRequestKind::BootstrapProject,
                               QStringLiteral("Invalid bootstrap request"));
    }
    if (m_impl->bootstrapRequestId != 0)
        m_impl->discard(m_impl->bootstrapRequestId);
    ++m_impl->bootstrapGeneration;
    const quint64 requestId = m_impl->allocate();
    m_impl->bootstrapRequestId = requestId;
    auto accumulator = std::make_shared<Impl::BootstrapAccumulator>();
    accumulator->requestId = requestId;
    accumulator->generation = m_impl->bootstrapGeneration;
    accumulator->projectId = *id;
    accumulator->requestedAfter = afterSequence;
    accumulator->currentAfter = afterSequence;
    accumulator->pageLimit = pageLimit;
    accumulator->elapsed.start();
    m_impl->issueBootstrapPage(accumulator);
    return requestId;
}

quint64 CloudProjectClient::lookupOperation(
    const QString& projectId, const QString& operationId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto operation = m_impl->uuidInput(operationId);
    if (!project || !operation) {
        return m_impl->invalid(CloudRequestKind::LookupOperation,
                               QStringLiteral("Invalid operation lookup"));
    }

    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::LookupOperation,
        QByteArrayLiteral("GET"),
        QStringLiteral("desktop/projects/%1/ops/%2")
            .arg(*project, *operation),
        {}, {}, {200}, kMaxRegularResponseBytes, true,
        [this, requestId, projectId = *project,
         operationId = *operation](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::LookupOperation, response);
            if (!object) return;
            const auto head = exactUnsigned(
                object->value(QStringLiteral("head_seq")));
            if (!exactKeys(*object, {"found", "head_seq", "operation"}) ||
                !object->value(QStringLiteral("found")).isBool() || !head) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::LookupOperation,
                    {CloudClientErrorCode::InvalidResponse,
                     QStringLiteral("Invalid operation lookup shape")});
                return;
            }

            CloudOperationLookup lookup;
            lookup.projectId = projectId;
            lookup.operationId = operationId;
            lookup.headSequence = *head;
            const bool found =
                object->value(QStringLiteral("found")).toBool();
            const QJsonValue operationValue =
                object->value(QStringLiteral("operation"));
            ParseFailure parse;
            if (found && operationValue.isObject()) {
                auto parsed = parseOperation(operationValue.toObject(), &parse);
                const QString parsedId = parsed
                    ? QString::fromStdString(
                          parsed->command.meta.operationId)
                    : QString();
                if (!parsed || parsed->projectId != projectId ||
                    parsedId != operationId ||
                    parsed->serverSequence > lookup.headSequence) {
                    if (parse.message.isEmpty()) {
                        parse.message = QStringLiteral(
                            "Operation lookup does not match request");
                    }
                    m_impl->invalidResponse(
                        requestId, CloudRequestKind::LookupOperation, parse);
                    return;
                }
                lookup.operation = std::move(*parsed);
            } else if (found || !operationValue.isNull()) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::LookupOperation,
                    {CloudClientErrorCode::InvalidResponse,
                     QStringLiteral("Operation lookup presence mismatch")});
                return;
            }
            emit operationLookupReceived(requestId, lookup);
        });
    return requestId;
}

quint64 CloudProjectClient::getActiveSession(const QString& projectId) {
    const auto id = m_impl->uuidInput(projectId);
    if (!id)
        return m_impl->invalid(CloudRequestKind::GetActiveSession,
                               QStringLiteral("Invalid project id"));
    return m_impl->requestSession(
        CloudRequestKind::GetActiveSession, QByteArrayLiteral("GET"),
        QStringLiteral("desktop/projects/%1/sessions/active").arg(*id),
        nullptr, 200, *id);
}

quint64 CloudProjectClient::startSession(const QString& projectId,
                                         CloudSessionMode mode) {
    const auto id = m_impl->uuidInput(projectId);
    const QString version = QCoreApplication::applicationVersion().trimmed();
    if (!id || !validApplicationVersion(version))
        return m_impl->invalid(CloudRequestKind::StartSession,
                               QStringLiteral("Invalid session compatibility"));
    const QJsonObject body{
        {QStringLiteral("mode"), sessionModeName(mode)},
        {QStringLiteral("appVersion"), version},
        {QStringLiteral("engineVersion"), version},
        {QStringLiteral("commandSchemaVersion"),
         int(daw::collab::kProjectCommandSchemaVersion)},
        {QStringLiteral("projectFormatVersion"),
         daw::ProjectSerializer::kFormatVersion},
    };
    return m_impl->requestSession(
        CloudRequestKind::StartSession, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/sessions").arg(*id), &body, 201,
        *id);
}

quint64 CloudProjectClient::joinSession(const QString& projectId,
                                        const QString& sessionId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    const QString version = QCoreApplication::applicationVersion().trimmed();
    if (!project || !session || !validApplicationVersion(version))
        return m_impl->invalid(CloudRequestKind::JoinSession,
                               QStringLiteral("Invalid session compatibility"));
    const QJsonObject body{
        {QStringLiteral("appVersion"), version},
        {QStringLiteral("engineVersion"), version},
        {QStringLiteral("commandSchemaVersion"),
         int(daw::collab::kProjectCommandSchemaVersion)},
        {QStringLiteral("projectFormatVersion"),
         daw::ProjectSerializer::kFormatVersion},
    };
    return m_impl->requestSession(
        CloudRequestKind::JoinSession, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/sessions/%2/join")
            .arg(*project, *session),
        &body, 200, *project, *session);
}

quint64 CloudProjectClient::leaveSession(const QString& projectId,
                                         const QString& sessionId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    if (!project || !session)
        return m_impl->invalid(CloudRequestKind::LeaveSession,
                               QStringLiteral("Invalid session id"));
    return m_impl->requestSession(
        CloudRequestKind::LeaveSession, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/sessions/%2/leave")
            .arg(*project, *session),
        nullptr, 200, *project, *session);
}

quint64 CloudProjectClient::endSession(const QString& projectId,
                                       const QString& sessionId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    if (!project || !session)
        return m_impl->invalid(CloudRequestKind::EndSession,
                               QStringLiteral("Invalid session id"));
    return m_impl->requestVoid(
        CloudRequestKind::EndSession, QByteArrayLiteral("DELETE"),
        QStringLiteral("desktop/projects/%1/sessions/%2")
            .arg(*project, *session),
        *session);
}

quint64 CloudProjectClient::handoffSession(
    const QString& projectId, const QString& sessionId,
    const QString& targetMemberId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    const auto member = m_impl->uuidInput(targetMemberId);
    if (!project || !session || !member)
        return m_impl->invalid(CloudRequestKind::HandoffSession,
                               QStringLiteral("Invalid host handoff"));
    const QJsonObject body{
        {QStringLiteral("target_member_id"), *member},
    };
    return m_impl->requestSession(
        CloudRequestKind::HandoffSession, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/sessions/%2/host")
            .arg(*project, *session),
        &body, 200, *project, *session);
}

quint64 CloudProjectClient::acquireRecordingLease(
    const QString& projectId, const QString& sessionId,
    const QString& trackId, int ttlSeconds) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    const auto track = m_impl->uuidInput(trackId);
    if (!project || !session || !track ||
        (ttlSeconds != 0 &&
         (ttlSeconds < kMinimumRecordingLeaseTtlSeconds ||
          ttlSeconds > kMaximumRecordingLeaseTtlSeconds))) {
        return m_impl->invalid(CloudRequestKind::AcquireRecordingLease,
                               QStringLiteral("Invalid recording lease request"));
    }

    QJsonObject body{{QStringLiteral("track_id"), *track}};
    if (ttlSeconds != 0)
        body.insert(QStringLiteral("ttl_seconds"), ttlSeconds);
    return m_impl->requestLease(
        CloudRequestKind::AcquireRecordingLease, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/sessions/%2/leases")
            .arg(*project, *session),
        body, 201, *project, *session, {}, *track);
}

quint64 CloudProjectClient::renewRecordingLease(
    const QString& projectId, const QString& sessionId,
    const QString& leaseId, int ttlSeconds) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    const auto lease = m_impl->uuidInput(leaseId);
    if (!project || !session || !lease ||
        (ttlSeconds != 0 &&
         (ttlSeconds < kMinimumRecordingLeaseTtlSeconds ||
          ttlSeconds > kMaximumRecordingLeaseTtlSeconds))) {
        return m_impl->invalid(CloudRequestKind::RenewRecordingLease,
                               QStringLiteral("Invalid recording lease renewal"));
    }

    QJsonObject body;
    if (ttlSeconds != 0)
        body.insert(QStringLiteral("ttl_seconds"), ttlSeconds);
    return m_impl->requestLease(
        CloudRequestKind::RenewRecordingLease, QByteArrayLiteral("PATCH"),
        QStringLiteral("desktop/projects/%1/sessions/%2/leases/%3")
            .arg(*project, *session, *lease),
        body, 200, *project, *session, *lease);
}

quint64 CloudProjectClient::releaseRecordingLease(
    const QString& projectId, const QString& sessionId,
    const QString& leaseId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto session = m_impl->uuidInput(sessionId);
    const auto lease = m_impl->uuidInput(leaseId);
    if (!project || !session || !lease)
        return m_impl->invalid(CloudRequestKind::ReleaseRecordingLease,
                               QStringLiteral("Invalid recording lease release"));
    return m_impl->requestVoid(
        CloudRequestKind::ReleaseRecordingLease, QByteArrayLiteral("DELETE"),
        QStringLiteral("desktop/projects/%1/sessions/%2/leases/%3")
            .arg(*project, *session, *lease),
        *lease);
}

quint64 CloudProjectClient::listMembers(const QString& projectId) {
    const auto project = m_impl->uuidInput(projectId);
    if (!project)
        return m_impl->invalid(CloudRequestKind::ListMembers,
                               QStringLiteral("Invalid project id"));
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::ListMembers, QByteArrayLiteral("GET"),
        QStringLiteral("desktop/projects/%1/members").arg(*project), {}, {},
        {200}, kMaxRegularResponseBytes, true,
        [this, requestId, projectId = *project](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::ListMembers, response);
            if (!object) return;
            if (!exactKeys(*object, {"members"}) ||
                !object->value(QStringLiteral("members")).isArray()) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::ListMembers,
                    {CloudClientErrorCode::InvalidResponse,
                     QStringLiteral("Invalid member list shape")});
                return;
            }
            const QJsonArray values =
                object->value(QStringLiteral("members")).toArray();
            if (values.size() > kMaximumMembers) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::ListMembers,
                    {CloudClientErrorCode::ResponseTooLarge,
                     QStringLiteral("Too many project members")},
                    CloudClientErrorCode::ResponseTooLarge);
                return;
            }
            QVector<CloudProjectMember> members;
            QSet<QString> users;
            ParseFailure parse;
            members.reserve(values.size());
            for (const QJsonValue& value : values) {
                if (!value.isObject()) {
                    parse.message = QStringLiteral("Invalid member list item");
                    break;
                }
                auto member = parseMember(value.toObject(), &parse);
                if (!member || member->projectId != projectId ||
                    users.contains(member->userId)) {
                    if (parse.message.isEmpty())
                        parse.message = QStringLiteral("Member list mismatch");
                    break;
                }
                users.insert(member->userId);
                members.push_back(std::move(*member));
            }
            if (!parse.message.isEmpty()) {
                m_impl->invalidResponse(requestId,
                                        CloudRequestKind::ListMembers, parse);
                return;
            }
            emit membersListed(requestId, members);
        });
    return requestId;
}

quint64 CloudProjectClient::putMember(
    const QString& projectId, const QString& userId,
    const PutCloudProjectMemberInput& input) {
    const auto project = m_impl->uuidInput(projectId);
    const auto user = m_impl->uuidInput(userId);
    if (!project || !user || input.colorIndex < 0 || input.colorIndex > 31)
        return m_impl->invalid(CloudRequestKind::PutMember,
                               QStringLiteral("Invalid member update"));
    const QJsonObject body{
        {QStringLiteral("role"), memberRoleName(input.role)},
        {QStringLiteral("color_index"), input.colorIndex},
    };
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::PutMember, QByteArrayLiteral("PUT"),
        QStringLiteral("desktop/projects/%1/members/%2").arg(*project, *user),
        {}, QJsonDocument(body).toJson(QJsonDocument::Compact), {200},
        kMaxRegularResponseBytes, true,
        [this, requestId, projectId = *project,
         userId = *user](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::PutMember, response);
            if (!object) return;
            ParseFailure parse;
            auto member = parseMember(*object, &parse);
            if (!member || member->projectId != projectId ||
                member->userId != userId) {
                if (parse.message.isEmpty())
                    parse.message = QStringLiteral("Member response mismatch");
                m_impl->invalidResponse(requestId,
                                        CloudRequestKind::PutMember, parse);
                return;
            }
            emit memberSaved(requestId, *member);
        });
    return requestId;
}

quint64 CloudProjectClient::removeMember(const QString& projectId,
                                         const QString& userId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto user = m_impl->uuidInput(userId);
    if (!project || !user)
        return m_impl->invalid(CloudRequestKind::RemoveMember,
                               QStringLiteral("Invalid member id"));
    return m_impl->requestVoid(
        CloudRequestKind::RemoveMember, QByteArrayLiteral("DELETE"),
        QStringLiteral("desktop/projects/%1/members/%2").arg(*project, *user),
        *user);
}

quint64 CloudProjectClient::transferOwnership(
    const QString& projectId, const QString& targetUserId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto target = m_impl->uuidInput(targetUserId);
    if (!project || !target || *target == currentUserId()) {
        return m_impl->invalid(CloudRequestKind::TransferOwnership,
                               QStringLiteral("Invalid ownership target"));
    }
    const QJsonObject body{
        {QStringLiteral("targetUserId"), *target},
    };
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::TransferOwnership,
        QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/ownership").arg(*project), {},
        QJsonDocument(body).toJson(QJsonDocument::Compact), {200},
        kMaxRegularResponseBytes, true,
        [this, requestId, projectId = *project,
         targetUserId = *target](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::TransferOwnership, response);
            if (!object ||
                !exactKeys(*object,
                           {"project", "previousOwnerUserId",
                            "newOwnerUserId"}) ||
                !object->value(QStringLiteral("project")).isObject()) {
                if (object) {
                    m_impl->invalidResponse(
                        requestId, CloudRequestKind::TransferOwnership,
                        {CloudClientErrorCode::InvalidResponse,
                         QStringLiteral("Invalid ownership response")});
                }
                return;
            }
            ParseFailure parse;
            auto parsedProject = parseProject(
                object->value(QStringLiteral("project")).toObject(), &parse);
            QString previousOwner;
            QString newOwner;
            if (!parsedProject || parsedProject->id != projectId ||
                !normalizedUuid(object->value(
                                    QStringLiteral("previousOwnerUserId")),
                                &previousOwner) ||
                !normalizedUuid(
                    object->value(QStringLiteral("newOwnerUserId")),
                    &newOwner) ||
                newOwner != targetUserId ||
                parsedProject->ownerUserId != newOwner) {
                if (parse.message.isEmpty()) {
                    parse.message =
                        QStringLiteral("Ownership response mismatch");
                }
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::TransferOwnership, parse);
                return;
            }
            emit ownershipTransferred(
                requestId,
                CloudOwnershipTransfer{*parsedProject, previousOwner,
                                       newOwner});
        });
    return requestId;
}

quint64 CloudProjectClient::listInvites(const QString& projectId) {
    const auto project = m_impl->uuidInput(projectId);
    if (!project) {
        return m_impl->invalid(CloudRequestKind::ListInvites,
                               QStringLiteral("Invalid project id"));
    }
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::ListInvites, QByteArrayLiteral("GET"),
        QStringLiteral("desktop/projects/%1/invites").arg(*project), {}, {},
        {200}, kMaxRegularResponseBytes, true,
        [this, requestId, projectId = *project](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::ListInvites, response);
            if (!object || !exactKeys(*object, {"invites"}) ||
                !object->value(QStringLiteral("invites")).isArray()) {
                if (object) {
                    m_impl->invalidResponse(
                        requestId, CloudRequestKind::ListInvites,
                        {CloudClientErrorCode::InvalidResponse,
                         QStringLiteral("Invalid invitation list")});
                }
                return;
            }
            const QJsonArray array =
                object->value(QStringLiteral("invites")).toArray();
            if (array.size() > 100) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::ListInvites,
                    {CloudClientErrorCode::ResponseTooLarge,
                     QStringLiteral("Invitation list is too large")});
                return;
            }
            QVector<CloudProjectInvite> invites;
            invites.reserve(array.size());
            for (const QJsonValue& value : array) {
                ParseFailure parse;
                auto invite = value.isObject()
                    ? parseInvite(value.toObject(), &parse)
                    : std::optional<CloudProjectInvite>{};
                if (!invite || invite->projectId != projectId) {
                    if (parse.message.isEmpty())
                        parse.message =
                            QStringLiteral("Invitation list mismatch");
                    m_impl->invalidResponse(
                        requestId, CloudRequestKind::ListInvites, parse);
                    return;
                }
                invites.push_back(std::move(*invite));
            }
            emit invitesListed(requestId, invites);
        });
    return requestId;
}

quint64 CloudProjectClient::createInvite(
    const QString& projectId,
    const CreateCloudProjectInviteInput& input) {
    const auto project = m_impl->uuidInput(projectId);
    static const QRegularExpression emailPattern(
        QStringLiteral("^[^@\\s]+@[^@\\s]+$"));
    const QString email = input.targetEmail.trimmed();
    if (!project || input.expiresInSeconds < 3600 ||
        input.expiresInSeconds > 2'592'000 || email.size() > 320 ||
        (!email.isEmpty() && !emailPattern.match(email).hasMatch())) {
        return m_impl->invalid(CloudRequestKind::CreateInvite,
                               QStringLiteral("Invalid project invitation"));
    }
    QJsonObject body{
        {QStringLiteral("role"), memberRoleName(input.role)},
        {QStringLiteral("expiresInSeconds"),
         double(input.expiresInSeconds)},
    };
    if (!email.isEmpty()) body.insert(QStringLiteral("targetEmail"), email);
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::CreateInvite, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/projects/%1/invites").arg(*project), {},
        QJsonDocument(body).toJson(QJsonDocument::Compact), {201},
        kMaxRegularResponseBytes, true,
        [this, requestId, projectId = *project](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::CreateInvite, response);
            if (!object) return;
            if (!exactKeys(*object, {"invite", "token"}) ||
                !object->value(QStringLiteral("invite")).isObject()) {
                m_impl->invalidResponse(
                    requestId, CloudRequestKind::CreateInvite,
                    {CloudClientErrorCode::InvalidResponse,
                     QStringLiteral("Invalid invitation response")});
                return;
            }
            ParseFailure parse;
            auto invite = parseInvite(
                object->value(QStringLiteral("invite")).toObject(), &parse);
            QString token;
            if (!invite || invite->projectId != projectId ||
                !boundedString(object->value(QStringLiteral("token")), 32, 256,
                               &token) ||
                token.contains(QLatin1Char('\r')) ||
                token.contains(QLatin1Char('\n'))) {
                if (parse.message.isEmpty())
                    parse.message = QStringLiteral("Invitation response mismatch");
                m_impl->invalidResponse(requestId,
                                        CloudRequestKind::CreateInvite, parse);
                return;
            }
            emit inviteCreated(requestId,
                               CreatedCloudProjectInvite{*invite, token});
        });
    return requestId;
}

quint64 CloudProjectClient::revokeInvite(const QString& projectId,
                                         const QString& inviteId) {
    const auto project = m_impl->uuidInput(projectId);
    const auto invite = m_impl->uuidInput(inviteId);
    if (!project || !invite)
        return m_impl->invalid(CloudRequestKind::RevokeInvite,
                               QStringLiteral("Invalid invitation id"));
    return m_impl->requestVoid(
        CloudRequestKind::RevokeInvite, QByteArrayLiteral("DELETE"),
        QStringLiteral("desktop/projects/%1/invites/%2")
            .arg(*project, *invite),
        *invite);
}

quint64 CloudProjectClient::acceptInvite(const QString& oneTimeToken) {
    if (oneTimeToken.size() < 32 || oneTimeToken.size() > 256 ||
        oneTimeToken.contains(QLatin1Char('\r')) ||
        oneTimeToken.contains(QLatin1Char('\n'))) {
        return m_impl->invalid(CloudRequestKind::AcceptInvite,
                               QStringLiteral("Invalid invitation token"));
    }
    const QJsonObject body{{QStringLiteral("token"), oneTimeToken}};
    const quint64 requestId = m_impl->allocate();
    m_impl->issue(
        requestId, CloudRequestKind::AcceptInvite, QByteArrayLiteral("POST"),
        QStringLiteral("desktop/project-invites/accept"), {},
        QJsonDocument(body).toJson(QJsonDocument::Compact), {200},
        kMaxRegularResponseBytes, true,
        [this, requestId](const QByteArray& response) {
            const auto object = m_impl->responseObject(
                requestId, CloudRequestKind::AcceptInvite, response);
            if (!object) return;
            ParseFailure parse;
            auto view = parseProjectView(*object, &parse);
            if (!view) {
                m_impl->invalidResponse(requestId,
                                        CloudRequestKind::AcceptInvite, parse);
                return;
            }
            emit inviteAccepted(requestId, *view);
        });
    return requestId;
}

bool CloudProjectClient::cancel(quint64 requestId) {
    return m_impl && m_impl->cancel(requestId);
}

void CloudProjectClient::cancelAll() {
    if (m_impl) m_impl->cancelAll();
}

} // namespace collab

namespace collab {
namespace {

class FakeCloudReply final : public QNetworkReply {
public:
    FakeCloudReply(const QNetworkRequest& request, int status,
                   QByteArray body, QByteArray contentType,
                   const QUrl& redirectTarget, QObject* parent)
        : QNetworkReply(parent), m_body(std::move(body)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (!redirectTarget.isEmpty()) {
            setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                         redirectTarget);
        }
        if (!contentType.isEmpty())
            setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void abort() override {
        if (m_completed) return;
        m_completed = true;
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
        if (m_completed) return;
        m_completed = true;
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
    bool m_completed = false;
};

class FakeCloudNetwork final : public QNetworkAccessManager {
public:
    struct Script {
        int status = 200;
        QByteArray body;
        QByteArray contentType = QByteArrayLiteral("application/json");
        QUrl redirectTarget;
        bool deferred = false;
    };
    struct Captured {
        QNetworkRequest request;
        QByteArray method;
        QByteArray body;
    };

    QVector<Script> scripts;
    QVector<Captured> captured;
    QVector<QPointer<FakeCloudReply>> deferred;

protected:
    QNetworkReply* createRequest(Operation operation,
                                 const QNetworkRequest& request,
                                 QIODevice* outgoingData) override {
        QByteArray method =
            request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        if (method.isEmpty()) {
            switch (operation) {
                case GetOperation: method = QByteArrayLiteral("GET"); break;
                case PostOperation: method = QByteArrayLiteral("POST"); break;
                case PutOperation: method = QByteArrayLiteral("PUT"); break;
                case DeleteOperation: method = QByteArrayLiteral("DELETE"); break;
                default: method = QByteArrayLiteral("CUSTOM"); break;
            }
        }
        QByteArray body;
        if (outgoingData) body = outgoingData->readAll();
        captured.push_back({request, method, body});
        Script script;
        if (!scripts.isEmpty()) script = scripts.takeFirst();
        else {
            script.status = 500;
            script.body = QByteArrayLiteral(
                R"({"code":"missing_fixture","message":"Missing fixture","request_id":"test"})");
        }
        auto* reply = new FakeCloudReply(
            request, script.status, std::move(script.body),
            std::move(script.contentType), script.redirectTarget, this);
        if (script.deferred) {
            deferred.push_back(reply);
        } else {
            QTimer::singleShot(0, reply, [reply] { reply->complete(); });
        }
        return reply;
    }
};

QJsonObject testProject(const QString& projectId, quint64 head,
                        quint64 snapshot) {
    return {
        {QStringLiteral("id"), projectId},
        {QStringLiteral("owner_user_id"),
         QStringLiteral("11111111-1111-4111-8111-111111111111")},
        {QStringLiteral("title"), QStringLiteral("Cloud Test")},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("format_version"),
         daw::ProjectSerializer::kFormatVersion},
        {QStringLiteral("engine_version"), QStringLiteral("0.1.2")},
        {QStringLiteral("minimum_app_version"), QStringLiteral("0.1.2")},
        {QStringLiteral("head_seq"), double(head)},
        {QStringLiteral("snapshot_seq"), double(snapshot)},
        {QStringLiteral("created_at"),
         QStringLiteral("2026-08-29T10:00:00Z")},
        {QStringLiteral("updated_at"),
         QStringLiteral("2026-08-29T10:01:00Z")},
        {QStringLiteral("archived_at"), QJsonValue::Null},
    };
}

QJsonObject testSnapshot(const QString& projectId, quint64 sequence) {
    return {
        {QStringLiteral("id"),
         QStringLiteral("22222222-2222-4222-8222-222222222222")},
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("seq"), double(sequence)},
        {QStringLiteral("blob_id"),
         QStringLiteral("33333333-3333-4333-8333-333333333333")},
        {QStringLiteral("schema_version"),
         daw::ProjectSerializer::kFormatVersion},
        {QStringLiteral("asset_ids"), QJsonArray{}},
        {QStringLiteral("created_by"),
         QStringLiteral("11111111-1111-4111-8111-111111111111")},
        {QStringLiteral("created_at"),
         QStringLiteral("2026-08-29T10:00:30Z")},
    };
}

QJsonObject testTrackLease(const QString& projectId,
                           const QString& sessionId,
                           const QString& leaseId,
                           const QString& trackId) {
    return {
        {QStringLiteral("id"), leaseId},
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("track_id"), trackId},
        {QStringLiteral("lease_kind"), QStringLiteral("record")},
        {QStringLiteral("holder_member_id"),
         QStringLiteral("99999999-9999-4999-8999-999999999999")},
        {QStringLiteral("acquired_at"),
         QStringLiteral("2026-08-29T10:00:00Z")},
        {QStringLiteral("renewed_at"),
         QStringLiteral("2026-08-29T10:00:00Z")},
        {QStringLiteral("expires_at"),
         QStringLiteral("2026-08-29T10:00:31Z")},
    };
}

QJsonObject testOperation(const QString& projectId, quint64 sequence,
                          const QString& operationId, double tempo) {
    return {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("serverSeq"), double(sequence)},
        {QStringLiteral("opId"), operationId},
        {QStringLiteral("actor_user_id"),
         QStringLiteral("11111111-1111-4111-8111-111111111111")},
        {QStringLiteral("actor_device_id"),
         QStringLiteral("44444444-4444-4444-8444-444444444444")},
        {QStringLiteral("kind"), QStringLiteral("project.setScalar")},
        {QStringLiteral("schemaVersion"),
         int(daw::collab::kProjectCommandSchemaVersion)},
        {QStringLiteral("baseServerSeq"), double(sequence - 1)},
        {QStringLiteral("payload"),
         QJsonObject{{QStringLiteral("field"), QStringLiteral("tempo")},
                     {QStringLiteral("value"), tempo}}},
        {QStringLiteral("preconditions"), QJsonArray{}},
        {QStringLiteral("touchedFields"),
         QJsonArray{QStringLiteral("project:tempo"),
                    QStringLiteral("project:tempoCascade")}},
        {QStringLiteral("created_at"),
         QStringLiteral("2026-08-29T10:01:00Z")},
    };
}

QJsonObject testBootstrapPage(const QString& projectId, quint64 head,
                              quint64 snapshotSequence,
                              const QJsonArray& operations,
                              quint64 nextAfter, bool hasMore) {
    return {
        {QStringLiteral("project"),
         testProject(projectId, head, snapshotSequence)},
        {QStringLiteral("role"), QStringLiteral("owner")},
        {QStringLiteral("snapshot"),
         testSnapshot(projectId, snapshotSequence)},
        {QStringLiteral("operations"), operations},
        {QStringLiteral("field_heads"), QJsonArray{}},
        {QStringLiteral("head_seq"), double(head)},
        {QStringLiteral("next_after_seq"), double(nextAfter)},
        {QStringLiteral("has_more"), hasMore},
    };
}

QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool waitUntil(const std::function<bool()>& done, int timeoutMs = 1500) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return done();
}

} // namespace

bool checkCloudProjectClientForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString secondProjectId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QByteArray testToken = QByteArrayLiteral("test-bearer-secret");
    CloudProjectClient::Credentials credentials;
    credentials.apiOrigin = QStringLiteral("https://api.vlt.test/v1");
    credentials.bearerToken = testToken;
    credentials.userId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    credentials.deviceId =
        QStringLiteral("44444444-4444-4444-8444-444444444444");
    credentials.authenticated = true;

    FakeCloudNetwork network;
    CloudProjectClient client([credentials] { return credentials; }, &network,
                              nullptr);
    int failures = 0;
    CloudClientError lastError;
    CloudRequestKind lastFailureKind = CloudRequestKind::GetProject;
    QObject::connect(
        &client, &CloudProjectClient::requestFailed,
        [&](quint64, CloudRequestKind kind, const CloudClientError& value) {
            ++failures;
            lastFailureKind = kind;
            lastError = value;
        });

    const QString sessionId =
        QStringLiteral("55555555-5555-4555-8555-555555555555");
    const QString sessionMemberId =
        QStringLiteral("99999999-9999-4999-8999-999999999999");
    ParseFailure sessionParse;
    const auto typedSession = parseSessionState(
        QJsonObject{
            {QStringLiteral("session"),
             QJsonObject{
                 {QStringLiteral("id"), sessionId},
                 {QStringLiteral("project_id"), projectId},
                 {QStringLiteral("created_by"), credentials.userId},
                 {QStringLiteral("host_member_id"), sessionMemberId},
                 {QStringLiteral("mode"), QStringLiteral("independent")},
                 {QStringLiteral("status"), QStringLiteral("active")},
                 {QStringLiteral("version"), 1.0},
                 {QStringLiteral("created_at"),
                  QStringLiteral("2026-08-29T10:00:00Z")},
                 {QStringLiteral("started_at"),
                  QStringLiteral("2026-08-29T10:00:01Z")},
                 {QStringLiteral("updated_at"),
                  QStringLiteral("2026-08-29T10:00:02Z")},
                 {QStringLiteral("ended_at"), QJsonValue::Null},
             }},
            {QStringLiteral("members"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("id"), sessionMemberId},
                 {QStringLiteral("session_id"), sessionId},
                 {QStringLiteral("user_id"), credentials.userId},
                 {QStringLiteral("device_id"), credentials.deviceId},
                 {QStringLiteral("desktop_session_id"),
                  QStringLiteral("abababab-abab-4bab-8bab-abababababab")},
                 {QStringLiteral("joined_at"),
                  QStringLiteral("2026-08-29T10:00:01Z")},
                 {QStringLiteral("last_seen_at"),
                  QStringLiteral("2026-08-29T10:00:02Z")},
                 {QStringLiteral("left_at"), QJsonValue::Null},
             }}},
        },
        &sessionParse);
    if (!typedSession || typedSession->members.size() != 1 ||
        typedSession->members.front().desktopSessionId !=
            QLatin1String("abababab-abab-4bab-8bab-abababababab")) {
        return fail(QStringLiteral("typed desktop session identity was rejected"));
    }
    QJsonObject endingSessionJson;
    {
        QJsonObject root{
            {QStringLiteral("session"),
             QJsonObject{
                 {QStringLiteral("id"), sessionId},
                 {QStringLiteral("project_id"), projectId},
                 {QStringLiteral("created_by"), credentials.userId},
                 {QStringLiteral("host_member_id"), QJsonValue::Null},
                 {QStringLiteral("mode"), QStringLiteral("independent")},
                 {QStringLiteral("status"), QStringLiteral("ending")},
                 {QStringLiteral("version"), 2.0},
                 {QStringLiteral("created_at"),
                  QStringLiteral("2026-08-29T10:00:00Z")},
                 {QStringLiteral("started_at"),
                  QStringLiteral("2026-08-29T10:00:01Z")},
                 {QStringLiteral("updated_at"),
                  QStringLiteral("2026-08-29T10:00:03Z")},
                 {QStringLiteral("ended_at"), QJsonValue::Null},
             }},
            {QStringLiteral("members"), QJsonArray{}},
        };
        endingSessionJson = std::move(root);
    }
    ParseFailure endingParse;
    const auto endingSession =
        parseSessionState(endingSessionJson, &endingParse);
    if (!endingSession ||
        endingSession->session.status != CloudSessionStatus::Ending ||
        !endingSession->session.endedAt.isNull()) {
        return fail(QStringLiteral("ending session status was rejected"));
    }

    ParseFailure snapshotParse;
    QJsonObject strictSnapshot = testSnapshot(projectId, 0);
    if (!parseSnapshot(strictSnapshot, &snapshotParse))
        return fail(QStringLiteral("canonical empty asset manifest was rejected"));
    strictSnapshot.remove(QStringLiteral("asset_ids"));
    if (parseSnapshot(strictSnapshot, &snapshotParse))
        return fail(QStringLiteral("snapshot without asset manifest was accepted"));
    strictSnapshot = testSnapshot(projectId, 0);
    const QString manifestId =
        QStringLiteral("12121212-1212-4212-8212-121212121212");
    strictSnapshot.insert(QStringLiteral("asset_ids"),
                          QJsonArray{manifestId, manifestId});
    if (parseSnapshot(strictSnapshot, &snapshotParse))
        return fail(QStringLiteral("duplicate snapshot asset ids were accepted"));

    network.scripts.push_back({
        200,
        compact(QJsonObject{{
            QStringLiteral("projects"),
            QJsonArray{QJsonObject{
                {QStringLiteral("project"), testProject(projectId, 4, 2)},
                {QStringLiteral("role"), QStringLiteral("owner")},
            }},
        }}),
    });
    int listSignals = 0;
    QObject::connect(
        &client, &CloudProjectClient::projectsListed,
        [&](quint64, const QVector<CloudProjectView>& projects) {
            if (projects.size() == 1 && projects.front().project.id == projectId)
                ++listSignals;
        });
    client.listProjects();
    if (!waitUntil([&] { return listSignals == 1 || failures != 0; }) ||
        listSignals != 1 || failures != 0 || network.captured.size() != 1) {
        return fail(QStringLiteral("authenticated project list did not parse"));
    }
    const FakeCloudNetwork::Captured listRequest = network.captured.front();
    if (listRequest.request.url().scheme() != QLatin1String("https") ||
        listRequest.request.url().host() != QLatin1String("api.vlt.test") ||
        listRequest.request.url().path() !=
            QLatin1String("/v1/desktop/projects") ||
        listRequest.request.url().hasQuery() ||
        !listRequest.request.url().userInfo().isEmpty() ||
        listRequest.request.rawHeader("Authorization") !=
            QByteArrayLiteral("Bearer ") + testToken ||
        listRequest.request.url().toString().contains(
            QString::fromUtf8(testToken)) ||
        listRequest.request.attribute(QNetworkRequest::RedirectPolicyAttribute)
                .toInt() != QNetworkRequest::ManualRedirectPolicy) {
        return fail(QStringLiteral("authorization escaped same-origin headers"));
    }

    // Join must never use the old empty body: compatibility is authoritative
    // before the WebSocket is opened.
    network.scripts.push_back({200, {}, {}, {}, true});
    const quint64 joinId = client.joinSession(
        projectId, sessionId);
    if (network.captured.size() != 2) {
        return fail(QStringLiteral("join compatibility request was not sent"));
    }
    QJsonParseError joinParseError;
    const QJsonDocument joinBody = QJsonDocument::fromJson(
        network.captured.back().body, &joinParseError);
    if (joinParseError.error != QJsonParseError::NoError ||
        !joinBody.isObject() || joinBody.object().size() != 4 ||
        joinBody.object().value(QStringLiteral("appVersion")).toString() !=
            QCoreApplication::applicationVersion() ||
        joinBody.object().value(QStringLiteral("engineVersion")).toString() !=
            QCoreApplication::applicationVersion() ||
        joinBody.object()
                .value(QStringLiteral("commandSchemaVersion"))
                .toInt() != int(daw::collab::kProjectCommandSchemaVersion) ||
        joinBody.object()
                .value(QStringLiteral("projectFormatVersion"))
                .toInt() != daw::ProjectSerializer::kFormatVersion) {
        return fail(QStringLiteral("join omitted compatibility metadata"));
    }
    if (!client.cancel(joinId) || failures != 1 ||
        lastError.code != CloudClientErrorCode::Cancelled) {
        return fail(QStringLiteral("request cancellation did not complete"));
    }
    failures = 0;

    const QJsonObject firstPage = testBootstrapPage(
        projectId, 4, 2,
        QJsonArray{testOperation(
            projectId, 3,
            QStringLiteral("66666666-6666-4666-8666-666666666666"),
            130.0)},
        3, true);
    const QJsonObject secondPage = testBootstrapPage(
        projectId, 4, 2,
        QJsonArray{testOperation(
            projectId, 4,
            QStringLiteral("77777777-7777-4777-8777-777777777777"),
            132.0)},
        4, false);
    network.scripts.push_back({200, compact(firstPage)});
    network.scripts.push_back({200, compact(secondPage)});
    int bootstrapSignals = 0;
    CloudProjectBootstrap bootstrap;
    QObject::connect(
        &client, &CloudProjectClient::bootstrapCompleted,
        [&](quint64, const CloudProjectBootstrap& value) {
            ++bootstrapSignals;
            bootstrap = value;
        });
    client.bootstrapProject(projectId, 0, 1);
    if (!waitUntil([&] { return bootstrapSignals == 1 || failures != 0; }) ||
        failures != 0 || bootstrapSignals != 1 ||
        bootstrap.replayBaseSequence != 2 || bootstrap.headSequence != 4 ||
        bootstrap.operations.size() != 2 ||
        bootstrap.operations[0].serverSequence != 3 ||
        bootstrap.operations[1].serverSequence != 4 ||
        network.captured.size() != 4) {
        return fail(QStringLiteral(
                        "canonical bootstrap pagination failed "
                        "(failures=%1, signals=%2, base=%3, head=%4, ops=%5, "
                        "requests=%6, code=%7, message=%8)")
                        .arg(failures)
                        .arg(bootstrapSignals)
                        .arg(bootstrap.replayBaseSequence)
                        .arg(bootstrap.headSequence)
                        .arg(bootstrap.operations.size())
                        .arg(network.captured.size())
                        .arg(int(lastError.code))
                        .arg(lastError.safeMessage));
    }
    const QUrlQuery firstQuery(network.captured[2].request.url());
    const QUrlQuery secondQuery(network.captured[3].request.url());
    if (firstQuery.queryItemValue(QStringLiteral("after_seq")) !=
            QLatin1String("0") ||
        secondQuery.queryItemValue(QStringLiteral("after_seq")) !=
            QLatin1String("3") ||
        network.captured[2].request.url().toString().contains(
            QString::fromUtf8(testToken)) ||
        network.captured[3].request.url().toString().contains(
            QString::fromUtf8(testToken))) {
        return fail(QStringLiteral("bootstrap pagination query was unsafe"));
    }

    network.scripts.push_back({
        200,
        compact(testBootstrapPage(
            projectId, 4, 2,
            QJsonArray{testOperation(
                projectId, 4,
                QStringLiteral("88888888-8888-4888-8888-888888888888"),
                140.0)},
            4, false)),
    });
    client.bootstrapProject(projectId, 0, 10);
    if (!waitUntil([&] { return failures != 0; }) ||
        lastError.code != CloudClientErrorCode::BootstrapGap ||
        bootstrapSignals != 1) {
        return fail(QStringLiteral("bootstrap sequence gap was accepted"));
    }
    failures = 0;

    network.scripts.push_back({200, compact(firstPage)});
    network.scripts.push_back({
        200,
        compact(testBootstrapPage(
            projectId, 4, 2,
            QJsonArray{testOperation(
                projectId, 4,
                QStringLiteral("66666666-6666-4666-8666-666666666666"),
                132.0)},
            4, false)),
    });
    client.bootstrapProject(projectId, 0, 1);
    if (!waitUntil([&] { return failures != 0; }) ||
        lastError.code != CloudClientErrorCode::BootstrapMismatch ||
        bootstrapSignals != 1) {
        return fail(QStringLiteral("bootstrap duplicate opId was accepted"));
    }
    failures = 0;

    // Snapshot field-writer metadata is deliberately not a complete durable-op
    // index. Recovery therefore probes the retained log by opId and receives
    // the result together with the exact head observed in the same transaction.
    const QString probedOperationId =
        QStringLiteral("91919191-9191-4191-8191-919191919191");
    int lookupSignals = 0;
    CloudOperationLookup lastLookup;
    QObject::connect(
        &client, &CloudProjectClient::operationLookupReceived,
        [&](quint64, const CloudOperationLookup& lookup) {
            ++lookupSignals;
            lastLookup = lookup;
        });
    network.scripts.push_back({
        200,
        compact(QJsonObject{
            {QStringLiteral("found"), true},
            {QStringLiteral("head_seq"), 4.0},
            {QStringLiteral("operation"),
             testOperation(projectId, 4, probedOperationId, 134.0)},
        }),
    });
    client.lookupOperation(projectId, probedOperationId);
    if (!waitUntil([&] { return lookupSignals == 1 || failures != 0; }) ||
        failures != 0 || lookupSignals != 1 || !lastLookup.found() ||
        lastLookup.projectId != projectId ||
        lastLookup.operationId != probedOperationId ||
        lastLookup.headSequence != 4 ||
        lastLookup.operation->serverSequence != 4 ||
        network.captured.back().method != QByteArrayLiteral("GET") ||
        network.captured.back().request.url().path() !=
            QStringLiteral("/v1/desktop/projects/%1/ops/%2")
                .arg(projectId, probedOperationId) ||
        network.captured.back().request.url().hasQuery()) {
        return fail(QStringLiteral("authoritative operation lookup failed"));
    }

    network.scripts.push_back({
        200,
        compact(QJsonObject{
            {QStringLiteral("found"), false},
            {QStringLiteral("head_seq"), 7.0},
            {QStringLiteral("operation"), QJsonValue::Null},
        }),
    });
    client.lookupOperation(projectId, probedOperationId);
    if (!waitUntil([&] { return lookupSignals == 2 || failures != 0; }) ||
        failures != 0 || lookupSignals != 2 || lastLookup.found() ||
        lastLookup.headSequence != 7) {
        return fail(QStringLiteral("absent operation proof was rejected"));
    }

    network.scripts.push_back({
        200,
        compact(QJsonObject{
            {QStringLiteral("found"), false},
            {QStringLiteral("head_seq"), 4.0},
            {QStringLiteral("operation"),
             testOperation(projectId, 4, probedOperationId, 134.0)},
        }),
    });
    client.lookupOperation(projectId, probedOperationId);
    if (!waitUntil([&] { return failures != 0; }) ||
        lastFailureKind != CloudRequestKind::LookupOperation ||
        lastError.code != CloudClientErrorCode::InvalidResponse ||
        lookupSignals != 2) {
        return fail(QStringLiteral(
            "operation lookup accepted a found/value mismatch"));
    }
    failures = 0;

    network.scripts.push_back({
        200,
        compact(QJsonObject{
            {QStringLiteral("found"), true},
            {QStringLiteral("head_seq"), 3.0},
            {QStringLiteral("operation"),
             testOperation(projectId, 4, probedOperationId, 134.0)},
        }),
    });
    client.lookupOperation(projectId, probedOperationId);
    if (!waitUntil([&] { return failures != 0; }) ||
        lastFailureKind != CloudRequestKind::LookupOperation ||
        lastError.code != CloudClientErrorCode::InvalidResponse ||
        lookupSignals != 2) {
        return fail(QStringLiteral(
            "operation lookup accepted a sequence beyond its observed head"));
    }
    failures = 0;

    const qsizetype requestsBeforeInvalidLookup = network.captured.size();
    client.lookupOperation(projectId, QStringLiteral("not-an-operation"));
    if (failures != 1 ||
        lastFailureKind != CloudRequestKind::LookupOperation ||
        lastError.code != CloudClientErrorCode::InvalidInput ||
        network.captured.size() != requestsBeforeInvalidLookup) {
        return fail(QStringLiteral("invalid operation lookup reached network"));
    }
    failures = 0;

    // A replacement bootstrap invalidates the earlier generation before its
    // deferred reply can complete. Only the replacement may emit.
    network.scripts.push_back({200, compact(firstPage),
                               QByteArrayLiteral("application/json"), {}, true});
    network.scripts.push_back({
        200,
        compact(testBootstrapPage(secondProjectId, 0, 0, {}, 0, false)),
    });
    const quint64 staleId = client.bootstrapProject(projectId, 0, 1);
    const quint64 freshId = client.bootstrapProject(secondProjectId, 0, 1);
    Q_UNUSED(staleId);
    quint64 completedId = 0;
    QMetaObject::Connection completion = QObject::connect(
        &client, &CloudProjectClient::bootstrapCompleted,
        [&](quint64 requestId, const CloudProjectBootstrap& value) {
            if (value.project.id == secondProjectId) completedId = requestId;
        });
    if (!waitUntil([&] { return completedId != 0 || failures != 0; }) ||
        completedId != freshId || failures != 0) {
        QObject::disconnect(completion);
        return fail(QStringLiteral("stale bootstrap generation emitted"));
    }
    QObject::disconnect(completion);

    const QString leaseId =
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const QString otherLeaseId =
        QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const QString trackId =
        QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    ParseFailure leaseParse;
    QJsonObject strictLease =
        testTrackLease(projectId, sessionId, leaseId, trackId);
    if (!parseTrackLease(strictLease, &leaseParse))
        return fail(QStringLiteral("canonical recording lease was rejected"));
    strictLease.insert(QStringLiteral("lease_kind"),
                       QStringLiteral("freeze"));
    if (parseTrackLease(strictLease, &leaseParse))
        return fail(QStringLiteral("non-recording lease kind was accepted"));
    strictLease = testTrackLease(projectId, sessionId, leaseId, trackId);
    strictLease.insert(QStringLiteral("expires_at"),
                       QStringLiteral("2026-08-29T09:59:59Z"));
    if (parseTrackLease(strictLease, &leaseParse))
        return fail(QStringLiteral("invalid recording lease times were accepted"));

    int leaseSignals = 0;
    quint64 lastLeaseRequestId = 0;
    CloudRequestKind lastLeaseRequestKind = CloudRequestKind::GetProject;
    CloudProjectTrackLease lastLease;
    QObject::connect(
        &client, &CloudProjectClient::leaseReceived,
        [&](quint64 requestId, CloudRequestKind kind,
            const CloudProjectTrackLease& lease) {
            ++leaseSignals;
            lastLeaseRequestId = requestId;
            lastLeaseRequestKind = kind;
            lastLease = lease;
        });

    // Invalid identifiers and TTLs are rejected before the network boundary.
    const qsizetype requestsBeforeInvalidLease = network.captured.size();
    client.acquireRecordingLease(projectId, sessionId,
                                 QStringLiteral("not-a-track"), 30);
    if (failures != 1 ||
        lastFailureKind != CloudRequestKind::AcquireRecordingLease ||
        lastError.code != CloudClientErrorCode::InvalidInput ||
        network.captured.size() != requestsBeforeInvalidLease) {
        return fail(QStringLiteral("invalid lease acquire reached the network"));
    }
    failures = 0;
    client.renewRecordingLease(projectId, sessionId, leaseId, 4);
    if (failures != 1 ||
        lastFailureKind != CloudRequestKind::RenewRecordingLease ||
        lastError.code != CloudClientErrorCode::InvalidInput ||
        network.captured.size() != requestsBeforeInvalidLease) {
        return fail(QStringLiteral("invalid lease TTL reached the network"));
    }
    failures = 0;
    client.releaseRecordingLease(projectId, QStringLiteral("not-a-session"),
                                 leaseId);
    if (failures != 1 ||
        lastFailureKind != CloudRequestKind::ReleaseRecordingLease ||
        lastError.code != CloudClientErrorCode::InvalidInput ||
        network.captured.size() != requestsBeforeInvalidLease) {
        return fail(QStringLiteral("invalid lease release reached the network"));
    }
    failures = 0;

    network.scripts.push_back(
        {201, compact(testTrackLease(projectId, sessionId, leaseId, trackId))});
    const quint64 acquireId =
        client.acquireRecordingLease(projectId, sessionId, trackId, 30);
    if (!waitUntil([&] { return leaseSignals == 1 || failures != 0; }) ||
        failures != 0 || leaseSignals != 1 ||
        lastLeaseRequestId != acquireId ||
        lastLeaseRequestKind != CloudRequestKind::AcquireRecordingLease ||
        lastLease.id != leaseId || lastLease.projectId != projectId ||
        lastLease.sessionId != sessionId || lastLease.trackId != trackId ||
        lastLease.kind != CloudProjectLeaseKind::Record ||
        lastLease.holderMemberId != sessionMemberId ||
        !lastLease.acquiredAt.isValid() || !lastLease.renewedAt.isValid() ||
        !lastLease.expiresAt.isValid() ||
        lastLease.acquiredAt != lastLease.renewedAt) {
        return fail(QStringLiteral("recording lease acquire did not parse"));
    }
    const FakeCloudNetwork::Captured acquireRequest = network.captured.back();
    QJsonParseError acquireBodyError;
    const QJsonDocument acquireBody = QJsonDocument::fromJson(
        acquireRequest.body, &acquireBodyError);
    if (acquireRequest.method != QByteArrayLiteral("POST") ||
        acquireRequest.request.url().path() !=
            QStringLiteral("/v1/desktop/projects/%1/sessions/%2/leases")
                .arg(projectId, sessionId) ||
        acquireBodyError.error != QJsonParseError::NoError ||
        !acquireBody.isObject() ||
        !exactKeys(acquireBody.object(), {"track_id", "ttl_seconds"}) ||
        acquireBody.object().value(QStringLiteral("track_id")).toString() !=
            trackId ||
        acquireBody.object().value(QStringLiteral("ttl_seconds")).toInt() !=
            30) {
        return fail(QStringLiteral("recording lease acquire request drifted"));
    }

    QJsonObject renewedLease =
        testTrackLease(projectId, sessionId, leaseId, trackId);
    renewedLease.insert(QStringLiteral("renewed_at"),
                        QStringLiteral("2026-08-29T10:00:10Z"));
    renewedLease.insert(QStringLiteral("expires_at"),
                        QStringLiteral("2026-08-29T10:00:40Z"));
    network.scripts.push_back({200, compact(renewedLease)});
    const quint64 renewId =
        client.renewRecordingLease(projectId, sessionId, leaseId);
    if (!waitUntil([&] { return leaseSignals == 2 || failures != 0; }) ||
        failures != 0 || leaseSignals != 2 || lastLeaseRequestId != renewId ||
        lastLeaseRequestKind != CloudRequestKind::RenewRecordingLease ||
        lastLease.id != leaseId) {
        return fail(QStringLiteral("recording lease renewal did not parse"));
    }
    const FakeCloudNetwork::Captured renewRequest = network.captured.back();
    QJsonParseError renewBodyError;
    const QJsonDocument renewBody = QJsonDocument::fromJson(
        renewRequest.body, &renewBodyError);
    if (renewRequest.method != QByteArrayLiteral("PATCH") ||
        renewRequest.request.url().path() !=
            QStringLiteral("/v1/desktop/projects/%1/sessions/%2/leases/%3")
                .arg(projectId, sessionId, leaseId) ||
        renewBodyError.error != QJsonParseError::NoError ||
        !renewBody.isObject() || !renewBody.object().isEmpty()) {
        return fail(QStringLiteral("recording lease renewal request drifted"));
    }

    int releaseSignals = 0;
    QObject::connect(
        &client, &CloudProjectClient::operationCompleted,
        [&](quint64, CloudRequestKind kind, const QString& resourceId) {
            if (kind == CloudRequestKind::ReleaseRecordingLease &&
                resourceId == leaseId) {
                ++releaseSignals;
            }
        });
    network.scripts.push_back({204, {}, {}});
    client.releaseRecordingLease(projectId, sessionId, leaseId);
    if (!waitUntil([&] { return releaseSignals == 1 || failures != 0; }) ||
        failures != 0 || releaseSignals != 1) {
        return fail(QStringLiteral("recording lease release did not complete"));
    }
    const FakeCloudNetwork::Captured releaseRequest = network.captured.back();
    if (releaseRequest.method != QByteArrayLiteral("DELETE") ||
        releaseRequest.request.url().path() !=
            QStringLiteral("/v1/desktop/projects/%1/sessions/%2/leases/%3")
                .arg(projectId, sessionId, leaseId) ||
        !releaseRequest.body.isEmpty()) {
        return fail(QStringLiteral("recording lease release request drifted"));
    }

    // A successful HTTP status is insufficient: every identity in the lease
    // response is pinned to the request context.
    network.scripts.push_back({
        200,
        compact(testTrackLease(projectId, sessionId, otherLeaseId, trackId)),
    });
    client.renewRecordingLease(projectId, sessionId, leaseId, 60);
    if (!waitUntil([&] { return failures != 0; }) ||
        failures != 1 ||
        lastFailureKind != CloudRequestKind::RenewRecordingLease ||
        lastError.code != CloudClientErrorCode::InvalidResponse ||
        leaseSignals != 2) {
        return fail(QStringLiteral("mismatched recording lease was accepted"));
    }
    failures = 0;

    QJsonObject foreignSessionLease =
        testTrackLease(projectId, secondProjectId, leaseId, trackId);
    network.scripts.push_back({201, compact(foreignSessionLease)});
    client.acquireRecordingLease(projectId, sessionId, trackId);
    if (!waitUntil([&] { return failures != 0; }) ||
        failures != 1 ||
        lastError.code != CloudClientErrorCode::InvalidResponse ||
        leaseSignals != 2) {
        return fail(QStringLiteral("foreign lease session was accepted"));
    }
    failures = 0;

    QJsonObject foreignProjectLease =
        testTrackLease(secondProjectId, sessionId, leaseId, trackId);
    network.scripts.push_back({201, compact(foreignProjectLease)});
    client.acquireRecordingLease(projectId, sessionId, trackId);
    if (!waitUntil([&] { return failures != 0; }) ||
        failures != 1 ||
        lastError.code != CloudClientErrorCode::InvalidResponse ||
        leaseSignals != 2) {
        return fail(QStringLiteral("foreign lease project was accepted"));
    }
    failures = 0;

    QJsonObject extraLease =
        testTrackLease(projectId, sessionId, leaseId, trackId);
    extraLease.insert(QStringLiteral("debug"), true);
    network.scripts.push_back({201, compact(extraLease)});
    client.acquireRecordingLease(projectId, sessionId, trackId);
    if (!waitUntil([&] { return failures != 0; }) ||
        failures != 1 ||
        lastError.code != CloudClientErrorCode::InvalidResponse ||
        leaseSignals != 2) {
        return fail(QStringLiteral("extra recording lease fields were accepted"));
    }
    failures = 0;

    network.scripts.push_back({
        200,
        compact(testTrackLease(projectId, sessionId, leaseId, trackId)),
    });
    client.acquireRecordingLease(projectId, sessionId, trackId);
    if (!waitUntil([&] { return failures != 0; }) ||
        failures != 1 ||
        lastError.code != CloudClientErrorCode::UnexpectedStatus ||
        leaseSignals != 2) {
        return fail(QStringLiteral("wrong recording lease status was accepted"));
    }
    failures = 0;

    network.scripts.push_back({
        201,
        compact(testTrackLease(projectId, sessionId, leaseId, trackId)),
        QByteArrayLiteral("application/json"), {}, true,
    });
    const quint64 cancelledLeaseId =
        client.acquireRecordingLease(projectId, sessionId, trackId);
    const FakeCloudNetwork::Captured defaultTtlRequest =
        network.captured.back();
    QJsonParseError defaultTtlBodyError;
    const QJsonDocument defaultTtlBody = QJsonDocument::fromJson(
        defaultTtlRequest.body, &defaultTtlBodyError);
    if (defaultTtlBodyError.error != QJsonParseError::NoError ||
        !defaultTtlBody.isObject() ||
        !exactKeys(defaultTtlBody.object(), {"track_id"}) ||
        !client.cancel(cancelledLeaseId) || failures != 1 ||
        lastFailureKind != CloudRequestKind::AcquireRecordingLease ||
        lastError.code != CloudClientErrorCode::Cancelled ||
        leaseSignals != 2) {
        return fail(QStringLiteral("recording lease cancellation was unsafe"));
    }
    failures = 0;

    network.scripts.push_back(
        {302, {}, {}, QUrl(QStringLiteral("https://evil.invalid/steal"))});
    client.listProjects();
    if (!waitUntil([&] { return failures != 0; }) ||
        lastError.code != CloudClientErrorCode::RedirectRejected) {
        return fail(QStringLiteral("HTTP redirect was followed or accepted"));
    }
    return true;
}

} // namespace collab
