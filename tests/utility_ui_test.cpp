#include "UtilityPanel.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "Internal/UtilityInstance.hpp"
#include <QApplication>
#include <QAccessible>
#include <QAbstractButton>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL utility UI: %s\n", message); std::exit(1); }
}
template <class T> T* child(QWidget& panel, const char* name) {
    auto* result = panel.findChild<T*>(QString::fromUtf8(name));
    check(result, name);
    return result;
}
void layoutCheck(QWidget& panel) {
    QApplication::processEvents();
    for (auto* widget : panel.findChildren<QWidget*>()) {
        if (!widget->isVisible() || (!qobject_cast<QAbstractButton*>(widget) && !qobject_cast<QSlider*>(widget) && !qobject_cast<QDoubleSpinBox*>(widget))) continue;
        const QRect bounds(widget->mapTo(&panel, QPoint()), widget->size());
        if (!panel.rect().contains(bounds)) {
            std::fprintf(stderr, "clipped %s at %d,%d %dx%d in %dx%d\n", widget->objectName().toUtf8().constData(), bounds.x(), bounds.y(), bounds.width(), bounds.height(), panel.width(), panel.height());
            check(false, "visible control fits panel");
        }
        check(!widget->accessibleName().isEmpty(), "control has accessible name");
        check(widget->focusPolicy() != Qt::NoFocus, "control is reachable by keyboard");
    }
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temp;
    app.setProperty("dawHeadlessDataRoot", temp.path());
    app.setOrganizationName("VLTONE-Utility-Test");
    app.setApplicationName("Utility UI");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    ui::initializeApplicationFonts();
    check(ui::checkBundledFontsForTest(nullptr), "bundled fonts contain Latin and Cyrillic glyphs");
    QTranslator translator;
    const auto translation = qEnvironmentVariable("VLT_UTILITY_TRANSLATION");
    if (!translation.isEmpty()) {
        check(translator.load(translation), "Russian translation loads");
        app.installTranslator(&translator);
    }
    daw::EngineController controller;
    check(bool(controller.initialize(48000, 256, false)), "engine initializes");
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Utility UI");
    const auto output = qEnvironmentVariable("VLT_UTILITY_SCREENSHOTS");
    if (!output.isEmpty()) QDir().mkpath(output);
    using namespace daw::plugins::utility;
    std::vector<std::string> inserts;
    for (const auto kind : {Kind::Gain, Kind::Stereo, Kind::Reverse}) {
        const auto descriptor = descriptorFor(kind);
        const auto insert = controller.addInsert(track, descriptor);
        inserts.push_back(insert);
        RackParameterBinding binding(&controller, QString::fromStdString(track), QString::fromStdString(insert), nullptr);
        UtilityPanel panel(&binding, QString::fromStdString(descriptor.uid));
        panel.resize(panel.sizeHint());
        panel.show();
        QApplication::processEvents();
        auto value = [&](const char* id) { return controller.insertParameter(track, insert, id); };
        const char* parameter = kind == Kind::Gain ? "gain" : kind == Kind::Stereo ? "width" : "mix";
        auto* slider = child<QSlider>(panel, (QStringLiteral("RackParameter.") + parameter).toUtf8().constData());
        const double before = value(parameter);
        auto* accessible = QAccessible::queryAccessibleInterface(slider);
        check(accessible && accessible->valueInterface() && accessible->valueInterface()->currentValue().toDouble() == before,
              "assistive technology receives the plain parameter value");
        QTest::keyClick(slider, kind == Kind::Reverse ? Qt::Key_Left : Qt::Key_Up);
        check(value(parameter) != before, "slider responds to keyboard");
        controller.undo(); panel.refresh();
        check(std::abs(value(parameter) - before) < 1.e-6, "keyboard edit is undoable");
        int commits = 0;
        const auto connection = QObject::connect(&binding, &RackParameterBinding::edited, [&] { ++commits; });
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, slider->rect().center());
        QTest::mouseMove(slider, slider->rect().center() + QPoint(15, -14));
        QTest::mouseMove(slider, slider->rect().center() + QPoint(25, -24), 1);
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, slider->rect().center() + QPoint(25, -24));
        check(commits == 1, "drag commits one undo gesture");
        controller.undo(); panel.refresh();
        check(std::abs(value(parameter) - before) < 1.e-6, "drag undo restores initial value");
        QObject::disconnect(connection);
        if (kind == Kind::Gain) {
            auto* number = child<QDoubleSpinBox>(panel, "UtilityValue.gain");
            number->setFocus(); number->selectAll();
            QTest::keyClicks(number, "24.0"); QTest::keyClick(number, Qt::Key_Return);
            check(value("gain") == 24, "gain supports exact +24 dB input");
            controller.undo(); panel.refresh();
            check(value("gain") == 0, "typed gain is undoable");
            child<QAbstractButton>(panel, "UtilityToggle.mute")->click();
            check(value("mute") == 1, "mute button changes parameter");
        } else if (kind == Kind::Stereo) {
            child<QAbstractButton>(panel, "UtilityChoice.mode.2")->click();
            check(value("mode") == 2 && !slider->isEnabled(), "Side disables width");
            child<QAbstractButton>(panel, "UtilityChoice.mode.0")->click();
            check(slider->isEnabled(), "Stereo restores width");
            child<QAbstractButton>(panel, "UtilityToggle.invertLeft")->click();
            check(value("invertLeft") == 1, "polarity button works");
        } else {
            child<QAbstractButton>(panel, "UtilityChoice.timeMode.1")->click();
            check(child<QSlider>(panel, "RackParameter.timeMs")->isVisible(), "milliseconds exposes length slider");
            check(!child<QWidget>(panel, "RackChoice.division")->isVisible(), "milliseconds hides tempo division");
            child<QAbstractButton>(panel, "UtilityChoice.timeMode.0")->click();
            child<QAbstractButton>(panel, "UtilityChoice.division.4")->click();
            check(value("division") == 4, "division buttons write parameter");
        }
        child<QAbstractButton>(panel, "UtilityReset")->click();
        slider->setFocus(); panel.refresh();
        for (const auto& p : binding.parameters()) check(std::abs(value(p.id.c_str()) - p.defaultValue) < 1.e-6, "reset restores defaults");
        for (const auto* theme : {"dark", "light"}) {
            ThemeManager::instance().setThemeId(theme, false);
            for (const bool compact : {false, true}) {
                UtilityPanel preview(&binding, QString::fromStdString(descriptor.uid), nullptr, compact);
                preview.resize(compact ? QSize(kind == Kind::Gain ? 168 : 344, 128) : preview.sizeHint());
                preview.show();
                layoutCheck(preview);
                const auto stem = QString::fromStdString(descriptor.uid) + "-" + theme + (compact ? "-rack" : "-editor");
                if (!output.isEmpty()) preview.grab().save(output + "/" + stem + ".png");
                if (kind == Kind::Reverse) {
                    child<QAbstractButton>(preview, "UtilityChoice.timeMode.1")->click();
                    layoutCheck(preview);
                    if (!output.isEmpty()) preview.grab().save(output + "/" + stem + "-ms.png");
                    child<QAbstractButton>(preview, "UtilityReset")->click();
                }
                if (!compact) { preview.resize(preview.minimumSizeHint()); layoutCheck(preview); }
            }
        }
        panel.hide();
    }
    if (!output.isEmpty()) for (const auto* theme : {"dark", "light"}) {
        ThemeManager::instance().setThemeId(theme, false);
        QWidget gallery;
        auto* row = new QHBoxLayout(&gallery);
        row->setContentsMargins(12, 12, 12, 12);
        row->setSpacing(12);
        for (int i = 0; i < 3; ++i) {
            const auto& descriptor = descriptorFor(Kind(i));
            auto* binding = new RackParameterBinding(&controller, QString::fromStdString(track), QString::fromStdString(inserts[i]), &gallery);
            auto* panel = new UtilityPanel(binding, QString::fromStdString(descriptor.uid), &gallery);
            panel->setFixedSize(panel->sizeHint());
            row->addWidget(panel, 0, Qt::AlignVCenter);
        }
        gallery.show();
        QApplication::processEvents();
        gallery.grab().save(output + "/gallery-" + theme + ".png");
    }
    std::puts("PASS utility UI: keyboard, numeric input, drag undo, mode controls, reset, themes and compact layouts");
}
