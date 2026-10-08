#pragma once
#include "BackgroundPreparation.hpp"
#include "EngineController.hpp"
#include <QMessageBox>

namespace ui {
inline bool prepareAudioImport(QWidget* parent, daw::EngineController& controller, const QString& path,
                               daw::ClipMusicalAnalysisModel* tempoAnalysis = nullptr) {
    const bool cached = controller.hasPreparedAudio(path.toStdString());
    if (cached && !tempoAnalysis) return true;
    const double rate = controller.sampleRate();
    if (tempoAnalysis) *tempoAnalysis = {};
    struct Prepared {
        daw::EngineController::PreparedAudio audio;
        daw::ClipMusicalAnalysisModel analysis;
        audio::Result result = audio::Result::ok();
    };
    try {
        auto prepared = prepareInBackground<Prepared>(parent, QObject::tr("Preparing audio…"),
            [path = path.toStdString(), rate, cached,
             detectTempo = tempoAnalysis != nullptr](const auto& keepGoing) {
                Prepared prepared;
                if (!cached)
                    prepared.result = daw::EngineController::prepareAudio(path, rate, prepared.audio, keepGoing);
                if (prepared.result && detectTempo && keepGoing()) {
                    daw::analysis::MusicalAnalysisRequest request;
                    request.detectKey = false;
                    // Bound import latency and analysis memory for long recordings.
                    request.durationSeconds = 90.0;
                    daw::analysis::MusicalAnalysisResult result;
                    if (daw::analysis::analyzeAudioFile(path, request, result,
                            [&keepGoing](double, std::string_view) { return keepGoing(); }))
                        prepared.analysis = daw::analysis::toClipAnalysisModel(result, request);
                    // Analysis failure leaves a normal, manually editable import.
                }
                return prepared;
            });
        if (!prepared) return false;
        if (prepared->result && controller.sampleRate() == rate &&
            (cached ? controller.hasPreparedAudio(path.toStdString())
                    : controller.adoptPreparedAudio(std::move(prepared->audio)))) {
            if (tempoAnalysis) *tempoAnalysis = std::move(prepared->analysis);
            return true;
        }
        QMessageBox::warning(parent, QObject::tr("Import failed"),
            prepared->result ? QObject::tr("The audio device changed. Please retry the import.")
                             : QString::fromStdString(prepared->result.message()));
    } catch (const std::exception& error) {
        QMessageBox::warning(parent, QObject::tr("Import failed"), QString::fromUtf8(error.what()));
    }
    return false;
}
} // namespace ui
