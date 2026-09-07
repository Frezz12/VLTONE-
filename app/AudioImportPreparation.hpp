#pragma once
#include "BackgroundPreparation.hpp"
#include "EngineController.hpp"
#include <QMessageBox>

namespace ui {
inline bool prepareAudioImport(QWidget* parent, daw::EngineController& controller, const QString& path) {
    if (controller.hasPreparedAudio(path.toStdString())) return true;
    struct Prepared {
        daw::EngineController::PreparedAudio audio;
        audio::Result result = audio::Result::ok();
    };
    try {
        auto prepared = prepareInBackground<Prepared>(parent, QObject::tr("Preparing audio…"),
            [path = path.toStdString(), rate = controller.sampleRate()](const auto& keepGoing) {
                Prepared prepared;
                prepared.result = daw::EngineController::prepareAudio(path, rate, prepared.audio, keepGoing);
                return prepared;
            });
        if (!prepared) return false;
        if (prepared->result && controller.adoptPreparedAudio(std::move(prepared->audio))) return true;
        QMessageBox::warning(parent, QObject::tr("Import failed"),
            prepared->result ? QObject::tr("The audio device changed. Please retry the import.")
                             : QString::fromStdString(prepared->result.message()));
    } catch (const std::exception& error) {
        QMessageBox::warning(parent, QObject::tr("Import failed"), QString::fromUtf8(error.what()));
    }
    return false;
}
} // namespace ui
