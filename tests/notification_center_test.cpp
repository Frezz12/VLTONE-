#include "NotificationCenter.hpp"
#include "Theme.hpp"
#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTest>
#include <QTranslator>
#include <QWindow>
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool value, const char* text) { std::printf("%s %s\n", value ? "PASS" : "FAIL", text); failures += !value; };
    QTranslator translation;
    if (app.arguments().contains("--ru")) {
        check(translation.load(QCoreApplication::applicationDirPath() + "/../app/vlt_ru.qm"), "Russian catalog loads");
        app.installTranslator(&translation);
    }
    const auto translated = [](const char* text) { return QCoreApplication::translate("MainWindow", text); };
    QWidget surface; surface.resize(1100, 820);
    QLineEdit input(&surface); input.move(20, 20); surface.show(); input.setFocus();
    ui::NotificationCenter center(&surface);
    QTest::qWait(30);
    const auto focus = QApplication::focusWidget();
    int actions = 0;
    ui::Notification notice{"slot", QString::fromUtf8("Компрессор — Вокал"),
        translated("Effect disabled. The track continues without it. Recovery will wait until playback and monitoring stop."), {}, 1, false,
        {{translated("Stop and restore"), [&] { ++actions; }}}};
    center.showNotification(notice);
    QTest::qWait(30);
    auto* card = surface.findChild<QFrame*>(QStringLiteral("AudioNotificationCard"));
    const auto onSurface = [&](QWidget* widget) {
        return surface.rect().contains(QRect(widget->mapTo(&surface, QPoint()), widget->size()));
    };
    check(card && card->parentWidget()->parentWidget() == &surface && !card->isWindow() && center.visibleCount() == 1,
          "card is an in-window overlay");
    check(card && !card->testAttribute(Qt::WA_NativeWindow) &&
          card->parentWidget()->testAttribute(Qt::WA_NativeWindow) && !card->parentWidget()->isWindow(),
          "cards share one native child layer above plugin editors");
    check(QApplication::focusWidget() == focus, "a notification never takes keyboard focus");
    check(card && !card->accessibleName().isEmpty() && !card->accessibleDescription().isEmpty(), "card has accessible names and explanation");
    check(card && onSurface(card), "card fits inside its surface");
    QWidget nativeEditor(&surface);
    nativeEditor.setAttribute(Qt::WA_DontCreateNativeAncestors);
    nativeEditor.setAttribute(Qt::WA_NativeWindow);
    nativeEditor.setProperty("vlt.nativeOverlay", true);
    nativeEditor.setGeometry(QRect(card->mapTo(&surface, QPoint()), card->size()));
    nativeEditor.show(); nativeEditor.raise();
    if (nativeEditor.windowHandle()) nativeEditor.windowHandle()->raise();
    QTest::qWait(30);
    check(surface.childAt(card->mapTo(&surface, QPoint(4, 4))) == card,
          "notification layer stays above a newly raised native editor");
    nativeEditor.hide();
    QTest::mouseClick(card->findChild<QPushButton*>(), Qt::LeftButton);
    check(actions == 1, "explicit recovery action invokes its callback once");
    auto* keyboardAction = card->findChild<QPushButton*>();
    keyboardAction->setFocus(); QTest::keyClick(keyboardAction, Qt::Key_Space);
    check(actions == 2, "recovery action is keyboard accessible");
    center.showNotification(notice);
    check(surface.findChildren<QFrame*>(QStringLiteral("AudioNotificationCard")).size() == 1, "same incident updates the existing card");
    check(card->findChild<QPushButton*>() == keyboardAction && keyboardAction->hasFocus(),
          "an update preserves its action button and keyboard focus");
    auto changed = notice;
    changed.title = translated("Audio device unavailable");
    changed.message = translated("Trying to reconnect the audio device. Any interrupted take has been preserved.");
    changed.actions = {{translated("Reconnect"), [&] { actions += 10; }},
                       {translated("Audio settings"), [] {}}};
    center.showNotification(changed); QTest::qWait(30);
    bool fits = true;
    for (auto* label : card->findChildren<QLabel*>()) {
        const int needed = label->hasHeightForWidth() ? label->heightForWidth(label->width()) : label->sizeHint().height();
        fits &= label->height() >= needed && card->rect().contains(label->geometry());
    }
    for (auto* button : card->findChildren<QPushButton*>())
        if (button->isVisible()) fits &= card->rect().contains(button->geometry());
    check(fits, "changed text and newly added actions fit the updated card without clipping");
    QTest::mouseClick(keyboardAction, Qt::LeftButton);
    check(actions == 12, "a reused action button invokes the current callback");
    actions = 2;
    center.showNotification(notice); QTest::qWait(30);
    QTest::mouseClick(card->findChild<QToolButton*>(), Qt::LeftButton);
    center.showNotification(notice);
    check(center.dismissed("slot") && center.visibleCount() == 0 && actions == 2,
          "dismissal stays dismissed and never invokes recovery");
    center.showNotification(notice, true);
    for (int i = 0; i < 4; ++i) { auto next = notice; next.id = QString::number(i); center.showNotification(next); }
    check(center.visibleCount() == 3 && surface.findChild<QToolButton*>("MoreAudioNotifications")->isVisible(),
          "only three cards appear and remaining notifications have an accessible disclosure");
    for (const auto& theme : ThemeManager::instance().presets()) {
        if (theme.id != "studio-gray" && theme.id != "light") continue;
        ThemeManager::instance().setThemeId(theme.id, false);
        QTest::qWait(30);
        check(card->grab().toImage().pixelColor(4, 4) == ThemeManager::instance().theme().surfaceElevated,
              "opaque overlay paints its complete theme background");
        surface.grab().save(QStringLiteral("/tmp/vlt-audio-notices-%1.png").arg(theme.dark ? "dark" : "light"));
    }
    surface.resize(620, 320); QTest::qWait(30);
    int visible = 0;
    for (auto* frame : surface.findChildren<QFrame*>("AudioNotificationCard")) if (frame->isVisible()) {
        ++visible; check(onSurface(frame), "small-window card remains on screen");
    }
    check(visible < 3, "small windows move excess cards into disclosure without overlapping");
    center.clear(); surface.resize(1100, 820);
    notice.resolved = true; notice.message = "Plugin restored";
    center.showNotification(notice);
    card = surface.findChild<QFrame*>(QStringLiteral("AudioNotificationCard"));
    auto* action = card->findChild<QPushButton*>();
    action->setFocus(); QTest::qWait(5200);
    check(center.visibleCount() == 1, "success timeout pauses while a child has keyboard focus");
    input.setFocus(); QTest::mouseMove(card, QPoint(2, 2)); QTest::qWait(5200);
    check(center.visibleCount() == 1, "success timeout pauses while hovered");
    input.setFocus(); QTest::mouseMove(&input); QTest::qWait(4500);
    ++notice.incident;
    center.showNotification(notice); QTest::qWait(800);
    check(center.visibleCount() == 1, "a new resolved incident receives its own five-second timeout");
    QTest::qWait(4400);
    check(center.visibleCount() == 0, "success expires after five seconds without hover or focus");
    center.showNotification(notice); center.clear();
    check(center.visibleCount() == 0 && !surface.findChild<QFrame*>("AudioNotificationCard"), "project reset removes cards and their action closures");
    return failures ? 1 : 0;
}
