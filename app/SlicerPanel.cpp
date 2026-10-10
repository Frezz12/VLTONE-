#include "PluginStyle.hpp"
#include "ScrollMotion.hpp"
#include "SlicerPanel.hpp"
#include "AudioImportPreparation.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "FileTypes.hpp"
#include "SliceAnalysis.hpp"
#include "Theme.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QPaintEvent>
#include <QStyle>
#include <QRadialGradient>
#include <tuple>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

namespace p = daw::plugins::slicer;
namespace slicing = daw::slicer;
namespace {
constexpr char kPadMime[] = "application/x-vlt-slicer-pad";
constexpr int kPadSide = 42, kPadColumns = 8, kPadGap = 4;
QColor sliceColor(quint32 id, const Theme& theme) {
    const int hue = int((210 + quint64(id ? id - 1 : 0) * 137) % 360);
    return QColor::fromHsv(hue, theme.dark ? 96 : 128, theme.dark ? 226 : 154);
}
QString noteName(int key) {
    return QString::fromStdString(p::noteName(key));
}
QString msText(double seconds) { return seconds < 1.0 ? QString::number(seconds * 1000.0, 'f', 1) + QCoreApplication::translate("SlicerPanel", " ms") : QString::number(seconds, 'f', 2) + QCoreApplication::translate("SlicerPanel", " s"); }
QWidget* scrollPage(QWidget* content) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content); return scroll;
}
QFormLayout* form(QWidget* parent) {
    auto* layout = new QFormLayout(parent);
    layout->setContentsMargins(12, 12, 12, 12); layout->setVerticalSpacing(9);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return layout;
}
void formRow(QFormLayout* layout, const QString& text, QWidget* widget) {
    auto* label = new QLabel(text); label->setBuddy(widget); label->setWordWrap(true);
    widget->setAccessibleName(text); layout->addRow(label, widget);
}
const daw::plugins::ParameterInfo* infoFor(const QString& id) {
    for (const auto& info : p::parameterTable()) if (info.id == id.toStdString()) return &info;
    return nullptr;
}
// Local materials: the editor's controls share the same rubber housing while
// retaining the host knob's keyboard, numeric entry and accessibility support.
QColor shellColor() { return pluginStyle::shell(); }
QColor slicerAccent() { return pluginStyle::accent(); }
class SlicerSurface final : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        pluginStyle::surface(p, QRectF(rect()).adjusted(1,1,-1,-2));
    }
};
class SlicerKnob final : public ui::Knob {
public:
    SlicerKnob(const QString& caption, QWidget* parent=nullptr) : ui::Knob(caption,parent) {
        setFixedSize(80,100); setFocusPolicy(Qt::StrongFocus);
        setFormatter([this](double v) { return display ? display(v) : QString::number(v,'f',2); });
    }
    std::function<QString(double)> display;
    void setLogarithmic(bool value) { m_logarithmic=value; ui::Knob::setLogarithmic(value); }
    QRectF reliefBounds() const { return shadowRect().united(lightRect()); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const auto& t=th();
        const double fraction=m_logarithmic && minimumValue()>0
            ? std::log(value()/minimumValue())/std::log(maximumValue()/minimumValue())
            : (value()-minimumValue())/std::max(1e-9,maximumValue()-minimumValue());
        pluginStyle::knob(painter, QRectF(width()/2.-36,0,72,72), fraction,
                          isEditing(), isEnabled(), hasFocus());
        auto face=font(); face.setPixelSize(11); face.setWeight(QFont::Medium); painter.setFont(face);
        painter.setPen(isEnabled()?t.textPrimary:t.textSecondary);
        painter.drawText(QRect(2,74,width()-4,13),Qt::AlignCenter,painter.fontMetrics().elidedText(accessibleName(),Qt::ElideRight,width()-4));
        face.setPixelSize(11); face.setWeight(QFont::DemiBold); painter.setFont(face);
        painter.setPen(isEnabled()?mixColors(t.textPrimary,t.accent,.3):t.textSecondary);
        painter.drawText(QRect(2,87,width()-4,13),Qt::AlignCenter,display?display(value()):QString::number(value(),'f',2));
    }
private:
    QPointF headCenter() const { return {width()/2.0,36+(isEditing()?1.0:0.0)}; }
    QRectF shadowRect() const { const auto c=headCenter()+QPointF(3,5); return {c.x()-30,c.y()-30,60,60}; }
    QRectF lightRect() const { const auto c=headCenter()-QPointF(4,5); return {c.x()-29,c.y()-29,58,58}; }
    bool m_logarithmic=false;
};
bool copyAtomically(const QString& from, const QString& to) {
    if (QFileInfo(from).canonicalFilePath() == QFileInfo(to).canonicalFilePath() && QFileInfo(to).exists()) return true;
    QFile input(from); QSaveFile output(to);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return false;
    while (!input.atEnd()) {
        const auto data = input.read(256 * 1024);
        if (data.isEmpty() || output.write(data) != data.size()) return false;
    }
    return output.commit();
}
}

SlicerEffectPad::SlicerEffectPad(QWidget* parent) : QWidget(parent) {
    setObjectName("SlicerEffectPad"); setMinimumSize(168,130); setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Effect XY pad"));
    setToolTip(tr("Drag to shape the effect · Arrow keys adjust · Shift for precision · Esc cancels"));
    connect(&ThemeManager::instance(),&ThemeManager::changed,this,QOverload<>::of(&QWidget::update));
}
QRectF SlicerEffectPad::fieldRect() const { return QRectF(rect()).adjusted(18,30,-18,-34); }
void SlicerEffectPad::setValues(double x,double y,const QString& xLabel,const QString& yLabel,bool enabled) {
    const QPointF next(x,y);
    const bool changed=m_value!=next || m_xLabel!=xLabel || m_yLabel!=yLabel || isEnabled()!=enabled;
    if (!m_dragging) m_value=next;
    m_xLabel=xLabel; m_yLabel=yLabel; setEnabled(enabled);
    setAccessibleDescription(tr("Horizontal: %1. Vertical: %2. Exact values are available in the controls below.").arg(xLabel,yLabel));
    if (changed) update();
}
void SlicerEffectPad::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto& t=th(); const auto r=QRectF(rect()).adjusted(1,1,-1,-1); const auto field=fieldRect();
    QLinearGradient well(r.topLeft(),r.bottomLeft()); well.setColorAt(0,t.well().darker(t.dark?125:104)); well.setColorAt(1,t.well());
    painter.setPen(QPen(t.edgeDark(shellColor()),1)); painter.setBrush(well); painter.drawRoundedRect(r,12,12);
    painter.setPen(QPen(mixColors(t.textSecondary,t.well(),.45),1.6,Qt::SolidLine,Qt::RoundCap));
    for (int x=0;x<=8;++x) for (int y=0;y<=6;++y)
        painter.drawPoint(QPointF(field.left()+field.width()*x/8.0,field.top()+field.height()*y/6.0));
    auto label=font(); label.setPixelSize(11); painter.setFont(label); painter.setPen(t.textSecondary);
    painter.drawText(QRectF(14,7,width()-28,18),Qt::AlignLeft|Qt::AlignVCenter,isEnabled()?m_yLabel+" ↑":tr("Choose an effect"));
    painter.drawText(QRectF(14,height()-25,width()-28,18),Qt::AlignRight|Qt::AlignVCenter,isEnabled()?m_xLabel+" →":tr("Per slice"));
    if (isEnabled()) {
        const QPointF handle(field.left()+m_value.x()*field.width(),field.bottom()-m_value.y()*field.height());
        QColor glow=slicerAccent(); glow.setAlpha(38); QRadialGradient halo(handle,48); halo.setColorAt(0,glow); halo.setColorAt(1,Qt::transparent);
        painter.setPen(Qt::NoPen); painter.setBrush(halo); painter.drawEllipse(handle,48,48);
        auto line=slicerAccent(); line.setAlpha(90); painter.setPen(QPen(line,1));
        painter.drawLine(QPointF(field.left(),handle.y()),QPointF(field.right(),handle.y()));
        painter.drawLine(QPointF(handle.x(),field.top()),QPointF(handle.x(),field.bottom()));
        painter.setBrush(t.well()); painter.setPen(QPen(slicerAccent(),2)); painter.drawEllipse(handle,7,7);
        painter.setBrush(t.textPrimary); painter.setPen(Qt::NoPen); painter.drawEllipse(handle,2,2);
    }
}
void SlicerEffectPad::editPosition(QPointF next) {
    next.setX(std::clamp(next.x(),0.0,1.0)); next.setY(std::clamp(next.y(),0.0,1.0));
    if (next==m_value) return;
    m_value=next; update(); emit positionEdited(next.x(),next.y());
}
void SlicerEffectPad::mousePressEvent(QMouseEvent* e) {
    if (e->button()!=Qt::LeftButton || !fieldRect().adjusted(-8,-8,8,8).contains(e->position())) return;
    setFocus(); m_dragging=true; m_lastPointer=e->position(); const auto r=fieldRect();
    const QPointF handle(r.left()+m_value.x()*r.width(),r.bottom()-m_value.y()*r.height());
    m_grabOffset=QLineF(handle,e->position()).length()<=16 ? e->position()-handle : QPointF();
    if (m_grabOffset.isNull()) editPosition({(e->position().x()-r.left())/r.width(),(r.bottom()-e->position().y())/r.height()});
    e->accept();
}
void SlicerEffectPad::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return; const auto r=fieldRect();
    // Relative deltas keep Shift changes and boundary reversals immediate.
    const double fine=e->modifiers().testFlag(Qt::ShiftModifier)?.15:1.0;
    const auto delta=e->position()-m_lastPointer; m_lastPointer=e->position();
    editPosition(m_value+QPointF(delta.x()/r.width()*fine,-delta.y()/r.height()*fine)); e->accept();
}
void SlicerEffectPad::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button()==Qt::LeftButton && m_dragging) { m_dragging=false; emit editFinished(); e->accept(); }
}
bool SlicerEffectPad::cancelDrag() { return std::exchange(m_dragging,false); }
bool SlicerEffectPad::event(QEvent* e) {
    if ((e->type()==QEvent::UngrabMouse || e->type()==QEvent::Hide || e->type()==QEvent::WindowDeactivate) && m_dragging) { m_dragging=false; emit editFinished(); }
    if (e->type()==QEvent::ShortcutOverride) {
        const auto* key=static_cast<QKeyEvent*>(e);
        if (key->key()>=Qt::Key_Left && key->key()<=Qt::Key_Down) { e->accept(); return true; }
    }
    return QWidget::event(e);
}
void SlicerEffectPad::keyPressEvent(QKeyEvent* e) {
    QPointF next=m_value; const double step=e->modifiers().testFlag(Qt::ShiftModifier)?.0025:.02;
    switch(e->key()) {
        case Qt::Key_Left: next.rx()-=step; break;
        case Qt::Key_Right: next.rx()+=step; break;
        case Qt::Key_Up: next.ry()+=step; break;
        case Qt::Key_Down: next.ry()-=step; break;
        default: QWidget::keyPressEvent(e); return;
    }
    editPosition(next); emit editFinished(); e->accept();
}

SlicerWaveform::SlicerWaveform(QWidget* parent) : ui::FrameWidget(parent) {
    setMinimumHeight(150); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true); setFocusPolicy(Qt::StrongFocus); setObjectName("SlicerWaveform");
    setAccessibleName(tr("Sample waveform and slice boundaries"));
    setToolTip(tr("Wheel: zoom at pointer · Shift+wheel: scroll · Drag ruler: slicing range · Double-click: split · Middle drag: pan"));
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, QOverload<>::of(&QWidget::update));
}
void SlicerWaveform::setSample(std::shared_ptr<const daw::engine::SampleBuffer> sample) {
    if (m_sample == sample) return;
    cancelDrag(); m_sample = std::move(sample); m_minima.clear(); m_maxima.clear();
    ++m_generation; fitAll(); requestPeaks(); update();
}
void SlicerWaveform::setTable(std::shared_ptr<const p::SliceTable> table) {
    if (m_table == table) return; m_table = std::move(table); update();
}
void SlicerWaveform::setSelection(const QSet<quint32>& ids, quint32 primary) {
    if (m_selected == ids && m_primary == primary) return; m_selected = ids; m_primary = primary; update();
}
void SlicerWaveform::setRange(quint32 start, quint32 end) {
    if (m_rangeStart == start && m_rangeEnd == end) return; m_rangeStart = start; m_rangeEnd = end; update();
}
QRectF SlicerWaveform::plotRect() const { return QRectF(12, 48, std::max(1, width() - 24), std::max(1, height() - 63)); }
double SlicerWaveform::xForFrame(double frame) const { const auto r = plotRect(); return r.left() + (frame - m_viewStart) / m_viewSpan * r.width(); }
double SlicerWaveform::frameForX(double x) const {
    const auto r = plotRect(); return std::clamp(m_viewStart + (x - r.left()) / r.width() * m_viewSpan, 0.0, double(m_sample ? m_sample->frames() : 1));
}
void SlicerWaveform::setView(double start, double span) {
    const double total = m_sample ? std::max(1.0, double(m_sample->frames())) : 1.0;
    m_viewSpan = std::clamp(span, std::min(total, 16.0), total);
    m_viewStart = std::clamp(start, 0.0, total - m_viewSpan); update();
}
void SlicerWaveform::fitAll() { setView(0, m_sample ? m_sample->frames() : 1); }
void SlicerWaveform::fitSelection() {
    if (!m_table || m_selected.empty()) return;
    double first = m_table->frames, last = 0;
    for (quint32 i = 0; i < m_table->count; ++i) if (m_selected.contains(m_table->slices[i].id)) {
        first = std::min(first, double(m_table->slices[i].start)); last = std::max(last, double(m_table->slices[i].end));
    }
    const double margin = (last - first) * 0.07; if (last > first) setView(first - margin, last - first + margin * 2);
}
void SlicerWaveform::zoom(double factor, double anchor) {
    const auto r = plotRect(); if (anchor < 0) anchor = r.center().x();
    const double fraction = std::clamp((anchor - r.left()) / r.width(), 0.0, 1.0), frame = frameForX(anchor);
    const double total = m_sample ? std::max(1.0, double(m_sample->frames())) : 1.0;
    const double span = std::clamp(m_viewSpan / factor, std::min(16.0, total), total);
    setView(frame - fraction * span, span);
}
int SlicerWaveform::sliceAt(double x) const {
    if (!m_table || !plotRect().contains(QPointF(x, plotRect().center().y()))) return -1;
    const double f = frameForX(x);
    for (quint32 i = 0; i < m_table->count; ++i) if (f >= m_table->slices[i].start && f < m_table->slices[i].end) return int(i);
    return -1;
}
int SlicerWaveform::boundaryAt(double x) const {
    if (!m_table) return -1;
    for (quint32 i = 1; i < m_table->count; ++i) if (std::abs(xForFrame(m_table->slices[i].start) - x) <= 6) return int(i);
    return -1;
}
void SlicerWaveform::requestPeaks() {
    if (!m_sample) return;
    const auto sample = m_sample; const auto generation = m_generation; const QPointer<SlicerWaveform> guard(this);
    QThreadPool::globalInstance()->start([sample, generation, guard] {
        const int bins = int(std::min<quint32>(sample->frames(), 8192));
        QVector<float> lo(bins), hi(bins);
        for (int b = 0; b < bins; ++b) {
            const auto start = quint32(quint64(sample->frames()) * b / bins);
            const auto end = quint32(quint64(sample->frames()) * (b + 1) / bins);
            for (daw::engine::ChannelCount ch = 0; ch < sample->channels(); ++ch) {
                const float* data = sample->channel(ch);
                for (auto i = start; i < end; ++i) if (std::isfinite(data[i])) { lo[b] = std::min(lo[b], data[i]); hi[b] = std::max(hi[b], data[i]); }
            }
        }
        QMetaObject::invokeMethod(qApp, [guard, generation, lo = std::move(lo), hi = std::move(hi)]() mutable {
            if (!guard || guard->m_generation != generation) return;
            guard->m_minima = std::move(lo); guard->m_maxima = std::move(hi); guard->update();
        }, Qt::QueuedConnection);
    });
}
void SlicerWaveform::paintEvent(QPaintEvent* event) { QPainter painter(this); paintScene(painter, event->region()); }
void SlicerWaveform::paintScene(QPainter& painter, const QRegion&) {
    const auto& t = th(); const auto r = plotRect();
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient glass(0,0,0,height()); glass.setColorAt(0,t.well().darker(t.dark?125:102)); glass.setColorAt(1,t.well());
    painter.setBrush(glass); painter.setPen(t.edgeDark(shellColor())); painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),12,12);
    if (!m_sample) {
        painter.setPen(t.textSecondary); painter.drawText(r, Qt::AlignCenter, tr("Drop a sample here or choose Load sample")); return;
    }
    const double total = m_sample->frames(), rate = std::max(1.0, m_sample->sampleRate());
    // Overview is kept independent of zoom and never rescans PCM on a resize.
    painter.setPen(mixColors(t.waveform, t.well(), .4));
    for (int x = 12; x < width() - 12 && !m_maxima.empty(); ++x) {
        const int b = std::clamp(int(double(x - 12) / r.width() * m_maxima.size()), 0, int(m_maxima.size()) - 1);
        painter.drawLine(QPointF(x, 13 - 8 * m_maxima[b]), QPointF(x, 13 - 8 * m_minima[b]));
    }
    QColor overviewFill = t.accent; overviewFill.setAlpha(32);
    painter.setPen(t.accent); painter.setBrush(overviewFill);
    painter.drawRoundedRect(QRectF(12 + m_viewStart / total * r.width(), 3, m_viewSpan / total * r.width(), 20), 3, 3);
    painter.setBrush(Qt::NoBrush);
    const double seconds = m_viewSpan / rate;
    const double rough = seconds * 85 / r.width(), power = std::pow(10.0, std::floor(std::log10(std::max(rough, 1e-7))));
    const double step = power * (rough / power > 5 ? 10 : rough / power > 2 ? 5 : rough / power > 1 ? 2 : 1);
    for (double s = std::ceil(m_viewStart / rate / step) * step; s <= (m_viewStart + m_viewSpan) / rate; s += step) {
        const auto x = xForFrame(s * rate); painter.setPen(t.gridLine); painter.drawLine(QPointF(x, 44), QPointF(x, r.bottom()));
        const auto label = QString::number(s, 'f', step < 1 ? std::min(4, int(-std::floor(std::log10(step)))) : 0) + QCoreApplication::translate("SlicerPanel", " s");
        const auto labelWidth = painter.fontMetrics().horizontalAdvance(label) + 8;
        painter.setPen(t.textSecondary); painter.drawText(QRectF(std::min(x + 4, r.right() - labelWidth), 26, labelWidth, 18), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    painter.save(); painter.setClipRect(r);
    const quint32 rangeEnd = m_rangeEnd > m_rangeStart ? m_rangeEnd : m_sample->frames();
    QColor outside = t.background; outside.setAlpha(150);
    if (m_rangeStart > 0) painter.fillRect(QRectF(r.left(), r.top(), xForFrame(m_rangeStart) - r.left(), r.height()), outside);
    if (rangeEnd < m_sample->frames()) painter.fillRect(QRectF(xForFrame(rangeEnd), r.top(), r.right() - xForFrame(rangeEnd), r.height()), outside);
    if (m_table) for (quint32 i = 0; i < m_table->count; ++i) {
        const auto& s = m_table->slices[i]; const auto a = xForFrame(s.start), b = xForFrame(s.end);
        if (b < r.left() || a > r.right()) continue;
        QColor fill = sliceColor(s.id, t);
        fill.setAlpha(s.flags & p::kSliceMuted ? 7 : m_selected.contains(s.id) ? 42 : 16);
        painter.fillRect(QRectF(a, r.top(), b - a, r.height()), fill);
    }
    quint32 waveSlice = 0;
    quint32 coloredId = 0;
    QColor coloredWave = t.waveform;
    const auto colorAtFrame = [&](double frame) {
        while (m_table && waveSlice < m_table->count && frame >= m_table->slices[waveSlice].end) ++waveSlice;
        const auto* slice = m_table && waveSlice < m_table->count && frame >= m_table->slices[waveSlice].start ? &m_table->slices[waveSlice] : nullptr;
        const quint32 id = slice ? slice->id : 0;
        if (id != coloredId) {
            coloredId = id; const auto ink=mixColors(t.textPrimary,t.textSecondary,.35);
            coloredWave = slice ? mixColors(ink,sliceColor(id,t),m_selected.contains(id)?.35:.08) : ink;
            if (slice && slice->flags & p::kSliceMuted) coloredWave = mixColors(coloredWave, t.well(), .65);
        }
        return coloredWave;
    };
    painter.setPen(QPen(t.waveform, 1));
    const int pixels = int(r.width());
    if (m_viewSpan <= pixels * 6 && m_sample) {
        QPainterPath path; QColor pathColor = t.waveform; QPointF previous;
        for (int x = 0; x <= pixels; ++x) {
            const auto frame = quint32(std::min(total - 1, frameForX(r.left() + x)));
            const QPointF point(r.left() + x, r.center().y() - m_sample->readSample(0, frame) * r.height() * .43);
            const auto color = colorAtFrame(frame);
            if (x == 0) { path.moveTo(point); pathColor = color; }
            else {
                if (color != pathColor) {
                    painter.setPen(QPen(pathColor, 1)); painter.drawPath(path);
                    path = QPainterPath(); path.moveTo(previous); pathColor = color;
                }
                path.lineTo(point);
            }
            previous = point;
        }
        painter.setPen(QPen(pathColor, 1)); painter.drawPath(path);
    } else if (!m_maxima.empty()) {
        for (int x = 0; x < pixels; ++x) {
            const int a = std::clamp(int(frameForX(r.left() + x) / total * m_maxima.size()), 0, int(m_maxima.size()) - 1);
            const int b = std::clamp(int(frameForX(r.left() + x + 1) / total * m_maxima.size()), a, int(m_maxima.size()) - 1);
            float lo = 0, hi = 0; for (int k = a; k <= b; ++k) { lo = std::min(lo, m_minima[k]); hi = std::max(hi, m_maxima[k]); }
            painter.setPen(QPen(colorAtFrame(frameForX(r.left() + x)), 1));
            painter.drawLine(QPointF(r.left() + x, r.center().y() - hi * r.height() * .43), QPointF(r.left() + x, r.center().y() - lo * r.height() * .43));
        }
    }
    if (m_table) for (quint32 i = 0; i < m_table->count; ++i) {
        const auto& s = m_table->slices[i]; const double x = xForFrame(s.start), next = xForFrame(s.end);
        QColor color = sliceColor(s.id, t);
        if (s.flags & p::kSliceMuted) color = mixColors(color, t.well(), .6);
        QColor edge = color; edge.setAlpha(m_selected.contains(s.id) ? 235 : 145);
        painter.setPen(QPen(edge, m_primary == s.id ? 2 : 1));
        painter.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        painter.fillRect(QRectF(x + 1, r.top(), std::max(0.0, next - x - 2), 3), color);
        const auto label = noteName(s.key);
        if (next - x > painter.fontMetrics().horizontalAdvance(label) + 12) {
            painter.setPen(t.textPrimary);
            painter.drawText(QRectF(x + 6, r.top() + 6, next - x - 10, 20), Qt::AlignLeft | Qt::AlignTop, label);
        }
    }
    painter.restore();
    painter.setPen(t.textSecondary); painter.drawText(QRectF(12, height() - 14, width() - 24, 13), Qt::AlignRight, tr("%1× zoom").arg(total / m_viewSpan, 0, 'f', 1));
}
void SlicerWaveform::mousePressEvent(QMouseEvent* e) {
    if (!m_sample) return;
    setFocus(); m_pressPosition = e->position(); m_pressFrame = frameForX(e->position().x()); m_panStart = m_viewStart;
    if (e->button() == Qt::MiddleButton) { m_panning = true; setCursor(Qt::ClosedHandCursor); return; }
    if (e->button() != Qt::LeftButton) return;
    if (e->position().y() < 24) { m_overview = true; mouseMoveEvent(e); return; }
    if (e->position().y() < 48 || e->modifiers().testFlag(Qt::AltModifier)) { m_selectingRange = true; return; }
    m_dragBoundary = boundaryAt(e->position().x());
    if (m_dragBoundary > 0 && e->modifiers() == Qt::NoModifier) { emit boundaryBegin(); setCursor(Qt::SplitHCursor); return; }
    m_dragBoundary = -1; emit selectRequested(sliceAt(e->position().x()), e->modifiers(), e->modifiers() == Qt::NoModifier);
}
void SlicerWaveform::mouseMoveEvent(QMouseEvent* e) {
    if (m_overview) { setView((e->position().x() - 12) / plotRect().width() * m_sample->frames() - m_viewSpan / 2, m_viewSpan); return; }
    if (m_panning) { setView(m_panStart - (e->position().x() - m_pressPosition.x()) / plotRect().width() * m_viewSpan, m_viewSpan); return; }
    if (m_dragBoundary > 0) { emit boundaryMoved(m_dragBoundary, quint32(frameForX(e->position().x()))); return; }
    if (m_selectingRange) {
        const auto f = quint32(frameForX(e->position().x())); m_rangeStart = std::min(f, quint32(m_pressFrame)); m_rangeEnd = std::max(f, quint32(m_pressFrame)); update(); return;
    }
    setCursor(boundaryAt(e->position().x()) > 0 ? Qt::SplitHCursor : Qt::PointingHandCursor);
}
void SlicerWaveform::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragBoundary > 0) emit boundaryCommit();
    if (m_selectingRange && m_rangeEnd > m_rangeStart) emit rangeRequested(m_rangeStart, m_rangeEnd);
    m_dragBoundary = -1; m_panning = m_overview = m_selectingRange = false; setCursor(Qt::PointingHandCursor); emit auditionReleased();
}
void SlicerWaveform::mouseDoubleClickEvent(QMouseEvent* e) {
    cancelDrag(); if (e->button() == Qt::LeftButton && plotRect().contains(e->position())) emit splitRequested(quint32(frameForX(e->position().x())));
}
bool SlicerWaveform::cancelDrag() {
    const bool active = m_dragBoundary > 0 || m_selectingRange || m_panning || m_overview;
    if (m_dragBoundary > 0) emit boundaryCancel();
    m_dragBoundary = -1; m_selectingRange = m_panning = m_overview = false; setCursor(Qt::PointingHandCursor); return active;
}
void SlicerWaveform::contextMenuEvent(QContextMenuEvent* e) {
    const int i = sliceAt(e->pos().x()); if (i < 0) return;
    QMenu menu(this); auto* split = menu.addAction(tr("Split here")); split->setEnabled(m_table->count < p::kMaxSlices);
    auto* merge = menu.addAction(tr("Merge with previous")); merge->setEnabled(i > 0);
    auto* next = menu.addAction(tr("Merge with next")); next->setEnabled(i + 1 < int(m_table->count));
    auto* result = menu.exec(e->globalPos());
    if (result == split) emit splitRequested(quint32(frameForX(e->pos().x())));
    if (result == merge) emit mergeRequested(i); if (result == next) emit mergeRequested(i + 1);
}
void SlicerWaveform::wheelEvent(QWheelEvent* e) {
    const double delta = e->pixelDelta().isNull() ? e->angleDelta().y() / 120.0 : e->pixelDelta().y() / 40.0;
    if (e->modifiers().testFlag(Qt::ShiftModifier) || std::abs(e->angleDelta().x()) > 0 || std::abs(e->pixelDelta().x()) > 0) {
        const double horizontal = e->pixelDelta().x() ? e->pixelDelta().x() / 40.0 : e->angleDelta().x() / 120.0;
        const double pixels=std::max(1.,plotRect().width());
        ui::ScrollMotion::scroll(this,{-(horizontal ? horizontal : delta)*pixels*.1,0},!e->pixelDelta().isNull(),
            [this,pixels]{return QPointF(m_viewStart/m_viewSpan*pixels,0);},
            [this,pixels](QPointF p){setView(p.x()/pixels*m_viewSpan,m_viewSpan);});
    } else {ui::ScrollMotion::cancel(this);zoom(std::pow(1.25, delta), e->position().x());}
    e->accept();
}

SlicerPad::SlicerPad(QWidget* parent) : QPushButton(parent) {
    setFixedHeight(kPadSide); setMinimumWidth(36); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::StrongFocus); setAcceptDrops(true); setObjectName("SlicerPad"); setCheckable(true);
    connect(this, &QPushButton::clicked, this, [this] { if (!isDown()) emit selectRequested(m_index, Qt::NoModifier, true); });
}
void SlicerPad::configure(const p::Slice* slice, int index, double rate, bool selected, bool active, const QString& token) {
    const bool muted = slice && (slice->flags & p::kSliceMuted);
    const bool visualChanged = m_id != (slice ? slice->id : 0) || m_muted != muted || m_playing != active;
    m_index = index; m_id = slice ? slice->id : 0; m_token = token; setEnabled(slice != nullptr);
    m_muted = muted; m_playing = active;
    setChecked(selected);
    const QString text = slice ? noteName(slice->key) : QString("—");
    QStringList badges;
    if (slice) {
        if (slice->flags & p::kSliceMuted) badges << tr("MUTE");
        if (slice->flags & p::kSliceReverse) badges << tr("REV");
        if (slice->loopMode || slice->flags & p::kSliceLoop) badges << tr("LOOP");
        if (slice->locked) badges << tr("LOCK");
    }
    if (text != this->text()) setText(text);
    setAccessibleName(slice ? tr("Slice %1, MIDI %2, %3").arg(index + 1).arg(noteName(slice->key)).arg(msText(double(slice->end - slice->start) / std::max(1.0, rate))) : tr("Empty pad"));
    setAccessibleDescription(badges.join(" · "));
    setToolTip(accessibleName() + (badges.empty() ? QString() : "\n" + badges.join(" · ")) + "\n" + tr("Click to audition · Shift/Ctrl: select multiple · Drag to exchange MIDI assignments"));
    setProperty("selected", selected); setProperty("playing", active);
    if (visualChanged) update();
}
void SlicerPad::paintEvent(QPaintEvent*) {
    const auto& t = th(); QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const bool pressed = isDown(), lit = pressed || m_playing;
    QColor color = isEnabled() ? sliceColor(m_id, t) : t.separator();
    if (m_muted && !lit) color = mixColors(color, t.well(), .65);
    const qreal light = lit ? 1.0 : isChecked() ? .55 : underMouse() ? .3 : .06;
    const QRectF rim = QRectF(rect()).adjusted(4, 4, -4, -4);
    painter.setBrush(Qt::NoBrush);
    for (int i = 4; i > 0; --i) {
        QColor glow = color; glow.setAlpha(int((5 - i) * 18 * light * (t.dark ? 1 : .65)));
        painter.setPen(QPen(glow, 2));
        painter.drawRoundedRect(rim.adjusted(-i * .7, -i * .7, i * .7, i * .7), 5 + i * .7, 5 + i * .7);
    }
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(0, 0, 0, pressed ? 14 : t.dark ? 65 : 27));
    painter.drawRoundedRect(rim.translated(0, pressed ? 1 : 2), 5, 5);
    QLinearGradient ring(rim.topLeft(), rim.bottomLeft());
    ring.setColorAt(0, isChecked() || lit ? color : t.edgeLight(shellColor())); ring.setColorAt(1, isChecked() || lit ? color.darker(140) : t.edgeDark(shellColor()));
    painter.setBrush(ring); painter.drawRoundedRect(rim, 5, 5);
    QRectF face = rim.adjusted(isChecked() || lit ? 2.3 : 1.5, isChecked() || lit ? 2.3 : 1.5, isChecked() || lit ? -2.3 : -1.5, isChecked() || lit ? -2.3 : -1.5);
    if (pressed) face = face.adjusted(.4, .4, -.4, -.4).translated(0, .6);
    QColor base = shellColor();
    base = mixColors(base, color, lit ? .2 : isChecked() ? .10 : .04);
    if (pressed) base = base.darker(112);
    QLinearGradient rubber(face.topLeft(), face.bottomLeft());
    rubber.setColorAt(0, t.edgeLight(base)); rubber.setColorAt(1, t.edgeDark(base));
    painter.setBrush(rubber); painter.drawRoundedRect(face, 3.5, 3.5);
    auto labelFont = font(); labelFont.setPixelSize(11); labelFont.setWeight(QFont::DemiBold); painter.setFont(labelFont);
    painter.setPen(isEnabled() && !m_muted ? t.textPrimary : t.textSecondary);
    painter.drawText(face.translated(0, pressed ? .4 : 0), Qt::AlignCenter, text());
    if(width()>62 && m_index>=0) {
        auto indexFont=font(); indexFont.setPixelSize(9); painter.setFont(indexFont); painter.setPen(t.textSecondary);
        painter.drawText(face.adjusted(5,3,-4,-2),Qt::AlignLeft|Qt::AlignTop,QString::number(m_index+1).rightJustified(2,'0'));
    }
    painter.setPen(QPen(color,2,Qt::SolidLine,Qt::RoundCap));
    painter.drawLine(QPointF(face.center().x()-4,face.bottom()-4),QPointF(face.center().x()+4,face.bottom()-4));
}
bool SlicerPad::event(QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
    return QPushButton::event(event);
}
void SlicerPad::mousePressEvent(QMouseEvent* e) {
    m_press = e->pos();
    if (e->button() == Qt::LeftButton) { setFocus(); setDown(true); emit selectRequested(m_index, e->modifiers(), e->modifiers() == Qt::NoModifier); e->accept(); return; }
    QPushButton::mousePressEvent(e);
}
void SlicerPad::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons().testFlag(Qt::LeftButton) && (e->pos() - m_press).manhattanLength() >= QApplication::startDragDistance()) {
        setDown(false); emit auditionReleased();
        auto* mime = new QMimeData; mime->setData(kPadMime, (m_token + ":" + QString::number(m_id)).toUtf8());
        auto* drag = new QDrag(this); drag->setMimeData(mime); drag->setPixmap(grab()); drag->exec(Qt::MoveAction); return;
    }
    QPushButton::mouseMoveEvent(e);
}
void SlicerPad::mouseReleaseEvent(QMouseEvent* e) { setDown(false); emit auditionReleased(); e->accept(); }
void SlicerPad::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kPadMime) && QString::fromUtf8(e->mimeData()->data(kPadMime)).startsWith(m_token + ":")) e->acceptProposedAction();
}
void SlicerPad::dropEvent(QDropEvent* e) {
    const auto text = QString::fromUtf8(e->mimeData()->data(kPadMime));
    if (!text.startsWith(m_token + ":")) return;
    emit swapRequested(text.mid(m_token.size() + 1).toUInt(), m_id); e->acceptProposedAction();
}

SlicerPanel::SlicerPanel(daw::EngineController* controller, QString channelId, QString slotId, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channelId(std::move(channelId)), m_slotId(std::move(slotId)), m_dragToken(QUuid::createUuid().toString()) {
    m_refreshing = true; setObjectName("SlicerPanel"); setAcceptDrops(true); setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(780, 390); resize(980, 410);
    setAttribute(Qt::WA_StyledBackground);
    auto* root = new QVBoxLayout(this); root->setContentsMargins(8, 8, 8, 8); root->setSpacing(0);
    const auto button = [this](const QString& text, const QString& name, QBoxLayout* row, auto callback) {
        auto* b = new QPushButton(text, this); b->setObjectName(name); b->setAccessibleName(text); b->setMinimumHeight(30);
        row->addWidget(b); connect(b, &QPushButton::clicked, this, callback); return b;
    };
    m_body = new QBoxLayout(QBoxLayout::LeftToRight); m_body->setSpacing(8); root->addLayout(m_body, 1);
    m_mainArea = new SlicerSurface(this); auto* main = new QVBoxLayout(m_mainArea); main->setContentsMargins(8,8,8,8); main->setSpacing(6);
    auto* navigation = new QHBoxLayout; navigation->setSpacing(2);
    const auto iconButton = [this, navigation](icons::Glyph glyph, const QString& text, const QString& name, auto callback) {
        auto* b = new ui::IconButton(glyph, text, this); b->setObjectName(name); b->setAccessibleName(text);
        b->setFocusPolicy(Qt::StrongFocus); b->setButtonSize(28,28); navigation->addWidget(b);
        connect(b, &QAbstractButton::clicked, this, callback); return b;
    };
    iconButton(icons::Glyph::Folder, tr("Load sample"), "SlicerLoad", [this] { loadSample(); });
    m_fileLabel = new QLabel(tr("No sample loaded"), this); m_fileLabel->setObjectName("SlicerFile");
    m_fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); navigation->addWidget(m_fileLabel, 1);
    m_cancelButton = iconButton(icons::Glyph::Close, tr("Cancel"), "SlicerCancel", [this] { cancelWork(); status(tr("Cancelled; previous state kept")); });
    m_cancelButton->hide();
    m_bank = new QComboBox(this); m_bank->setObjectName("SlicerBank"); m_bank->setAccessibleName(tr("Pad page")); navigation->addWidget(m_bank);
    connect(m_bank, &QComboBox::currentIndexChanged, this, [this] { if (!m_refreshing) refreshSelection(); });
    iconButton(icons::Glyph::ZoomOut, tr("Zoom out"), "SlicerZoomOut", [this] { m_waveform->zoom(1.0 / 1.5); });
    iconButton(icons::Glyph::ZoomIn, tr("Zoom in"), "SlicerZoomIn", [this] { m_waveform->zoom(1.5); });
    iconButton(icons::Glyph::ZoomFit, tr("Fit all"), "SlicerFitAll", [this] { m_waveform->fitAll(); });
    iconButton(icons::Glyph::Gear, tr("Slicer menu"), "SlicerMenu", [this] { showMenu(); });
    main->addLayout(navigation); m_waveform = new SlicerWaveform(m_mainArea); main->addWidget(m_waveform, 1);
    connect(m_waveform, &SlicerWaveform::selectRequested, this, &SlicerPanel::selectSlice);
    connect(m_waveform, &SlicerWaveform::auditionReleased, this, [this] { releaseAudition(); });
    connect(m_waveform, &SlicerWaveform::splitRequested, this, &SlicerPanel::splitAt);
    connect(m_waveform, &SlicerWaveform::mergeRequested, this, &SlicerPanel::mergeAt);
    connect(m_waveform, &SlicerWaveform::rangeRequested, this, [this](quint32 a, quint32 b) { setAnalysis([=](Analysis& s) { s.rangeStart = a; s.rangeEnd = b; }); });
    connect(m_waveform, &SlicerWaveform::boundaryBegin, this, [this] {
        commitField(); cancelWork(); m_dragFiles.clear(); releaseAudition(); m_boundaryGesture = m_controller && m_controller->beginSlicerEdit(m_channelId.toStdString(), m_slotId.toStdString());
    });
    connect(m_waveform, &SlicerWaveform::boundaryMoved, this, [this](int right, quint32 frame) {
        const auto snapshot = slicer();
        if (!m_boundaryGesture || !snapshot || !snapshot->state.table || !snapshot->state.audio) return;
        auto table = std::make_shared<Table>(*snapshot->state.table);
        const auto settings = snapshot->state.analysis; const auto source = snapshot->state.audio;
        const auto minimum = quint32(std::clamp(settings.minimumMs * source->sampleRate() / 1000, 1.0, double(source->frames())));
        if (settings.zeroCrossing && right > 0 && right < int(table->count)) frame = slicing::snapToZero(*source, frame, table->slices[right-1].start+1, table->slices[right].end-1);
        if (slicing::moveBoundary(*table, right, frame, minimum)) { m_controller->updateSlicerEdit(table, settings); refresh(); }
    });
    connect(m_waveform, &SlicerWaveform::boundaryCommit, this, [this] {
        if (!m_boundaryGesture) return; m_boundaryGesture = false;
        if (m_controller->commitSlicerEdit("Move slice boundary")) emit projectEdited(); refresh();
    });
    connect(m_waveform, &SlicerWaveform::boundaryCancel, this, [this] {
        if (!m_boundaryGesture) return; m_boundaryGesture = false; m_controller->cancelSlicerEdit(); refresh();
    });
    auto* chopRow=new QHBoxLayout; chopRow->setSpacing(6);
    m_quickMode=new QComboBox(this); m_quickMode->addItems({tr("Transients"),tr("Random"),tr("Grid"),tr("Manual")});
    m_quickMode->setAccessibleName(tr("Slicing mode")); chopRow->addWidget(m_quickMode,1);
    m_quickCount=new QSpinBox(this); m_quickCount->setRange(1,128); m_quickCount->setSuffix(" · "+tr("Slices"));
    m_quickCount->setAccessibleName(tr("Slice count")); chopRow->addWidget(m_quickCount);
    m_quickSlice=button(tr("Chop"),"SlicerQuickApply",chopRow,[this]{runSlice();});
    connect(m_quickMode,&QComboBox::currentIndexChanged,this,[this](int v){setAnalysis([=](Analysis& a){a.mode=p::SliceMode(v);});});
    connect(m_quickCount,&QSpinBox::valueChanged,this,[this](int v){setAnalysis([=](Analysis& a){a.targetCount=v;});});
    main->addLayout(chopRow);
    m_padArea = new QWidget(m_mainArea);
    m_padArea->setFixedHeight(2*kPadSide+kPadGap); m_padArea->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    auto* pads = new QGridLayout(m_padArea); pads->setContentsMargins(0, 0, 0, 0); pads->setSpacing(kPadGap);
    for(int c=0;c<kPadColumns;++c)pads->setColumnStretch(c,1);
    for (int i = 0; i < 16; ++i) {
        auto* pad = new SlicerPad(m_padArea); m_pads[i] = pad; pad->setObjectName("SlicerPad"); pads->addWidget(pad, i / kPadColumns, i % kPadColumns);
        connect(pad, &SlicerPad::selectRequested, this, &SlicerPanel::selectSlice);
        connect(pad, &SlicerPad::auditionReleased, this, [this] { releaseAudition(); });
        connect(pad, &SlicerPad::swapRequested, this, &SlicerPanel::swapKeys);
    }
    main->addWidget(m_padArea);
    m_settingsWindow=new QDialog(this,Qt::Tool); m_settingsWindow->setWindowTitle(tr("Slicer settings"));
    m_settingsWindow->resize(440,660); m_settingsWindow->setMinimumSize(400,360);
    auto* settingsLayout=new QVBoxLayout(m_settingsWindow); settingsLayout->setContentsMargins(10,10,10,10);
    m_tabs = new QTabWidget(m_settingsWindow); m_tabs->setObjectName("SlicerInspector"); settingsLayout->addWidget(m_tabs);
    m_tabs->addTab(scrollPage(buildAnalysis()), tr("Chop"));
    m_tabs->addTab(scrollPage(buildSliceInspector()), tr("Slice"));
    m_tabs->addTab(scrollPage(buildPlayback()), tr("Playback"));
    m_tabs->addTab(scrollPage(buildProcessing()), tr("Process"));
    m_tabs->addTab(scrollPage(buildExport()), tr("Export"));
    m_statusLabel = new QLabel(m_settingsWindow); m_statusLabel->setWordWrap(true); settingsLayout->addWidget(m_statusLabel);
    m_soundPanel=buildSoundPanel();
    m_body->addWidget(m_mainArea, 1); m_body->addWidget(m_soundPanel);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &SlicerPanel::applyTheme);
    for (auto* child : findChildren<QWidget*>()) child->installEventFilter(this);
    installEventFilter(this); m_refreshing = false; applyTheme(); refresh();
    m_poll = new QTimer(this); m_poll->setInterval(80); connect(m_poll, &QTimer::timeout, this, &SlicerPanel::refresh);
}
SlicerPanel::~SlicerPanel() { cancelWork(); releaseAudition(true); if (m_controller && (m_boundaryGesture || m_fieldEditor)) m_controller->cancelSlicerEdit(); }
void SlicerPanel::showSettings(int tab) {
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    m_settingsWindow->show(); m_settingsWindow->raise(); m_settingsWindow->activateWindow();
}
void SlicerPanel::showMenu() {
    QMenu menu(this);
    menu.addAction(tr("Settings"), this, [this] { showSettings(); });
    menu.addAction(tr("Export…"), this, [this] { showSettings(4); });
    auto* file = menu.addMenu(tr("File / preset"));
    file->addAction(tr("Load sample"), this, [this] { loadSample(); });
    file->addAction(tr("Show source file"), this, [this] {
        if (const auto snapshot = slicer()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(snapshot->state.path)).absolutePath()));
    });
    file->addSeparator();
    file->addAction(tr("Save portable preset…"), this, [this] { preset(false); });
    file->addAction(tr("Load portable preset…"), this, [this] { preset(true); });
    file->addSeparator();
    file->addAction(tr("Clear sample"), this, [this] {
        if (!slicer()) return; cancelWork(); commitField(); releaseAudition(true);
        m_controller->clearSlicerSample(m_channelId.toStdString(), m_slotId.toStdString()); refresh(); emit projectEdited();
    });
    auto* edit = menu.addMenu(tr("Edit"));
    const auto snapshot = slicer();
    edit->setEnabled(snapshot && snapshot->state.table && snapshot->state.table->count);
    edit->addAction(tr("Select all"), this, [this] {
        const auto snapshot = slicer();
        if (!snapshot || !snapshot->state.table) return;
        const auto& table = *snapshot->state.table;
        for (quint32 i = 0; i < table.count; ++i) m_selected.insert(table.slices[i].id);
        refreshSelection();
    });
    edit->addAction(tr("Copy"), this, &SlicerPanel::copySettings);
    edit->addAction(tr("Paste"), this, &SlicerPanel::pasteSettings)->setEnabled(m_copied.has_value());
    edit->addAction(tr("Reset"), this, &SlicerPanel::resetSettings);
    edit->addSeparator();
    edit->addAction(tr("Split"), this, [this] {
        const auto snapshot = slicer();
        if (!snapshot || !snapshot->state.table) return;
        const auto table = snapshot->state.table; const int i = table->indexForId(m_primary);
        if (i >= 0) splitAt(table->slices[i].start + (table->slices[i].end - table->slices[i].start) / 2);
    });
    edit->addAction(tr("Merge"), this, [this] {
        const auto snapshot = slicer();
        if (!snapshot || !snapshot->state.table) return;
        const int i = snapshot->state.table->indexForId(m_primary); if (i >= 0) mergeAt(i > 0 ? i : 1);
    });
    menu.addSeparator();
    menu.addAction(tr("Fit selection"), m_waveform, &SlicerWaveform::fitSelection);
    auto* anchor = findChild<QWidget*>("SlicerMenu");
    menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}
QWidget* SlicerPanel::buildExport() {
    auto* page = new QWidget; auto* layout = new QVBoxLayout(page); layout->setContentsMargins(12,12,12,12); layout->setSpacing(10);
    const auto heading = [page, layout](const QString& text) {
        auto* label = new QLabel(text, page); label->setObjectName("SlicerGroupTitle"); layout->addWidget(label);
    };
    const auto button = [this, page, layout](const QString& text, const QString& name, auto callback) {
        auto* b = new QPushButton(text, page); b->setObjectName(name); b->setAccessibleName(text); b->setMinimumHeight(30);
        layout->addWidget(b); connect(b, &QPushButton::clicked, this, callback); return b;
    };
    heading("MIDI");
    m_phraseOrder = new QComboBox(page); m_phraseOrder->addItems({tr("Source order"), tr("Reverse order"), tr("Seeded shuffle")});
    m_phraseOrder->setAccessibleName(tr("MIDI phrase order")); layout->addWidget(m_phraseOrder);
    button(tr("Create MIDI clip"), "SlicerMidiClip", [this] { exportMidi(false); });
    button(tr("Save MIDI…"), "SlicerMidiSave", [this] { exportMidi(true); });
    layout->addSpacing(10); heading("WAV");
    button(tr("Selected · processed WAV"), "SlicerWavSelected", [this] { exportWav(false, true); });
    button(tr("All · processed WAV"), "SlicerWavAll", [this] { exportWav(true, true); });
    button(tr("Selected · original WAV"), "SlicerWavRawSelected", [this] { exportWav(false, false); });
    button(tr("All · original WAV"), "SlicerWavRawAll", [this] { exportWav(true, false); });
    m_dragButton = button(tr("Prepare WAV drag"), "SlicerWavDrag", [this] {
        if (m_dragFiles.empty()) { exportWav(false, true, true); return; }
        status(tr("WAV ready · drag the ready button into the arrangement"));
    });
    layout->addStretch(); return page;
}
std::optional<daw::SlicerSnapshot> SlicerPanel::slicer(bool includeActivity) const {
    return m_controller ? m_controller->slicerSnapshot(m_channelId.toStdString(), m_slotId.toStdString(), includeActivity) : std::nullopt;
}
QDoubleSpinBox* SlicerPanel::number(QFormLayout* layout, const QString& caption, const QString& name, double lo, double hi, int decimals, const QString& suffix) {
    auto* box = new QDoubleSpinBox(this); box->setObjectName("Slicer_" + name); box->setRange(lo, hi); box->setDecimals(decimals);
    box->setSuffix(suffix); box->setSingleStep(decimals ? .1 : 1); box->setKeyboardTracking(false); box->setMinimumHeight(27);
    formRow(layout, caption, box); m_numbers.insert(name, box);
    connect(box, &QDoubleSpinBox::valueChanged, this, [this, box] {
        if (m_refreshing || !m_controller || !slicer()) return;
        if (m_fieldEditor != box) { commitField(); cancelWork(); if (m_controller->beginSlicerEdit(m_channelId.toStdString(), m_slotId.toStdString())) m_fieldEditor = box; }
    });
    connect(box, &QDoubleSpinBox::editingFinished, this, [this, box] { if (m_fieldEditor == box) commitField(); });
    return box;
}
QComboBox* SlicerPanel::choice(QFormLayout* layout, const QString& caption, const QString& name, const QStringList& items) {
    auto* box = new QComboBox(this); box->setObjectName("Slicer_" + name); box->addItems(items); box->setMinimumHeight(27);
    formRow(layout, caption, box); m_choices.insert(name, box); return box;
}
QCheckBox* SlicerPanel::toggle(QFormLayout* layout, const QString& caption, const QString& name) {
    auto* box = new QCheckBox(caption, this); box->setObjectName("Slicer_" + name); box->setAccessibleName(caption); layout->addRow(box); m_toggles.insert(name, box); return box;
}
QWidget* SlicerPanel::buildAnalysis() {
    auto* page = new QWidget; auto* f = form(page);
    auto* mode = choice(f, tr("Mode"), "mode", {tr("Transients"), tr("Random"), tr("Grid"), tr("Manual")});
    connect(mode, &QComboBox::currentIndexChanged, this, [this](int v) { setAnalysis([=](Analysis& s) { s.mode = p::SliceMode(v); }); });
    auto* count = number(f, tr("Slice count"), "count", 1, 128, 0);
    connect(count, &QDoubleSpinBox::valueChanged, this, [this](double v) { setAnalysis([=](Analysis& s) { s.targetCount = int(v); }); });
    const auto analysisNumber = [this, f](const QString& caption, const QString& name, double lo, double hi, auto member, const QString& suffix = QString()) {
        auto* box = number(f, caption, name, lo, hi, 2, suffix);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, member](double v) { setAnalysis([=](Analysis& s) { s.*member = v; }); }); return box;
    };
    analysisNumber(tr("Threshold"), "threshold", 0, 1, &Analysis::sensitivity);
    analysisNumber(tr("Pre-attack"), "pre", 0, 100, &Analysis::preAttackMs, tr(" ms"));
    analysisNumber(tr("Minimum length"), "minimum", 0, 10000, &Analysis::minimumMs, tr(" ms"));
    auto* zero = toggle(f, tr("Snap to zero crossing"), "zero");
    connect(zero, &QCheckBox::toggled, this, [this](bool v) { setAnalysis([=](Analysis& s) { s.zeroCrossing = v; }); });
    analysisNumber(tr("Random spread"), "spread", 0, .49, &Analysis::randomSpread);
    auto* seed = number(f, tr("Seed"), "seed", 0, 2147483647, 0);
    connect(seed, &QDoubleSpinBox::valueChanged, this, [this](double v) { setAnalysis([=](Analysis& s) { s.seed = quint64(v); }); });
    analysisNumber(tr("Source BPM"), "bpm", 20, 999, &Analysis::sourceBpm);
    auto* bars = new QWidget; auto* barRow = new QHBoxLayout(bars); barRow->setContentsMargins(0, 0, 0, 0);
    for (int n : {1, 2, 4, 8}) {
        auto* b = new QPushButton(tr("%1 bars").arg(n), bars); barRow->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, n] {
            const auto snapshot = slicer();
            if (!snapshot || !snapshot->state.audio) return;
            const auto a = snapshot->state.audio; const auto s = snapshot->state.analysis;
            const auto span = (s.rangeEnd > s.rangeStart ? s.rangeEnd : a->frames()) - s.rangeStart;
            const auto& doc = m_controller->project();
            setAnalysis([=](Analysis& v) { v.sourceBpm = std::clamp(double(n) * doc.timeSigNumerator * 4.0 / doc.timeSigDenominator * 60.0 * a->sampleRate() / std::max(1u, span), 20.0, 999.0); });
        });
    }
    f->addRow(tr("Loop length"), bars);
    auto* grid = choice(f, tr("Grid division"), "grid", {tr("Equal parts"), "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"});
    constexpr double divisions[]{0, .125, .25, .5, 1, 2, 4};
    for (int i = 0; i < grid->count(); ++i) grid->setItemData(i, divisions[i]);
    connect(grid, &QComboBox::currentIndexChanged, this, [this, grid](int) { setAnalysis([=](Analysis& s) { s.gridBeats = grid->currentData().toDouble(); }); });
    auto* begin = number(f, tr("Range start"), "rangeStart", 0, 36000000, 2, tr(" ms"));
    auto* end = number(f, tr("Range end"), "rangeEnd", 0, 36000000, 2, tr(" ms"));
    connect(begin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const auto snapshot = slicer();
        if (!snapshot || !snapshot->state.audio) return; const auto audio = snapshot->state.audio;
        setAnalysis([=](Analysis& s) { const auto end = s.rangeEnd > 0 ? s.rangeEnd : audio->frames(); s.rangeStart = std::min(quint32(v * audio->sampleRate() / 1000.0), end - 1); });
    });
    connect(end, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const auto snapshot = slicer();
        if (!snapshot || !snapshot->state.audio) return; const auto audio = snapshot->state.audio;
        setAnalysis([=](Analysis& s) { s.rangeEnd = std::clamp(quint32(v * audio->sampleRate() / 1000.0), s.rangeStart + 1, audio->frames()); });
    });
    auto* root = number(f, tr("Layout root"), "layoutRoot", 0, 127, 0);
    connect(root, &QDoubleSpinBox::valueChanged, this, [this](double v) { setAnalysis([=](Analysis& s) { s.rootNote = int(v); }, true); });
    auto* scale = choice(f, tr("Scale"), "scale", {});
    for (int i = 0; i < p::kScaleCount; ++i) scale->addItem(QCoreApplication::translate("SlicerPanel", p::scaleName(i)));
    connect(scale, &QComboBox::currentIndexChanged, this, [this](int v) { setAnalysis([=](Analysis& s) { s.scale = v; }, true); });
    auto* layout = choice(f, tr("Layout"), "layout", {tr("Ascending"), tr("Descending")});
    connect(layout, &QComboBox::currentIndexChanged, this, [this](int v) { setAnalysis([=](Analysis& s) { s.descending = v != 0; }, true); });
    m_sliceButton = new QPushButton(tr("Apply slicing"), page); m_sliceButton->setObjectName("SlicerApply"); m_sliceButton->setMinimumHeight(34);
    f->addRow(m_sliceButton); connect(m_sliceButton, &QPushButton::clicked, this, [this] { runSlice(); });
    auto* regenerate = new QPushButton(tr("New seed + slice"), page); f->addRow(regenerate); connect(regenerate, &QPushButton::clicked, this, [this] { runSlice(true); });
    auto* help = new QLabel(tr("Analysis runs only when you apply slicing. Root, scale and layout reassign MIDI notes without changing boundaries."), page); help->setWordWrap(true); help->setObjectName("SlicerHelp"); f->addRow(help);
    return page;
}
QWidget* SlicerPanel::buildSliceInspector() {
    auto* page = new QWidget; auto* f = form(page);
    auto* help = new QLabel(tr("Edits apply to every selected slice. MIDI note and boundaries edit the primary slice."), page); help->setWordWrap(true); help->setObjectName("SlicerHelp"); f->addRow(help);
    auto* key = number(f, tr("MIDI note"), "key", 0, 127, 0);
    connect(key, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        editTable(tr("Assign MIDI note"), [=, this](Table& t) {
            const int i = t.indexForId(m_primary); if (i < 0) return; const int other = t.indexForKey(int(v));
            if (other >= 0 && other != i) std::swap(t.slices[i].key, t.slices[other].key); else t.slices[i].key = std::int16_t(v);
        });
    });
    auto* start = number(f, tr("Start"), "start", 0, 36000000, 2, tr(" ms"));
    auto* end = number(f, tr("End"), "end", 0, 36000000, 2, tr(" ms"));
    connect(start, &QDoubleSpinBox::valueChanged, this, [this](double v) { editBoundary(false, v); });
    connect(end, &QDoubleSpinBox::valueChanged, this, [this](double v) { editBoundary(true, v); });
    const auto sliceNumber = [this, f](const QString& caption, const QString& name, double lo, double hi, auto member, int decimals = 2, const QString& suffix = QString()) {
        auto* box = number(f, caption, name, lo, hi, decimals, suffix);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, member, caption](double v) { editSelected(caption, [=](Slice& s) { s.*member = v; }); }); return box;
    };
    auto* gain = number(f, tr("Gain"), "gain", -60, 24, 1, tr(" dB"));
    connect(gain, &QDoubleSpinBox::valueChanged, this, [this](double v) { editSelected(tr("Slice gain"), [=](Slice& s) { s.gain = float(std::pow(10.0, v / 20.0)); }); });
    sliceNumber(tr("Pan"), "slicePan", -1, 1, &Slice::pan);
    sliceNumber(tr("Transpose"), "sliceTune", -48, 48, &Slice::transpose, 0, tr(" st"));
    sliceNumber(tr("Fine tune"), "sliceFine", -100, 100, &Slice::fineTune, 1, tr(" ct"));
    for (const auto& [name, text, flag] : std::array<std::tuple<QString, QString, quint8>, 2>{{{"reverse", tr("Reverse"), p::kSliceReverse}, {"mute", tr("Mute"), p::kSliceMuted}}}) {
        auto* box = toggle(f, text, name); connect(box, &QCheckBox::toggled, this, [this, flag, text](bool v) { editSelected(text, [=](Slice& s) { s.flags = (s.flags & ~flag) | (v ? flag : 0); }); });
    }
    auto* lock = toggle(f, tr("Lock against randomization"), "lock"); connect(lock, &QCheckBox::toggled, this, [this](bool v) { editSelected(tr("Lock slices"), [=](Slice& s) { s.locked = v; }); });
    sliceNumber(tr("Choke group"), "group", 0, 16, &Slice::chokeGroup, 0);
    auto* filter = choice(f, tr("Filter"), "filter", {tr("Off"), tr("Low-pass"), tr("High-pass"), tr("Band-pass")});
    connect(filter, &QComboBox::currentIndexChanged, this, [this](int v) { editSelected(tr("Slice filter"), [=](Slice& s) { s.filter = quint8(v); s.effect=0; }); });
    auto* cutoff = number(f, tr("Cutoff"), "cutoff", 20, 20000, 0, tr(" Hz"));
    connect(cutoff, &QDoubleSpinBox::valueChanged, this, [this](double v) { editSelected(tr("Filter cutoff"), [=](Slice& s) { s.cutoff = float(std::log(std::max(20.0, v) / 20.0) / std::log(1000.0)); }); });
    sliceNumber(tr("Resonance"), "resonance", 0, 1, &Slice::resonance);
    auto* inherit = toggle(f, tr("Use global envelope"), "inherit");
    connect(inherit, &QCheckBox::toggled, this, [this](bool v) { editSelected(tr("Envelope inheritance"), [=](Slice& s) { s.useGlobalEnvelope = v; }); });
    sliceNumber(tr("Attack"), "sliceAttack", 0, 5, &Slice::attack, 3, tr(" s"));
    sliceNumber(tr("Decay"), "sliceDecay", 0, 5, &Slice::decay, 3, tr(" s"));
    sliceNumber(tr("Sustain"), "sliceSustain", 0, 1, &Slice::sustain);
    sliceNumber(tr("Release"), "sliceRelease", 0, 5, &Slice::release, 3, tr(" s"));
    sliceNumber(tr("Fade in"), "fadeIn", 0, 10000, &Slice::fadeInMs, 2, tr(" ms"));
    auto* fadeOut = sliceNumber(tr("Fade out"), "fadeOut", -1, 10000, &Slice::fadeOutMs, 2, tr(" ms")); fadeOut->setSpecialValueText(tr("Auto (3 ms)"));
    auto* loop = choice(f, tr("Loop"), "loop", {tr("Off"), tr("Forward"), tr("Ping-pong")});
    connect(loop, &QComboBox::currentIndexChanged, this, [this](int v) { editSelected(tr("Slice loop"), [=](Slice& s) { s.loopMode = quint8(v); s.flags = (s.flags & ~p::kSliceLoop) | (v ? p::kSliceLoop : 0); }); });
    sliceNumber(tr("Loop crossfade"), "crossfade", 0, 1000, &Slice::crossfadeMs, 2, tr(" ms"));
    auto* normalization = new QLabel(page); normalization->setObjectName("SlicerNormalization"); f->addRow(tr("Normalization"), normalization);
    auto* norm = new QPushButton(tr("Normalize selected to −1 dBFS"), page); f->addRow(norm); connect(norm, &QPushButton::clicked, this, &SlicerPanel::normalizeSelection);
    return page;
}

QWidget* SlicerPanel::buildSoundPanel() {
    auto* panel=new QWidget(this); panel->setMinimumWidth(414); panel->setMaximumWidth(452);
    auto* row=new QHBoxLayout(panel); row->setContentsMargins(0,0,0,0); row->setSpacing(8);
    auto* sound=new SlicerSurface(panel); auto* soundLayout=new QVBoxLayout(sound);
    soundLayout->setContentsMargins(8,8,8,8); soundLayout->setSpacing(4);
    m_soundLabel=new QLabel(tr("Select a slice"),sound); m_soundLabel->setObjectName("SlicerGroupTitle");
    m_soundLabel->setAlignment(Qt::AlignCenter); soundLayout->addWidget(m_soundLabel);
    auto* grid=new QGridLayout; grid->setHorizontalSpacing(6); grid->setVerticalSpacing(2);
    const auto addKnob=[this](const QString& id,const QString& label,double low,double high,double initial) {
        auto* k=new SlicerKnob(label,this); k->setObjectName("SlicerSound_"+id);
        k->setRange(low,high); k->setDefaultValue(initial); k->setBipolar(low<0);
        k->setToolTip(label+tr(" · Drag vertically · Shift for precision · Double-click resets · Right-click to enter a value"));
        connect(k,&ui::Knob::valueChanged,this,[this,k,id](double v) {
            if(m_refreshing) return; beginSliceGesture(k);
            const double attack=readParameter("att"),decay=readParameter("dec"),sustain=readParameter("sus"),release=readParameter("rel");
            const int effect=m_effectChoice->currentIndex();
            if(id=="fxX") {
                if(effect==4)v=(std::pow(10.0,v/20.0)-1)/31;
                else if(effect==5)v=(24-v)/20;
                else v=std::log(v/20)/std::log(effect==6?100.0:1000.0);
            } else if(id=="fxY") {
                if(effect==4)v=std::log(v/250)/std::log(72.0);
                else if(effect==5)v=(v-1)/47;
                else v/=100;
            } else if(id=="mix")v/=100;
            editSelected(tr("Slice sound"),[=](Slice& s) {
                if(id=="gain") s.gain=float(std::pow(10.0,v/20.0));
                else if(id=="pan") s.pan=float(v);
                else if(id=="tune") s.transpose=std::int16_t(v);
                else if(id=="fine") s.fineTune=float(v);
                else if(id=="attack" || id=="release") {
                    if(s.useGlobalEnvelope) { s.attack=float(attack); s.decay=float(decay); s.sustain=float(sustain); s.release=float(release); }
                    s.useGlobalEnvelope=false;
                    if(id=="attack") s.attack=float(v); else s.release=float(v);
                } else if(id=="mix") s.effectMix=float(v);
                else if(id=="fxX") { if(s.effect) s.effectX=float(v); else s.cutoff=float(v); }
                else if(id=="fxY") { if(s.effect) s.effectY=float(v); else s.resonance=float(v); }
            });
        });
        connect(k,&ui::Knob::editFinished,this,[this,k]{if(m_fieldEditor==k) commitField();});
        m_sliceKnobs.insert(id,k); return k;
    };
    auto* gain=addKnob("gain",tr("Gain"),-60,24,0);
    gain->display=[this](double v){return QString::number(v,'f',1)+tr(" dB");};
    auto* pan=addKnob("pan",tr("Pan"),-1,1,0);
    pan->display=[this](double v){return std::abs(v)<.005?tr("Center"):QString::number(std::abs(v)*100,'f',0)+(v<0?tr(" L"):tr(" R"));};
    auto* attack=addKnob("attack",tr("Attack"),0,5,0); attack->display=[](double v){return msText(v);};
    auto* release=addKnob("release",tr("Release"),0,5,.05); release->display=[](double v){return msText(v);};
    auto* tune=addKnob("tune",tr("Pitch"),-48,48,0); tune->setStepped(true);
    tune->display=[this](double v){return QString::number(v,'f',0)+tr(" st");};
    auto* fine=addKnob("fine",tr("Detune"),-100,100,0);
    fine->display=[this](double v){return QString::number(v,'f',1)+tr(" ct");};
    const std::array<SlicerKnob*,6> knobs{gain,pan,attack,release,tune,fine};
    for(int i=0;i<6;++i) { knobs[i]->setFixedWidth(72); grid->addWidget(knobs[i],i/2,i%2,Qt::AlignCenter); }
    soundLayout->addStretch(); soundLayout->addLayout(grid); soundLayout->addStretch();
    auto* envelope=new QCheckBox(tr("Global ADSR"),sound); envelope->setObjectName("SlicerSoundEnvelope");
    envelope->setToolTip(tr("Attack and release follow the instrument. Moving either knob creates a local envelope."));
    connect(envelope,&QCheckBox::toggled,this,[this](bool v){if(!m_refreshing){commitField();editSelected(tr("Envelope inheritance"),[=](Slice& s){s.useGlobalEnvelope=v;});}});
    soundLayout->addWidget(envelope,0,Qt::AlignHCenter);
    auto* fx=new SlicerSurface(panel); auto* fxLayout=new QVBoxLayout(fx); fxLayout->setContentsMargins(8,8,8,8); fxLayout->setSpacing(6);
    m_effectChoice=new QComboBox(fx); m_effectChoice->setObjectName("SlicerEffectChoice"); m_effectChoice->setMinimumHeight(28);
    m_effectChoice->setAccessibleName(tr("Slice effect"));
    m_effectChoice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_effectChoice->setMinimumContentsLength(10);
    m_effectChoice->addItems({tr("Off"),tr("Low-pass"),tr("High-pass"),tr("Band-pass"),tr("Saturation"),tr("Bitcrusher"),tr("Ring modulation")});
    fxLayout->addWidget(m_effectChoice);
    connect(m_effectChoice,&QComboBox::currentIndexChanged,this,[this](int v){
        if(m_refreshing) return; commitField();
        editSelected(tr("Slice effect"),[=](Slice& s){s.filter=v<=3?quint8(v):0;s.effect=v>3?quint8(v-3):0;});
    });
    m_effectPad=new SlicerEffectPad(fx); fxLayout->addWidget(m_effectPad,1);
    connect(m_effectPad,&SlicerEffectPad::positionEdited,this,[this](double x,double y){
        if(m_refreshing) return; beginSliceGesture(m_effectPad);
        editSelected(tr("Shape slice effect"),[=](Slice& s){if(s.effect){s.effectX=float(x);s.effectY=float(y);}else{s.cutoff=float(x);s.resonance=float(y);}});
    });
    connect(m_effectPad,&SlicerEffectPad::editFinished,this,[this]{if(m_fieldEditor==m_effectPad)commitField();});
    auto* fxKnobs=new QHBoxLayout; fxKnobs->setSpacing(4);
    for(const auto& id:{QString("fxX"),QString("fxY"),QString("mix")}) {
        auto* k=addKnob(id,id=="mix"?tr("Mix"):QString(),0,id=="mix"?100:1,id=="mix"?100:.5);
        k->setMinimumWidth(72); k->setMaximumWidth(90); k->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        k->display=[](double v){return QString::number(v,'f',0)+"%";}; fxKnobs->addWidget(k);
    }
    fxLayout->addLayout(fxKnobs);
    m_effectChoice->setToolTip(tr("Each slice keeps its own effect"));
    row->addWidget(sound); row->addWidget(fx,1); return panel;
}
void SlicerPanel::beginSliceGesture(QWidget* editor) {
    if(m_refreshing || !m_controller || !slicer() || m_selected.empty() || m_fieldEditor==editor) return;
    commitField(); cancelWork();
    if(m_controller->beginSlicerEdit(m_channelId.toStdString(),m_slotId.toStdString())) m_fieldEditor=editor;
}
void SlicerPanel::refreshEffect(const Slice* s) {
    const int effect=s?(s->effect?3+s->effect:s->filter):0;
    m_effectChoice->setEnabled(s); m_effectChoice->setCurrentIndex(effect);
    QString xLabel=tr("Cutoff"),yLabel=tr("Resonance");
    if(effect==4){xLabel=tr("Drive");yLabel=tr("Tone");}
    if(effect==5){xLabel=tr("Bit depth");yLabel=tr("Downsample");}
    if(effect==6){xLabel=tr("Frequency");yLabel=tr("Spread");}
    const double x=s?(s->effect?s->effectX:s->cutoff):.5,y=s?(s->effect?s->effectY:s->resonance):.5;
    m_effectPad->setValues(x,y,xLabel,yLabel,s && effect>0);
    auto* xKnob=static_cast<SlicerKnob*>(m_sliceKnobs["fxX"]); auto* yKnob=static_cast<SlicerKnob*>(m_sliceKnobs["fxY"]);
    const auto xValue=[effect](double v){
        if(effect==4)return 20*std::log10(1+31*v);
        if(effect==5)return 24-20*v;
        return 20*std::pow(effect==6?100.0:1000.0,v);
    };
    const auto yValue=[effect](double v){if(effect==4)return 250*std::pow(72.0,v);if(effect==5)return 1+std::round(47*v);return v*100;};
    if(m_effectUiType!=effect) {
    m_effectUiType=effect;
    xKnob->setAccessibleName(effect<=3?tr("Frequency"):effect==5?tr("Bits"):xLabel); yKnob->setAccessibleName(effect==5?tr("Rate"):yLabel);
    xKnob->setCaption(xLabel); yKnob->setCaption(yLabel);
    xKnob->setToolTip(xLabel+tr(" · Right-click to enter a value"));
    yKnob->setToolTip(yLabel+tr(" · Right-click to enter a value"));
    xKnob->setRange(effect==5?4:xValue(0),effect==5?24:xValue(1)); xKnob->setLogarithmic(effect<=3 || effect==6);
    yKnob->setRange(yValue(0),yValue(1)); yKnob->setLogarithmic(effect==4); yKnob->setStepped(effect==5);
    xKnob->setDefaultValue(xValue(effect<=3?1:.5)); yKnob->setDefaultValue(yValue(effect<=3?0:.5));
    xKnob->display=[this,effect](double v){
        if(effect==5)return QString::number(v,'f',1)+tr(" bit");
        if(effect==4)return QString::number(v,'f',1)+tr(" dB");
        return v>=1000?QString::number(v/1000,'f',1)+tr(" kHz"):QString::number(v,'f',0)+tr(" Hz");
    };
    yKnob->display=[this,effect](double v){
        if(effect==5)return QString::number(v,'f',0)+"×";
        if(effect==4)return v>=1000?QString::number(v/1000,'f',1)+tr(" kHz"):QString::number(v,'f',0)+tr(" Hz");
        return QString::number(v,'f',0)+"%";
    };
    }
    for(const auto& [name,value]:std::array<std::pair<QString,double>,3>{{{"fxX",xValue(x)},{"fxY",yValue(y)},{"mix",s?s->effectMix*100:100.0}}}) {
        auto* k=m_sliceKnobs[name]; k->setEnabled(s && effect>0); if(k!=m_fieldEditor && !k->isEditing()) k->setValue(value);
    }
}

ui::Knob* SlicerPanel::knob(const QString& id, const QString& caption) {
    const auto* info = infoFor(id); if (!info) return nullptr;
    auto* control = new SlicerKnob(caption, this); control->setObjectName("SlicerParam_" + id); control->setAccessibleName(caption);
    control->setRange(info->minValue, info->maxValue); control->setDefaultValue(info->defaultValue);
    control->setStepped(info->isStepped); control->setBipolar(info->minValue < 0);
    control->setFixedWidth(160); control->setAutomatable(true);
    control->display=[this, index = info->index, id](double value) {
        if (id == "att" || id == "dec" || id == "rel") return value < 1 ? QString::number(value * 1000, 'f', 0) + tr(" ms") : QString::number(value, 'f', 2) + tr(" s");
        if (id == "tune" || id == "bend") return QString::number(value, 'f', 0) + tr(" st");
        if (id == "finepitch") return QString::number(value, 'f', 1) + tr(" ct");
        if (id == "keytrack" && value == 0) return tr("Off");
        if (id == "poly" || id == "bits") return QString::number(value, 'f', 0);
        if (id == "vol") return (value <= .0001 ? QString("−∞") : QString::number(20 * std::log10(value), 'f', 1)) + tr(" dB");
        return QString::fromStdString(p::parameterText(index, value));
    };
    control->setToolTip(caption + tr(" · Right-click for automation"));
    control->setProperty("parameterId", id);
    connect(control, &ui::Knob::valueChanged, this, [this, id](double v) { if (m_refreshing) return; beginGesture(id); writeParameter(id, v); });
    connect(control, &ui::Knob::editFinished, this, [this, id] { endGesture(id); });
    connect(control, &ui::Knob::automateRequested, this, [this, id] { emit automationRequested(id); });
    m_knobs.insert(id, control); return control;
}
QWidget* SlicerPanel::buildPlayback() {
    auto* page = new QWidget; auto* layout = new QVBoxLayout(page); layout->setContentsMargins(10, 12, 10, 12); layout->setSpacing(10);
    auto* switches = new QWidget; auto* f = form(switches); f->setContentsMargins(0, 0, 0, 0);
    auto* mode = choice(f, tr("Playback"), "playmode", {tr("Gate"), tr("One shot")});
    auto* choke = choice(f, tr("Choke"), "choke", {tr("Off"), tr("All voices"), tr("By group")});
    for (auto* box : {mode, choke}) {
        const auto id = box == mode ? QString("playmode") : QString("choke");
        connect(box, &QComboBox::currentIndexChanged, this, [this, id](int v) { if (m_refreshing) return; beginGesture(id); writeParameter(id, v); endGesture(id); });
    }
    layout->addWidget(switches);
    const std::array<std::pair<QString, QString>, 14> controls{{
        {"vol", tr("Volume")}, {"pan", tr("Pan")}, {"tune", tr("Transpose")}, {"finepitch", tr("Fine tune")},
        {"root", tr("Pitch root")}, {"keytrack", tr("Key track")}, {"att", tr("Attack")}, {"dec", tr("Decay")},
        {"sus", tr("Sustain")}, {"rel", tr("Release")}, {"gate", tr("Length")}, {"veld", tr("Velocity")},
        {"poly", tr("Polyphony")}, {"bend", tr("Bend range")}
    }};
    auto* grid = new QGridLayout; grid->setSpacing(8); int i = 0;
    for (const auto& [id, text] : controls) { grid->addWidget(knob(id, text), i / 2, i % 2); ++i; }
    for (int c = 0; c < 2; ++c) grid->setColumnStretch(c, 1);
    layout->addLayout(grid); layout->addStretch();
    auto* help = new QLabel(tr("One shot finishes unlooped slices after Note Off. Loops always release on Note Off. Right-click a knob to create automation."), page); help->setWordWrap(true); help->setObjectName("SlicerHelp"); layout->addWidget(help);
    return page;
}
QWidget* SlicerPanel::buildProcessing() {
    auto* page = new QWidget; auto* layout = new QVBoxLayout(page); layout->setContentsMargins(10, 12, 10, 12);
    auto* grid = new QGridLayout;
    grid->addWidget(knob("drive", tr("Drive")), 0, 0); grid->addWidget(knob("crushmix", tr("Crusher mix")), 0, 1);
    grid->addWidget(knob("bits", tr("Bit depth")), 1, 0); grid->addWidget(knob("downsample", tr("Downsample")), 1, 1); layout->addLayout(grid);
    auto* random = new QWidget; auto* f = form(random); f->setContentsMargins(0, 12, 0, 0);
    auto* heading = new QLabel(tr("Randomize selected slices"), random); heading->setObjectName("SlicerGroupTitle"); f->addRow(heading);
    for (const auto& [id, text] : std::array<std::pair<QString, QString>, 6>{{{"randPitch", tr("Pitch")}, {"randGain", tr("Gain")}, {"randPan", tr("Pan")}, {"randReverse", tr("Reverse")}, {"randFilter", tr("Filter")}, {"randKeys", tr("MIDI assignments")}}}) toggle(f, text, id);
    m_toggles["randPitch"]->setChecked(true);
    number(f, tr("Pitch range ±"), "randPitchRange", 0, 48, 0, tr(" st"))->setValue(12);
    number(f, tr("Gain range ±"), "randGainRange", 0, 24, 1, tr(" dB"))->setValue(6);
    number(f, tr("Pan range ±"), "randPanRange", 0, 1)->setValue(1);
    number(f, tr("Filter minimum"), "randCutoffMin", 20, 20000, 0, tr(" Hz"))->setValue(80);
    number(f, tr("Filter maximum"), "randCutoffMax", 20, 20000, 0, tr(" Hz"))->setValue(12000);
    number(f, tr("Seed"), "randSeed", 0, 2147483647, 0)->setValue(1);
    auto* apply = new QPushButton(tr("Randomize"), random); f->addRow(apply); connect(apply, &QPushButton::clicked, this, [this] { randomizeSelection(); });
    auto* reroll = new QPushButton(tr("New seed + randomize"), random); f->addRow(reroll); connect(reroll, &QPushButton::clicked, this, [this] { randomizeSelection(true); });
    auto* help = new QLabel(tr("Locked slices are excluded. MIDI randomization exchanges existing notes and keeps them unique."), random); help->setWordWrap(true); help->setObjectName("SlicerHelp"); f->addRow(help);
    layout->addWidget(random); layout->addStretch(); return page;
}
void SlicerPanel::commitField() {
    if (!m_fieldEditor) return; m_fieldEditor = nullptr;
    if (m_controller && m_controller->commitSlicerEdit("Edit Slicer settings")) emit projectEdited();
}
void SlicerPanel::editTable(const QString& label, const std::function<void(Table&)>& edit) {
    if (m_refreshing || !slicer() || !m_controller) return;
    cancelWork(); m_dragFiles.clear(); const auto instance = slicer(); auto table = std::make_shared<Table>();
    if (instance->state.table) *table = *instance->state.table;
    edit(*table); table->rebuild();
    const bool field = m_fieldEditor != nullptr;
    if (!field && !m_controller->beginSlicerEdit(m_channelId.toStdString(), m_slotId.toStdString())) return;
    m_controller->updateSlicerEdit(table, instance->state.analysis);
    if (!field && m_controller->commitSlicerEdit(label.toStdString())) emit projectEdited();
    refresh();
}
void SlicerPanel::editSelected(const QString& label, const std::function<void(Slice&)>& edit) {
    if (m_selected.empty()) return;
    editTable(label, [this, &edit](Table& table) { for (quint32 i = 0; i < table.count; ++i) if (m_selected.contains(table.slices[i].id)) edit(table.slices[i]); });
}
void SlicerPanel::setAnalysis(const std::function<void(Analysis&)>& edit, bool remap) {
    if (m_refreshing || !slicer() || !m_controller) return;
    cancelWork(); m_dragFiles.clear(); const auto instance = slicer(); auto settings = instance->state.analysis; edit(settings);
    if (!m_fieldEditor && !m_controller->beginSlicerEdit(m_channelId.toStdString(), m_slotId.toStdString())) return;
    std::shared_ptr<const Table> table = instance->state.table;
    if (remap && table && table->count) {
        auto next = std::make_shared<Table>(*table); const bool fallback = slicing::assignKeys(*next, settings); table = std::move(next);
        if (fallback) status(tr("Scale has too few notes; chromatic MIDI layout used"));
    }
    m_controller->updateSlicerEdit(table, settings);
    if (!m_fieldEditor && m_controller->commitSlicerEdit(remap ? "Remap slice MIDI notes" : "Edit slicing settings")) emit projectEdited();
    refresh();
}
std::vector<std::uint32_t> SlicerPanel::selectedIds() const {
    std::vector<std::uint32_t> ids;
    const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table) return ids;
    const auto& table = *snapshot->state.table;
    ids.reserve(m_selected.size());
    for (quint32 i = 0; i < table.count; ++i)
        if (m_selected.contains(table.slices[i].id)) ids.push_back(table.slices[i].id);
    return ids;
}
void SlicerPanel::selectSlice(int index, Qt::KeyboardModifiers modifiers, bool play) {
    commitField();
    const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table || index < 0 || index >= int(snapshot->state.table->count)) return;
    const auto table = snapshot->state.table; const auto id = table->slices[index].id;
    if (modifiers.testFlag(Qt::ShiftModifier) && m_selectionAnchor >= 0) {
        for (int i = std::min(index, m_selectionAnchor); i <= std::max(index, m_selectionAnchor) && i < int(table->count); ++i) m_selected.insert(table->slices[i].id);
    } else if (modifiers.testFlag(Qt::ControlModifier)) {
        if (m_selected.contains(id)) m_selected.remove(id); else m_selected.insert(id); m_selectionAnchor = index;
    } else { m_selected = {id}; m_selectionAnchor = index; }
    m_primary = m_selected.contains(id) ? id : (m_selected.empty() ? 0 : *m_selected.begin());
    const int bank = index / 16; if (m_bank->currentIndex() != bank) { QSignalBlocker block(m_bank); m_bank->setCurrentIndex(bank); }
    refreshSelection(); if (play && modifiers == Qt::NoModifier) audition(index);
}
void SlicerPanel::editBoundary(bool end, double ms) {
    const auto snapshot = slicer();
    if (m_refreshing || !snapshot || !snapshot->state.audio) return;
    const bool ownsGesture = !m_fieldEditor;
    if (ownsGesture) {
        if (!m_controller->beginSlicerEdit(m_channelId.toStdString(), m_slotId.toStdString())) return;
        m_fieldEditor = this;
    }
    const auto audio = snapshot->state.audio; const auto analysis = snapshot->state.analysis;
    const quint32 frame = quint32(std::clamp(ms * audio->sampleRate() / 1000.0,0.0,double(audio->frames())));
    const quint32 minimum = quint32(std::clamp(analysis.minimumMs * audio->sampleRate() / 1000.0, 1.0, double(audio->frames())));
    editTable(tr("Move slice boundary"), [this, end, frame, audio, analysis, minimum](Table& t) {
        const int i = t.indexForId(m_primary); if (i < 0) return;
        const int right = end ? i + 1 : i;
        const auto snapped = analysis.zeroCrossing && right > 0 && right < int(t.count) ? slicing::snapToZero(*audio, frame, t.slices[right-1].start+1, t.slices[right].end-1) : frame;
        if (end) {
            if (i + 1 < int(t.count)) slicing::moveBoundary(t, i + 1, snapped, minimum);
            else t.slices[i].end = std::clamp(frame, t.slices[i].start + std::min(minimum, t.frames - t.slices[i].start), t.frames);
        } else {
            if (i > 0) slicing::moveBoundary(t, i, snapped, minimum); else t.slices[i].start = std::min(frame, t.slices[i].end - std::min(minimum, t.slices[i].end));
        }
    });
    if (const auto updated = slicer(); updated && updated->state.table && updated->state.table->count) {
        const auto table = updated->state.table;
        setAnalysis([table](Analysis& a) { a.rangeStart = table->slices[0].start; a.rangeEnd = table->slices[table->count - 1].end; });
    }
    if (ownsGesture) commitField();
}

void SlicerPanel::swapKeys(quint32 from, quint32 to) {
    if (from == to) return; commitField(); releaseAudition();
    editTable(tr("Exchange slice MIDI notes"), [=](Table& table) { const int a = table.indexForId(from), b = table.indexForId(to); if (a >= 0 && b >= 0) std::swap(table.slices[a].key, table.slices[b].key); });
}
void SlicerPanel::splitAt(quint32 frame) {
    commitField(); const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.audio) return;
    const auto settings = snapshot->state.analysis; const auto source = snapshot->state.audio;
    const auto minimum = quint32(std::clamp(settings.minimumMs * source->sampleRate() / 1000, 1.0, double(source->frames())));
    editTable(tr("Split slice"), [=](Table& table) mutable {
        if (settings.zeroCrossing) for (quint32 i=0;i<table.count;++i) if (frame > table.slices[i].start && frame < table.slices[i].end)
            frame = slicing::snapToZero(*source,frame,table.slices[i].start+1,table.slices[i].end-1);
        slicing::split(table, frame, minimum);
    });
}
void SlicerPanel::mergeAt(int index) {
    commitField(); editTable(tr("Merge slices"), [=](Table& table) { slicing::merge(table, index); });
}
void SlicerPanel::copySettings() {
    const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table) return; const auto t = snapshot->state.table; const int i = t->indexForId(m_primary);
    if (i >= 0) { m_copied = t->slices[i]; status(tr("Slice settings copied")); }
}
void SlicerPanel::pasteSettings() {
    if (!m_copied) return; commitField(); const Slice copied = *m_copied;
    editSelected(tr("Paste slice settings"), [=](Slice& s) { const auto id = s.id; const auto start = s.start, end = s.end; const auto key = s.key; s = copied; s.id = id; s.start = start; s.end = end; s.key = key; });
}
void SlicerPanel::resetSettings() {
    commitField(); editSelected(tr("Reset slice settings"), [](Slice& s) { const auto id = s.id, start = s.start, end = s.end; const auto key = s.key; s = Slice{}; s.id = id; s.start = start; s.end = end; s.key = key; });
}
void SlicerPanel::audition(int index) {
    releaseAudition(); const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table) return;
    const auto* slice = snapshot->state.table->forIndex(quint32(index)); if (!slice) return;
    if (m_controller->liveNoteOn(m_channelId.toStdString(), slice->key, 127)) { m_auditionKey = slice->key; m_auditionOutstanding = true; }
}
void SlicerPanel::releaseAudition(bool panic) {
    if (m_controller && (m_auditionKey >= 0 || (panic && m_auditionOutstanding))) {
        if (panic) m_controller->liveMidiEvent(m_channelId.toStdString(), 0xB0, 120, 0);
        else m_controller->liveNoteOff(m_channelId.toStdString(), m_auditionKey);
    }
    m_auditionKey = -1; if (panic) m_auditionOutstanding = false;
}
void SlicerPanel::beginGesture(const QString& id) {
    if (m_gestureStart.contains(id)) return; commitField(); m_gestureStart.insert(id, readParameter(id));
}
void SlicerPanel::endGesture(const QString& id) {
    if (!m_gestureStart.contains(id) || !m_controller) return;
    const double before = m_gestureStart.take(id);
    m_controller->commitInsertParameterEdit(m_channelId.toStdString(), m_slotId.toStdString(), id.toStdString(), before, "Edit Slicer " + id.toStdString());
    if (before != readParameter(id)) emit projectEdited();
}
void SlicerPanel::writeParameter(const QString& id, double value) {
    cancelWork(); m_dragFiles.clear();
    if (m_controller) m_controller->setInsertParameter(m_channelId.toStdString(), m_slotId.toStdString(), id.toStdString(), value);
}
double SlicerPanel::readParameter(const QString& id) const {
    const auto* info = infoFor(id);
    return info && m_controller ? m_controller->insertParameter(m_channelId.toStdString(), m_slotId.toStdString(), info->id)
                                : (info ? info->defaultValue : 0);
}
void SlicerPanel::status(const QString& message) {
    m_statusLabel->setText(message); m_fileLabel->setText(message); m_fileLabel->setToolTip(message);
    QTimer::singleShot(8000, this, [this, message] {
        if (m_busy || m_fileLabel->text() != message) return;
        m_fileLabel->setText(m_fileLabel->property("sourceName").toString());
        m_fileLabel->setToolTip(m_fileLabel->property("sourceInfo").toString());
    });
}

void SlicerPanel::refresh() {
    const auto instance = slicer();
    if ((instance ? instance->identity : daw::PluginIdentity{}) != m_seenInstance || (instance && (instance->state.audio != m_seenAudio || instance->sourceRevision != m_seenSource))) {
        cancelWork(); releaseAudition(true); m_waveform->cancelDrag(); m_fieldEditor = nullptr;
        m_seenInstance = instance ? instance->identity : daw::PluginIdentity{}; m_seenAudio = instance ? instance->state.audio : nullptr; m_seenSource = instance ? instance->sourceRevision : 0;
        m_seenTable.reset(); m_selected.clear(); m_primary = 0; m_selectionAnchor = -1; m_dragFiles.clear(); m_dragToken = QUuid::createUuid().toString();
        m_waveform->setSample(m_seenAudio);
        m_fileLabel->setText(instance && m_seenAudio ? QString::fromStdString(instance->name) : tr("No sample loaded"));
        m_fileLabel->setToolTip(m_seenAudio ? QString::fromStdString(instance->state.path) + "\n" + tr("%1 · %2 Hz · %3 channels · up to 128 slices").arg(msText(double(m_seenAudio->frames()) / m_seenAudio->sampleRate())).arg(m_seenAudio->sampleRate(), 0, 'f', 0).arg(m_seenAudio->channels()) : tr("Load audio to start chopping and remixing"));
        m_fileLabel->setProperty("sourceName", m_fileLabel->text()); m_fileLabel->setProperty("sourceInfo", m_fileLabel->toolTip());
    }
    m_refreshing = true;
    const auto table = instance ? instance->state.table : nullptr;
    if (table != m_seenTable) {
        m_seenTable = table; m_waveform->setTable(table);
        for (auto it = m_selected.begin(); it != m_selected.end();) { if (!table || table->indexForId(*it) < 0) it = m_selected.erase(it); else ++it; }
        if (table && table->count && m_selected.empty()) { m_primary = table->slices[0].id; m_selected.insert(m_primary); m_selectionAnchor = 0; }
        if (table && table->indexForId(m_primary) < 0) m_primary = m_selected.empty() ? 0 : *m_selected.begin();
        const int pages = std::max(1, int((table ? table->count : 0) + 15) / 16);
        if (m_bank->count() != pages) {
            QSignalBlocker block(m_bank); const int bank = m_bank->currentIndex(); m_bank->clear();
            for (int i = 0; i < pages; ++i) m_bank->addItem(QString("%1–%2").arg(i * 16 + 1).arg(std::min((i + 1) * 16, table ? int(table->count) : 16)));
            m_bank->setCurrentIndex(std::clamp(bank, 0, pages - 1));
        }
    }
    const auto setNumber = [this](const QString& name, double v) {
        auto* box = m_numbers.value(name); if (box && box != m_fieldEditor && !box->hasFocus() && (!box->focusWidget() || !box->focusWidget()->hasFocus())) box->setValue(v);
    };
    if (instance) {
        const auto a = instance->state.analysis;
        m_quickMode->setCurrentIndex(int(a.mode)); m_quickCount->setValue(a.targetCount);
        m_quickCount->setEnabled(a.mode!=p::SliceMode::Manual && (a.mode!=p::SliceMode::Grid || a.gridBeats==0));
        m_choices["mode"]->setCurrentIndex(int(a.mode)); setNumber("count", a.targetCount); setNumber("threshold", a.sensitivity);
        setNumber("pre", a.preAttackMs); setNumber("minimum", a.minimumMs); setNumber("spread", a.randomSpread);
        setNumber("seed", double(a.seed)); setNumber("bpm", a.sourceBpm); setNumber("layoutRoot", a.rootNote);
        m_toggles["zero"]->setChecked(a.zeroCrossing); m_choices["scale"]->setCurrentIndex(a.scale); m_choices["layout"]->setCurrentIndex(a.descending ? 1 : 0);
        m_choices["grid"]->setCurrentIndex(std::max(0, m_choices["grid"]->findData(a.gridBeats)));
        m_numbers["count"]->setEnabled(a.mode != p::SliceMode::Manual && (a.mode != p::SliceMode::Grid || a.gridBeats == 0));
        m_numbers["threshold"]->setEnabled(a.mode == p::SliceMode::Transients); m_numbers["pre"]->setEnabled(a.mode == p::SliceMode::Transients);
        m_numbers["spread"]->setEnabled(a.mode == p::SliceMode::Random); m_numbers["seed"]->setEnabled(a.mode == p::SliceMode::Random);
        m_choices["grid"]->setEnabled(a.mode == p::SliceMode::Grid);
        if (m_seenAudio) { const auto rate = m_seenAudio->sampleRate(); for (const auto& name : {"rangeStart","rangeEnd","start","end"}) m_numbers[name]->setMaximum(double(m_seenAudio->frames()) / rate * 1000); setNumber("rangeStart", a.rangeStart / rate * 1000.0); setNumber("rangeEnd", (a.rangeEnd > a.rangeStart ? a.rangeEnd : m_seenAudio->frames()) / rate * 1000.0); }
        m_waveform->setRange(a.rangeStart, a.rangeEnd);
        for (auto it = m_knobs.begin(); it != m_knobs.end(); ++it) if (!it.value()->isEditing() && !m_gestureStart.contains(it.key())) it.value()->setValue(readParameter(it.key()));
        for (const auto& id : {QString("playmode"), QString("choke")}) m_choices[id]->setCurrentIndex(int(readParameter(id)));
    }
    m_sliceButton->setEnabled(instance && m_seenAudio && !m_busy);
    m_quickSlice->setEnabled(m_sliceButton->isEnabled()); m_quickMode->setEnabled(instance && m_seenAudio && !m_busy);
    m_dragButton->setText(m_dragFiles.empty() ? tr("Prepare WAV drag") : tr("Drag WAV · ready"));
    m_refreshing = false; refreshSelection();
}
void SlicerPanel::refreshSelection() {
    const bool previous = m_refreshing; m_refreshing = true;
    const auto snapshot = slicer(true);
    const auto table = snapshot ? snapshot->state.table : nullptr;
    const int index = table ? table->indexForId(m_primary) : -1; const auto* s = index >= 0 ? &table->slices[index] : nullptr;
    const double rate = m_seenAudio ? std::max(1.0, m_seenAudio->sampleRate()) : 48000.0;
    m_waveform->setSelection(m_selected, m_primary);
    m_soundLabel->setText(s?tr("Slice %1").arg(index+1,2,10,QChar('0')):tr("Select a slice"));
    m_soundLabel->setToolTip(s ? tr("Slice %1 · %2 selected · %3").arg(index + 1).arg(m_selected.size()).arg(noteName(s->key)) : tr("Select a slice"));
    auto* envelope=findChild<QCheckBox*>("SlicerSoundEnvelope"); envelope->setEnabled(s); envelope->setChecked(s && s->useGlobalEnvelope);
    for(const auto& name:{"gain","pan","attack","release","tune","fine"}) m_sliceKnobs[name]->setEnabled(s);
    if(s) {
        const std::array<std::pair<QString,double>,6> values{{
            {"gain",20*std::log10(std::max(1e-3f,s->gain))},{"pan",s->pan},{"tune",s->transpose},{"fine",s->fineTune},
            {"attack",s->useGlobalEnvelope?readParameter("att"):s->attack},{"release",s->useGlobalEnvelope?readParameter("rel"):s->release}}};
        for(const auto& [name,value]:values){auto* k=m_sliceKnobs[name];if(k!=m_fieldEditor && !k->isEditing())k->setValue(value);}
    }
    refreshEffect(s);
    for (int i = 0; i < 16; ++i) {
        const int padIndex = std::max(0, m_bank->currentIndex()) * 16 + i;
        const auto* slice = table ? table->forIndex(quint32(padIndex)) : nullptr;
        m_pads[i]->configure(slice, padIndex, rate, slice && m_selected.contains(slice->id), slice && (slice->key >= 0 && slice->key < 128 && snapshot->activeKeys[std::size_t(slice->key)]), m_dragToken);
    }
    const auto setNumber = [this](const QString& name, double value) { auto* box = m_numbers.value(name); if (box && box != m_fieldEditor && !box->hasFocus() && (!box->focusWidget() || !box->focusWidget()->hasFocus())) box->setValue(value); };
    if (s) {
        setNumber("key", s->key); setNumber("start", s->start / rate * 1000); setNumber("end", s->end / rate * 1000);
        setNumber("gain", 20 * std::log10(std::max(1e-3f, s->gain))); setNumber("slicePan", s->pan); setNumber("sliceTune", s->transpose); setNumber("sliceFine", s->fineTune);
        setNumber("group", s->chokeGroup); setNumber("cutoff", 20 * std::pow(1000.0, s->cutoff)); setNumber("resonance", s->resonance);
        setNumber("sliceAttack", s->attack); setNumber("sliceDecay", s->decay); setNumber("sliceSustain", s->sustain); setNumber("sliceRelease", s->release);
        setNumber("fadeIn", s->fadeInMs); setNumber("fadeOut", s->fadeOutMs); setNumber("crossfade", s->crossfadeMs);
        m_toggles["reverse"]->setChecked(s->flags & p::kSliceReverse); m_toggles["mute"]->setChecked(s->flags & p::kSliceMuted);
        m_toggles["lock"]->setChecked(s->locked); m_toggles["inherit"]->setChecked(s->useGlobalEnvelope);
        m_choices["filter"]->setCurrentIndex(s->filter); m_choices["loop"]->setCurrentIndex(s->loopMode ? s->loopMode : (s->flags & p::kSliceLoop ? 1 : 0));
    }
    auto* norm = findChild<QLabel*>("SlicerNormalization"); if (norm) norm->setText(s ? QString::number(20 * std::log10(std::max(1e-12f, s->normalization)), 'f', 1) + tr(" dB") : "—");
    for (const auto& name : {"sliceAttack", "sliceDecay", "sliceSustain", "sliceRelease"}) m_numbers[name]->setEnabled(s && !s->useGlobalEnvelope);
    // One primary slice owns each outer edge; inner changes update its neighbour.
    m_numbers["start"]->setEnabled(s); m_numbers["end"]->setEnabled(s);
    m_numbers["crossfade"]->setEnabled(s && (s->loopMode == 1 || (s->loopMode == 0 && (s->flags & p::kSliceLoop))));
    m_refreshing = previous;
}
QString SlicerPanel::mediaCache() const {
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/slicer-media";
    QDir().mkpath(dir); return dir;
}
void SlicerPanel::cancelWork() {
    ++m_workGeneration; if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed); m_cancelled.reset(); m_busy = false;
    if (m_cancelButton) m_cancelButton->hide();
    if (m_sliceButton) { const auto snapshot = slicer(); m_sliceButton->setEnabled(snapshot && snapshot->state.audio); }
}
void SlicerPanel::startWork(const QString& message, std::function<WorkResult(const std::function<bool()>&)> work, std::function<void(WorkResult)> finish) {
    commitField(); cancelWork(); m_dragFiles.clear(); const auto instance = slicer(); if (!instance) return;
    m_busy = true; m_cancelButton->show(); m_sliceButton->setEnabled(false); status(message);
    const auto cancelled = m_cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto generation = m_workGeneration, sourceRevision = instance->sourceRevision; const auto source = instance->state.audio; const auto table = instance->state.table; const auto analysis = instance->state.analysis;
    const auto identity = instance->identity;
    const QPointer<SlicerPanel> guard(this);
    QThreadPool::globalInstance()->start([work = std::move(work), finish = std::move(finish), cancelled, generation, sourceRevision, source, table, analysis, identity, guard] {
        WorkResult result;
        try { result = work([cancelled] { return !cancelled->load(std::memory_order_relaxed); }); }
        catch (const std::exception& e) { result.error = e.what(); }
        catch (...) { result.error = "Operation failed."; }
        QMetaObject::invokeMethod(qApp, [guard, cancelled, generation, sourceRevision, source, table, analysis, identity, finish = std::move(finish), result = std::move(result)]() mutable {
            if (!guard || cancelled->load(std::memory_order_relaxed) || guard->m_workGeneration != generation) return;
            const auto current = guard->slicer();
            if (!current || current->identity != identity || current->sourceRevision != sourceRevision || current->state.audio != source || current->state.table != table || current->state.analysis != analysis) {
                guard->cancelWork(); guard->status(tr("Source or instrument changed; result discarded")); return;
            }
            guard->m_busy = false; guard->m_cancelled.reset(); guard->m_cancelButton->hide();
            if (!result.ok) { guard->status(tr("Operation failed; previous state kept") + (result.error.empty() ? QString() : " · " + QString::fromStdString(result.error))); guard->refresh(); return; }
            finish(std::move(result)); guard->refresh();
        }, Qt::QueuedConnection);
    });
}
void SlicerPanel::loadSample(const QString& supplied) {
    const auto path = supplied.isEmpty() ? QFileDialog::getOpenFileName(this, tr("Load Slicer sample"), {}, ui::audioNameFilter()) : supplied;
    const auto snapshot = slicer();
    if (path.isEmpty() || !snapshot) return;
    auto state = snapshot->state; state.path = path.toStdString(); state.analysis.rangeStart = state.analysis.rangeEnd = 0; state.analysis.sourceBpm = m_controller->tempo();
    startWork(tr("Loading and analysing sample…"), [state](const auto& keepGoing) mutable {
        WorkResult result; audio::platform::DecodedAudio decoded; audio::platform::DecodeOptions opts; opts.keepGoing = keepGoing;
        const auto rc = audio::platform::decodeAudioFile(state.path, decoded, opts);
        if (!rc || decoded.frames == 0 || decoded.frames > UINT32_MAX || decoded.channels == 0 || !keepGoing()) { result.error = rc.message(); return result; }
        state.audio = daw::engine::SampleBuffer::fromInterleaved(decoded.interleaved, decoded.channels, quint32(decoded.frames), decoded.sampleRate);
        state.table = std::make_shared<Table>(slicing::cut(*state.audio, state.analysis, keepGoing));
        if (!keepGoing() || state.table->count == 0) return result;
        result.state = std::move(state); result.ok = true; return result;
    }, [this](WorkResult result) {
        result.state.parameters = slicer()->state.parameters;
        releaseAudition(true); if (m_controller->applySlicerState(m_channelId.toStdString(), m_slotId.toStdString(), result.state, "Load Slicer sample")) emit projectEdited();
        status(result.state.table->chromaticFallback ? tr("Scale has too few notes; chromatic MIDI layout used") : tr("Sample loaded and sliced"));
    });
}
void SlicerPanel::runSlice(bool newSeed) {
    const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.audio) return; auto state = snapshot->state;
    if (newSeed) ++state.analysis.seed;
    startWork(tr("Analysing slice boundaries…"), [state](const auto& keepGoing) mutable {
        WorkResult result; auto table = std::make_shared<Table>(slicing::cut(*state.audio, state.analysis, keepGoing));
        table->nextId = state.table ? state.table->nextId : 1;
        for (quint32 i = 0; i < table->count; ++i) table->slices[i].id = 0;
        table->rebuild(); state.table = std::move(table);
        result.ok = keepGoing() && state.table->count > 0; result.state = std::move(state); return result;
    }, [this](WorkResult result) {
        // Only the table and analysis change: currently playing voices retain their bounds.
        if (m_controller->beginSlicerEdit(m_channelId.toStdString(), m_slotId.toStdString())) {
            m_controller->updateSlicerEdit(result.state.table, result.state.analysis);
            if (m_controller->commitSlicerEdit("Slice sample")) emit projectEdited();
        }
        status(result.state.table->chromaticFallback ? tr("Scale has too few notes; chromatic MIDI layout used") : tr("Slicing complete · %1 slices").arg(result.state.table->count));
    });
}
void SlicerPanel::normalizeSelection() {
    const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table || m_selected.empty()) return;
    const auto state = snapshot->state; const auto ids = selectedIds();
    startWork(tr("Measuring slice peaks…"), [state, ids](const auto& keepGoing) {
        WorkResult result; auto table = std::make_shared<Table>(*state.table);
        result.ok = slicing::normalize(*state.audio, *table, ids, keepGoing); result.table = std::move(table); return result;
    }, [this](WorkResult result) {
        m_controller->publishSlicerTable(m_channelId.toStdString(), m_slotId.toStdString(), result.table); emit projectEdited(); status(tr("Selected slices normalized to −1 dBFS"));
    });
}
void SlicerPanel::randomizeSelection(bool newSeed) {
    if (!slicer() || m_selected.empty()) return; commitField();
    if (newSeed) { QSignalBlocker block(m_numbers["randSeed"]); m_numbers["randSeed"]->setValue(std::fmod(m_numbers["randSeed"]->value() + 1, 2147483648.0)); }
    slicing::RandomSettings options; options.seed = quint64(m_numbers["randSeed"]->value());
    options.pitch = m_toggles["randPitch"]->isChecked(); options.gain = m_toggles["randGain"]->isChecked(); options.pan = m_toggles["randPan"]->isChecked();
    options.reverse = m_toggles["randReverse"]->isChecked(); options.filter = m_toggles["randFilter"]->isChecked(); options.keys = m_toggles["randKeys"]->isChecked();
    options.pitchRange = m_numbers["randPitchRange"]->value(); options.gainRangeDb = m_numbers["randGainRange"]->value(); options.panRange = m_numbers["randPanRange"]->value();
    options.cutoffMin = std::log(m_numbers["randCutoffMin"]->value() / 20) / std::log(1000.0); options.cutoffMax = std::log(m_numbers["randCutoffMax"]->value() / 20) / std::log(1000.0);
    const auto ids = selectedIds(); editTable(tr("Randomize slices"), [=](Table& t) { slicing::randomize(t, ids, options); }); status(tr("Randomization applied; locked slices kept"));
}
void SlicerPanel::preset(bool load) {
    if (!slicer()) return;
    const auto path = load ? QFileDialog::getOpenFileName(this, tr("Load portable Slicer preset"), {}, "Slicer (*.vltslicer)") : QFileDialog::getSaveFileName(this, tr("Save portable Slicer preset"), {}, "Slicer (*.vltslicer)");
    const auto snapshot = slicer();
    if (path.isEmpty() || !snapshot) return; const auto state = snapshot->state; const auto cache = mediaCache();
    const QString destination = load || path.endsWith(".vltslicer", Qt::CaseInsensitive) ? path : path + ".vltslicer";
    startWork(load ? tr("Importing portable preset…") : tr("Saving portable preset…"), [=](const auto& keepGoing) {
        WorkResult result;
        result.ok = load ? slicing::loadPreset(destination.toStdString(), cache.toStdString(), result.state, result.error, keepGoing) : slicing::savePreset(destination.toStdString(), state, result.error, keepGoing);
        return result;
    }, [this, load](WorkResult result) {
        if (load) { releaseAudition(true); if (m_controller->applySlicerState(m_channelId.toStdString(), m_slotId.toStdString(), result.state, "Load Slicer preset")) emit projectEdited(); }
        status(load ? tr("Portable preset loaded") : tr("Portable preset saved with embedded audio"));
    });
}
void SlicerPanel::exportMidi(bool toFile) {
    commitField(); const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table || !snapshot->state.audio) return;
    const auto phrase = slicing::midiPhrase(snapshot->state, slicing::PhraseOrder(m_phraseOrder->currentIndex()), quint64(m_numbers["randSeed"]->value()));
    if (!toFile) {
        const auto group = m_controller->beginUndoGroup(); const auto tempo = m_controller->tempo(); const auto position = m_controller->positionSeconds();
        const auto id = m_controller->addMidiClip(m_channelId.toStdString(), position, phrase.lengthBeats * 60 / tempo);
        if (!id.empty()) m_controller->replaceMidiClipFromFile(m_channelId.toStdString(), id, phrase);
        m_controller->collapseUndo(group, "Create Slicer MIDI clip"); emit projectEdited(); status(tr("MIDI clip created at playhead")); return;
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Export Slicer MIDI"), {}, "MIDI (*.mid)"); if (path.isEmpty()) return;
    if (!path.endsWith(".mid", Qt::CaseInsensitive)) path += ".mid";
    startWork(tr("Writing MIDI…"), [phrase, path](const auto& keepGoing) {
        WorkResult result; std::vector<std::uint8_t> bytes;
        if (!daw::midifile::encode(phrase, bytes, result.error) || !keepGoing()) return result;
        QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) { result.error = file.errorString().toStdString(); return result; }
        result.ok = file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) == qint64(bytes.size()) && keepGoing() && file.commit(); return result;
    }, [this](WorkResult) { status(tr("MIDI saved; project tempo unchanged")); });
}
void SlicerPanel::exportWav(bool all, bool processed, bool prepareDrag) {
    commitField(); const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table || !snapshot->state.audio) return; const auto state = snapshot->state;
    auto ids = selectedIds(); if (all) { ids.clear(); for (quint32 i = 0; i < state.table->count; ++i) ids.push_back(state.table->slices[i].id); }
    if (ids.empty()) return;
    QString destination;
    if (!prepareDrag) {
        destination = ids.size() == 1 ? QFileDialog::getSaveFileName(this, tr("Export slice WAV"), {}, "WAV (*.wav)") : QFileDialog::getExistingDirectory(this, tr("Export slices to folder"));
        if (destination.isEmpty()) return;
        if (QFileInfo(destination).exists() && QFileInfo(destination).canonicalFilePath() == QFileInfo(QString::fromStdString(state.path)).canonicalFilePath()) {
            status(tr("Choose another filename; this is the source sample")); return;
        }
        if (ids.size() == 1 && !destination.endsWith(".wav", Qt::CaseInsensitive)) destination += ".wav";
    }
    const auto cache = mediaCache(), runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    startWork(tr("Rendering WAV · float32 · source sample rate…"), [=](const auto& keepGoing) {
        WorkResult result;
        for (auto id : ids) {
            const int index = state.table->indexForId(id); const QString name = QString("slice-%1-%2.wav").arg(index + 1, 3, 10, QChar('0')).arg(noteName(state.table->slices[index].key).replace('#', 's'));
            const auto cached = cache + "/" + runId + "-" + name;
            if (!slicing::renderWav(cached.toStdString(), state, id, processed, result.error, keepGoing)) return result;
            result.files << cached;
        }
        if (!keepGoing()) return result;
        if (!prepareDrag) for (int i = 0; i < result.files.size(); ++i) {
            const auto target = ids.size() == 1 ? destination : QDir(destination).filePath(QFileInfo(result.files[i]).fileName().mid(runId.size() + 1));
            if (QFileInfo(target).exists() && QFileInfo(target).canonicalFilePath() == QFileInfo(QString::fromStdString(state.path)).canonicalFilePath()) { result.error = "The export would replace the source sample."; return result; }
            if (!keepGoing() || !copyAtomically(result.files[i], target)) { result.error = "Could not save completed WAV."; return result; }
        }
        result.ok = true; return result;
    }, [this, prepareDrag](WorkResult result) {
        if (prepareDrag) m_dragFiles = result.files;
        status(prepareDrag ? tr("WAV ready · drag the ready button into the arrangement") : tr("WAV export complete"));
    });
}
void SlicerPanel::applyTheme() {
    const auto& t = th();
    auto accentTheme = t; accentTheme.accent = slicerAccent();
    setStyleSheet(QString(R"(
#SlicerPanel { background: %BG%; border: 1px solid %EDGE%; border-radius: 16px; }
#SlicerFile { color: %DIM%; font-size: 11px; }
#SlicerHelp { color: %DIM%; }
#SlicerGroupTitle { color: %TEXT%; font-weight: 600; }
QPushButton { color: %TEXT%; padding: 4px 8px; border-radius: 8px; border: 1px solid %EDGE%; background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 %FACE%,stop:1 %BG%); }
QPushButton:hover { border-color: %DIM%; }
QPushButton:pressed { background: %WELL%; border-color: %BORDER%; }
QPushButton:disabled { color: %DIM%; }
#SlicerApply, #SlicerQuickApply { background: %ACCENT%; color: %INK%; font-weight: 600; }
QScrollArea, QScrollArea > QWidget > QWidget { background: %BG%; }
QTabWidget::pane { border: 1px solid %BORDER%; border-radius: 7px; }
QTabBar::tab { padding: 8px 6px; }
QDoubleSpinBox, QSpinBox, QComboBox { color: %TEXT%; background: %WELL%; border: 1px solid %BORDER%; border-radius: 7px; min-height: 25px; padding: 2px 6px; }
QLabel { background: transparent; }
)").replace("%BG%", shellColor().name()).replace("%TEXT%", t.textPrimary.name()).replace("%DIM%", t.textSecondary.name())
        .replace("%FACE%",t.edgeLight(shellColor()).name()).replace("%EDGE%",t.edgeLight(shellColor()).name()).replace("%WELL%",t.well().name())
        .replace("%ACCENT%", slicerAccent().name()).replace("%INK%", accentTheme.accentText().name())
        .replace("%BORDER%", t.separator().name()).replace("%FOCUS%", t.accentHighlight.name()));
    for (auto* pad : m_pads) pad->update();
}
bool SlicerPanel::hasHeightForWidth() const {
    return false;
}
void SlicerPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e); if (!m_body) return;
    m_waveform->setMinimumHeight(130);
    m_soundPanel->setMaximumWidth(width() < 900 ? 414 : 452);
    refreshSelection();
}
void SlicerPanel::showEvent(QShowEvent* e) { QWidget::showEvent(e); refresh(); if (m_poll) m_poll->start(); }
void SlicerPanel::hideEvent(QHideEvent* e) { if (m_poll) m_poll->stop(); m_settingsWindow->hide(); cancelWork(); commitField(); m_waveform->cancelDrag(); releaseAudition(true); QWidget::hideEvent(e); }
void SlicerPanel::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls() && !e->mimeData()->urls().empty() && e->mimeData()->urls().first().isLocalFile() && ui::isAudioFile(e->mimeData()->urls().first().toLocalFile())) e->acceptProposedAction(); }
void SlicerPanel::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasUrls() || e->mimeData()->urls().empty()) return;
    const auto path = e->mimeData()->urls().first().toLocalFile(); if (path.isEmpty()) return; loadSample(path); e->acceptProposedAction();
}
void SlicerPanel::handleKey(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        if (m_waveform->cancelDrag()) { e->accept(); return; }
        if (m_fieldEditor) { m_fieldEditor = nullptr; m_effectPad->cancelDrag(); m_controller->cancelSlicerEdit(); for(auto* k:m_sliceKnobs)k->finishEditing(); setFocus(); refresh(); }
        else if(m_settingsWindow->isVisible()) m_settingsWindow->hide();
        releaseAudition(true); e->accept(); return;
    }
    if (e->matches(QKeySequence::Undo)) { commitField(); cancelWork(); m_controller->undo(); refresh(); e->accept(); return; }
    if (e->matches(QKeySequence::Redo)) { commitField(); cancelWork(); m_controller->redo(); refresh(); e->accept(); return; }
    const auto snapshot = slicer();
    if (!snapshot || !snapshot->state.table) return;
    const auto table = snapshot->state.table; const int i = table->indexForId(m_primary);
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { audition(i); e->accept(); return; }
    int next = i;
    if (e->key() == Qt::Key_Left) --next; else if (e->key() == Qt::Key_Right) ++next;
    else if (e->key() == Qt::Key_Up) next -= kPadColumns;
    else if (e->key() == Qt::Key_Down) next += kPadColumns;
    else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) { mergeAt(i > 0 ? i : 1); e->accept(); return; }
    else return;
    selectSlice(std::clamp(next, 0, int(table->count) - 1), e->modifiers(), false); e->accept();
}
bool SlicerPanel::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool navigation = object == this || object == m_waveform || qobject_cast<SlicerPad*>(object);
        const bool sliceKey = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
            key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up ||
            key->key() == Qt::Key_Down || key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace;
        if (key->key() == Qt::Key_Escape || key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo) || (navigation && sliceKey)) {
            // Claim editor keys before the application's transport/arrangement shortcuts.
            key->accept(); return true;
        }
    }
    if (object == m_dragButton && !m_dragFiles.empty()) {
        if (event->type() == QEvent::MouseButtonPress) { auto* e = static_cast<QMouseEvent*>(event); m_dragOrigin = e->pos(); m_dragArmed = e->button() == Qt::LeftButton; }
        if (event->type() == QEvent::MouseMove && m_dragArmed) {
            auto* e = static_cast<QMouseEvent*>(event);
            if ((e->pos() - m_dragOrigin).manhattanLength() >= QApplication::startDragDistance()) {
                m_dragArmed = false; m_dragButton->setDown(false);
                auto* data = new QMimeData; QList<QUrl> urls; for (const auto& path : m_dragFiles) urls << QUrl::fromLocalFile(path);
                data->setUrls(urls); auto* drag = new QDrag(m_dragButton); drag->setMimeData(data); drag->exec(Qt::CopyAction); return true;
            }
        }
        if (event->type() == QEvent::MouseButtonRelease) m_dragArmed = false;
    }
    if (event->type() == QEvent::Wheel && (qobject_cast<QAbstractSpinBox*>(object) || qobject_cast<QComboBox*>(object) || qobject_cast<ui::Knob*>(object))) { event->ignore(); return true; }
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool numeric = qobject_cast<QDoubleSpinBox*>(object) || (object->parent() && qobject_cast<QDoubleSpinBox*>(object->parent()));
        const bool choice = qobject_cast<QComboBox*>(object);
        if (key->key() == Qt::Key_Escape || key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo) || (!numeric && !choice && (object == this || object == m_waveform || qobject_cast<SlicerPad*>(object)))) {
            key->setAccepted(false); handleKey(key); if (key->isAccepted()) return true;
        }
    }
    if (event->type() == QEvent::KeyRelease && (object == this || object == m_waveform || qobject_cast<SlicerPad*>(object))) {
        auto* key = static_cast<QKeyEvent*>(event); if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { releaseAudition(); return true; }
    }
    return QWidget::eventFilter(object, event);
}

bool SlicerPanel::checkLayoutForTest() {
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto descriptor = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.slicer");
    if (!descriptor) return false;
    const auto track = controller.addTrack(daw::TrackKind::Instrument, "Slicer UI check");
    if (!controller.setTrackInstrumentPlugin(track, *descriptor)) return false;
    const auto slot = controller.project().findTrack(track)->instrument.id;
    auto* instance = dynamic_cast<p::SlicerInstance*>(controller.insertInstance(track, slot)); if (!instance) return false;
    auto source = std::make_shared<daw::engine::SampleBuffer>(2, 96000, 48000);
    for (int ch = 0; ch < 2; ++ch) for (quint32 i = 0; i < source->frames(); ++i) {
        const double pulse = std::exp(-double(i % 6000) / 1700.0);
        source->writableChannel(ch)[i] = float(.6 * pulse * std::sin(2 * std::numbers::pi * (ch ? 220 : 110) * i / 48000.0));
    }
    auto state = instance->captureState(); state.audio = source; state.path = "Slicer UI fixture.wav";
    state.analysis.mode = p::SliceMode::Grid; state.analysis.targetCount = 128;
    state.table = std::make_shared<Table>(slicing::cut(*source, state.analysis));
    controller.applySlicerState(track, slot, state, "Slicer fixture");
    SlicerPanel panel(&controller, QString::fromStdString(track), QString::fromStdString(slot));
    // Test the full logical viewport even when a high-DPI desktop cannot show
    // it. Keep ordinary native windows for desktop sizes that can contain it.
    const auto* screen = panel.screen();
    if (QApplication::platformName() == "windows" && screen &&
        (screen->availableGeometry().width() < 1440 || screen->availableGeometry().height() < 940))
        panel.setAttribute(Qt::WA_DontShowOnScreen);
    panel.show(); QApplication::processEvents();
    const auto require = [](bool ok, const char* label) { std::fprintf(stderr, "%s slicer UI: %s\n", ok ? "PASS" : "FAIL", label); return ok; };
    bool ok = true;
    panel.selectSlice(0, Qt::NoModifier, false); panel.selectSlice(3, Qt::ShiftModifier, false);
    ok &= require(panel.m_selected.size() == 4 && panel.m_auditionKey < 0, "multiple selection does not audition");
    ok &= require(panel.m_pads[0]->isChecked() && panel.m_pads[0]->text().contains(QString::fromStdString(p::noteName(instance->sliceTable()->slices[0].key))), "pads expose selected state and the project MIDI octave convention");
    const auto depth = controller.undoDepth(); const auto before = instance->sliceTable();
    panel.editSelected("Batch gain", [](Slice& s) { s.gain = .5f; });
    ok &= require(controller.undoDepth() == depth + 1 && instance->sliceTable()->slices[3].gain == .5f && instance->sliceTable()->slices[4].gain == 1, "one undo for a selected batch");
    controller.undo(); panel.refresh(); ok &= require(instance->sliceTable()->slices[3].gain == before->slices[3].gain, "undo restores slice sound");
    controller.redo(); panel.refresh();
    const auto dragDepth = controller.undoDepth(); const auto originalBoundary = instance->sliceTable()->slices[1].start;
    emit panel.m_waveform->boundaryBegin();
    emit panel.m_waveform->boundaryMoved(1, originalBoundary + 10); emit panel.m_waveform->boundaryMoved(1, originalBoundary + 20); emit panel.m_waveform->boundaryCommit();
    ok &= require(controller.undoDepth() == dragDepth + 1 && instance->sliceTable()->slices[1].start == originalBoundary + 20, "one undo for an entire boundary drag");
    emit panel.m_waveform->boundaryBegin(); emit panel.m_waveform->boundaryMoved(1, originalBoundary + 30); emit panel.m_waveform->boundaryCancel();
    ok &= require(instance->sliceTable()->slices[1].start == originalBoundary + 20 && controller.undoDepth() == dragDepth + 1, "cancel restores the drag without history");
    panel.selectSlice(0, Qt::NoModifier, false); QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(panel.m_waveform, &right);
    ok &= require(instance->sliceTable()->indexForId(panel.m_primary) == 1, "arrow selects next slice");
    QKeyEvent enterOverride(QEvent::ShortcutOverride, Qt::Key_Return, Qt::NoModifier); enterOverride.ignore(); QApplication::sendEvent(panel.m_waveform, &enterOverride);
    ok &= require(enterOverride.isAccepted(), "Enter takes priority over the application transport shortcut");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(panel.m_waveform, &enter);
    ok &= require(panel.m_auditionKey == instance->sliceTable()->slices[1].key, "Enter auditions");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(panel.m_waveform, &escape);
    ok &= require(panel.m_auditionKey < 0, "Escape stops audition");
    const auto wheelBefore = panel.m_numbers["slicePan"]->value();
    QWheelEvent wheel(QPointF(10, 10), panel.m_numbers["slicePan"]->mapToGlobal(QPoint(10, 10)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(panel.m_numbers["slicePan"], &wheel);
    ok &= require(panel.m_numbers["slicePan"]->value() == wheelBefore, "scrolling controls never changes sound");
    panel.m_effectChoice->setCurrentIndex(4);
    ok &= require(instance->sliceTable()->slices[1].effect==1 && instance->sliceTable()->slices[0].effect==0,"effect selector edits only the selected slice");
    auto* xy=panel.m_effectPad; const QPointF xyStart(xy->width()/2.0,xy->height()/2.0);
    const auto sendPointer=[xy](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons){
        QMouseEvent event(type,point,xy->mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier); QApplication::sendEvent(xy,&event);
    };
    const auto fxDepth=controller.undoDepth(); const auto fxBefore=instance->sliceTable()->slices[1];
    sendPointer(QEvent::MouseButtonPress,xyStart,Qt::LeftButton,Qt::LeftButton);
    sendPointer(QEvent::MouseMove,xyStart+QPointF(22,-14),Qt::NoButton,Qt::LeftButton);
    sendPointer(QEvent::MouseMove,xyStart+QPointF(32,-22),Qt::NoButton,Qt::LeftButton);
    sendPointer(QEvent::MouseButtonRelease,xyStart+QPointF(32,-22),Qt::LeftButton,Qt::NoButton);
    ok &= require(controller.undoDepth()==fxDepth+1 && instance->sliceTable()->slices[1].effectX!=fxBefore.effectX,"XY drag edits both axes as one undo gesture");
    controller.undo(); panel.refresh();
    ok &= require(instance->sliceTable()->slices[1]==fxBefore,"undo restores both XY axes together");
    sendPointer(QEvent::MouseButtonPress,xyStart,Qt::LeftButton,Qt::LeftButton);
    sendPointer(QEvent::MouseMove,xyStart+QPointF(-20,10),Qt::NoButton,Qt::LeftButton);
    QKeyEvent cancelFx(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier); QApplication::sendEvent(xy,&cancelFx);
    sendPointer(QEvent::MouseButtonRelease,xyStart,Qt::LeftButton,Qt::NoButton);
    ok &= require(instance->sliceTable()->slices[1]==fxBefore && !panel.m_fieldEditor,"Escape restores an unfinished XY gesture");
    QKeyEvent fxRight(QEvent::KeyPress,Qt::Key_Right,Qt::ShiftModifier); QApplication::sendEvent(xy,&fxRight);
    ok &= require(instance->sliceTable()->slices[1].effectX>fxBefore.effectX && instance->sliceTable()->slices[1].effectX<fxBefore.effectX+.01f,"XY keyboard precision adjusts the effect without changing selection");
    panel.m_sliceKnobs["attack"]->editValue(.08);
    ok &= require(!instance->sliceTable()->slices[1].useGlobalEnvelope && std::abs(instance->sliceTable()->slices[1].attack-.08f)<1e-6 && instance->sliceTable()->slices[1].sustain==float(panel.readParameter("sus")),"moving a local envelope knob preserves the other inherited stages");
    panel.selectSlice(2,Qt::NoModifier,false);
    ok &= require(panel.m_effectChoice->currentIndex()==0 && !xy->isEnabled(),"switching slices recalls their own effect and disabled state");
    panel.selectSlice(1,Qt::NoModifier,false);
    ok &= require(panel.m_effectChoice->currentIndex()==4 && xy->isEnabled(),"returning to a slice restores its effect controls");
    const auto originalTheme = ThemeManager::instance().theme();
    const QString screenshotDir = qEnvironmentVariable("DAW_SLICER_CHECK_DIR"); if (!screenshotDir.isEmpty()) QDir().mkpath(screenshotDir);
    bool visitedLight = false, visitedDark = false;
    for (const auto& theme : ThemeManager::instance().presets()) {
        if (theme.dark ? visitedDark : visitedLight) continue; if (theme.dark) visitedDark = true; else visitedLight = true;
        ThemeManager::instance().setThemeId(theme.id, false);
        for (const QSize size : {QSize(980, 410), QSize(780, 390), QSize(860, 390), QSize(1120, 560), QSize(1440, 900)}) {
            panel.resize(size); QApplication::processEvents();
            bool padsFit = true;
            for (auto* pad : panel.m_pads) padsFit &= panel.m_padArea->rect().contains(pad->geometry());
            for (int i = 0; i < 16; ++i) for (int j = i + 1; j < 16; ++j) padsFit &= !panel.m_pads[i]->geometry().intersects(panel.m_pads[j]->geometry());
            ok &= require(padsFit, "all 16 pads remain visible without overlap");
            const bool geometryFits = panel.size() == size && panel.rect().contains(panel.m_waveform->geometry().translated(panel.m_mainArea->pos())) && panel.m_mainArea->rect().contains(panel.m_padArea->geometry()) && panel.rect().contains(panel.m_soundPanel->geometry());
            ok &= require(geometryFits, "waveform and inspector fit requested size");
            ok &= require(panel.m_mainArea->y() == panel.m_soundPanel->y() && panel.m_mainArea->height() == panel.m_soundPanel->height(), "sample and sound controls share the same top and bottom edges");
            bool controlsFit=true;
            for(auto* k:panel.m_sliceKnobs) controlsFit &= panel.rect().contains(QRect(k->mapTo(&panel,QPoint()),k->size())) && k->parentWidget()->rect().contains(k->geometry());
            controlsFit &= panel.rect().contains(QRect(panel.m_effectPad->mapTo(&panel,QPoint()),panel.m_effectPad->size()));
            ok &= require(controlsFit,"soft knobs and XY field fit the requested size");
            bool reliefFits=true; QVector<QRectF> reliefs;
            for(auto* k:panel.m_sliceKnobs) {
                const auto relief=static_cast<SlicerKnob*>(k)->reliefBounds().adjusted(-1,-1,1,1);
                reliefFits &= QRectF(k->rect()).contains(relief);
                reliefs.push_back(relief.translated(k->mapTo(&panel,QPoint())));
            }
            for(int i=0;i<reliefs.size();++i) for(int j=i+1;j<reliefs.size();++j) reliefFits &= !reliefs[i].intersects(reliefs[j]);
            for(auto* k:panel.m_knobs) reliefFits &= QRectF(k->rect()).contains(static_cast<SlicerKnob*>(k)->reliefBounds().adjusted(-1,-1,1,1));
            ok &= require(reliefFits,"complete knob shadows and highlights fit without clipping or overlap");
            if (!geometryFits) {
                const auto minimum = panel.minimumSize(); const auto hint = panel.minimumSizeHint();
                std::fprintf(stderr, "FAIL slicer size: requested %dx%d, actual %dx%d, minimum %dx%d, hint %dx%d\n", size.width(), size.height(), panel.width(), panel.height(), minimum.width(), minimum.height(), hint.width(), hint.height());
            }
            for (auto* widget : panel.findChildren<QWidget*>()) {
                if (!widget->isVisible() || widget->isWindow() || widget->parentWidget() != &panel) continue;
                if (!panel.rect().contains(widget->geometry())) { std::fprintf(stderr, "FAIL slicer geometry: %s %d,%d %dx%d\n", widget->objectName().toUtf8().constData(), widget->x(), widget->y(), widget->width(), widget->height()); ok = false; }
            }
            if (!screenshotDir.isEmpty()) panel.grab().save(QDir(screenshotDir).filePath(QString("slicer-%1-%2x%3-%4.png").arg(theme.dark ? "dark" : "light").arg(size.width()).arg(size.height()).arg(qEnvironmentVariable("QT_SCALE_FACTOR", "1"))));
        }
    }
    if (originalTheme.id == "custom") ThemeManager::instance().applyCustomTheme(originalTheme, false); else ThemeManager::instance().setThemeId(originalTheme.id, false);
    QElapsedTimer clock; clock.start();
    for (int i = 0; i < 128; ++i) { panel.selectSlice(i, Qt::NoModifier, false); panel.m_waveform->zoom(i % 2 ? 1 / 1.01 : 1.01); }
    ok &= require(clock.elapsed() < 2000, "128-slice selection and zoom remain responsive");
    if (!screenshotDir.isEmpty()) {
        auto display = instance->captureState(); display.analysis.targetCount = 16;
        auto table = std::make_shared<Table>(slicing::cut(*source, display.analysis));
        table->slices[2].flags = p::kSliceReverse; table->slices[2].effect=1; table->slices[2].effectX=.42f; table->slices[2].effectY=.68f;
        table->slices[3].loopMode = 1; table->slices[3].crossfadeMs = 3;
        table->slices[5].flags = p::kSliceMuted; table->slices[7].locked = true; display.table = table;
        controller.applySlicerState(track, slot, display, "Display fixture"); panel.refresh(); panel.selectSlice(2, Qt::NoModifier, false); panel.resize(980,410);
        QApplication::processEvents(); panel.grab().save(QDir(screenshotDir).filePath("slicer-soft-performance.png"));
        panel.resize(780,390);
        for (int effect=1;effect<=6;++effect) {
            panel.m_effectChoice->setCurrentIndex(effect); QApplication::processEvents();
            panel.grab().save(QDir(screenshotDir).filePath(QString("slicer-effect-%1.png").arg(effect)));
        }
        panel.m_effectChoice->setCurrentIndex(4); panel.resize(980,410);
        panel.m_settingsWindow->show();
        for (int tab=0;tab<panel.m_tabs->count();++tab) { panel.m_tabs->setCurrentIndex(tab); QApplication::processEvents(); panel.m_settingsWindow->grab().save(QDir(screenshotDir).filePath(QString("slicer-inspector-%1.png").arg(tab))); }
        panel.m_settingsWindow->hide();
        panel.m_tabs->setCurrentIndex(0); panel.m_pads[2]->setDown(true);
        panel.grab().save(QDir(screenshotDir).filePath("slicer-pad-pressed.png")); panel.m_pads[2]->setDown(false);
    }
    // Worker completion is guarded against source replacement, slot removal and cancellation.
    const auto stale = instance->sliceTable();
    panel.startWork("Fixture", [state](const auto&) { WorkResult result; result.ok = true; result.state = state; return result; }, [&ok](WorkResult) { ok = false; });
    auto replacement = instance->captureState(); replacement.path = "new-fixture.wav";
    controller.applySlicerState(track, slot, replacement, "Replace fixture"); panel.refresh();
    QThreadPool::globalInstance()->waitForDone(10000); QApplication::processEvents();
    ok &= require(instance->samplePath() == "new-fixture.wav", "stale analysis does not replace a new source");
    panel.startWork("Fixture", [state](const auto&) { WorkResult result; result.ok = true; result.state = state; return result; }, [&ok](WorkResult) { ok = false; });
    panel.cancelWork(); QThreadPool::globalInstance()->waitForDone(10000); QApplication::processEvents();
    ok &= require(!panel.m_busy, "cancelled work keeps previous state");
    // No refresh/cancel before completion: the result must check identity itself.
    panel.m_poll->stop();
    const auto oldIdentity = panel.slicer()->identity;
    bool staleFinished = false;
    panel.startWork("Fixture", [state](const auto&) { WorkResult result; result.ok=true; result.state=state; return result; }, [&staleFinished](WorkResult) { staleFinished=true; });
    controller.setTrackInstrumentPlugin(track, {});
    controller.undo();
    const auto restored = panel.slicer();
    ok &= require(restored && restored->identity != oldIdentity, "Undo assigns a fresh Slicer identity");
    QThreadPool::globalInstance()->waitForDone(10000); QApplication::processEvents();
    ok &= require(!staleFinished && !panel.m_busy, "completion rejects an instrument recreated in the same slot");
    panel.startWork("Fixture", [state](const auto&) { WorkResult result; result.ok=true; result.state=state; return result; }, [&ok](WorkResult) { ok=false; });
    controller.setTrackInstrumentPlugin(track, {}); panel.refresh();
    QThreadPool::globalInstance()->waitForDone(10000); QApplication::processEvents();
    ok &= require(!panel.slicer(), "slot removal discards pending results");
    panel.hide(); controller.shutdown(); return ok;
}
