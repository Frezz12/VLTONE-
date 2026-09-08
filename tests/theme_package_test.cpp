#include "ThemePackage.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char** argv) {
    QTemporaryDir root;
    if (!root.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());
    QCoreApplication::setOrganizationName(QStringLiteral("VLTONE Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Theme Package"));
    QApplication app(argc, argv);
    app.setProperty("dawHeadlessDataRoot", root.path());

    QString error;
    if (!ui::ThemePackage::checkForTest(&error)) {
        std::fprintf(stderr, "theme package test failed: %s\n",
                     error.toUtf8().constData());
        return 1;
    }
    return 0;
}
