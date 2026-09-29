#include "SlideCurveEditor.hpp"
#include "SlideNotes.hpp"
#include "Theme.hpp"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
std::vector<daw::AutomationPoint> copiedShape;
}
class SlideCurveCanvas final : public QWidget {
  public:
    daw::SlideNoteModel slide, before;
    double initial = 60, minimum = 48, maximum = 72, bendRange = 0, bendCenter = 60;
    double timeGrid = 1.0 / 16, pitchGrid = 1;
    bool pencil = false, gesture = false, bending = false;
    int selected = 1;
    QPointF press;
    std::function<void(bool)> changed, capacityReached;
    explicit SlideCurveCanvas(QWidget *parent) : QWidget(parent) {
        setObjectName("slideCurveCanvas");
        setMinimumSize(440, 210);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(SlideCurveEditor::tr("Slide pitch curve"));
        setToolTip(SlideCurveEditor::tr(
            "Drag points; click to add. Alt-drag a segment to bend it. Right-click "
            "deletes a point. Escape cancels the gesture."));
    }
    void fit() {
        minimum = maximum = initial;
        for (auto &p : slide.points) {
            minimum = std::min(minimum, p.value);
            maximum = std::max(maximum, p.value);
        }
        minimum = std::max(0.0, std::floor(minimum) - 2);
        maximum = std::min(127.0, std::ceil(maximum) + 2);
    }
    QRectF plot() const { return QRectF(48, 12, width() - 64, height() - 38); }
    QPointF screen(double time, double pitch) const {
        auto r = plot();
        return {r.left() + time * r.width(),
                r.bottom() - (pitch - minimum) / (maximum - minimum) * r.height()};
    }
    QPointF value(QPointF pos, bool snap = true) const {
        auto r = plot();
        double t = std::clamp((pos.x() - r.left()) / r.width(), 0.0, 1.0);
        double p = std::clamp(minimum + (r.bottom() - pos.y()) / r.height() * (maximum - minimum),
                              0.0, 127.0);
        if (snap && timeGrid > 0)
            t = std::clamp(std::round(t / timeGrid) * timeGrid, 0.0, 1.0);
        if (snap && pitchGrid > 0)
            p = std::round(p / pitchGrid) * pitchGrid;
        return {t, std::clamp(p, 0.0, 127.0)};
    }
    void publish(bool commit) {
        if (changed)
            changed(commit);
        update();
    }
    void cancel() {
        if (!gesture)
            return;
        slide = before;
        gesture = false;
        publish(false);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), th().well());
        auto r = plot();
        p.setPen(th().textSecondary);
        const int stride = std::max(1, int(std::ceil((maximum - minimum) / 12)));
        for (int k = int(minimum); k <= maximum; k += stride) {
            double y = screen(0, k).y();
            p.setPen(th().gridLine);
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(th().textSecondary);
            p.drawText(QRectF(0, y - 8, 42, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(k));
        }
        for (int i = 0; i <= 8; ++i) {
            double x = screen(i / 8.0, 0).x();
            p.setPen(th().gridLine);
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(th().textSecondary);
        p.drawText(
            QRectF(r.left(), r.bottom() + 4, r.width(), 20), Qt::AlignLeft,
            SlideCurveEditor::tr("Time in slide · %1 beats").arg(slide.lengthBeats, 0, 'f', 3));
        if (bendRange > 0) {
            p.save();
            p.setClipRect(r);
            p.setPen(QPen(Theme::mute(), 1, Qt::DashLine));
            QColor outside = Theme::mute();
            outside.setAlpha(40);
            const double top = screen(0, bendCenter + bendRange).y();
            const double bottom = screen(0, bendCenter - bendRange).y();
            if (top > r.top())
                p.fillRect(QRectF(r.left(), r.top(), r.width(), top - r.top()), outside);
            if (bottom < r.bottom())
                p.fillRect(QRectF(r.left(), bottom, r.width(), r.bottom() - bottom), outside);
            for (double k : {bendCenter - bendRange, bendCenter + bendRange})
                p.drawLine(screen(0, k), screen(1, k));
            p.restore();
        }
        QPainterPath path;
        for (std::size_t i = 0; i + 1 < slide.points.size(); ++i) {
            auto a = slide.points[i], b = slide.points[i + 1];
            if (i == 0)
                a.value = initial;
            for (int j = 0; j <= 32; ++j) {
                double t = j / 32.;
                double v = a.value +
                           (b.value - a.value) *
                               daw::engine::curve::shapeT(t, daw::toCurveShape(a.shape), a.curve);
                auto q = screen(a.beats + (b.beats - a.beats) * t, v);
                if (i == 0 && j == 0)
                    path.moveTo(q);
                else
                    path.lineTo(q);
            }
        }
        p.setPen(QPen(Theme::automationAccent(), 2));
        p.drawPath(path);
        for (std::size_t i = 0; i < slide.points.size(); ++i) {
            auto q = screen(slide.points[i].beats, i ? slide.points[i].value : initial);
            p.setBrush(int(i) == selected ? th().textPrimary : th().surface);
            p.drawEllipse(q, 4, 4);
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        setFocus();
        before = slide;
        press = e->position();
        auto v = value(press, !(e->modifiers() & Qt::ShiftModifier));
        int hit = -1;
        double distance = 100;
        for (int i = 0; i < int(slide.points.size()); ++i) {
            auto q = screen(slide.points[i].beats, i ? slide.points[i].value : initial);
            double d = std::hypot(q.x() - press.x(), q.y() - press.y());
            if (d < distance) {
                hit = i;
                distance = d;
            }
        }
        if (e->button() == Qt::RightButton) {
            if (distance < 12 && hit > 0 && hit + 1 < int(slide.points.size())) {
                slide.points.erase(slide.points.begin() + hit);
                selected = std::min(hit, int(slide.points.size()) - 1);
                publish(true);
            }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        gesture = true;
        bending = bool(e->modifiers() & Qt::AltModifier);
        if (bending) {
            selected = std::max(
                0, int(std::upper_bound(slide.points.begin(), slide.points.end(), v.x(),
                                        [](double t, const auto &p) { return t < p.beats; }) -
                       slide.points.begin()) -
                       1);
            selected = std::min(selected, int(slide.points.size()) - 2);
            return;
        }
        if (distance < 12 && !pencil)
            selected = hit;
        else {
            if (v.x() <= 0)
                selected = 0;
            else if (v.x() >= 1)
                selected = int(slide.points.size()) - 1;
            else {
                auto it = std::lower_bound(slide.points.begin(), slide.points.end(), v.x(),
                                           [](const auto &p, double t) { return p.beats < t; });
                selected = int(it - slide.points.begin());
                if (it == slide.points.end() || std::abs(it->beats - v.x()) > 1e-8)
                    slide.points.insert(it, {v.x(), v.y()});
            }
        }
        if (!acceptDrawing(before)) {
            gesture = false;
            return;
        }
        selected = std::clamp(selected, 0, int(slide.points.size()) - 1);
        move(e);
        publish(false);
    }
    bool acceptDrawing(const daw::SlideNoteModel &previous) {
        auto compact = slide;
        daw::slides::normalize(compact);
        const bool accepted = daw::slides::valid(compact) && slide.points.size() <= 4096;
        if (!accepted)
            slide = previous;
        if (capacityReached)
            capacityReached(!accepted);
        selected = std::clamp(selected, 0, int(slide.points.size()) - 1);
        return accepted;
    }
    void move(QMouseEvent *e) {
        if (!gesture)
            return;
        const auto previous = slide;
        auto v = value(e->position(), !(e->modifiers() & Qt::ShiftModifier));
        if (bending) {
            slide.points[selected].curve = std::clamp(
                before.points[selected].curve + (press.y() - e->position().y()) / 80., -1., 1.);
            publish(false);
            return;
        }
        if (pencil && v.x() > 0 && v.x() < 1) {
            // Keep the previous stroke vertex. Removing it on every move would
            // flatten the whole drawing to a single moving point.
            const double old = slide.points[selected].beats;
            const double lo = std::min(old, v.x()), hi = std::max(old, v.x());
            slide.points[selected].shape = daw::AutomationSegment::Linear;
            slide.points[selected].curve = 0;
            std::erase_if(slide.points, [&](const auto &p) {
                return p.beats > 0 && p.beats < 1 &&
                       ((p.beats > lo && p.beats < hi) || std::abs(p.beats - v.x()) < 1e-8);
            });
            auto it = std::lower_bound(slide.points.begin(), slide.points.end(), v.x(),
                                       [](const auto &p, double t) { return p.beats < t; });
            selected = int(it - slide.points.begin());
            slide.points.insert(it, {v.x(), v.y()});
        } else if (selected > 0) {
            auto &p = slide.points[selected];
            p.value = v.y();
            if (selected + 1 < int(slide.points.size()))
                p.beats = std::clamp(v.x(), slide.points[selected - 1].beats + 1e-7,
                                     slide.points[selected + 1].beats - 1e-7);
        }
        slide.points.front().value = initial;
        if (acceptDrawing(previous))
            publish(false);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (gesture && !(e->buttons() & Qt::LeftButton)) {
            gesture = false;
            publish(true);
            return;
        }
        move(e);
    }
    bool event(QEvent *e) override {
        if (e->type() == QEvent::WindowDeactivate || e->type() == QEvent::UngrabMouse)
            cancel();
        return QWidget::event(e);
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (gesture) {
            gesture = false;
            daw::slides::normalize(slide);
            selected = std::min(selected, int(slide.points.size()) - 1);
            publish(true);
        }
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape && gesture) {
            cancel();
            e->accept();
            return;
        }
        QWidget::keyPressEvent(e);
    }
};
SlideCurveEditor::SlideCurveEditor(daw::SlideNoteModel slide, double initialPitch, QWidget *parent)
    : QDialog(parent, Qt::Tool) {
    setWindowTitle(tr("Slide curve"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(640, 360);
    auto *root = new QVBoxLayout(this);
    auto *tools = new QHBoxLayout;
    root->addLayout(tools);
    m_canvas = new SlideCurveCanvas(this);
    m_canvas->slide = std::move(slide);
    m_canvas->initial = initialPitch;
    m_canvas->fit();
    auto *mode = new QComboBox(this);
    mode->setObjectName("slideDrawingTool");
    mode->addItems({tr("Points"), tr("Pencil")});
    mode->setAccessibleName(tr("Drawing tool"));
    tools->addWidget(mode);
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this](int i) { m_canvas->pencil = i == 1; });
    auto *preset = new QComboBox(this);
    preset->addItems({tr("Shape…"), tr("Linear"), tr("Smooth"), tr("Wave"), tr("Return")});
    preset->setAccessibleName(tr("Curve preset"));
    tools->addWidget(preset);
    connect(preset, &QComboBox::activated, this, [this, preset](int i) {
        if (!i)
            return;
        daw::slides::preset(m_canvas->slide, m_canvas->initial, m_canvas->slide.points.back().value,
                            i - 1);
        m_canvas->fit();
        m_canvas->publish(true);
        preset->setCurrentIndex(0);
    });
    auto *time = new QComboBox(this);
    time->addItems({tr("Time: free"), tr("Time: 1/16"), tr("Time: 1/32")});
    time->setCurrentIndex(1);
    tools->addWidget(time);
    connect(time, &QComboBox::currentIndexChanged, this,
            [this](int i) { m_canvas->timeGrid = i ? 1. / (i == 1 ? 16 : 32) : 0; });
    auto *pitch = new QComboBox(this);
    pitch->addItems({tr("Pitch: semitone"), tr("Pitch: cent"), tr("Pitch: free")});
    tools->addWidget(pitch);
    connect(pitch, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_canvas->pitchGrid = i == 0 ? 1 : i == 1 ? .01 : 0;
    });
    root->addWidget(m_canvas, 1);
    auto *numbers = new QHBoxLayout;
    root->addLayout(numbers);
    auto *pointTime = new QDoubleSpinBox(this);
    pointTime->setRange(0, 100);
    pointTime->setDecimals(3);
    pointTime->setSuffix(tr(" % time"));
    pointTime->setAccessibleName(tr("Selected point time"));
    numbers->addWidget(pointTime);
    auto *pointPitch = new QDoubleSpinBox(this);
    pointPitch->setRange(0, 127);
    pointPitch->setDecimals(2);
    pointPitch->setSingleStep(.01);
    pointPitch->setSuffix(tr(" st"));
    pointPitch->setAccessibleName(tr("Selected point pitch in semitones and cents"));
    numbers->addWidget(pointPitch);
    auto *hint =
        new QLabel(tr("First point follows the voice · Alt-drag bends · Esc cancels"), this);
    hint->setWordWrap(true);
    numbers->addWidget(hint, 1);
    const auto sync = [this, pointTime, pointPitch] {
        QSignalBlocker a(pointTime), b(pointPitch);
        int i = std::clamp(m_canvas->selected, 0, int(m_canvas->slide.points.size()) - 1);
        const auto &p = m_canvas->slide.points[i];
        pointTime->setValue(p.beats * 100);
        pointPitch->setValue(i ? p.value : m_canvas->initial);
        pointPitch->setEnabled(i > 0);
        pointTime->setEnabled(i > 0 && i + 1 < int(m_canvas->slide.points.size()));
    };
    m_sync = sync;
    m_canvas->capacityReached = [hint](bool limit) {
        hint->setText(
            limit ? tr("Point limit reached (256). Finish this stroke or use a second slide.")
                  : tr("First point follows the voice · Alt-drag bends · Esc cancels"));
    };
    m_canvas->changed = [this, sync](bool commit) {
        sync();
        if (changed) {
            auto compact = m_canvas->slide;
            daw::slides::normalize(compact);
            changed(compact, commit);
        }
    };
    sync();
    connect(pointPitch, &QDoubleSpinBox::editingFinished, this, [this, pointPitch] {
        int i = m_canvas->selected;
        if (i > 0 && i < int(m_canvas->slide.points.size())) {
            m_canvas->slide.points[i].value = pointPitch->value();
            m_canvas->publish(true);
        }
    });
    connect(pointTime, &QDoubleSpinBox::editingFinished, this, [this, pointTime] {
        int i = m_canvas->selected;
        if (i > 0 && i + 1 < int(m_canvas->slide.points.size())) {
            m_canvas->slide.points[i].beats =
                std::clamp(pointTime->value() / 100, m_canvas->slide.points[i - 1].beats + 1e-7,
                           m_canvas->slide.points[i + 1].beats - 1e-7);
            m_canvas->publish(true);
        }
    });
    auto *actions = new QHBoxLayout;
    root->addLayout(actions);
    auto button = [&](QString label, auto fn) {
        auto *b = new QPushButton(label, this);
        actions->addWidget(b);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };
    button(tr("Copy shape"), [this] {
        copiedShape = m_canvas->slide.points;
        copiedShape.front().value = m_canvas->initial;
        for (auto &p : copiedShape)
            p.value -= m_canvas->initial;
    });
    button(tr("Paste shape"), [this] {
        if (copiedShape.empty())
            return;
        m_canvas->slide.points = copiedShape;
        for (auto &p : m_canvas->slide.points)
            p.value = std::clamp(p.value + m_canvas->initial, 0., 127.);
        m_canvas->fit();
        m_canvas->publish(true);
    });
    button(tr("Rebind"), [this] {
        if (rebind)
            rebind();
        close();
    });
    button(tr("Extend base note"), [this] {
        if (extendBase)
            extendBase();
    });
    button(tr("Audition phrase"), [this] {
        if (audition)
            audition();
    });
}
void SlideCurveEditor::reject() {
    if (m_canvas->gesture) {
        m_canvas->cancel();
        return;
    }
    QDialog::reject();
}
void SlideCurveEditor::setBendRange(double semitones, double basePitch) {
    m_canvas->bendRange = semitones;
    m_canvas->bendCenter = basePitch;
    m_canvas->update();
}

void SlideCurveEditor::closeEvent(QCloseEvent *event) {
    m_canvas->cancel();
    QDialog::closeEvent(event);
}

bool SlideCurveEditor::editing() const { return m_canvas->gesture; }
void SlideCurveEditor::refresh(const daw::SlideNoteModel &slide, double initial) {
    if (editing() || (m_canvas->slide == slide && m_canvas->initial == initial))
        return;
    m_canvas->slide = slide;
    m_canvas->initial = initial;
    m_canvas->selected = std::clamp(m_canvas->selected, 0, int(slide.points.size()) - 1);
    m_canvas->fit();
    if (m_sync)
        m_sync();
    m_canvas->update();
}
