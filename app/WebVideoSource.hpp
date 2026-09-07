#pragma once

#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QUrl>
#include <QWebEngineFrame>
#include <functional>

class QObject;
class QWebEnginePage;
class QWebEngineProfile;

namespace ui {

// Only durable page/element hints are persisted. Chromium handles and blob
// URLs belong to the current document and must never be restored from disk.
struct WebVideoSource {
    QUrl pageUrl;
    QUrl frameUrl;
    QList<int> framePath;
    QString elementId;
    QString mediaUrl;
    QString title;
    int videoIndex = 0;
    double position = 0;
    bool playing = false;
    double area = 0;
    QString token;
    QWebEngineFrame frame;

    QJsonObject toJson() const;
    static WebVideoSource fromJson(const QJsonObject& json);
    bool valid() const;
};

QWebEngineProfile* createWebBrowserProfile(QObject* owner);
void discoverWebVideos(QWebEnginePage* page, QObject* context,
                       std::function<void(QList<WebVideoSource>)> callback);
QString webVideoLookupScript(const WebVideoSource& source);
void pauseWebVideo(const WebVideoSource& source);

} // namespace ui
Q_DECLARE_METATYPE(ui::WebVideoSource)
