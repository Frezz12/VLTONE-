#include "ChannelStrip.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "MiniModuleRack.hpp"
#include "MixerPreferences.hpp"
#include "model/MiniModules.hpp"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstdio>

bool ChannelStrip::checkSectionControlsForTest() {
    bool ok = true;
    const auto check = [&](bool result, const char* label) {
        std::printf("%s channel sections: %s\n", result ? "PASS" : "FAIL", label);
        ok &= result;
    };
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return false;
    auto& prefs = ui::MixerPreferences::instance();
    const bool wasVisible = prefs.colorVisible();
    prefs.setColorVisible(true);
    const auto id = controller.addTrack(daw::TrackKind::Audio, "Channel controls");
    const auto destination = controller.addTrack(daw::TrackKind::Aux, "Reverb");
    const auto destination2 = controller.addTrack(daw::TrackKind::Aux, "Delay");
    const auto mini1 = controller.addMiniModule(id, daw::plugins::mini::builtin("color"));
    const auto mini2 = controller.addMiniModule(id, daw::plugins::mini::builtin("chorus"));
    controller.addMiniModule(id, daw::plugins::mini::builtin("doubler"));
    const auto fx1 = controller.addInsert(id, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    const auto fx2 = controller.addInsert(id, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    const auto send1 = controller.addSend(id, destination);
    const auto send2 = controller.addSend(id, destination2);
    if (mini1.empty() || mini2.empty() || fx1.empty() || fx2.empty() || send1.empty() || send2.empty())
        return false;
    controller.setInsertParameter(id, mini1, "drive", -29.125);
    controller.setInsertBypassed(id, mini2, true);
    controller.setInsertBypassed(id, fx2, true);
    controller.setSendLevel(id, send1, .32f);
    controller.setSendLevel(id, send2, .71f);
    controller.setSendPreFader(id, send2, true);
    controller.setSendEnabled(id, send2, false);
    QWidget stripHost;
    stripHost.resize(320, 800);
    ChannelStrip strip(&controller, QString::fromStdString(id), false, &stripHost);
    stripHost.show();
    strip.show();
    QApplication::processEvents();
    auto* mini = strip.findChild<ui::IconButton*>("ChannelMiniPower");
    auto* settings = strip.findChild<ui::IconButton*>("ChannelStripMenu");
    check(mini && settings && strip.m_fxPower && strip.m_sendsPower,
          "all three sections have a group switch and MINI owns channel settings");
    if (!mini || !settings || !strip.m_fxPower || !strip.m_sendsPower) return false;
    check(!mini->isChecked() && !strip.m_fxPower->isChecked() && !strip.m_sendsPower->isChecked(),
          "mixed per-slot states leave the group switch ready to bypass");
    const auto miniFlags = [&] {
        std::vector<bool> flags;
        for (const auto& module : controller.miniModules(id)) flags.push_back(module.bypassed);
        return flags;
    };
    const auto fxFlags = [&] {
        std::vector<bool> flags;
        for (const auto& insert : *controller.channelInserts(id)) flags.push_back(insert.bypassed);
        return flags;
    };
    const auto sendFlags = [&] {
        std::vector<bool> flags;
        for (const auto& send : controller.project().findTrack(id)->sends) flags.push_back(!send.enabled);
        return flags;
    };
    const auto miniBefore = miniFlags(), fxBefore = fxFlags(), sendsBefore = sendFlags();
    const auto exercise = [&](ui::IconButton* button, const auto& flags, const auto& before, const char* label) {
        const auto depth = controller.undoDepth();
        button->click();
        strip.syncFromModel();
        auto state = flags();
        check(button->isChecked() && controller.undoDepth() == depth + 1 &&
              std::all_of(state.begin(), state.end(), [](bool off) { return off; }), label);
        controller.undo();
        strip.syncFromModel();
        check(flags() == before && !button->isChecked(), "one Undo restores the exact mixed states and switch");
        controller.redo();
        strip.syncFromModel();
        state = flags();
        check(button->isChecked() && std::all_of(state.begin(), state.end(), [](bool off) { return off; }),
              "Redo bypasses the complete section");
        button->click();
        strip.syncFromModel();
        state = flags();
        check(!button->isChecked() && std::none_of(state.begin(), state.end(), [](bool off) { return off; }),
              "second click enables the complete section");
        controller.undo();
        controller.undo();
        strip.syncFromModel();
    };
    exercise(mini, miniFlags, miniBefore, "MINI bypasses all modules with one Undo entry");
    check(fxFlags() == fxBefore && sendFlags() == sendsBefore, "MINI leaves FX and sends unchanged");
    exercise(strip.m_fxPower, fxFlags, fxBefore, "FX bypasses all inserts with one Undo entry");
    check(miniFlags() == miniBefore && sendFlags() == sendsBefore, "FX leaves MINI and sends unchanged");
    exercise(strip.m_sendsPower, sendFlags, sendsBefore, "Sends switches all routes off with one Undo entry");
    const auto& sends = controller.project().findTrack(id)->sends;
    check(miniFlags() == miniBefore && fxFlags() == fxBefore &&
          std::abs(controller.insertParameter(id, mini1, "drive") + 29.125) < 1e-9 &&
          std::abs(sends[0].level - .32f) < 1e-6 && std::abs(sends[1].level - .71f) < 1e-6 && sends[1].preFader,
          "section toggles preserve module parameters, send levels, taps and other sections");
    settings->click();
    QApplication::processEvents();
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    check(menu && !menu->actions().empty(), "relocated gear opens the existing channel menu");
    if (menu) menu->close();
    const auto shots = qEnvironmentVariable("DAW_COLOR_SCREENSHOTS");
    for (int width : {75, 100, 150}) {
        strip.setStripWidth(width);
        QApplication::processEvents();
        bool aligned = true;
        const int x = mini->mapTo(&strip, QPoint()).x();
        for (auto* power : {mini, strip.m_fxPower, strip.m_sendsPower}) {
            const auto* caption = power->parentWidget()->findChild<QLabel*>("ChannelSectionCaption");
            aligned &= caption && power->mapTo(&strip, QPoint()).x() == x &&
                power->width() == 20 && power->height() == 20 && power->property("circular").toBool() &&
                power->mapTo(&strip, QPoint(power->width(), 0)).x() <= caption->mapTo(&strip, QPoint()).x();
        }
        aligned &= settings->parentWidget() == mini->parentWidget() &&
            settings->mapTo(&strip, QPoint(settings->width(), 0)).x() <= width;
        check(aligned, "75/100/150 px strips align power before every caption and settings above mini cards");
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            check(strip.grab().save(shots + QString("/channel-controls-%1.png").arg(width)), "section screenshot saved");
        }
    }
    const auto empty = controller.addTrack(daw::TrackKind::Audio, "Empty");
    ChannelStrip emptyStrip(&controller, QString::fromStdString(empty), false);
    check(!emptyStrip.m_fxPower->isEnabled() && !emptyStrip.m_sendsPower->isEnabled() &&
          !emptyStrip.findChild<ui::IconButton*>("ChannelMiniPower")->isEnabled(),
          "empty sections disable their switches");
    const auto masterId = std::string(daw::EngineController::kMasterChannelId);
    controller.addMiniModule(masterId, daw::plugins::mini::builtin("color"));
    ChannelStrip master(&controller, QString::fromStdString(masterId), true);
    auto* masterPower = master.findChild<ui::IconButton*>("ChannelMiniPower");
    masterPower->click();
    check(controller.miniModules(masterId).front().bypassed && miniFlags() == miniBefore,
          "Master MINI switch addresses Master independently");
    ui::IconButton round(icons::Glyph::Power, "Circle");
    round.setButtonSize(40, 24);
    round.setProperty("circular", true);
    round.setCheckable(true);
    round.setChecked(true);
    QImage pixels(round.size(), QImage::Format_ARGB32_Premultiplied);
    pixels.fill(Qt::transparent);
    round.render(&pixels, QPoint(), QRegion(), QWidget::DrawChildren);
    QRect ink;
    for (int y = 0; y < pixels.height(); ++y)
        for (int x = 0; x < pixels.width(); ++x)
            if (qAlpha(pixels.pixel(x, y)) > 8) ink |= QRect(x, y, 1, 1);
    check(!ink.isEmpty() && std::abs(ink.width() - ink.height()) <= 1 && ink.width() < round.width(),
          "round button plates remain circles inside rectangular hit targets");
    prefs.setColorVisible(wasVisible);
    return ok;
}
