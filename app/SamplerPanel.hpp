#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QTimer;
class QVBoxLayout;

namespace ui {
class Knob;
class Led;
class IconButton;
class FaderWidget;
class LevelMeter;
class PanKnob;
}

namespace daw {
class EngineController;
namespace plugins::sampler {
class SamplerInstance;
struct SampleData;
}
} // namespace daw

class SamplerEnvelopeView;
class SamplerKeyboard;

/// The waveform strip at the top of the sampler: the sample as it will be
/// heard, with the start offset, the loop points and the fades drawn on it and
/// draggable.
///
/// It reads the *processed* sample — the one the precomputed effects were baked
/// into — because that is what plays, and a reverse or a reverb that the
/// display did not follow would make the markers point at the wrong place.
class SamplerWaveform : public QWidget {
    Q_OBJECT
public:
    explicit SamplerWaveform(QWidget* parent = nullptr);

    /// Called whenever the panel refreshes — five times a second while the
    /// window is open, so it must cost nothing when nothing moved. It repaints
    /// only on a real change, and rescans the audio only when the buffer itself
    /// is replaced.
    void setSample(std::shared_ptr<const daw::plugins::sampler::SampleData> sample);
    void setMarkers(double startOffset, double endOffset, double loopStart,
                    double loopEnd, int loopMode, double fadeIn, double fadeOut);

signals:
    /// A marker was dragged. Ids are the sampler's own parameter ids.
    void markerMoved(const QString& parameterId, double value);
    /// The drag finished — one undo entry per gesture, like every other control.
    void markerReleased(const QString& parameterId);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    /// Which marker a press at `x` grabs, or an empty string for none.
    QString markerAt(int x) const;
    void rebuildPeaks();
    /// Fraction 0…1 of the *base* sample → x. The reverb tail sits past the
    /// right edge of the marker area, so the two mappings differ.
    double xForFraction(double fraction) const;
    double fractionForX(int x) const;

    std::shared_ptr<const daw::plugins::sampler::SampleData> m_sample;
    const void* m_peaksFor = nullptr;   ///< the buffer the peaks were built from
    /// The envelope at a fixed resolution rather than one entry per pixel.
    /// Scanning the whole sample is O(its length), and tying that to the
    /// widget's width meant every step of a window resize re-read the audio;
    /// mapping buckets to pixels at paint time is a few thousand comparisons.
    QVector<float> m_minima;
    QVector<float> m_maxima;

    double m_startOffset = 0.0;
    double m_endOffset = 1.0;
    double m_loopStart = 0.0;
    double m_loopEnd = 1.0;
    int m_loopMode = 0;
    double m_fadeIn = 0.0;
    double m_fadeOut = 0.0;

    QString m_dragging;
};

/// The unified Sampler / audio-clip editor.
///
/// It is a plain panel rather than a window of its own so that it drops
/// straight into `PluginEditorWindow` — the sampler is a plugin like any other
/// as far as the slot, the registry and the close/orphan handling are
/// concerned, and only the contents of the window differ.
///
/// Every control is bound by stable id. Instrument context writes to the live
/// sampler instance; Clip context writes to the selected ClipModel. Both share
/// one layout and interaction code, so double-clicking timeline audio cannot
/// drift into a reduced second editor.
class SamplerPanel : public QWidget {
    Q_OBJECT
public:
    enum class Context { Instrument, Clip };

    SamplerPanel(daw::EngineController* controller, QString channelId, QString slotId,
                 QWidget* parent = nullptr);
    SamplerPanel(daw::EngineController* controller, Context context,
                 QString ownerId, QString objectId, QWidget* parent = nullptr);
    ~SamplerPanel() override;

    /// How wide the arrangement's grid is, in seconds, or 0 when snapping is
    /// off. Asked every time it is needed rather than stored, so changing the snap
    /// while the editor is open takes effect immediately. Only the clip context
    /// uses it: stretching a clip moves its end along the timeline, and that is
    /// the edge worth landing on a bar line.
    void setSnapProvider(std::function<double()> provider);

    /// Re-read the clip/instrument model, including an FX chain edited from
    /// another surface such as the Context Panel.
    void refresh();

signals:
    /// Nested editors still go through MainWindow's one-window registry.
    void pluginEditorRequested(const QString& channelId, const QString& insertId);
    /// A gesture finished: the project is dirty and one undo entry exists.
    void projectEdited();
    /// A control moved *during* a drag. Editing a clip changes what the
    /// timeline shows — its length, and with it the shape of its waveform — so
    /// the arrangement has to follow the knob rather than wait for the mouse to
    /// come up. Carries no undo and does not mark the project dirty; that is
    /// still `projectEdited`'s job at the end of the gesture.
    void liveEdited();
    /// A parameter knob requested automation in the *instrument* context: make
    /// the automation lane for it. Never emitted for a clip's own FX — a clip
    /// insert is not addressable as an automation target yet, and offering the
    /// gesture where it cannot work is worse than not offering it.
    void automationRequested(const QString& parameterId);

private:
    daw::plugins::sampler::SamplerInstance* sampler() const;
    std::shared_ptr<const daw::plugins::sampler::SampleData> currentSample();

    QWidget* buildFxStrip();
    QWidget* buildSamplerBody();
    QWidget* buildEnvelopeSection();
    QWidget* buildToolSection();
    QWidget* buildWaveformSection();
    void rebuildFxSlots();
    void showFxMenu(int index, const QString& replaceId = {});
    void showFxContext(const QString& insertId, int index, const QPoint& globalPos);

    /// A knob wired to a parameter: range, default, formatter and both halves
    /// of the undo split all come from the parameter table.
    ui::Knob* knob(const QString& parameterId, const QString& caption,
                   bool compact = false);
    ui::Led* led(const QString& parameterId, const QString& caption);
    QComboBox* combo(const QString& parameterId, const QStringList& items);

    void applyTheme();
    void openSampleDialog();
    void revealSample();

protected:
    /// The file name is a label, and a label has no clicked signal — this is
    /// what turns it into one.
    bool eventFilter(QObject* watched, QEvent* event) override;
    /// Dropping an audio file anywhere on the panel loads it. Accepted on the
    /// panel rather than on the waveform alone: the strip is 110 px of a
    /// 600 px window, and aiming for it is the kind of precision a drop should
    /// not ask for.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    /// The poll only has to run while the panel can be seen. A sampler window
    /// left open behind the arrangement was re-reading every bound parameter
    /// five times a second to update nothing.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:

    daw::EngineController* m_controller = nullptr;
    QString m_channelId;
    QString m_slotId;
    Context m_context = Context::Instrument;

    QLabel* m_fileLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    SamplerWaveform* m_waveform = nullptr;
    SamplerEnvelopeView* m_envelope = nullptr;
    SamplerKeyboard* m_keyboard = nullptr;
    QWidget* m_fxSlotsHost = nullptr;
    QVBoxLayout* m_fxSlotsLayout = nullptr;
    ui::PanKnob* m_fxPan = nullptr;
    ui::IconButton* m_fxBypass = nullptr;
    ui::FaderWidget* m_fxVolume = nullptr;
    ui::LevelMeter* m_fxMeter = nullptr;
    QLabel* m_fxPanLabel = nullptr;
    QLabel* m_fxGainLabel = nullptr;
    QTimer* m_poll = nullptr;
    ui::Knob* m_formantKnob = nullptr;
    float m_fxGestureVolume = 1.0f;
    float m_fxGesturePan = 0.0f;
    bool m_fxLevelGesture = false;
    QString m_fxSignature;

    /// Every bound control, by parameter id, so `refresh` can walk them.
    QHash<QString, ui::Knob*> m_knobs;
    QHash<QString, ui::Led*> m_leds;
    QHash<QString, QComboBox*> m_combos;
    /// Value at the start of the gesture in flight, per parameter.
    QHash<QString, double> m_gestureStart;
    std::function<double()> m_snapProvider;

    void beginGesture(const QString& parameterId);
    void endGesture(const QString& parameterId);
    void writeParameter(const QString& parameterId, double value);
    double readParameter(const QString& parameterId);
};
