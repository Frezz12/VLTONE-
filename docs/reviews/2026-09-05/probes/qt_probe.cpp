#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QTimer>
#include <cstdio>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget main;
    QDialog dialog(&main);
    QPushButton cancel("Cancel", &dialog);
    main.show(); dialog.show();
    main.setEnabled(false);
    int ticks = 0;
    QTimer timer(&main);
    QObject::connect(&timer, &QTimer::timeout, [&]{ ++ticks; });
    timer.start(0);
    app.processEvents();
    std::printf("disabled main: dialog enabled=%d, cancel enabled=%d, timer ticks=%d\n",
        dialog.isEnabled(), cancel.isEnabled(), ticks);
}
