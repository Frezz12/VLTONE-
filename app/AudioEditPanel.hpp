#pragma once
#include "EngineController.hpp"
#include "UiFrameClock.hpp"
#include "graphics/ScenePaintSource.hpp"
#include <QWidget>
#include <QColor>
#include <functional>

class QDoubleSpinBox;
class QLabel;
class QScrollBar;
class QTimer;
class QAction;
class QPushButton;
class QScrollArea;
namespace ui { class Knob; }

struct AudioEditPeaks {
    struct Pair { float low=0, high=0; };
    using Level=std::vector<Pair>;
    std::vector<std::vector<Level>> channels;
    static std::shared_ptr<const AudioEditPeaks> build(const daw::engine::SampleBuffer&, const daw::audioedit::Continue& = {});
};

class AudioEditCanvas final : public ui::FrameWidget, public ui::graphics::ScenePaintSource {
    Q_OBJECT
public:
    enum class Tool { Select, Move, Split, Slip };
    explicit AudioEditCanvas(QWidget* parent=nullptr);
    void setAudio(const daw::AudioEditDocument&,std::shared_ptr<const daw::engine::SampleBuffer>,std::shared_ptr<const AudioEditPeaks>);
    void setSelection(daw::AudioEditFrame first,daw::AudioEditFrame last);
    void fit(bool selection=false);
    void showRange(daw::AudioEditFrame first, daw::AudioEditFrame last);
    void zoom(double factor,double anchor=-1);
    void setTool(Tool);
    Tool tool() const { return m_tool; }
    void setClipColor(const QColor&);
    void setPlayhead(double frame);
    double playhead() const { return m_playhead; }
    void setEditingEnabled(bool);
    void setAmplitude(double);
    void scrollTo(double);
    double scroll() const { return m_scroll; }
    double span() const { return width()*m_framesPerPixel; }
    daw::AudioEditFrame first() const {return m_first;}
    daw::AudioEditFrame last() const {return m_last;}
    daw::AudioEditFrame cursor() const {return m_cursor;}
    void cancelGesture();
signals:
    void selectionChanged();
    void viewChanged();
    void splitRequested(qint64);
    void moveRequested(const QString&,qint64);
    void slipRequested(double);
    void fadeRequested(bool fadeIn, qint64 length, double curve);
    void editActionRequested(const QString&);
    void toolChanged(AudioEditCanvas::Tool);
protected:
    void paintEvent(QPaintEvent*) override;
    void paintScene(QPainter&,const QRegion&) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    daw::AudioEditFrame frame(double x) const;
    double x(daw::AudioEditFrame frame) const;
    void drawWave(QPainter&,int channel,double center,double height);
    const daw::AudioEditRegion* regionAt(daw::AudioEditFrame) const;
    void updatePointer(const QPointF&);
    daw::AudioEditDocument m_doc;
    std::shared_ptr<const daw::engine::SampleBuffer> m_audio;
    std::shared_ptr<const AudioEditPeaks> m_peaks;
    Tool m_tool=Tool::Select;
    QColor m_clipColor;
    double m_playhead=-1;
    double m_scroll=0,m_framesPerPixel=1,m_amplitude=1;
    daw::AudioEditFrame m_first=0,m_last=0,m_cursor=0,m_anchor=0,m_pressFrame=0,m_moveStart=0,m_moveLength=0;
    std::string m_region;
    int m_drag=0;
    bool m_initial=true,m_editingEnabled=true,m_dragMoved=false;
    daw::AudioEditFrame m_savedFirst=0,m_savedLast=0,m_savedCursor=0;
    daw::AudioEditFrame m_fadeFrames=1;
    double m_fadeCurve=0,m_pressY=0,m_pressX=0,m_savedScroll=0,m_slipDelta=0;
};

class AudioEditPanel final : public QWidget {
    Q_OBJECT
public:
    AudioEditPanel(daw::EngineController*,daw::EngineController::AudioEditTarget,QWidget* parent=nullptr);
    ~AudioEditPanel() override;
    void refresh();
    void triggerAction(const QString& id) { action(id); }
    static bool checkForTest();
signals:
    void projectEdited();
    void liveEdited();
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    using Edit=std::function<bool(daw::AudioEditDocument&)>;
    void action(const QString&);
    void edit(const QString&,Edit);
    void prepare(daw::AudioEditDocument,const QString& label,bool commit);
    void syncSelection();
    void finishGain();
    void cancelPending();
    void updateActions();
    void splitAt(daw::AudioEditFrame);
    void refreshPlayhead();
    void updateAppearance();
    bool currentRevision();
    daw::EngineController* m_controller;
    daw::EngineController::AudioEditTarget m_target;
    AudioEditCanvas* m_canvas=nullptr;
    QScrollBar* m_scroll=nullptr;
    QLabel* m_status=nullptr;
    QLabel* m_links=nullptr;
    QLabel* m_selectionInfo=nullptr;
    QLabel* m_hint=nullptr;
    QScrollArea* m_inspector=nullptr;
    QPushButton* m_flatten=nullptr;
    QDoubleSpinBox *m_start=nullptr,*m_end=nullptr,*m_length=nullptr,*m_fadeLength=nullptr,*m_curve=nullptr,*m_normalize=nullptr,*m_gainValue=nullptr;
    ui::Knob* m_gain=nullptr;
    QTimer* m_poll=nullptr;
    ui::FrameTimer* m_frameTimer=nullptr;
    std::vector<QAction*> m_rangeActions;
    std::vector<QAction*> m_editActions;
    QAction *m_undo=nullptr,*m_redo=nullptr,*m_listen=nullptr,*m_stop=nullptr;
    daw::AudioEditDocument m_doc;
    std::optional<daw::AudioEditDocument> m_gainBase;
    std::shared_ptr<const daw::engine::SampleBuffer> m_audio;
    std::shared_ptr<const AudioEditPeaks> m_peaks;
    std::shared_ptr<std::atomic<std::uint64_t>> m_generation;
    double m_scrollExtent=1;
    double m_auditionStart=0;
    std::string m_auditionPath;
    bool m_busy=false,m_sync=false,m_gainFinished=false,m_auditionPending=false;
};
