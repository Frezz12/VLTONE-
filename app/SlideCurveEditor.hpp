#pragma once
#include "model/Document.hpp"
#include <QDialog>
#include <functional>

class SlideCurveCanvas;
class SlideCurveEditor final : public QDialog {
    Q_OBJECT
  public:
    explicit SlideCurveEditor(daw::SlideNoteModel slide, double initialPitch,
                              QWidget *parent = nullptr);
    // Preview is temporary. Commit receives the complete result of one gesture.
    std::function<void(const daw::SlideNoteModel &, bool)> changed;
    std::function<void()> extendBase, audition, rebind;
    void setBendRange(double semitones, double basePitch);
    bool editing() const;
    void refresh(const daw::SlideNoteModel &, double initialPitch);

  protected:
    void reject() override;
    void closeEvent(QCloseEvent *) override;

  private:
    SlideCurveCanvas *m_canvas = nullptr;
    std::function<void()> m_sync;
};
