#pragma once

#include "MidiTools.hpp"
#include "ai/CompositionEngine.hpp"
#include <QDialog>
#include <array>
#include <future>
#include <memory>

namespace daw { class EngineController; }
class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QTimer;
class MidiGeneratorPreview;

/// Owns copied generation input and candidates. The live project is only edited
/// by Apply; the worker never touches a controller, plugin or QWidget.
class MidiGeneratorDialog final : public QDialog {
    Q_OBJECT
public:
    MidiGeneratorDialog(daw::EngineController&, std::string trackId,
                        std::string clipId, double startBeats, QWidget* parent = nullptr);
    ~MidiGeneratorDialog() override;
    void done(int result) override;
    static bool checkForTest(const QString& screenshotDirectory);

protected:
    void hideEvent(QHideEvent*) override;

signals:
    void applied();

private:
    const daw::ClipModel* target() const;
    bool contextMatches() const;
    void scheduleGeneration();
    void generate();
    void poll();
    void selectCandidate(int index);
    void refreshAvailability();
    void toggleListen();
    void stopListening();
    void apply();
    void style();

    daw::EngineController& m_controller;
    std::string m_trackId, m_clipId;
    double m_startBeats = 0, m_clipStart = 0, m_clipDuration = 0, m_clipOffset = 0;
    double m_contentOffset = 0, m_tempo = 120;
    std::uint64_t m_revision = 0, m_serial = 0, m_jobSerial = 0;
    std::string m_takeId;
    daw::miditools::Notes m_baseNotes;
    std::vector<daw::SlideNoteModel> m_baseSlides;
    daw::ai::CompositionRequest m_request;
    std::future<std::vector<daw::ai::CompositionCandidate>> m_job;
    std::vector<daw::ai::CompositionCandidate> m_candidates;
    std::array<daw::miditools::Notes, 3> m_prepared;
    std::vector<std::pair<std::string, std::string>> m_harmonySources;
    daw::miditools::Notes m_harmonyNotes;
    std::pair<std::string, std::string> m_harmonySource;
    double m_harmonyStart = 0, m_harmonyOffset = 0, m_harmonyDuration = 0;
    bool m_pending = false, m_closed = false, m_listening = false;
    int m_selected = -1;
    std::shared_ptr<daw::EngineController> m_draft;

    QComboBox *m_mode, *m_key, *m_scale, *m_character, *m_harmony;
    QComboBox *m_low, *m_high, *m_ending, *m_chordRate, *m_voicing, *m_insert;
    QSpinBox *m_bars, *m_swing, *m_expression, *m_seed;
    QSlider *m_density, *m_creativity;
    QLabel* m_status;
    QPushButton *m_generate, *m_listen, *m_apply;
    std::array<QRadioButton*, 3> m_variants{};
    MidiGeneratorPreview* m_preview;
    QTimer *m_debounce, *m_poll;
};
