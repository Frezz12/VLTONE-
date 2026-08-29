#include "EqualizerPanel.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace daw::plugins;
namespace eq = daw::plugins::equalizer;

namespace {

constexpr auto kUserPresetKey = "equalizer/userPresets.v1";

QColor bandColor(int band, bool dark) {
    return QColor::fromHsv((band * 47 + 188) % 360, dark ? 174 : 205,
                           dark ? 242 : 190);
}

bool containsBand(const std::vector<int>& bands, int band) {
    return std::find(bands.begin(), bands.end(), band) != bands.end();
}

const ParameterInfo* parameterInfo(const QString& id) {
    const std::string key = id.toStdString();
    for (const ParameterInfo& info : eq::parameterTable())
        if (info.id == key) return &info;
    return nullptr;
}

double clampedValue(const ParameterInfo& info, double value) {
    if (!std::isfinite(value)) return info.defaultValue;
    value = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(value) : value;
}

bool factoryNameReserved(const QString& name) {
    const QString folded = name.trimmed().toCaseFolded();
    return std::any_of(eq::factoryPresets().begin(), eq::factoryPresets().end(),
        [&](const eq::FactoryPreset& preset) {
            return QString::fromUtf8(preset.name.data(), int(preset.name.size()))
                       .toCaseFolded() == folded;
        });
}

QWidget* controlCell(QWidget* control, const QString& caption, QWidget* parent) {
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(2);
    layout->addWidget(control, 0, Qt::AlignHCenter);
    auto* label = new QLabel(caption.toUpper(), cell);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("font-size: 9px; color: palette(mid);"));
    layout->addWidget(label);
    return cell;
}

} // namespace

EqualizerGraph::EqualizerGraph(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(275);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAccessibleName(tr("Equalizer frequency response graph"));
    setToolTip(tr("Double-click to add a band. Drag bands to change frequency and gain; "
                  "use the mouse wheel for Q and Alt-drag for dynamic range."));
}

void EqualizerGraph::setData(
    const std::array<eq::BandState, eq::kBandCount>& bands,
    const std::array<double, 256>& response, const eq::Telemetry& telemetry,
    bool linearPhase, double displayRange) {
    m_bands = bands;
    m_response = response;
    m_telemetry = telemetry;
    m_linearPhase = linearPhase;
    m_displayRange = std::clamp(displayRange, 3.0, 30.0);
    update();
}

void EqualizerGraph::setSelection(std::vector<int> bands) {
    std::erase_if(bands, [](int band) { return band < 0 || band >= int(eq::kBandCount); });
    std::sort(bands.begin(), bands.end());
    bands.erase(std::unique(bands.begin(), bands.end()), bands.end());
    if (m_selection == bands) return;
    m_selection = std::move(bands);
    update();
}

QRectF EqualizerGraph::plotRect() const {
    return QRectF(rect()).adjusted(42.0, 13.0, -13.0, -27.0);
}

double EqualizerGraph::xForFrequency(double frequency) const {
    const QRectF plot = plotRect();
    const double t = std::log10(std::clamp(frequency, 10.0, 30000.0) / 10.0) /
                     std::log10(3000.0);
    return plot.left() + t * plot.width();
}

double EqualizerGraph::yForGain(double gain) const {
    const QRectF plot = plotRect();
    return plot.center().y() - std::clamp(gain, -m_displayRange, m_displayRange) /
                               m_displayRange * plot.height() * 0.5;
}

double EqualizerGraph::frequencyAt(double x) const {
    const QRectF plot = plotRect();
    const double t = std::clamp((x - plot.left()) / plot.width(), 0.0, 1.0);
    return 10.0 * std::pow(3000.0, t);
}

double EqualizerGraph::gainAt(double y) const {
    const QRectF plot = plotRect();
    return std::clamp((plot.center().y() - y) / (plot.height() * 0.5) *
                      m_displayRange, -30.0, 30.0);
}

QPointF EqualizerGraph::bandPoint(int band) const {
    if (band < 0 || band >= int(eq::kBandCount)) return {};
    return {xForFrequency(m_bands[std::size_t(band)].frequency),
            yForGain(m_bands[std::size_t(band)].gainDb)};
}

int EqualizerGraph::bandAt(const QPointF& point) const {
    int found = -1;
    double distance = 14.0;
    for (int band = 0; band < int(eq::kBandCount); ++band) {
        if (!m_bands[std::size_t(band)].enabled) continue;
        const double candidate = QLineF(point, bandPoint(band)).length();
        if (candidate < distance) { distance = candidate; found = band; }
    }
    return found;
}

void EqualizerGraph::choose(int band, Qt::KeyboardModifiers modifiers) {
    std::vector<int> selected = m_selection;
    if (band < 0) {
        if (!(modifiers & Qt::ControlModifier)) selected.clear();
    } else if (modifiers & Qt::ControlModifier) {
        if (containsBand(selected, band))
            std::erase(selected, band);
        else
            selected.push_back(band);
    } else {
        selected = {band};
    }
    setSelection(selected);
    if (selectionChanged) selectionChanged(m_selection);
}

void EqualizerGraph::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Theme& theme = th();
    painter.fillRect(rect(), theme.well());
    const QRectF plot = plotRect();

    QLinearGradient bed(plot.topLeft(), plot.bottomLeft());
    QColor top = theme.surface;
    QColor bottom = theme.background;
    top.setAlpha(theme.dark ? 135 : 205);
    bottom.setAlpha(theme.dark ? 210 : 235);
    bed.setColorAt(0.0, top);
    bed.setColorAt(1.0, bottom);
    painter.fillRect(plot, bed);

    static constexpr double frequencies[]{20, 50, 100, 200, 500, 1000,
                                           2000, 5000, 10000, 20000};
    painter.setFont(QFont(painter.font().family(), 8));
    for (double frequency : frequencies) {
        const double x = xForFrequency(frequency);
        painter.setPen(QPen(frequency == 1000 ? theme.gridLineStrong
                                              : theme.gridLine, 1.0));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(theme.textSecondary);
        const QString label = frequency >= 1000
            ? QStringLiteral("%1k").arg(frequency / 1000.0, 0, 'g', 2)
            : QString::number(int(frequency));
        painter.drawText(QRectF(x - 18, plot.bottom() + 3, 36, 17),
                         Qt::AlignHCenter | Qt::AlignTop, label);
    }
    for (double gain : {-m_displayRange, -m_displayRange * 0.5, 0.0,
                        m_displayRange * 0.5, m_displayRange}) {
        const double y = yForGain(gain);
        painter.setPen(QPen(std::abs(gain) < 0.01 ? theme.gridLineStrong
                                                  : theme.gridLine, 1.0));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(theme.textSecondary);
        painter.drawText(QRectF(2, y - 8, 35, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1%2").arg(gain > 0 ? "+" : "")
                                               .arg(gain, 0, 'g', 2));
    }

    painter.save();
    painter.setClipRect(plot);
    auto drawSpectrum = [&](const std::array<float, eq::kSpectrumBinCount>& spectrum,
                            const QColor& color, Qt::PenStyle style, double width) {
        QPainterPath path;
        for (std::size_t i = 0; i < spectrum.size(); ++i) {
            const double frequency = 20.0 * std::pow(1000.0,
                double(i) / double(spectrum.size() - 1));
            const double x = xForFrequency(frequency);
            const double level = std::clamp(double(spectrum[i]), -96.0, 6.0);
            const double y = plot.bottom() - (level + 96.0) / 102.0 * plot.height();
            if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        painter.setPen(QPen(color, width, style, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    };
    QColor pre = theme.textSecondary; pre.setAlpha(115);
    QColor post = theme.accentHighlight; post.setAlpha(185);
    QColor side = Theme::automationAccent(); side.setAlpha(155);
    drawSpectrum(m_telemetry.pre, pre, Qt::DashLine, 1.0);
    drawSpectrum(m_telemetry.post, post, Qt::SolidLine, 1.25);
    drawSpectrum(m_telemetry.sidechain, side, Qt::DotLine, 1.0);

    QPainterPath response;
    for (std::size_t i = 0; i < m_response.size(); ++i) {
        const double frequency = 10.0 * std::pow(3000.0,
            double(i) / double(m_response.size() - 1));
        const QPointF point(xForFrequency(frequency), yForGain(m_response[i]));
        if (i == 0) response.moveTo(point); else response.lineTo(point);
    }
    QColor fill = theme.accent;
    fill.setAlpha(theme.dark ? 28 : 20);
    QPainterPath area = response;
    area.lineTo(plot.right(), yForGain(0.0));
    area.lineTo(plot.left(), yForGain(0.0));
    area.closeSubpath();
    painter.fillPath(area, fill);
    painter.setPen(QPen(theme.accentHighlight, 2.4, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(response);

    for (int band = 0; band < int(eq::kBandCount); ++band) {
        const eq::BandState& state = m_bands[std::size_t(band)];
        if (!state.enabled) continue;
        const QPointF point = bandPoint(band);
        QColor color = bandColor(band, theme.dark);
        const bool selected = containsBand(m_selection, band);
        if (state.dynamicEnabled && std::abs(state.dynamicRangeDb) > 0.01) {
            QColor range = color; range.setAlpha(52);
            painter.setPen(QPen(range, selected ? 5.0 : 3.0, Qt::SolidLine,
                                Qt::RoundCap));
            painter.drawLine(point, QPointF(point.x(),
                yForGain(state.gainDb + state.dynamicRangeDb)));
        }
        if (selected) {
            QColor halo = color; halo.setAlpha(72);
            painter.setPen(QPen(halo, 5.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(point, 11.0, 11.0);
        }
        painter.setPen(QPen(theme.dark ? QColor(255,255,255,225)
                                     : QColor(0,0,0,205), 1.2));
        painter.setBrush(color);
        painter.drawEllipse(point, selected ? 8.0 : 6.5, selected ? 8.0 : 6.5);
        painter.setPen(theme.dark ? Qt::black : Qt::white);
        painter.drawText(QRectF(point.x() - 8, point.y() - 8, 16, 16),
                         Qt::AlignCenter, QString::number(band + 1));
        if (m_linearPhase && state.type == eq::FilterType::AllPass) {
            painter.setPen(QPen(Theme::record(), 2.0));
            painter.drawLine(point + QPointF(-6,-6), point + QPointF(6,6));
            painter.drawLine(point + QPointF(6,-6), point + QPointF(-6,6));
        }
    }
    painter.restore();

    painter.setPen(QPen(theme.separator(), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);
    if (hasFocus()) {
        painter.setPen(QPen(theme.accentHighlight, 2.0));
        painter.drawRect(QRectF(rect()).adjusted(1, 1, -2, -2));
    }
}

void EqualizerGraph::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const int band = bandAt(event->position());
    if (event->button() == Qt::MiddleButton && band >= 0) {
        if (auditionChanged) auditionChanged(band);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    choose(band, event->modifiers());
    if (band >= 0 && !m_selection.empty()) {
        m_pressPosition = event->position();
        m_dynamicDrag = (event->modifiers() & Qt::AltModifier) != 0;
        m_dragging = true;
        if (gestureStarted) gestureStarted();
    }
    event->accept();
}

void EqualizerGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || m_selection.empty()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const double startFrequency = frequencyAt(m_pressPosition.x());
    const double currentFrequency = frequencyAt(event->position().x());
    const double ratio = currentFrequency / std::max(10.0, startFrequency);
    double gainDelta = gainAt(event->position().y()) - gainAt(m_pressPosition.y());
    if (event->modifiers() & Qt::ShiftModifier) gainDelta *= 0.2;
    if (bandsMoved)
        bandsMoved(m_selection, ratio, m_dynamicDrag ? 0.0 : gainDelta,
                   m_dynamicDrag ? gainDelta : 0.0);
    event->accept();
}

void EqualizerGraph::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        if (auditionChanged) auditionChanged(-1);
        event->accept();
        return;
    }
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        if (gestureFinished) gestureFinished();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void EqualizerGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && plotRect().contains(event->position()) &&
        bandAt(event->position()) < 0) {
        if (bandCreated) bandCreated(frequencyAt(event->position().x()),
                                     gainAt(event->position().y()));
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void EqualizerGraph::wheelEvent(QWheelEvent* event) {
    const int band = bandAt(event->position());
    if (band >= 0 && !containsBand(m_selection, band)) choose(band, {});
    if (m_selection.empty()) return QWidget::wheelEvent(event);
    if (gestureStarted) gestureStarted();
    const double factor = std::pow(1.08, double(event->angleDelta().y()) / 120.0);
    if (qChanged) qChanged(m_selection, factor);
    if (gestureFinished) gestureFinished();
    event->accept();
}

void EqualizerGraph::nudge(double xOctaves, double gainDb) {
    if (m_selection.empty()) return;
    if (gestureStarted) gestureStarted();
    if (bandsMoved) bandsMoved(m_selection, std::pow(2.0, xOctaves), gainDb, 0.0);
    if (gestureFinished) gestureFinished();
}

void EqualizerGraph::keyPressEvent(QKeyEvent* event) {
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.2 : 1.0;
    switch (event->key()) {
        case Qt::Key_Left: nudge(-0.05 * fine, 0.0); break;
        case Qt::Key_Right: nudge(0.05 * fine, 0.0); break;
        case Qt::Key_Up: nudge(0.0, 0.5 * fine); break;
        case Qt::Key_Down: nudge(0.0, -0.5 * fine); break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (bandsDeleted) bandsDeleted(m_selection);
            break;
        case Qt::Key_Escape:
            choose(-1, {});
            break;
        default:
            QWidget::keyPressEvent(event);
            return;
    }
    event->accept();
}

void EqualizerGraph::contextMenuEvent(QContextMenuEvent* event) {
    const int band = bandAt(event->pos());
    if (band >= 0 && !containsBand(m_selection, band)) choose(band, {});
    QMenu menu(this);
    QAction* duplicate = menu.addAction(tr("Duplicate Band"));
    QAction* invert = menu.addAction(tr("Invert Gain"));
    QAction* reset = menu.addAction(tr("Reset Band"));
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("Delete Band"));
    const bool available = !m_selection.empty();
    duplicate->setEnabled(m_selection.size() == 1);
    invert->setEnabled(available);
    reset->setEnabled(available);
    remove->setEnabled(available);
    QAction* selected = menu.exec(event->globalPos());
    if (selected == duplicate && bandDuplicated) bandDuplicated(m_selection.front());
    if (selected == invert && gainsInverted) gainsInverted(m_selection);
    if (selected == reset && bandsReset) bandsReset(m_selection);
    if (selected == remove && bandsDeleted) bandsDeleted(m_selection);
}

// -- EqualizerPanel ---------------------------------------------------------

EqualizerPanel::EqualizerPanel(daw::EngineController* controller,
                               QString channelId, QString insertId,
                               QWidget* parent)
    : QWidget(parent), m_controller(controller),
      m_channelId(std::move(channelId)), m_insertId(std::move(insertId)),
      m_channelKey(m_channelId.toStdString()), m_insertKey(m_insertId.toStdString()) {
    setObjectName(QStringLiteral("EqualizerPanel"));
    setMinimumSize(820, 520);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(tr("VLT Equalizer editor"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 9);
    root->setSpacing(7);

    auto* top = new QHBoxLayout;
    top->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("VLT  EQUALIZER"), this);
    title->setAccessibleName(tr("VLT Equalizer"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    title->setFont(titleFont);
    top->addWidget(title);
    m_preset = new QPushButton(tr("Flat"), this);
    m_preset->setAccessibleName(tr("Equalizer preset browser"));
    m_preset->setMinimumWidth(175);
    top->addWidget(m_preset);
    m_a = new QPushButton(QStringLiteral("A"), this);
    m_b = new QPushButton(QStringLiteral("B"), this);
    auto* copy = new QPushButton(tr("Copy A/B"), this);
    m_a->setCheckable(true); m_b->setCheckable(true);
    m_a->setAccessibleName(tr("Use comparison A"));
    m_b->setAccessibleName(tr("Use comparison B"));
    copy->setAccessibleName(tr("Copy active comparison to inactive comparison"));
    top->addWidget(m_a); top->addWidget(m_b); top->addWidget(copy);
    top->addStretch(1);

    m_mode = new QComboBox(this);
    m_mode->setAccessibleName(tr("Processing mode"));
    m_mode->addItem(tr("Zero Latency"), int(eq::ProcessingMode::ZeroLatency));
    m_mode->addItem(tr("Analog Phase"), int(eq::ProcessingMode::AnalogPhase));
    m_mode->addItem(tr("Linear Phase"), int(eq::ProcessingMode::LinearPhase));
    m_resolution = new QComboBox(this);
    m_resolution->setAccessibleName(tr("Linear phase resolution"));
    m_resolution->addItem(tr("Low"), int(eq::LinearResolution::Low));
    m_resolution->addItem(tr("Medium"), int(eq::LinearResolution::Medium));
    m_resolution->addItem(tr("High"), int(eq::LinearResolution::High));
    m_displayRange = new QComboBox(this);
    m_displayRange->setAccessibleName(tr("Graph gain range"));
    for (int range : {6, 12, 18, 30})
        m_displayRange->addItem(QStringLiteral("%1 dB").arg(range), range);
    top->addWidget(m_mode);
    top->addWidget(m_resolution);
    top->addWidget(m_displayRange);
    root->addLayout(top);

    m_graph = new EqualizerGraph(this);
    root->addWidget(m_graph, 1);

    m_bandPanel = new QWidget(this);
    m_bandPanel->setAccessibleName(tr("Selected band controls"));
    auto* bandRow = new QHBoxLayout(m_bandPanel);
    bandRow->setContentsMargins(4, 3, 4, 3);
    bandRow->setSpacing(6);
    auto* bandLabel = new QLabel(tr("No band selected"), m_bandPanel);
    bandLabel->setObjectName(QStringLiteral("EqualizerBandLabel"));
    bandLabel->setMinimumWidth(76);
    bandRow->addWidget(bandLabel);

    m_type = new QComboBox(m_bandPanel);
    m_type->setAccessibleName(tr("Filter type"));
    for (const QString& name : {tr("Bell"), tr("Low Shelf"), tr("High Shelf"),
             tr("Low Cut"), tr("High Cut"), tr("Notch"), tr("Band Pass"),
             tr("Tilt"), tr("All Pass")})
        m_type->addItem(name);
    m_slope = new QComboBox(m_bandPanel);
    m_slope->setAccessibleName(tr("Filter slope"));
    for (int slope : {6, 12, 18, 24, 36, 48, 72, 96})
        m_slope->addItem(QStringLiteral("%1 dB/oct").arg(slope));
    m_placement = new QComboBox(m_bandPanel);
    m_placement->setAccessibleName(tr("Band channel placement"));
    m_placement->addItems({tr("Stereo"), tr("Left"), tr("Right"), tr("Mid"), tr("Side")});
    bandRow->addWidget(controlCell(m_type, tr("Type"), m_bandPanel));
    bandRow->addWidget(controlCell(m_slope, tr("Slope"), m_bandPanel));
    bandRow->addWidget(controlCell(m_placement, tr("Placement"), m_bandPanel));

    auto makeBandKnob = [this, bandRow](const QString& key, eq::BandParam field,
                                        const QString& caption, int diameter) {
        const QString example = bandId(0, field);
        const ParameterInfo* info = parameterInfo(example);
        auto* knob = new ui::Knob({}, m_bandPanel);
        if (info) {
            knob->setRange(info->minValue, info->maxValue);
            knob->setDefaultValue(info->defaultValue);
            knob->setStepped(info->isStepped);
            knob->setBipolar(info->minValue < 0.0 && info->maxValue > 0.0);
            const std::uint32_t index = info->index;
            knob->setFormatter([index](double value) {
                return QString::fromStdString(eq::parameterText(index, value));
            });
            knob->setToolTip(QString::fromStdString(info->name));
            knob->setAccessibleName(caption);
            knob->setAutomatable(info->isAutomatable);
        }
        knob->setBare(diameter);
        connect(knob, &ui::Knob::valueChanged, this,
                [this, field](double value) {
            if (m_refreshing || m_graph->selection().empty()) return;
            std::vector<QString> ids;
            for (int band : m_graph->selection()) ids.push_back(bandId(band, field));
            beginGesture(ids);
            for (const QString& id : ids) writeParameter(id, value);
        });
        connect(knob, &ui::Knob::editFinished, this,
                [this] { finishGesture("Edit Equalizer Band"); });
        connect(knob, &ui::Knob::automateRequested, this, [this, field] {
            const int band = selectedBand();
            if (band >= 0) emit automationRequested(bandId(band, field));
        });
        m_knobs.insert(key, knob);
        bandRow->addWidget(controlCell(knob, caption, m_bandPanel));
        return knob;
    };
    makeBandKnob(QStringLiteral("$band.frequency"), eq::BandParam::Frequency,
                 tr("Frequency"), 48);
    makeBandKnob(QStringLiteral("$band.gain"), eq::BandParam::Gain,
                 tr("Gain"), 48);
    makeBandKnob(QStringLiteral("$band.q"), eq::BandParam::Q, tr("Q"), 48);
    m_dynamic = new QCheckBox(tr("Dynamics"), m_bandPanel);
    m_dynamic->setAccessibleName(tr("Enable dynamics for selected band"));
    bandRow->addWidget(m_dynamic);
    root->addWidget(m_bandPanel);

    m_dynamicPanel = new QWidget(this);
    m_dynamicPanel->setAccessibleName(tr("Selected band dynamics"));
    auto* dynamicsRow = new QHBoxLayout(m_dynamicPanel);
    dynamicsRow->setContentsMargins(84, 2, 4, 3);
    dynamicsRow->setSpacing(7);
    auto makeDynamicsKnob = [this, dynamicsRow](const QString& key,
                                                 eq::BandParam field,
                                                 const QString& caption) {
        const QString example = bandId(0, field);
        const ParameterInfo* info = parameterInfo(example);
        auto* knob = new ui::Knob({}, m_dynamicPanel);
        if (info) {
            knob->setRange(info->minValue, info->maxValue);
            knob->setDefaultValue(info->defaultValue);
            knob->setBipolar(info->minValue < 0.0 && info->maxValue > 0.0);
            const std::uint32_t index = info->index;
            knob->setFormatter([index](double value) {
                return QString::fromStdString(eq::parameterText(index, value));
            });
            knob->setAccessibleName(caption);
            knob->setAutomatable(info->isAutomatable);
        }
        knob->setBare(42);
        connect(knob, &ui::Knob::valueChanged, this, [this, field](double value) {
            if (m_refreshing || m_graph->selection().empty()) return;
            std::vector<QString> ids;
            for (int band : m_graph->selection()) ids.push_back(bandId(band, field));
            beginGesture(ids);
            for (const QString& id : ids) writeParameter(id, value);
        });
        connect(knob, &ui::Knob::editFinished, this,
                [this] { finishGesture("Edit Equalizer Dynamics"); });
        connect(knob, &ui::Knob::automateRequested, this, [this, field] {
            const int band = selectedBand();
            if (band >= 0) emit automationRequested(bandId(band, field));
        });
        m_knobs.insert(key, knob);
        dynamicsRow->addWidget(controlCell(knob, caption, m_dynamicPanel));
    };
    makeDynamicsKnob(QStringLiteral("$band.dynamic.range"), eq::BandParam::DynamicRange,
                     tr("Range"));
    makeDynamicsKnob(QStringLiteral("$band.dynamic.threshold"), eq::BandParam::DynamicThreshold,
                     tr("Threshold"));
    makeDynamicsKnob(QStringLiteral("$band.dynamic.attack"), eq::BandParam::DynamicAttack,
                     tr("Attack"));
    makeDynamicsKnob(QStringLiteral("$band.dynamic.release"), eq::BandParam::DynamicRelease,
                     tr("Release"));
    makeDynamicsKnob(QStringLiteral("$band.detector.low"), eq::BandParam::DetectorLow,
                     tr("Detector Low"));
    makeDynamicsKnob(QStringLiteral("$band.detector.high"), eq::BandParam::DetectorHigh,
                     tr("Detector High"));
    m_dynamicAuto = new QCheckBox(tr("Auto"), m_dynamicPanel);
    m_external = new QCheckBox(tr("External SC"), m_dynamicPanel);
    m_detectorMode = new QComboBox(m_dynamicPanel);
    m_detectorMode->addItems({tr("Band detector"), tr("Free detector")});
    m_dynamicAuto->setAccessibleName(tr("Automatic dynamics timing and threshold"));
    m_external->setAccessibleName(tr("Use external sidechain"));
    m_detectorMode->setAccessibleName(tr("Dynamics detector range mode"));
    dynamicsRow->addWidget(m_dynamicAuto);
    dynamicsRow->addWidget(m_external);
    dynamicsRow->addWidget(m_detectorMode);
    root->addWidget(m_dynamicPanel);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(6);
    bottom->addWidget(controlCell(makeKnob(QStringLiteral("output.gain"), tr("Output"), 42),
                                  tr("Output"), this));
    bottom->addWidget(controlCell(makeKnob(QStringLiteral("output.balance"), tr("Balance"), 42),
                                  tr("Balance"), this));
    bottom->addWidget(controlCell(makeKnob(QStringLiteral("gain.scale"), tr("Gain Scale"), 42),
                                  tr("Gain Scale"), this));
    m_autoGain = new QCheckBox(tr("Auto Gain"), this);
    m_polarity = new QCheckBox(tr("Invert Polarity"), this);
    m_autoGain->setAccessibleName(tr("Automatic output gain compensation"));
    m_polarity->setAccessibleName(tr("Invert output polarity"));
    bottom->addWidget(m_autoGain);
    bottom->addWidget(m_polarity);
    bottom->addStretch(1);
    auto* analyzerLabel = new QLabel(tr("Analyzer:"), this);
    bottom->addWidget(analyzerLabel);
    m_pre = new QCheckBox(tr("Pre"), this);
    m_post = new QCheckBox(tr("Post"), this);
    m_side = new QCheckBox(tr("Sidechain"), this);
    m_freeze = new QCheckBox(tr("Freeze"), this);
    bottom->addWidget(m_pre); bottom->addWidget(m_post);
    bottom->addWidget(m_side); bottom->addWidget(m_freeze);
    m_sidechainStatus = new QLabel(this);
    m_sidechainStatus->setAccessibleName(tr("Sidechain connection status"));
    bottom->addWidget(m_sidechainStatus);
    root->addLayout(bottom);

    auto connectCombo = [this](QComboBox* combo, const QString& id,
                               const char* label) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, combo, id, label](int index) {
            if (m_refreshing || index < 0) return;
            beginGesture({id});
            writeParameter(id, combo->itemData(index).isValid()
                                 ? combo->itemData(index).toDouble() : index);
            finishGesture(label);
        });
    };
    connectCombo(m_mode, QStringLiteral("processing.mode"), "Change Equalizer Phase Mode");
    connectCombo(m_resolution, QStringLiteral("linear.resolution"),
                 "Change Equalizer Linear Resolution");

    auto connectBandCombo = [this](QComboBox* combo, eq::BandParam field,
                                   const char* label) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, combo, field, label](int index) {
            if (m_refreshing || index < 0 || m_graph->selection().empty()) return;
            std::vector<QString> ids;
            for (int band : m_graph->selection()) ids.push_back(bandId(band, field));
            beginGesture(ids);
            for (const QString& id : ids) writeParameter(id, index);
            finishGesture(label);
        });
    };
    connectBandCombo(m_type, eq::BandParam::Type, "Change Equalizer Filter Type");
    connectBandCombo(m_slope, eq::BandParam::Slope, "Change Equalizer Filter Slope");
    connectBandCombo(m_placement, eq::BandParam::Placement,
                     "Change Equalizer Band Placement");
    connectBandCombo(m_detectorMode, eq::BandParam::DetectorMode,
                     "Change Equalizer Detector");

    auto connectBandCheck = [this](QCheckBox* check, eq::BandParam field,
                                   const char* label) {
        connect(check, &QCheckBox::toggled, this, [this, field, label](bool checked) {
            if (m_refreshing || m_graph->selection().empty()) return;
            std::vector<QString> ids;
            for (int band : m_graph->selection()) ids.push_back(bandId(band, field));
            beginGesture(ids);
            for (const QString& id : ids) writeParameter(id, checked ? 1.0 : 0.0);
            finishGesture(label);
        });
    };
    connectBandCheck(m_dynamic, eq::BandParam::DynamicEnabled,
                     "Toggle Equalizer Dynamics");
    connectBandCheck(m_dynamicAuto, eq::BandParam::DynamicAuto,
                     "Toggle Equalizer Dynamic Auto");
    connectBandCheck(m_external, eq::BandParam::DynamicExternal,
                     "Change Equalizer Sidechain Source");

    auto connectGlobalCheck = [this](QCheckBox* check, const QString& id,
                                     const char* label) {
        connect(check, &QCheckBox::toggled, this, [this, id, label](bool checked) {
            if (m_refreshing) return;
            beginGesture({id});
            writeParameter(id, checked ? 1.0 : 0.0);
            finishGesture(label);
        });
    };
    connectGlobalCheck(m_autoGain, QStringLiteral("auto.gain"),
                       "Toggle Equalizer Auto Gain");
    connectGlobalCheck(m_polarity, QStringLiteral("output.polarity"),
                       "Toggle Equalizer Polarity");

    connect(m_preset, &QPushButton::clicked, this, &EqualizerPanel::showPresetMenu);
    connect(m_a, &QPushButton::clicked, this, [this] { switchComparison('A'); });
    connect(m_b, &QPushButton::clicked, this, [this] { switchComparison('B'); });
    connect(copy, &QPushButton::clicked, this, &EqualizerPanel::copyComparison);
    connect(m_displayRange, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                QSettings().setValue(QStringLiteral("equalizer/displayRange"),
                                     m_displayRange->currentData());
                refresh();
            });
    for (QCheckBox* check : {m_pre, m_post, m_side, m_freeze})
        connect(check, &QCheckBox::toggled, this, &EqualizerPanel::updateAnalyzerConfig);

    m_graph->selectionChanged = [this](const std::vector<int>& bands) {
        selectBands(bands);
    };
    m_graph->gestureStarted = [this] {
        m_graphGestureStart = {};
        std::vector<QString> ids;
        if (eq::EqualizerInstance* instance = equalizerInstance()) {
            for (int band : m_graph->selection()) {
                m_graphGestureStart[std::size_t(band)] = instance->bandState(band);
                ids.push_back(bandId(band, eq::BandParam::Frequency));
                ids.push_back(bandId(band, eq::BandParam::Gain));
                ids.push_back(bandId(band, eq::BandParam::DynamicRange));
                ids.push_back(bandId(band, eq::BandParam::Q));
            }
        }
        beginGesture(ids);
    };
    m_graph->bandsMoved = [this](const std::vector<int>& bands, double ratio,
                                 double gain, double dynamic) {
        moveBands(bands, ratio, gain, dynamic);
    };
    m_graph->qChanged = [this](const std::vector<int>& bands, double factor) {
        changeQ(bands, factor);
    };
    m_graph->gestureFinished = [this] { finishGesture("Move Equalizer Bands"); };
    m_graph->bandCreated = [this](double frequency, double gain) {
        createBand(frequency, gain);
    };
    m_graph->bandsDeleted = [this](const std::vector<int>& bands) { deleteBands(bands); };
    m_graph->bandDuplicated = [this](int band) { duplicateBand(band); };
    m_graph->gainsInverted = [this](const std::vector<int>& bands) { invertGains(bands); };
    m_graph->bandsReset = [this](const std::vector<int>& bands) { resetBands(bands); };
    m_graph->auditionChanged = [this](int band) {
        if (eq::EqualizerInstance* instance = equalizerInstance())
            instance->setAuditionBand(band);
    };

    QSettings settings;
    m_pre->setChecked(settings.value(QStringLiteral("equalizer/analyzer/pre"), true).toBool());
    m_post->setChecked(settings.value(QStringLiteral("equalizer/analyzer/post"), true).toBool());
    m_side->setChecked(settings.value(QStringLiteral("equalizer/analyzer/sidechain"), false).toBool());
    m_freeze->setChecked(false);
    const int storedRange = settings.value(QStringLiteral("equalizer/displayRange"), 12).toInt();
    m_displayRange->setCurrentIndex(std::max(0, m_displayRange->findData(storedRange)));

    m_timer = new QTimer(this);
    m_timer->setInterval(QSettings().value(QStringLiteral("ui/reduceMotion"), false).toBool()
                             ? 80 : 33);
    connect(m_timer, &QTimer::timeout, this, &EqualizerPanel::refresh);
    reloadUserPresets();
    updateAnalyzerConfig();
    refresh();
    if (qEnvironmentVariableIsSet("DAW_SHOT_EQUALIZER")) {
        if (eq::EqualizerInstance* instance = equalizerInstance()) {
            for (int band = 0; band < int(eq::kBandCount); ++band) {
                if (!instance->bandState(band).enabled) continue;
                m_graph->setSelection({band});
                selectBands({band});
                break;
            }
        }
    }
}

eq::EqualizerInstance* EqualizerPanel::equalizerInstance() const {
    if (!m_controller) return nullptr;
    return dynamic_cast<eq::EqualizerInstance*>(
        m_controller->insertInstance(m_channelKey, m_insertKey));
}

double EqualizerPanel::readParameter(const QString& id) const {
    return m_controller ? m_controller->insertParameter(m_channelKey, m_insertKey,
                                                         id.toStdString()) : 0.0;
}

void EqualizerPanel::writeParameter(const QString& id, double value) {
    if (!m_controller) return;
    m_controller->setInsertParameter(m_channelKey, m_insertKey, id.toStdString(), value);
    if (!m_refreshing) {
        m_selectedKind = QStringLiteral("custom");
        m_selectedName = tr("Custom");
        if (eq::EqualizerInstance* instance = equalizerInstance())
            instance->setPresetReference("custom", "Custom");
    }
}

ui::Knob* EqualizerPanel::makeKnob(const QString& id, const QString& caption, int size) {
    auto* knob = new ui::Knob({}, this);
    if (const ParameterInfo* info = parameterInfo(id)) {
        knob->setRange(info->minValue, info->maxValue);
        knob->setDefaultValue(info->defaultValue);
        knob->setStepped(info->isStepped);
        knob->setBipolar(info->minValue < 0.0 && info->maxValue > 0.0);
        const std::uint32_t index = info->index;
        knob->setFormatter([index](double value) {
            return QString::fromStdString(eq::parameterText(index, value));
        });
        knob->setAccessibleName(caption);
        knob->setToolTip(QString::fromStdString(info->name));
        knob->setAutomatable(info->isAutomatable);
    }
    knob->setBare(size);
    knob->setValue(readParameter(id));
    connect(knob, &ui::Knob::valueChanged, this, [this, id](double value) {
        if (m_refreshing) return;
        beginGesture({id});
        writeParameter(id, value);
    });
    connect(knob, &ui::Knob::editFinished, this,
            [this] { finishGesture("Edit Equalizer Output"); });
    connect(knob, &ui::Knob::automateRequested, this,
            [this, id] { emit automationRequested(id); });
    m_knobs.insert(id, knob);
    return knob;
}

void EqualizerPanel::beginGesture(const std::vector<QString>& ids) {
    for (const QString& id : ids)
        if (!m_gestureStart.contains(id)) m_gestureStart.insert(id, readParameter(id));
}

void EqualizerPanel::finishGesture(const char* label) {
    if (!m_controller || m_gestureStart.isEmpty()) return;
    const auto undo = m_controller->beginUndoGroup();
    const auto starts = m_gestureStart;
    m_gestureStart.clear();
    for (auto it = starts.cbegin(); it != starts.cend(); ++it)
        m_controller->commitInsertParameterEdit(m_channelKey, m_insertKey,
                                                it.key().toStdString(), it.value(), label);
    m_controller->collapseUndo(undo, label);
    emit projectEdited();
    refresh();
}

QString EqualizerPanel::bandId(int band, eq::BandParam field) const {
    if (band < 0 || band >= int(eq::kBandCount)) return {};
    return QString::fromStdString(eq::parameterId(
        eq::bandParameter(std::uint32_t(band), field)));
}

int EqualizerPanel::selectedBand() const {
    return m_graph && !m_graph->selection().empty() ? m_graph->selection().front() : -1;
}

void EqualizerPanel::selectBands(const std::vector<int>& bands) {
    if (m_graph && m_graph->selection() != bands) m_graph->setSelection(bands);
    refreshBandControls();
}

void EqualizerPanel::createBand(double frequency, double gain) {
    eq::EqualizerInstance* instance = equalizerInstance();
    if (!instance) return;
    int band = -1;
    for (int i = 0; i < int(eq::kBandCount); ++i)
        if (!instance->bandState(i).enabled) { band = i; break; }
    if (band < 0) {
        QMessageBox::information(this, tr("Equalizer is full"),
                                 tr("All 24 equalizer bands are already in use."));
        return;
    }
    const std::vector<QString> ids{bandId(band, eq::BandParam::Enabled),
                                   bandId(band, eq::BandParam::Frequency),
                                   bandId(band, eq::BandParam::Gain)};
    beginGesture(ids);
    writeParameter(ids[0], 1.0);
    writeParameter(ids[1], frequency);
    writeParameter(ids[2], gain);
    finishGesture("Create Equalizer Band");
    m_graph->setSelection({band});
    selectBands({band});
}

void EqualizerPanel::deleteBands(const std::vector<int>& bands) {
    std::vector<QString> ids;
    for (int band : bands) ids.push_back(bandId(band, eq::BandParam::Enabled));
    beginGesture(ids);
    for (const QString& id : ids) writeParameter(id, 0.0);
    finishGesture("Delete Equalizer Bands");
    m_graph->setSelection({});
    selectBands({});
}

void EqualizerPanel::duplicateBand(int source) {
    eq::EqualizerInstance* instance = equalizerInstance();
    if (!instance || source < 0) return;
    int target = -1;
    for (int i = 0; i < int(eq::kBandCount); ++i)
        if (!instance->bandState(i).enabled) { target = i; break; }
    if (target < 0) return;
    std::vector<QString> ids;
    for (std::uint32_t field = 0; field < eq::kBandParameterCount; ++field)
        ids.push_back(bandId(target, eq::BandParam(field)));
    beginGesture(ids);
    for (std::uint32_t field = 0; field < eq::kBandParameterCount; ++field)
        writeParameter(ids[field], readParameter(bandId(source, eq::BandParam(field))));
    const double moved = std::min(30000.0,
        readParameter(bandId(source, eq::BandParam::Frequency)) * std::pow(2.0, 1.0 / 12.0));
    writeParameter(bandId(target, eq::BandParam::Frequency), moved);
    finishGesture("Duplicate Equalizer Band");
    m_graph->setSelection({target});
    selectBands({target});
}

void EqualizerPanel::resetBands(const std::vector<int>& bands) {
    std::vector<QString> ids;
    for (int band : bands)
        for (std::uint32_t field = 0; field < eq::kBandParameterCount; ++field)
            ids.push_back(bandId(band, eq::BandParam(field)));
    beginGesture(ids);
    for (int band : bands) {
        for (std::uint32_t field = 0; field < eq::kBandParameterCount; ++field) {
            const QString id = bandId(band, eq::BandParam(field));
            if (const ParameterInfo* info = parameterInfo(id))
                writeParameter(id, field == std::uint32_t(eq::BandParam::Enabled)
                                       ? 1.0 : info->defaultValue);
        }
    }
    finishGesture("Reset Equalizer Bands");
}

void EqualizerPanel::invertGains(const std::vector<int>& bands) {
    std::vector<QString> ids;
    for (int band : bands) ids.push_back(bandId(band, eq::BandParam::Gain));
    beginGesture(ids);
    for (const QString& id : ids) writeParameter(id, -readParameter(id));
    finishGesture("Invert Equalizer Gains");
}

void EqualizerPanel::moveBands(const std::vector<int>& bands, double ratio,
                               double gainDelta, double dynamicDelta) {
    for (int band : bands) {
        const eq::BandState& start = m_graphGestureStart[std::size_t(band)];
        if (ratio != 1.0)
            writeParameter(bandId(band, eq::BandParam::Frequency),
                           std::clamp(start.frequency * ratio, 10.0, 30000.0));
        if (gainDelta != 0.0)
            writeParameter(bandId(band, eq::BandParam::Gain),
                           std::clamp(start.gainDb + gainDelta, -30.0, 30.0));
        if (dynamicDelta != 0.0)
            writeParameter(bandId(band, eq::BandParam::DynamicRange),
                           std::clamp(start.dynamicRangeDb + dynamicDelta, -30.0, 30.0));
    }
    refresh();
}

void EqualizerPanel::changeQ(const std::vector<int>& bands, double factor) {
    for (int band : bands) {
        const double start = m_graphGestureStart[std::size_t(band)].q;
        writeParameter(bandId(band, eq::BandParam::Q),
                       std::clamp(start * factor, 0.025, 40.0));
    }
    refresh();
}

EqualizerPanel::Values EqualizerPanel::currentValues() const {
    Values values{};
    for (const ParameterInfo& info : eq::parameterTable())
        values[info.index] = readParameter(QString::fromStdString(info.id));
    return values;
}

void EqualizerPanel::applyValues(const Values& values, const QString& kind,
                                 const QString& name, const char* label) {
    if (!m_controller) return;
    const auto undo = m_controller->beginUndoGroup();
    for (const ParameterInfo& info : eq::parameterTable()) {
        const QString id = QString::fromStdString(info.id);
        const double before = readParameter(id);
        if (std::abs(before - values[info.index]) < 1.0e-12) continue;
        writeParameter(id, values[info.index]);
        m_controller->commitInsertParameterEdit(m_channelKey, m_insertKey, info.id,
                                                before, label);
    }
    m_controller->collapseUndo(undo, label);
    m_selectedKind = kind;
    m_selectedName = name;
    if (eq::EqualizerInstance* instance = equalizerInstance())
        instance->setPresetReference(kind.toStdString(), name.toStdString());
    emit projectEdited();
    refresh();
}

void EqualizerPanel::applyFactoryPreset(int index) {
    const auto presets = eq::factoryPresets();
    if (index < 0 || index >= int(presets.size())) return;
    const eq::FactoryPreset& preset = presets[std::size_t(index)];
    applyValues(preset.values, QStringLiteral("factory"),
                QString::fromUtf8(preset.name.data(), int(preset.name.size())),
                "Apply Equalizer Preset");
    std::vector<int> active;
    if (eq::EqualizerInstance* instance = equalizerInstance())
        for (int band = 0; band < int(eq::kBandCount); ++band)
            if (instance->bandState(band).enabled) active.push_back(band);
    m_graph->setSelection(active.empty() ? std::vector<int>{}
                                         : std::vector<int>{active.front()});
}

void EqualizerPanel::applyUserPreset(const QString& name) {
    const auto found = std::find_if(m_userPresets.begin(), m_userPresets.end(),
        [&](const UserPreset& preset) {
            return preset.name.compare(name, Qt::CaseInsensitive) == 0;
        });
    if (found != m_userPresets.end())
        applyValues(found->values, QStringLiteral("user"), found->name,
                    "Apply Equalizer User Preset");
}

void EqualizerPanel::showPresetMenu() {
    QMenu menu(this);
    menu.addSection(tr("Factory Presets"));
    const Values actual = currentValues();
    const auto matches = [&](const Values& values) {
        for (std::uint32_t i = 0; i < eq::kParameterCount; ++i)
            if (std::abs(actual[i] - values[i]) > 1.0e-6) return false;
        return true;
    };
    const auto factories = eq::factoryPresets();
    for (int i = 0; i < int(factories.size()); ++i) {
        const eq::FactoryPreset& preset = factories[std::size_t(i)];
        QAction* action = menu.addAction(
            QString::fromUtf8(preset.name.data(), int(preset.name.size())));
        action->setCheckable(true);
        action->setChecked(matches(preset.values));
        connect(action, &QAction::triggered, this,
                [this, i] { applyFactoryPreset(i); });
    }
    if (!m_userPresets.empty()) {
        menu.addSection(tr("User Presets"));
        for (const UserPreset& preset : m_userPresets) {
            QAction* action = menu.addAction(preset.name);
            action->setCheckable(true);
            action->setChecked(matches(preset.values));
            connect(action, &QAction::triggered, this,
                    [this, name = preset.name] { applyUserPreset(name); });
        }
    }
    menu.addSeparator();
    connect(menu.addAction(tr("Save Current as User Preset...")),
            &QAction::triggered, this, &EqualizerPanel::saveUserPreset);
    QAction* rename = menu.addAction(tr("Rename User Preset..."));
    QAction* remove = menu.addAction(tr("Delete User Preset..."));
    const bool selectedUser = m_selectedKind == QStringLiteral("user") &&
        std::any_of(m_userPresets.begin(), m_userPresets.end(),
            [&](const UserPreset& preset) {
                return preset.name.compare(m_selectedName, Qt::CaseInsensitive) == 0;
            });
    rename->setEnabled(selectedUser);
    remove->setEnabled(selectedUser);
    connect(rename, &QAction::triggered, this, &EqualizerPanel::renameUserPreset);
    connect(remove, &QAction::triggered, this, &EqualizerPanel::deleteUserPreset);
    menu.exec(m_preset->mapToGlobal(QPoint(0, m_preset->height() + 2)));
}

void EqualizerPanel::reloadUserPresets() {
    m_userPresets.clear();
    const QByteArray raw = QSettings().value(QLatin1String(kUserPresetKey)).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    if (!document.isObject()) return;
    const QJsonArray stored = document.object().value(QStringLiteral("presets")).toArray();
    for (const QJsonValue& item : stored) {
        if (m_userPresets.size() >= 128 || !item.isObject()) break;
        const QJsonObject object = item.toObject();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || name.size() > 48 || factoryNameReserved(name)) continue;
        if (std::any_of(m_userPresets.begin(), m_userPresets.end(),
                [&](const UserPreset& preset) {
                    return preset.name.compare(name, Qt::CaseInsensitive) == 0;
                })) continue;
        UserPreset preset;
        preset.name = name;
        const QJsonObject values = object.value(QStringLiteral("params")).toObject();
        for (const ParameterInfo& info : eq::parameterTable()) {
            preset.values[info.index] = info.defaultValue;
            const QJsonValue value = values.value(QString::fromStdString(info.id));
            if (value.isDouble()) preset.values[info.index] =
                clampedValue(info, value.toDouble());
        }
        m_userPresets.push_back(std::move(preset));
    }
}

void EqualizerPanel::storeUserPresets() const {
    QJsonArray presets;
    for (const UserPreset& preset : m_userPresets) {
        QJsonObject values;
        for (const ParameterInfo& info : eq::parameterTable())
            values.insert(QString::fromStdString(info.id), preset.values[info.index]);
        presets.append(QJsonObject{{QStringLiteral("name"), preset.name},
                                   {QStringLiteral("params"), values}});
    }
    QSettings().setValue(QLatin1String(kUserPresetKey),
        QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                  {QStringLiteral("presets"), presets}})
            .toJson(QJsonDocument::Compact));
}

void EqualizerPanel::saveUserPreset() {
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Save Equalizer Preset"),
        tr("Preset name:"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (name.isEmpty() || name.size() > 48 || factoryNameReserved(name)) {
        QMessageBox::warning(this, tr("Invalid preset name"),
            tr("Use 1-48 characters and a name not reserved by a factory preset."));
        return;
    }
    auto found = std::find_if(m_userPresets.begin(), m_userPresets.end(),
        [&](const UserPreset& preset) {
            return preset.name.compare(name, Qt::CaseInsensitive) == 0;
        });
    if (found != m_userPresets.end()) {
        if (QMessageBox::question(this, tr("Replace Equalizer Preset"),
                tr("Replace \"%1\"?").arg(found->name),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) return;
    } else {
        if (m_userPresets.size() >= 128) {
            QMessageBox::warning(this, tr("Preset library full"),
                                 tr("VLT Equalizer supports up to 128 user presets."));
            return;
        }
        m_userPresets.push_back(UserPreset{});
        found = std::prev(m_userPresets.end());
    }
    found->name = name;
    found->values = currentValues();
    storeUserPresets();
    m_selectedKind = QStringLiteral("user");
    m_selectedName = name;
    if (eq::EqualizerInstance* instance = equalizerInstance())
        instance->setPresetReference("user", name.toStdString());
    refresh();
}

void EqualizerPanel::renameUserPreset() {
    auto current = std::find_if(m_userPresets.begin(), m_userPresets.end(),
        [&](const UserPreset& preset) {
            return preset.name.compare(m_selectedName, Qt::CaseInsensitive) == 0;
        });
    if (current == m_userPresets.end()) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename Equalizer Preset"),
        tr("Preset name:"), QLineEdit::Normal, current->name, &accepted).trimmed();
    if (!accepted || name == current->name) return;
    const bool invalid = name.isEmpty() || name.size() > 48 || factoryNameReserved(name) ||
        std::any_of(m_userPresets.begin(), m_userPresets.end(),
            [&](const UserPreset& preset) {
                return &preset != &*current &&
                    preset.name.compare(name, Qt::CaseInsensitive) == 0;
            });
    if (invalid) {
        QMessageBox::warning(this, tr("Invalid preset name"),
                             tr("Choose a unique name of 1-48 characters."));
        return;
    }
    current->name = name;
    m_selectedName = name;
    storeUserPresets();
    if (eq::EqualizerInstance* instance = equalizerInstance())
        instance->setPresetReference("user", name.toStdString());
    refresh();
}

void EqualizerPanel::deleteUserPreset() {
    auto current = std::find_if(m_userPresets.begin(), m_userPresets.end(),
        [&](const UserPreset& preset) {
            return preset.name.compare(m_selectedName, Qt::CaseInsensitive) == 0;
        });
    if (current == m_userPresets.end()) return;
    if (QMessageBox::question(this, tr("Delete Equalizer Preset"),
            tr("Delete \"%1\"? The current sound remains unchanged.").arg(current->name),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    m_userPresets.erase(current);
    storeUserPresets();
    m_selectedKind = QStringLiteral("custom");
    m_selectedName = tr("Custom");
    if (eq::EqualizerInstance* instance = equalizerInstance())
        instance->setPresetReference("custom", "Custom");
    refresh();
}

void EqualizerPanel::switchComparison(char slot) {
    eq::EqualizerInstance* instance = equalizerInstance();
    if (!instance || instance->activeComparison() == slot) return;
    const char before = instance->activeComparison();
    instance->captureComparison(before);
    const Values values = instance->comparison(slot);
    applyValues(values, QStringLiteral("custom"), tr("Custom"),
                "Switch Equalizer A/B");
    instance->setActiveComparison(slot);
    refresh();
}

void EqualizerPanel::copyComparison() {
    eq::EqualizerInstance* instance = equalizerInstance();
    if (!instance) return;
    const char source = instance->activeComparison();
    const char target = source == 'A' ? 'B' : 'A';
    instance->captureComparison(source);
    instance->copyComparison(source, target);
    emit projectEdited();
    refresh();
}

void EqualizerPanel::updateAnalyzerConfig() {
    QSettings settings;
    settings.setValue(QStringLiteral("equalizer/analyzer/pre"), m_pre->isChecked());
    settings.setValue(QStringLiteral("equalizer/analyzer/post"), m_post->isChecked());
    settings.setValue(QStringLiteral("equalizer/analyzer/sidechain"), m_side->isChecked());
    if (eq::EqualizerInstance* instance = equalizerInstance()) {
        eq::AnalyzerConfig config = instance->analyzerConfig();
        config.enabled = isVisible();
        config.pre = m_pre->isChecked();
        config.post = m_post->isChecked();
        config.sidechain = m_side->isChecked();
        config.frozen = m_freeze->isChecked();
        instance->setAnalyzerConfig(config);
    }
}

void EqualizerPanel::refreshBandControls() {
    const int band = selectedBand();
    const bool selected = band >= 0;
    m_bandPanel->setEnabled(selected);
    QLabel* label = m_bandPanel->findChild<QLabel*>(QStringLiteral("EqualizerBandLabel"));
    if (label) label->setText(selected ? tr("Band %1").arg(band + 1)
                                      : tr("No band selected"));
    if (!selected) {
        m_dynamicPanel->hide();
        return;
    }
    eq::EqualizerInstance* instance = equalizerInstance();
    if (!instance) return;
    const eq::BandState state = instance->bandState(std::uint32_t(band));
    auto setCombo = [](QComboBox* combo, int value) {
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(value);
    };
    setCombo(m_type, int(state.type));
    setCombo(m_slope, int(state.slope));
    setCombo(m_placement, int(state.placement));
    setCombo(m_detectorMode, int(state.detectorMode));
    {
        const QSignalBlocker one(m_dynamic), two(m_dynamicAuto), three(m_external);
        m_dynamic->setChecked(state.dynamicEnabled);
        m_dynamicAuto->setChecked(state.dynamicAuto);
        m_external->setChecked(state.externalSidechain);
    }
    auto setBandKnob = [this, band](const QString& key, eq::BandParam field) {
        if (ui::Knob* knob = m_knobs.value(key); knob && !knob->isEditing())
            knob->setValue(readParameter(bandId(band, field)));
    };
    setBandKnob(QStringLiteral("$band.frequency"), eq::BandParam::Frequency);
    setBandKnob(QStringLiteral("$band.gain"), eq::BandParam::Gain);
    setBandKnob(QStringLiteral("$band.q"), eq::BandParam::Q);
    setBandKnob(QStringLiteral("$band.dynamic.range"), eq::BandParam::DynamicRange);
    setBandKnob(QStringLiteral("$band.dynamic.threshold"), eq::BandParam::DynamicThreshold);
    setBandKnob(QStringLiteral("$band.dynamic.attack"), eq::BandParam::DynamicAttack);
    setBandKnob(QStringLiteral("$band.dynamic.release"), eq::BandParam::DynamicRelease);
    setBandKnob(QStringLiteral("$band.detector.low"), eq::BandParam::DetectorLow);
    setBandKnob(QStringLiteral("$band.detector.high"), eq::BandParam::DetectorHigh);
    const bool supportsDynamics = state.type == eq::FilterType::Bell ||
        state.type == eq::FilterType::LowShelf ||
        state.type == eq::FilterType::HighShelf || state.type == eq::FilterType::Tilt;
    m_dynamic->setEnabled(supportsDynamics);
    m_dynamicPanel->setVisible(supportsDynamics && state.dynamicEnabled);
    if (ui::Knob* threshold = m_knobs.value(QStringLiteral("$band.dynamic.threshold")))
        threshold->setEnabled(!state.dynamicAuto);
    for (const QString& key : {QStringLiteral("$band.detector.low"),
                               QStringLiteral("$band.detector.high")})
        if (ui::Knob* knob = m_knobs.value(key))
            knob->setEnabled(state.detectorMode == eq::DetectorMode::Free);
}

void EqualizerPanel::refresh() {
    if (!m_controller) return;
    m_refreshing = true;
    auto syncCombo = [this](QComboBox* combo, const QString& id) {
        const int value = int(std::lround(readParameter(id)));
        if (combo->currentIndex() != value) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(value);
        }
    };
    syncCombo(m_mode, QStringLiteral("processing.mode"));
    syncCombo(m_resolution, QStringLiteral("linear.resolution"));
    m_resolution->setVisible(m_mode->currentIndex() == int(eq::ProcessingMode::LinearPhase));
    for (const QString& id : {QStringLiteral("output.gain"),
                              QStringLiteral("output.balance"),
                              QStringLiteral("gain.scale")})
        if (ui::Knob* knob = m_knobs.value(id); knob && !knob->isEditing() &&
            !m_gestureStart.contains(id)) knob->setValue(readParameter(id));
    {
        const QSignalBlocker autoBlock(m_autoGain), polarityBlock(m_polarity);
        m_autoGain->setChecked(readParameter(QStringLiteral("auto.gain")) >= 0.5);
        m_polarity->setChecked(readParameter(QStringLiteral("output.polarity")) >= 0.5);
    }

    eq::Telemetry telemetry;
    std::array<eq::BandState, eq::kBandCount> bands{};
    std::array<double, 256> response{};
    if (eq::EqualizerInstance* instance = equalizerInstance()) {
        telemetry = instance->consumeTelemetry();
        for (std::uint32_t band = 0; band < eq::kBandCount; ++band)
            bands[band] = instance->bandState(band);
        for (std::size_t i = 0; i < response.size(); ++i) {
            const double frequency = 10.0 * std::pow(3000.0,
                double(i) / double(response.size() - 1));
            response[i] = instance->responseDb(frequency);
        }
        const char active = instance->activeComparison();
        m_a->setChecked(active == 'A');
        m_b->setChecked(active == 'B');
        const auto [kind, name] = instance->presetReference();
        m_selectedKind = QString::fromStdString(kind);
        m_selectedName = QString::fromStdString(name);
    }
    bool exact = false;
    const Values actual = currentValues();
    for (const eq::FactoryPreset& preset : eq::factoryPresets()) {
        bool same = true;
        for (std::uint32_t i = 0; i < eq::kParameterCount; ++i)
            if (std::abs(actual[i] - preset.values[i]) > 1.0e-6) { same = false; break; }
        if (same) {
            m_selectedKind = QStringLiteral("factory");
            m_selectedName = QString::fromUtf8(preset.name.data(), int(preset.name.size()));
            exact = true;
            break;
        }
    }
    if (!exact) {
        for (const UserPreset& preset : m_userPresets) {
            bool same = true;
            for (std::uint32_t i = 0; i < eq::kParameterCount; ++i)
                if (std::abs(actual[i] - preset.values[i]) > 1.0e-6) { same = false; break; }
            if (same) {
                m_selectedKind = QStringLiteral("user");
                m_selectedName = preset.name;
                exact = true;
                break;
            }
        }
    }
    m_preset->setText(exact ? m_selectedName : tr("Custom"));
    const bool externalWanted = std::any_of(bands.begin(), bands.end(),
        [](const eq::BandState& band) {
            return band.enabled && band.dynamicEnabled && band.externalSidechain;
        });
    m_sidechainStatus->setText(externalWanted && !telemetry.sidechainPresent
                                   ? tr("SC missing") : QString{});
    QFont statusFont = m_sidechainStatus->font();
    statusFont.setBold(externalWanted && !telemetry.sidechainPresent);
    m_sidechainStatus->setFont(statusFont);
    QPalette statusPalette = m_sidechainStatus->palette();
    statusPalette.setColor(QPalette::WindowText,
        externalWanted && !telemetry.sidechainPresent
            ? Theme::record() : th().textSecondary);
    m_sidechainStatus->setPalette(statusPalette);
    const double range = m_displayRange->currentData().toDouble();
    m_graph->setData(bands, response, telemetry,
                     m_mode->currentIndex() == int(eq::ProcessingMode::LinearPhase),
                     range > 0.0 ? range : 12.0);
    refreshBandControls();
    m_refreshing = false;
}

bool EqualizerPanel::checkForTest() {
    if (!m_controller || !equalizerInstance()) return false;
    const std::vector<std::uint8_t> saved = [&] {
        std::vector<std::uint8_t> state;
        equalizerInstance()->saveState(state);
        return state;
    }();
    const std::size_t createDepth = m_controller->undoDepth();
    createBand(1000.0, 3.0);
    const bool created = selectedBand() >= 0 &&
        equalizerInstance()->bandState(std::uint32_t(selectedBand())).enabled &&
        m_controller->undoDepth() == createDepth + 1;
    const int band = selectedBand();
    if (band >= 0) {
        m_graphGestureStart[std::size_t(band)] = equalizerInstance()->bandState(band);
        beginGesture({bandId(band, eq::BandParam::Frequency),
                      bandId(band, eq::BandParam::Gain)});
        moveBands({band}, 2.0, -1.0, 0.0);
        finishGesture("Equalizer UI Selftest Move");
    }
    const bool moved = band >= 0 &&
        equalizerInstance()->bandState(band).frequency > 1500.0;
    const char beforeSlot = equalizerInstance()->activeComparison();
    switchComparison(beforeSlot == 'A' ? 'B' : 'A');
    const bool comparison = equalizerInstance()->activeComparison() != beforeSlot;
    updateAnalyzerConfig();
    const bool analyzer = equalizerInstance()->analyzerConfig().enabled == isVisible();
    bool accessible = m_graph->focusPolicy() == Qt::StrongFocus &&
                      !m_graph->accessibleName().isEmpty();
    for (ui::Knob* knob : std::as_const(m_knobs))
        accessible = accessible && knob->focusPolicy() == Qt::StrongFocus &&
                     !knob->accessibleName().isEmpty();
    equalizerInstance()->loadState(saved);
    refresh();
    return created && moved && comparison && analyzer && accessible;
}

void EqualizerPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateAnalyzerConfig();
    refresh();
    if (m_timer) m_timer->start();
}

void EqualizerPanel::hideEvent(QHideEvent* event) {
    if (m_timer) m_timer->stop();
    if (eq::EqualizerInstance* instance = equalizerInstance()) {
        eq::AnalyzerConfig config = instance->analyzerConfig();
        config.enabled = false;
        instance->setAnalyzerConfig(config);
        instance->setAuditionBand(-1);
    }
    QWidget::hideEvent(event);
}
