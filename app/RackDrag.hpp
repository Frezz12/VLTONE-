#pragma once
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
};
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
} // namespace ui::rack
