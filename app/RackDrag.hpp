#pragma once
#include "EngineController.hpp"
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QStringList>
#include <vector>
namespace ui::rack {
inline constexpr auto mime = "application/x-vlt-rack-devices";
struct Drag {
    QString channel;
    QStringList ids;
    bool valid = false;
    QString document, token;
};
inline QHash<QString, std::shared_ptr<daw::EngineController::ChannelSnapshot>>& transfers() {
    static QHash<QString, std::shared_ptr<daw::EngineController::ChannelSnapshot>> value;
    return value;
}
class TransferMime final : public QMimeData {
public:
    QString token;
    ~TransferMime() override { transfers().remove(token); }
};
inline int nativeDragDepth = 0;
inline QList<QPointer<QWidget>> retiredDragViews;
struct DragScope {
    DragScope() { ++nativeDragDepth; }
    ~DragScope() {
        if (--nativeDragDepth) return;
        auto retired = std::exchange(retiredDragViews, {});
        // QDrag::exec() is nested inside the source widget's event handler.
        // Let that handler unwind before destroying its source and QDrag.
        for (auto& widget : retired) if (widget) widget->deleteLater();
        transfers().clear();
    }
};
inline void retireView(QWidget* widget) {
    if (!widget) return;
    if (!nativeDragDepth) { delete widget; return; }
    widget->hide(); widget->setParent(widget->window());
    retiredDragViews.append(widget);
}
inline QByteArray encode(const QString& channel, const QStringList& ids) {
    return QJsonDocument(QJsonObject{{"channel", channel}, {"ids", QJsonArray::fromStringList(ids)}})
        .toJson(QJsonDocument::Compact);
}
inline Drag decode(const QMimeData* data) {
    if (!data)
        return {};
    const auto payload = data->hasFormat(mime) ? data->data(mime) : data->data("application/x-daw-insert");
    if (payload.startsWith('{')) {
        const auto json = QJsonDocument::fromJson(payload).object();
        Drag result;
        result.channel = json["channel"].toString();
        for (const auto& id : json["ids"].toArray())
            if (id.isString() && !id.toString().isEmpty() && !result.ids.contains(id.toString()))
                result.ids.push_back(id.toString());
        result.valid = json.contains("channel") && !result.ids.isEmpty();
        result.document = json["document"].toString(); result.token = json["transfer"].toString();
        return result;
    }
    if (data->hasFormat("application/x-daw-insert")) {
        const auto parts = QString::fromUtf8(data->data("application/x-daw-insert")).split('\t');
        if (parts.size() == 2 && !parts[1].isEmpty())
            return {parts[0], {parts[1]}, true};
    }
    return {};
}
inline std::vector<std::string> ids(const QStringList& values) {
    std::vector<std::string> out;
    for (const auto& id : values)
        out.push_back(id.toStdString());
    return out;
}
inline QMimeData* ownedMime(daw::EngineController& controller, const QString& channel,
                           const QStringList& slotIds) {
    auto snapshot = std::make_shared<daw::EngineController::ChannelSnapshot>();
    if (!controller.captureRackSelection(channel.toStdString(), ids(slotIds), *snapshot) ||
        !controller.ownChannelTransferResources(*snapshot)) return nullptr;
    auto* data = new TransferMime;
    data->token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    transfers().insert(data->token, std::move(snapshot));
    auto json = QJsonDocument::fromJson(encode(channel, slotIds)).object();
    json["document"] = QString::fromStdString(controller.documentIdentity()); json["transfer"] = data->token;
    data->setData(mime, QJsonDocument(json).toJson(QJsonDocument::Compact));
    return data;
}
inline bool foreign(const Drag& drag, const daw::EngineController& controller) {
    return !drag.document.isEmpty() && drag.document.toStdString() != controller.documentIdentity();
}
inline audio::Result transfer(daw::EngineController& controller, const Drag& drag,
    const std::string& channel, std::size_t index, bool copy, const std::string& group = {},
    std::vector<std::string>* added = nullptr) {
    if (foreign(drag, controller)) {
        const auto snapshot = transfers().value(drag.token);
        if (!snapshot) return audio::Result::fail(audio::EngineError::InvalidArgument, "The drag snapshot is no longer available.");
        return controller.pasteRackSelection(channel, *snapshot, index, group, added);
    }
    return controller.transferRackSelection(drag.channel.toStdString(), ids(drag.ids), channel, index, copy, group, added);
}
} // namespace ui::rack
