#pragma once

#include <QWebEnginePage>
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#include <QWebEnginePermission>
#endif

// Qt 6.8's Windows binary does not export the shared-data destructor used
// when the new permission signal registers its metatype. Avoid that type
// entirely on this release line, including in the connection machinery.
inline void denyWebPagePermissions(QWebEnginePage* page, QObject* context) {
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
    QObject::connect(page, &QWebEnginePage::featurePermissionRequested, context,
        [page](const QUrl& origin, QWebEnginePage::Feature feature) {
            page->setFeaturePermission(origin, feature, QWebEnginePage::PermissionDeniedByUser);
        });
#else
    QObject::connect(page, &QWebEnginePage::permissionRequested, context,
        [](const QWebEnginePermission& permission) { permission.deny(); });
#endif
}
