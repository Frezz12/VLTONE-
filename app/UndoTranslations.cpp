#include "UndoTranslations.hpp"

#include <QCoreApplication>

namespace ui {

namespace {

[[maybe_unused]] const char* const kUndoCommandNames[] = {
    QT_TRANSLATE_NOOP("UndoCommands", "Add Tracks from Template"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Tempo"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Key"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Pattern Instrument"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Pattern Sample"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Track"),
    QT_TRANSLATE_NOOP("UndoCommands", "Remove Track"),
    QT_TRANSLATE_NOOP("UndoCommands", "Rename Track"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Volume"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Pan"),
    QT_TRANSLATE_NOOP("UndoCommands", "Duplicate Track"),
    QT_TRANSLATE_NOOP("UndoCommands", "Relink Pattern Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Duplicate Pattern"),
    QT_TRANSLATE_NOOP("UndoCommands", "Move Track"),
    QT_TRANSLATE_NOOP("UndoCommands", "Folder Summing"),
    QT_TRANSLATE_NOOP("UndoCommands", "Pack into Folder"),
    QT_TRANSLATE_NOOP("UndoCommands", "Reorder Sampler FX"),
    QT_TRANSLATE_NOOP("UndoCommands", "Reorder Clip FX"),
    QT_TRANSLATE_NOOP("UndoCommands", "Paste Plugins"),
    QT_TRANSLATE_NOOP("UndoCommands", "Paste Channel Strip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Apply Channel Strip Preset"),
    QT_TRANSLATE_NOOP("UndoCommands", "Load Sample"),
    QT_TRANSLATE_NOOP("UndoCommands", "Load Sampler with File"),
    QT_TRANSLATE_NOOP("UndoCommands", "Clear Sample"),
    QT_TRANSLATE_NOOP("UndoCommands", "Import Audio to New Track"),
    QT_TRANSLATE_NOOP("UndoCommands", "Import Audio"),
    QT_TRANSLATE_NOOP("UndoCommands", "Split Pattern Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Remove Pattern Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Remove Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Mute Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Rename Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Duplicate Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Instrument"),
    QT_TRANSLATE_NOOP("UndoCommands", "Extend Pattern Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add MIDI Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Note"),
    QT_TRANSLATE_NOOP("UndoCommands", "Remove Note"),
    QT_TRANSLATE_NOOP("UndoCommands", "Import MIDI File"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Automation Lane"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Automation Clip"),
    QT_TRANSLATE_NOOP("UndoCommands", "Retarget Automation"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Controller Lane"),
    QT_TRANSLATE_NOOP("UndoCommands", "Remove Controller Lane"),
    QT_TRANSLATE_NOOP("UndoCommands", "Retarget Lane"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Master Volume"),
    QT_TRANSLATE_NOOP("UndoCommands", "Set Master Pan"),
    QT_TRANSLATE_NOOP("UndoCommands", "Track Recording Mode"),
    QT_TRANSLATE_NOOP("UndoCommands", "Record"),
    QT_TRANSLATE_NOOP("UndoCommands", "Comp"),
    QT_TRANSLATE_NOOP("UndoCommands", "Select Take"),
    QT_TRANSLATE_NOOP("UndoCommands", "Duplicate Take"),
    QT_TRANSLATE_NOOP("UndoCommands", "Add Take"),
    QT_TRANSLATE_NOOP("UndoCommands", "Delete Take"),
    QT_TRANSLATE_NOOP("UndoCommands", "Flatten Comp"),
    QT_TRANSLATE_NOOP("UndoCommands", "Commit Comp"),
    QT_TRANSLATE_NOOP("UndoCommands", "Delete Unused Takes"),
    QT_TRANSLATE_NOOP("UndoCommands", "Edit Notes"),
    QT_TRANSLATE_NOOP("UndoCommands", "Change Note Velocity"),
    QT_TRANSLATE_NOOP("UndoCommands", "Change Note Pan"),
    QT_TRANSLATE_NOOP("UndoCommands", "Change Note Length"),
    QT_TRANSLATE_NOOP("UndoCommands", "Delete Notes"),
    QT_TRANSLATE_NOOP("UndoCommands", "Show Automation"),
    QT_TRANSLATE_NOOP("UndoCommands", "Analyze Audio Clip"),
};

QString withDynamicName(const QString& source, const QString& prefix,
                        const char* pattern) {
    if (!source.startsWith(prefix) || source.size() == prefix.size()) return {};
    return QCoreApplication::translate("UndoCommands", pattern)
        .arg(source.mid(prefix.size()));
}

} // namespace

QString translatedUndoLabel(const std::string& label) {
    const QString source = QString::fromStdString(label);
    const QByteArray utf8 = source.toUtf8();
    const QString direct = QCoreApplication::translate(
        "UndoCommands", utf8.constData());
    if (direct != source) return direct;

    const struct { const char* prefix; const char* pattern; } dynamic[] = {
        {"Add Sampler FX ", QT_TRANSLATE_NOOP("UndoCommands", "Add Sampler FX %1")},
        {"Remove Sampler FX ", QT_TRANSLATE_NOOP("UndoCommands", "Remove Sampler FX %1")},
        {"Replace Sampler FX ", QT_TRANSLATE_NOOP("UndoCommands", "Replace Sampler FX %1")},
        {"Add Clip FX ", QT_TRANSLATE_NOOP("UndoCommands", "Add Clip FX %1")},
        {"Remove Clip FX ", QT_TRANSLATE_NOOP("UndoCommands", "Remove Clip FX %1")},
        {"Replace Clip FX ", QT_TRANSLATE_NOOP("UndoCommands", "Replace Clip FX %1")},
        {"Automate ", QT_TRANSLATE_NOOP("UndoCommands", "Automate %1")},
        {"Add ", QT_TRANSLATE_NOOP("UndoCommands", "Add %1")},
        {"Remove ", QT_TRANSLATE_NOOP("UndoCommands", "Remove %1")},
        {"Reorder ", QT_TRANSLATE_NOOP("UndoCommands", "Reorder %1")},
        {"Replace ", QT_TRANSLATE_NOOP("UndoCommands", "Replace %1")},
    };
    for (const auto& item : dynamic) {
        const QString translated = withDynamicName(
            source, QString::fromLatin1(item.prefix), item.pattern);
        if (!translated.isEmpty()) return translated;
    }
    const QString changePrefix = QStringLiteral("Change ");
    const struct { const char* suffix; const char* pattern; } changes[] = {
        {" Channel Mode", QT_TRANSLATE_NOOP(
             "UndoCommands", "Change %1 Channel Mode")},
        {" Sidechain", QT_TRANSLATE_NOOP(
             "UndoCommands", "Change %1 Sidechain")},
    };
    for (const auto& item : changes) {
        const QString suffix = QString::fromLatin1(item.suffix);
        if (!source.startsWith(changePrefix) || !source.endsWith(suffix))
            continue;
        const QString name = source.mid(
            changePrefix.size(),
            source.size() - changePrefix.size() - suffix.size());
        return QCoreApplication::translate("UndoCommands", item.pattern)
            .arg(name);
    }
    return source;
}

} // namespace ui
