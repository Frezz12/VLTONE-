#include "KnobAppearance.hpp"
#include "ColorKnob.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "SettingsWindow.hpp"
#include "Theme.hpp"
#include "graphics/SceneRecorder.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopeGuard>
#include <QSettings>
#include <QVBoxLayout>
#include <array>
#include <cstdio>

namespace ui {
bool checkKnobAppearanceForTest(const QString& screenshotDirectory) {
    bool ok = true;
    const auto check = [&](bool result, const char* label) {
        std::printf("%s Knob appearance: %s\n", result ? "PASS" : "FAIL", label);
        ok &= result;
        return result;
    };
    const auto previousDesign = knobDesign();
    const auto previousPreference = QSettings().value("ui/knobDesign");
    using FaderDesign = FaderWidget::TrackHeaderDesign;
    const auto previousFaderDesign = FaderWidget::trackHeaderDesign();
    const auto previousFaderPreference = QSettings().value("ui/trackFaderDesign");
    const Theme previousTheme = th();
    const auto restore = qScopeGuard([&] {
        setKnobDesign(previousDesign);
        if (previousPreference.isValid()) QSettings().setValue("ui/knobDesign", previousPreference);
        else QSettings().remove("ui/knobDesign");
        FaderWidget::setTrackHeaderDesign(previousFaderDesign);
        if (previousFaderPreference.isValid()) QSettings().setValue("ui/trackFaderDesign", previousFaderPreference);
        else QSettings().remove("ui/trackFaderDesign");
        if (previousTheme.id == "custom") ThemeManager::instance().applyCustomTheme(previousTheme, false);
        else ThemeManager::instance().setThemeId(previousTheme.id, false);
    });
    daw::EngineController controller;
    if (!controller.initialize(48000, 512, false)) return false;
    setKnobDesign(KnobDesign::Classic);
    FaderWidget::setTrackHeaderDesign(FaderDesign::Classic);
    QWidget gallery;
    gallery.setAutoFillBackground(true);
    auto* column = new QVBoxLayout(&gallery);
    column->setContentsMargins(24, 24, 24, 24);
    column->setSpacing(24);
    auto* title = new QLabel(QStringLiteral("VLTONE · Knobs and volume faders"), &gallery);
    column->addWidget(title);
    auto* top = new QHBoxLayout;
    auto* hero = new Knob({}, &gallery);
    hero->setBare(144); hero->setValue(.5);
    top->addWidget(hero);
    top->addStretch();
    QVector<QWidget*> controls;
    std::array<PanKnob*, 3> pans{};
    for (int i = 0; i < 3; ++i) {
        auto* cell = new QVBoxLayout;
        pans[i] = new PanKnob(&gallery);
        pans[i]->setPan(i - 1.0);
        cell->addWidget(pans[i], 0, Qt::AlignCenter);
        cell->addWidget(new QLabel(i == 0 ? "L" : i == 1 ? "C" : "R"), 0, Qt::AlignCenter);
        top->addLayout(cell);
        controls.push_back(pans[i]);
    }
    auto* level = new FaderWidget(Qt::Horizontal, &gallery);
    level->setCompactKnob(true); level->setFixedSize(24, 24); level->setGain(.8);
    top->addWidget(level); controls.push_back(level);
    auto* color = new ColorKnob(QStringLiteral("Color"), &gallery);
    color->setDiameter(42); color->setValue(35);
    top->addWidget(color); controls.push_back(color);
    auto* verticalFader = new FaderWidget(Qt::Vertical, &gallery);
    verticalFader->setFixedSize(32, 144); verticalFader->setGain(.55);
    top->addWidget(verticalFader); controls.push_back(verticalFader);
    auto* horizontalFader = new FaderWidget(Qt::Horizontal, &gallery);
    horizontalFader->setFixedWidth(130); horizontalFader->setGain(1.2);
    top->addWidget(horizontalFader); controls.push_back(horizontalFader);
    top->addStretch();
    column->addLayout(top);
    auto* row = new QHBoxLayout;
    const std::array styles{Knob::VisualStyle::Standard, Knob::VisualStyle::SamplerDigital,
        Knob::VisualStyle::RackDigital, Knob::VisualStyle::Slicer,
        Knob::VisualStyle::Graphite, Knob::VisualStyle::Gravity};
    const std::array names{"Standard", "Sampler", "Rack", "Slicer", "Graphite", "Gravity"};
    for (int i = 0; i < int(styles.size()); ++i) {
        auto* cell = new QVBoxLayout;
        auto* knob = new Knob(QString::fromLatin1(names[i]), &gallery);
        knob->setVisualStyle(styles[i]); knob->setRange(0, 1); knob->setValue(.65);
        if (i >= 4) knob->setBare(64);
        cell->addWidget(knob, 0, Qt::AlignCenter);
        cell->addWidget(new QLabel(QString::fromLatin1(names[i])), 0, Qt::AlignCenter);
        row->addLayout(cell); controls.push_back(knob);
    }
    column->addLayout(row);
    auto* trackFader = new FaderWidget(Qt::Horizontal, &gallery);
    trackFader->setTrackHeaderStyle(true);
    trackFader->setFixedWidth(140);
    trackFader->setGain(.8);
    column->addWidget(trackFader, 0, Qt::AlignRight);
    gallery.resize(680, 340); gallery.show(); QApplication::processEvents();
    const auto save = [&](QWidget& widget, const QString& name) {
        if (screenshotDirectory.isEmpty()) return;
        QDir().mkpath(screenshotDirectory);
        QApplication::processEvents();
        check(widget.grab().save(QDir(screenshotDirectory).filePath(name + ".png")), "save visual reference");
    };
    QVector<QImage> classic;
    for (auto* widget : controls) classic.push_back(widget->grab().toImage());
    const auto retainedCap = [](FaderWidget* fader) {
        graphics::SceneRecorder recorder(fader->size(), 2.0);
        { QPainter p(&recorder); p.setFont(fader->font()); fader->paintScene(p, fader->rect()); }
        if (!recorder.supported()) return QImage();
        for (const auto& mesh : recorder.takeMeshes()) {
            if (!mesh.group) continue;
            for (const auto& part : mesh.group->meshes)
                if (!part.texture.isNull()) return part.texture;
        }
        return QImage();
    };
    const auto classicVerticalCap = retainedCap(verticalFader);
    const auto classicHorizontalCap = retainedCap(horizontalFader);
    check(!classicVerticalCap.isNull() && !classicHorizontalCap.isNull(), "GPU faders produce cached handle assets");
    save(gallery, "knobs-classic");
    int edits = 0;
    QObject::connect(trackFader, &FaderWidget::gainChanged, &gallery, [&] { ++edits; });
    for (auto* widget : controls) {
        if (auto* knob = qobject_cast<Knob*>(widget))
            QObject::connect(knob, &Knob::valueChanged, &gallery, [&] { ++edits; });
        if (auto* pan = qobject_cast<PanKnob*>(widget))
            QObject::connect(pan, &PanKnob::panChanged, &gallery, [&] { ++edits; });
        if (auto* fader = qobject_cast<FaderWidget*>(widget))
            QObject::connect(fader, &FaderWidget::gainChanged, &gallery, [&] { ++edits; });
    }
    {
        SettingsWindow settings(&controller, nullptr);
        settings.showTab(SettingsWindow::kThemesTab); settings.show();
        QApplication::processEvents();
        auto* selector = settings.findChild<QComboBox*>("KnobDesignSelector");
        if (!check(selector && selector->count() == 2, "design choices are available in Themes settings")) return false;
        check(selector->currentData().toInt() == int(KnobDesign::Classic), "opening settings preserves the current design");
        selector->setCurrentIndex(selector->findData(int(KnobDesign::BrushedMetal)));
        QApplication::processEvents();
        check(knobDesign() == KnobDesign::BrushedMetal, "selection immediately changes existing controls");
        QSettings prefs; prefs.sync();
        check(prefs.value("ui/knobDesign").toString() == "brushed-metal", "design is persisted for the next launch");
        bool expectedAppearance = true;
        for (int i = 0; i < controls.size(); ++i) {
            const bool round = controls[i] != verticalFader && controls[i] != horizontalFader;
            expectedAppearance &= (controls[i]->grab().toImage() != classic[i]) == round;
        }
        check(expectedAppearance, "only round controls change; linear volume faders retain their original face");
        const auto metalVerticalCap = retainedCap(verticalFader);
        const auto metalHorizontalCap = retainedCap(horizontalFader);
        check(!metalVerticalCap.isNull() && !metalHorizontalCap.isNull() &&
              metalVerticalCap == classicVerticalCap && metalHorizontalCap == classicHorizontalCap,
              "GPU volume faders also keep their original material with metal knobs enabled");
        check(edits == 0 && pans[0]->pan() == -1 && pans[1]->pan() == 0 &&
              pans[2]->pan() == 1 && level->gain() == .8 && hero->value() == .5 &&
              verticalFader->gain() == .55 && horizontalFader->gain() == 1.2,
              "appearance changes preserve values and emit no parameter edits");
        save(settings, "settings-metal");
        if (auto* group = settings.findChild<QGroupBox*>("KnobDesignGroup")) save(*group, "setting-detail");
    }
    {
        const auto classicTrack = trackFader->grab().toImage();
        const auto classicTrackCap = retainedCap(trackFader);
        SettingsWindow settings(&controller, nullptr);
        settings.showTab(SettingsWindow::kThemesTab); settings.show();
        QApplication::processEvents();
        auto* selector = settings.findChild<QComboBox*>("TrackFaderDesignSelector");
        if (!check(selector && selector->count() == 2, "timeline fader designs are available in Themes")) return false;
        check(selector->currentData().toInt() == int(FaderDesign::Classic),
              "opening settings preserves the timeline fader design");
        selector->setCurrentIndex(selector->findData(int(FaderDesign::Recessed)));
        QApplication::processEvents();
        QSettings prefs; prefs.sync();
        check(FaderWidget::trackHeaderDesign() == FaderDesign::Recessed &&
              prefs.value("ui/trackFaderDesign").toString() == "recessed" &&
              trackFader->grab().toImage() != classicTrack,
              "timeline fader choice updates existing controls and persists");
        const auto recessedCap = retainedCap(trackFader);
        check(!recessedCap.isNull() && recessedCap != classicTrackCap,
              "changing timeline fader design refreshes the GPU handle cache");
        check(retainedCap(verticalFader) == classicVerticalCap &&
              retainedCap(horizontalFader) == classicHorizontalCap,
              "timeline preference leaves other linear faders unchanged");
        save(settings, "settings-faders");
        if (auto* group = settings.findChild<QGroupBox*>("TrackFaderDesignGroup")) save(*group, "fader-setting-detail");

        trackFader->setCompactKnob(true);
        const auto compact = trackFader->grab().toImage();
        selector->setCurrentIndex(selector->findData(int(FaderDesign::Classic)));
        QApplication::processEvents();
        check(trackFader->size() == QSize(24, 24) && trackFader->grab().toImage() == compact,
              "switching linear designs preserves the compact round control");
        trackFader->setCompactKnob(false);
        QApplication::processEvents();
        check(trackFader->grab().toImage() == classicTrack &&
              retainedCap(trackFader) == classicTrackCap,
              "classic timeline fader is restored after compact mode");
        selector->setCurrentIndex(selector->findData(int(FaderDesign::Recessed)));
        check(trackFader->gain() == .8 && edits == 0,
              "timeline design changes preserve gain and emit no edits");
    }
    {
        SettingsWindow reopened(&controller, nullptr);
        reopened.showTab(SettingsWindow::kThemesTab);
        auto* selector = reopened.findChild<QComboBox*>("KnobDesignSelector");
        check(selector && selector->currentData().toInt() == int(KnobDesign::BrushedMetal),
              "reopened settings retain the selected design");
        auto* faderSelector = reopened.findChild<QComboBox*>("TrackFaderDesignSelector");
        check(faderSelector && faderSelector->currentData().toInt() == int(FaderDesign::Recessed),
              "reopened settings retain the selected timeline fader design");
    }
    save(gallery, "knobs-metal-dark");
    const auto dark = hero->grab().toImage();
    for (const auto& preset : ThemeManager::instance().presets()) {
        if (!preset.dark) {
            ThemeManager::instance().setThemeId(preset.id, false);
            break;
        }
    }
    QApplication::processEvents();
    check(knobDesign() == KnobDesign::BrushedMetal && hero->grab().toImage() != dark,
          "metal adapts to a light palette without resetting the selected design");
    save(gallery, "knobs-metal-light");
    check(PanKnob::checkInteractionForTest(), "metal pan preserves drag, keyboard and double-click reset");
    setKnobDesign(KnobDesign::Classic);
    check(QSettings().value("ui/knobDesign").toString() == "classic", "classic design can be restored");
    return ok;
}
}
