#include "ContextPanelPage.hpp"

#include "ContextPanel.hpp"
#include "Controls.hpp"
#include "PluginFormatPreference.hpp"

#include "Host/PluginInstance.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

#include <QString>

ContextPanelPage::ContextPanelPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(16, 16, 16, 16);
    column->setSpacing(8);

    auto* intro = new QLabel(
        tr("Choose which groups the context panel shows for each kind of "
           "object. Hidden groups stay available everywhere else."),
        this);
    intro->setWordWrap(true);
    column->addWidget(intro);

    QSettings settings;
    QString heading;
    for (const ContextTool& tool : contextPanelTools()) {
        const QString context = QCoreApplication::translate(
            "ContextPanelTools", tool.context);
        if (context != heading) {
            heading = context;
            column->addSpacing(6);
            column->addWidget(ui::sectionLabel(context.toUpper(), this));
        }

        const QString key = QStringLiteral("contextPanel/%1")
                                .arg(QLatin1String(tool.id));
        auto* check = new QCheckBox(QCoreApplication::translate(
                                        "ContextPanelTools", tool.label),
                                    this);
        check->setChecked(settings.value(key, true).toBool());
        connect(check, &QCheckBox::toggled, this, [this, key](bool on) {
            QSettings().setValue(key, on);
            emit changed();
        });
        column->addWidget(check);
    }

    column->addSpacing(10);
    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("POSITION"), this));

    auto* follow = new QCheckBox(tr("Follow the selection along the timeline"), this);
    follow->setToolTip(
        tr("The panel rides above the selected clip, or above the middle of a "
           "group of them. Switched off, it stays centred in the strip."));
    follow->setChecked(
        settings.value("contextPanel/followSelection", true).toBool());
    connect(follow, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("contextPanel/followSelection", on);
        emit changed();
    });
    column->addWidget(follow);

    column->addSpacing(10);
    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("APPEARANCE"), this));

    auto* reduce = new QCheckBox(tr("Reduce transparency"), this);
    reduce->setToolTip(
        tr("Draw the panel as a flat surface instead of blurring the "
           "arrangement behind it."));
    reduce->setChecked(ui::GlassPanel::reduceTransparency());
    connect(reduce, &QCheckBox::toggled, this, [this](bool on) {
        ui::GlassPanel::setReduceTransparency(on);
        emit changed();
    });
    column->addWidget(reduce);

    auto* reduceMotion = new QCheckBox(tr("Reduce plugin-search motion"), this);
    reduceMotion->setToolTip(
        tr("Open and close the plugin search immediately and disable its idle glow."));
    reduceMotion->setChecked(settings.value("ui/reduceMotion", false).toBool());
    connect(reduceMotion, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("ui/reduceMotion", on);
        emit changed();
    });
    column->addWidget(reduceMotion);

    column->addSpacing(10);
    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("PLUGIN SEARCH"), this));

    auto* pluginForm = new QFormLayout;
    pluginForm->setContentsMargins(0, 0, 0, 0);
    pluginForm->setHorizontalSpacing(12);
    pluginForm->setVerticalSpacing(8);

    auto addChoice = [this, &settings, pluginForm](
                         const QString& label, const QString& key,
                         const QList<QPair<QString, QString>>& choices,
                         const QString& fallback) {
        auto* combo = new QComboBox(this);
        for (const auto& [text, value] : choices) combo->addItem(text, value);
        const QString saved = settings.value(key, fallback).toString();
        const int selected = combo->findData(saved);
        combo->setCurrentIndex(selected >= 0 ? selected : 0);
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, combo, key](int index) {
                    QSettings().setValue(key, combo->itemData(index));
                    emit changed();
                });
        pluginForm->addRow(label, combo);
    };

    addChoice(tr("Default insert position"),
              QStringLiteral("contextPanel/pluginInsertPosition"),
              {{tr("End of chain"), QStringLiteral("end")},
               {tr("Start of chain"), QStringLiteral("start")},
               {tr("After selected plugin"), QStringLiteral("after")}},
              QStringLiteral("end"));
    addChoice(tr("Group the plugin menu by"),
              QStringLiteral("plugins/menuGrouping"),
              {{tr("Manufacturer"), QStringLiteral("vendor")},
               {tr("Kind of plugin"), QStringLiteral("category")}},
              QStringLiteral("vendor"));

    QList<QPair<QString, QString>> formatChoices;
    auto addFormatChoice = [&formatChoices](daw::plugins::Format format,
                                            const QString& label) {
        if (!daw::plugins::factoryFor(format)) return;
        formatChoices.push_back({label, ui::pluginFormatSettingValue(format)});
    };
#if defined(__APPLE__)
    addFormatChoice(daw::plugins::Format::AudioUnit, tr("Audio Unit (AU)"));
    addFormatChoice(daw::plugins::Format::Vst3, QStringLiteral("VST3"));
    addFormatChoice(daw::plugins::Format::Vst, QStringLiteral("VST"));
    addFormatChoice(daw::plugins::Format::Clap, QStringLiteral("CLAP"));
#else
    addFormatChoice(daw::plugins::Format::Vst3, QStringLiteral("VST3"));
    addFormatChoice(daw::plugins::Format::Vst, QStringLiteral("VST"));
    addFormatChoice(daw::plugins::Format::Clap, QStringLiteral("CLAP"));
    addFormatChoice(daw::plugins::Format::AudioUnit, tr("Audio Unit (AU)"));
#endif
    addChoice(tr("Preferred plugin format"),
              QLatin1String(ui::kPreferredPluginFormatSetting), formatChoices,
              ui::pluginFormatSettingValue(ui::defaultPreferredPluginFormat()));
    addChoice(tr("Search scope"),
              QStringLiteral("contextPanel/pluginSearchScope"),
              {{tr("All Plugins"), QStringLiteral("all")},
               {tr("Favorites Only"), QStringLiteral("favorites")},
               {tr("Instruments Only"), QStringLiteral("instruments")},
               {tr("Effects Only"), QStringLiteral("effects")}},
              QStringLiteral("all"));
    addChoice(tr("Glass animation speed"),
              QStringLiteral("contextPanel/pluginAnimationSpeed"),
              {{tr("Fast (150 ms)"), QStringLiteral("fast")},
               {tr("Normal (250 ms)"), QStringLiteral("normal")},
               {tr("Slow (400 ms)"), QStringLiteral("slow")}},
              QStringLiteral("normal"));
    column->addLayout(pluginForm);

    auto* formatHint = new QLabel(
        tr("When the same plugin exists in several formats, only the preferred "
           "variant is shown. A plugin that exists only as VST3, VST, AU, or CLAP "
           "always remains available."),
        this);
    formatHint->setWordWrap(true);
    formatHint->setObjectName(QStringLiteral("SettingsHint"));
    column->addWidget(formatHint);

    auto* thumbnails = new QCheckBox(tr("Show plugin thumbnails when available"), this);
    thumbnails->setChecked(
        settings.value("contextPanel/pluginThumbnails", true).toBool());
    connect(thumbnails, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("contextPanel/pluginThumbnails", on);
        emit changed();
    });
    column->addWidget(thumbnails);

    auto* preview = new QCheckBox(tr("Enable Quick Preview"), this);
    preview->setToolTip(
        tr("Unavailable until the audio engine can create and retire a "
           "temporary plugin instance without interrupting playback."));
    preview->setChecked(false);
    preview->setEnabled(false);
    column->addWidget(preview);

    column->addStretch(1);
}
