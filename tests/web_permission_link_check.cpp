#include "WebPermissionPolicy.hpp"
#include <QApplication>

// Packaging builds this small executable first. It instantiates the same
// signal connection as both web views without linking the entire DAW engine.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWebEnginePage page;
    denyWebPagePermissions(&page, &page);
    return 0;
}
