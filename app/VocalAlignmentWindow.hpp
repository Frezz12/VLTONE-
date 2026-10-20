#pragma once
#include <QDialog>
#include "SelectionModel.hpp"
#include "EngineController.hpp"
#include <memory>

class QLabel;
class QListWidget;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QProgressBar;
class QPushButton;
class QTimer;
class QToolButton;
class VocalAlignmentPreview;

class VocalAlignmentWindow : public QDialog {
    Q_OBJECT
public:
    VocalAlignmentWindow(daw::EngineController*, ui::SelectionModel*, QWidget* parent = nullptr);
    ~VocalAlignmentWindow() override;
    void takeSelection();
    ui::ClipSel guide() const { return m_guide; }
    const QVector<ui::ClipSel>& targets() const { return m_targets; }
signals:
    void alignmentApplied();
    void warpEditorRequested(const QString& trackId, const QString& clipId);
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void reject() override;
private:
    struct Job;
    daw::EngineController* m_controller;
    ui::SelectionModel* m_selection;
    quint64 m_generation;
    ui::ClipSel m_guide;
    QVector<ui::ClipSel> m_targets;
    std::vector<daw::EngineController::VocalAlignmentClip> m_sources;
    std::shared_ptr<Job> m_job;
    std::vector<daw::alignment::AlignmentProposal> m_results;
    QLabel *m_guideLabel, *m_message, *m_detail, *m_targetsTitle, *m_previewTitle;
    VocalAlignmentPreview* m_preview;
    QListWidget* m_list;
    QComboBox* m_method;
    QSlider* m_strength;
    QSpinBox *m_tolerance, *m_shift;
    QDoubleSpinBox* m_stretch;
    QProgressBar* m_progress;
    QPushButton* m_run;
    QToolButton* m_cancel;
    QWidget *m_setup, *m_options;
    QTimer* m_timer;
    std::uint64_t m_previewRevision = 0, m_waveformRevision = 0;
    bool m_stale = false;
    QString clipName(const ui::ClipSel&) const;
    bool isAudio(const ui::ClipSel&) const;
    void setGuide(const ui::ClipSel&);
    void addTargets(const QVector<ui::ClipSel>&);
    void rebuildList();
    void refreshPreview();
    void showResults();
    void resetSources();
    void run();
    void poll();
    void cancel();
};
