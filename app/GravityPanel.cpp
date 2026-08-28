#include "GravityPanel.hpp"

#include "Controls.hpp"
#include "Icons.hpp"
#include "EngineController.hpp"

#include <QActionGroup>
#include <QComboBox>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>

namespace gravity = daw::plugins::gravity;

namespace {

constexpr QColor kWell(0x09, 0x0A, 0x0C);
constexpr QColor kMuted(0x8B, 0x8E, 0x94);
constexpr QColor kRed(0xE8, 0x10, 0x48);
constexpr QColor kMagenta(0xFF, 0x26, 0xB5);
constexpr auto kUserPresetKey = "gravity/userPresets.v1";

const daw::plugins::ParameterInfo* parameterInfo(const QString& id) {
    const std::string key = id.toStdString();
    for (const daw::plugins::ParameterInfo& info : gravity::parameterTable()) {
        if (info.id == key) return &info;
    }
    return nullptr;
}

QWidget* knobCell(ui::Knob* knob, const QString& caption, QWidget* parent) {
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->setAlignment(Qt::AlignHCenter);
    layout->addWidget(knob, 0, Qt::AlignHCenter);
    auto* label = new QLabel(caption.toUpper(), cell);
    label->setObjectName(QStringLiteral("GravityKnobCaption"));
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    return cell;
}

QWidget* comboCell(QComboBox* combo, const QString& caption, QWidget* parent) {
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(6);
    auto* label = new QLabel(caption.toUpper(), cell);
    label->setObjectName(QStringLiteral("GravityKnobCaption"));
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    layout->addWidget(combo);
    return cell;
}

QString peakText(float peak) {
    if (peak <= 0.000001f) return QString::fromUtf8("\xE2\x88\x92\xE2\x88\x9E dB");
    return QString::number(20.0 * std::log10(double(peak)), 'f', 1) +
           QStringLiteral(" dB");
}

double meterFraction(float peak) {
    if (peak <= 0.000001f) return 0.0;
    return std::clamp((20.0 * std::log10(double(peak)) + 60.0) / 60.0,
                      0.0, 1.0);
}

bool factoryNameReserved(const QString& name) {
    const QString folded = name.trimmed().toCaseFolded();
    return std::any_of(gravity::factoryPresets().begin(),
                       gravity::factoryPresets().end(),
                       [&](const gravity::FactoryPreset& preset) {
        return QString::fromUtf8(preset.name).toCaseFolded() == folded;
    });
}

double clampedPresetValue(const daw::plugins::ParameterInfo& info,
                          double value) {
    if (!std::isfinite(value)) return info.defaultValue;
    value = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(value) : value;
}

} // namespace

// ── GravityField ──

GravityField::GravityField(QWidget* parent) : QWidget(parent) {
    setMinimumSize(510, 260);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setAccessibleName(tr("Gravity pitch and size field"));
    setAccessibleDescription(
        tr("Horizontal position controls pitch; vertical position controls size."));
    setToolTip(tr("Drag to set Pitch and Size. Use arrow keys for precise changes."));
    m_particles.reserve(1200);
}

QRectF GravityField::cloudRect() const {
    return QRectF(rect()).adjusted(52.0, 44.0, -52.0, -86.0);
}

QPointF GravityField::attractorPoint() const {
    const QRectF cloud = cloudRect();
    const double x = cloud.left() + ((m_pitch + 12.0) / 24.0) * cloud.width();
    const double y = cloud.top() + (1.0 - m_size) * cloud.height();
    return QPointF(x, y);
}

void GravityField::moveAttractor(const QPointF& point) {
    const QRectF cloud = cloudRect();
    const double x = std::clamp(point.x(), cloud.left(), cloud.right());
    const double y = std::clamp(point.y(), cloud.top(), cloud.bottom());
    m_pitch = ((x - cloud.left()) / std::max(1.0, cloud.width())) * 24.0 - 12.0;
    m_size = 1.0 - (y - cloud.top()) / std::max(1.0, cloud.height());
    if (valuesChanged) valuesChanged(m_pitch, m_size);
    update();
}

float GravityField::randomUnit() {
    m_randomState ^= m_randomState >> 12;
    m_randomState ^= m_randomState << 25;
    m_randomState ^= m_randomState >> 27;
    const std::uint64_t value = m_randomState * 2685821657736338717ull;
    return float((value >> 40) * (1.0 / 16777216.0));
}

void GravityField::addParticles(std::uint64_t count) {
    const std::size_t available = 1200 - m_particles.size();
    count = std::min<std::uint64_t>(count, available);
    for (std::uint64_t i = 0; i < count; ++i) {
        Particle particle;
        particle.angle = randomUnit() * float(std::numbers::pi * 2.0);
        particle.radius = 0.07f + randomUnit() * 0.93f;
        particle.speed = (0.0018f + randomUnit() * 0.0060f) *
                         (randomUnit() < 0.16f ? -1.0f : 1.0f);
        particle.life = 0.54f + randomUnit() * 0.46f;
        particle.tint = randomUnit();
        particle.mass = ((m_lastGrainSerial + i) & 1u) ? 1.0f : -1.0f;
        m_particles.push_back(particle);
    }
}

void GravityField::setState(double gravityValue, double pitchValue,
                            double feedbackValue, double decayValue,
                            double sizeValue, double pitchSpreadValue,
                            double motionValue, double densityValue,
                            int algorithmValue,
                            const gravity::Telemetry& telemetry,
                            bool reducedMotion) {
    m_gravity = gravityValue;
    m_pitch = pitchValue;
    m_feedback = feedbackValue;
    m_decay = decayValue;
    m_size = sizeValue;
    m_pitchSpread = pitchSpreadValue;
    m_motion = motionValue;
    m_density = densityValue;
    m_algorithm = algorithmValue;
    m_telemetry = telemetry;
    m_reducedMotion = reducedMotion;
    setAccessibleDescription(m_pitchSpread > 0.01
        ? tr("Pitch %1 semitones, size %2 percent, linked attractors spread plus or minus %3 semitones.")
              .arg(m_pitch, 0, 'f', 1).arg(int(std::lround(m_size * 100.0)))
              .arg(m_pitchSpread, 0, 'f', 1)
        : tr("Pitch %1 semitones and size %2 percent.")
              .arg(m_pitch, 0, 'f', 1).arg(int(std::lround(m_size * 100.0))));

    if (telemetry.grainSerial < m_lastGrainSerial) {
        m_particles.clear();
        m_lastGrainSerial = telemetry.grainSerial;
    }
    addParticles(telemetry.grainSerial - m_lastGrainSerial);
    m_lastGrainSerial = telemetry.grainSerial;

    // Motion is entirely visual. Reduced motion keeps the cloud and meters
    // live while removing orbit/drift and particle churn.
    if (!m_reducedMotion) {
        const float energy = std::clamp(telemetry.fieldEnergy, 0.0f, 1.0f);
        const float pull = float(0.0007 + m_gravity * 0.0012);
        for (Particle& particle : m_particles) {
            particle.angle += particle.speed *
                              float(0.55 + m_gravity * 1.7 + energy * 0.8) *
                              float(1.0 + m_motion);
            particle.radius -= pull * float(0.35 + m_feedback * 0.65);
            particle.life -= float(0.00045 + (1.0 - m_feedback) * 0.0010);
            if (particle.radius < 0.035f) particle.radius = 1.0f;
        }
        std::erase_if(m_particles, [](const Particle& particle) {
            return particle.life <= 0.0f;
        });
    }
    update();
}

void GravityField::clearParticles() {
    m_particles.clear();
    m_lastGrainSerial = 0;
    update();
}

void GravityField::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !cloudRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    m_dragging = true;
    if (gestureStarted) gestureStarted();
    moveAttractor(event->position());
    event->accept();
}

void GravityField::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    moveAttractor(event->position());
    event->accept();
}

void GravityField::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    moveAttractor(event->position());
    m_dragging = false;
    if (gestureFinished) gestureFinished();
    event->accept();
}

void GravityField::keyPressEvent(QKeyEvent* event) {
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.25 : 1.0;
    double pitchValue = m_pitch;
    double sizeValue = m_size;
    switch (event->key()) {
        case Qt::Key_Left: pitchValue -= 0.5 * fine; break;
        case Qt::Key_Right: pitchValue += 0.5 * fine; break;
        case Qt::Key_Up: sizeValue += 0.025 * fine; break;
        case Qt::Key_Down: sizeValue -= 0.025 * fine; break;
        case Qt::Key_Home: pitchValue = 0.0; sizeValue = 0.5; break;
        case Qt::Key_End: pitchValue = 0.0; sizeValue = 1.0; break;
        default:
            QWidget::keyPressEvent(event);
            return;
    }
    if (gestureStarted) gestureStarted();
    m_pitch = std::clamp(pitchValue, -12.0, 12.0);
    m_size = std::clamp(sizeValue, 0.0, 1.0);
    if (valuesChanged) valuesChanged(m_pitch, m_size);
    if (gestureFinished) gestureFinished();
    event->accept();
    update();
}

void GravityField::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    QLinearGradient shell(bounds.topLeft(), bounds.bottomLeft());
    shell.setColorAt(0.0, QColor(0x0D, 0x0E, 0x10));
    shell.setColorAt(0.63, kWell);
    shell.setColorAt(1.0, QColor(0x17, 0x08, 0x10));
    painter.setPen(QPen(QColor(0x35, 0x37, 0x3B), 1.0));
    painter.setBrush(shell);
    painter.drawRoundedRect(bounds, 25.0, 25.0);

    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(bounds.adjusted(3.0, 3.0, -3.0, -3.0), 22.0, 22.0);
    painter.setClipPath(clip);
    QRadialGradient glow(QPointF(width() * 0.66, height() * 0.86),
                         std::max(width(), height()) * 0.64);
    glow.setColorAt(0.0, QColor(0xA2, 0x08, 0x45, 58));
    glow.setColorAt(0.50, QColor(0x52, 0x05, 0x24, 28));
    glow.setColorAt(1.0, QColor(0x00, 0x00, 0x00, 0));
    painter.fillRect(bounds, glow);
    painter.restore();

    const QRectF cloud = cloudRect();
    const QPointF centre = cloud.center();
    const QPointF attractor = attractorPoint();
    const double spreadPixels = (m_pitchSpread / 24.0) * cloud.width();
    const QPointF attractorA(std::clamp(attractor.x() - spreadPixels,
                                        cloud.left(), cloud.right()), attractor.y());
    const QPointF attractorB(std::clamp(attractor.x() + spreadPixels,
                                        cloud.left(), cloud.right()), attractor.y());
    const bool dual = m_pitchSpread > 0.01;
    const double radiusX = cloud.width() * (0.27 + m_size * 0.20);
    const double radiusY = cloud.height() * (0.25 + m_size * 0.18);
    const double orbit = m_reducedMotion ? 0.0 : m_telemetry.orbitPhase;
    const double energy = std::clamp(double(m_telemetry.fieldEnergy), 0.0, 1.0);

    painter.setPen(Qt::NoPen);
    for (const Particle& particle : m_particles) {
        const double angle = particle.angle + orbit * (0.22 + m_gravity * 0.45);
        const double warped = particle.radius *
                              (0.54 + 0.46 * std::sin(angle * 2.0 + particle.tint));
        const QPointF orbitPoint(
            centre.x() + std::cos(angle) * radiusX * particle.radius,
            centre.y() + std::sin(angle) * radiusY *
                             (0.65 + 0.35 * warped));
        const double attraction = 0.10 + m_gravity * 0.18;
        const QPointF target = dual
            ? (particle.mass < 0.0f ? attractorA : attractorB)
            : attractor;
        const QPointF point = orbitPoint * (1.0 - attraction) + target * attraction;
        QColor ink = particle.tint < 0.52 ? kRed : kMagenta;
        ink.setAlpha(std::clamp(int((42.0 + energy * 150.0 +
                                    m_telemetry.transientPulse * 70.0) * particle.life),
                                18, 225));
        painter.setBrush(ink);
        const double dot = 0.65 + particle.tint * 1.15 + energy * 0.55 +
                           m_density * 0.25;
        painter.drawEllipse(point, dot, dot);
    }

    // The attractor remains visible in reduced-motion mode and is the keyboard
    // focus affordance for the two-dimensional control.
    QColor attractorInk = m_telemetry.frozen ? QColor(0x9C, 0x72, 0xFF) : kMagenta;
    attractorInk.setAlpha(hasFocus() ? 245 : 185);
    painter.setBrush(QColor(0x05, 0x05, 0x08, 185));
    QFont attractorFont = painter.font();
    attractorFont.setPixelSize(8);
    attractorFont.setBold(true);
    painter.setFont(attractorFont);
    if (dual) {
        painter.setPen(QPen(QColor(attractorInk.red(), attractorInk.green(),
                                  attractorInk.blue(), 80), 1.0, Qt::DashLine));
        painter.drawLine(attractorA, attractorB);
    }
    auto drawAttractor = [&](const QPointF& point, const QString& label) {
        painter.setPen(QPen(attractorInk, hasFocus() ? 2.0 : 1.25,
                            hasFocus() ? Qt::DashLine : Qt::SolidLine));
        painter.drawEllipse(point, hasFocus() ? 19.0 : 15.0,
                            hasFocus() ? 19.0 : 15.0);
        painter.setPen(QPen(attractorInk, 1.0));
        painter.drawLine(point + QPointF(-5, 0), point + QPointF(5, 0));
        painter.drawLine(point + QPointF(0, -5), point + QPointF(0, 5));
        if (!label.isEmpty())
            painter.drawText(QRectF(point.x() - 12.0, point.y() + 21.0, 24.0, 12.0),
                             Qt::AlignCenter, label);
    };
    if (dual) {
        drawAttractor(attractorA, QStringLiteral("A"));
        drawAttractor(attractorB, QStringLiteral("B"));
    } else {
        drawAttractor(attractor, {});
    }

    QFont font = painter.font();
    font.setPixelSize(9);
    font.setBold(true);
    font.setLetterSpacing(QFont::PercentageSpacing, 112);
    painter.setFont(font);
    painter.setPen(kMuted);
    painter.drawText(QRectF(0, 15, width(), 18), Qt::AlignCenter,
                     QStringLiteral("GRAVITY · %1%").arg(int(std::lround(m_gravity * 100.0))));

    auto drawMeter = [&](double x, float left, float right, const QString& label,
                         bool alignRight) {
        const double top = 64.0;
        const double bottom = height() - 69.0;
        const double heightAvailable = std::max(20.0, bottom - top);
        const std::array<float, 2> peaks{left, right};
        painter.setPen(Qt::NoPen);
        for (int channel = 0; channel < 2; ++channel) {
            const QRectF rail(x + channel * 8.0, top, 3.0, heightAvailable);
            painter.setBrush(QColor(0x2F, 0x31, 0x35));
            painter.drawRoundedRect(rail, 1.5, 1.5);
            const double filled = heightAvailable * meterFraction(peaks[channel]);
            QLinearGradient level(0, bottom, 0, top);
            level.setColorAt(0.0, kRed);
            level.setColorAt(0.72, kMagenta);
            level.setColorAt(1.0, QColor(0xF4, 0xD7, 0xEA));
            painter.setBrush(level);
            painter.drawRoundedRect(QRectF(rail.left(), bottom - filled,
                                           rail.width(), filled), 1.5, 1.5);
        }
        font.setPixelSize(8);
        painter.setFont(font);
        painter.setPen(kMuted);
        const QRectF textRect(alignRight ? x - 78.0 : x - 2.0,
                              height() - 57.0, 90.0, 14.0);
        painter.drawText(textRect, alignRight ? Qt::AlignRight : Qt::AlignLeft,
                         label);
    };
    drawMeter(23.0, m_telemetry.inputLeft, m_telemetry.inputRight,
              QStringLiteral("INPUT  %1").arg(peakText(std::max(
                  m_telemetry.inputLeft, m_telemetry.inputRight))), false);
    drawMeter(width() - 34.0, m_telemetry.outputLeft, m_telemetry.outputRight,
              QStringLiteral("OUTPUT  %1").arg(peakText(std::max(
                  m_telemetry.outputLeft, m_telemetry.outputRight))), true);

    font.setPixelSize(8);
    painter.setFont(font);
    painter.setPen(QColor(0xA4, 0xA7, 0xAD));
    const QString status =
        QStringLiteral("%1 PITCH   %2 DECAY   %3 FEEDBACK   %4 SIZE   %5 GRAINS   DUCK %6")
            .arg(gravity::parameterText(std::uint32_t(gravity::Param::Pitch),
                                        m_pitch).c_str())
            .arg(gravity::parameterText(std::uint32_t(gravity::Param::Decay),
                                        m_decay).c_str())
            .arg(gravity::parameterText(std::uint32_t(gravity::Param::Feedback),
                                        m_feedback).c_str())
            .arg(gravity::parameterText(std::uint32_t(gravity::Param::Size),
                                        m_size).c_str())
            .arg(m_telemetry.activeGrains)
            .arg(peakText(m_telemetry.duckGain));
    painter.drawText(QRectF(72.0, height() - 58.0, width() - 144.0, 18.0),
                     Qt::AlignCenter, status);
}

// ── GravityPanel ──

GravityPanel::GravityPanel(daw::EngineController* controller, QString channelId,
                           QString insertId, QWidget* parent)
    : QWidget(parent), m_controller(controller),
      m_channelId(std::move(channelId)), m_insertId(std::move(insertId)),
      m_channelKey(m_channelId.toStdString()), m_insertKey(m_insertId.toStdString()) {
    setObjectName(QStringLiteral("GravityPanel"));
    setMinimumSize(820, 600);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("GravityRail"));
    rail->setFixedWidth(252);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(22, 18, 22, 16);
    railLayout->setSpacing(12);

    auto* brand = new QLabel(QStringLiteral("⠿  GRAVITY"), rail);
    brand->setObjectName(QStringLiteral("GravityBrand"));
    brand->setAccessibleName(tr("Gravity effect"));
    railLayout->addWidget(brand);

    auto* macro = makeKnob(QStringLiteral("gravity"), 124);
    railLayout->addWidget(knobCell(macro, tr("Intensity"), rail), 0,
                          Qt::AlignHCenter);

    auto* controls = new QGridLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setHorizontalSpacing(18);
    controls->setVerticalSpacing(14);
    auto* pitch = makeKnob(QStringLiteral("pitch"), 66);
    auto* feedback = makeKnob(QStringLiteral("feedback"), 66);
    auto* decay = makeKnob(QStringLiteral("decay"), 66);
    auto* size = makeKnob(QStringLiteral("size"), 66);
    controls->addWidget(knobCell(pitch, tr("Pitch"), rail), 0, 0);
    controls->addWidget(knobCell(feedback, tr("Feedback"), rail), 0, 1);
    controls->addWidget(knobCell(decay, tr("Decay"), rail), 1, 0);
    controls->addWidget(knobCell(size, tr("Size"), rail), 1, 1);
    railLayout->addLayout(controls);

    auto* algorithmRow = new QHBoxLayout;
    algorithmRow->setSpacing(8);
    auto* algorithmLabel = new QLabel(tr("ALGORITHM"), rail);
    algorithmLabel->setObjectName(QStringLiteral("GravitySectionLabel"));
    m_algorithm = new QComboBox(rail);
    m_algorithm->setObjectName(QStringLiteral("GravityAlgorithm"));
    m_algorithm->addItems({tr("Orbit"), tr("Fall"), tr("Rise"), tr("Void"),
                           tr("Collapse"), tr("Zero G")});
    m_algorithm->setAccessibleName(tr("Gravity algorithm"));
    m_algorithm->setFocusPolicy(Qt::StrongFocus);
    algorithmRow->addWidget(algorithmLabel);
    algorithmRow->addWidget(m_algorithm, 1);
    railLayout->addLayout(algorithmRow);
    railLayout->addStretch(1);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(8);
    m_power = new ui::IconButton(icons::Glyph::Power, tr("Gravity power"), rail);
    m_power->setAccessibleName(tr("Gravity power"));
    m_power->setCheckable(true);
    m_power->setButtonSize(42, 42);
    m_settings = new ui::IconButton(icons::Glyph::Gear,
                                    tr("Gravity advanced controls"), rail);
    m_settings->setAccessibleName(tr("Advanced controls"));
    m_settings->setCheckable(true);
    m_settings->setButtonSize(42, 42);
    m_freeze = new ui::IconButton(icons::Glyph::Waveform,
                                  tr("Freeze Gravity tail"), rail);
    m_freeze->setAccessibleName(tr("Freeze tail"));
    m_freeze->setCheckable(true);
    m_freeze->setActiveColor(QColor(0xA4, 0x79, 0xFF));
    m_freeze->setButtonSize(42, 42);
    auto* clear = new ui::IconButton(icons::Glyph::Restart,
                                     tr("Clear Gravity tail"), rail);
    clear->setAccessibleName(tr("Clear tail"));
    clear->setButtonSize(42, 42);
    bottom->addWidget(m_power);
    bottom->addWidget(m_settings);
    bottom->addWidget(m_freeze);
    bottom->addWidget(clear);
    railLayout->addLayout(bottom);
    root->addWidget(rail);

    auto* stage = new QWidget(this);
    stage->setObjectName(QStringLiteral("GravityStage"));
    auto* stageLayout = new QVBoxLayout(stage);
    stageLayout->setContentsMargins(12, 10, 14, 12);
    stageLayout->setSpacing(5);

    auto* presetRow = new QHBoxLayout;
    presetRow->setContentsMargins(82, 0, 82, 0);
    presetRow->setSpacing(7);
    auto* previous = new ui::IconButton(icons::Glyph::ArrowLeft,
                                        tr("Previous Gravity preset"), stage);
    previous->setAccessibleName(tr("Previous preset"));
    previous->setButtonSize(30, 30);
    m_presetName = new QPushButton(stage);
    m_presetName->setObjectName(QStringLiteral("GravityPresetName"));
    m_presetName->setAccessibleName(tr("Gravity preset browser"));
    m_presetName->setFocusPolicy(Qt::StrongFocus);
    auto* next = new ui::IconButton(icons::Glyph::ArrowRight,
                                    tr("Next Gravity preset"), stage);
    next->setAccessibleName(tr("Next preset"));
    next->setButtonSize(30, 30);
    auto* list = new ui::IconButton(icons::Glyph::Layers,
                                    tr("Choose Gravity preset"), stage);
    list->setAccessibleName(tr("Preset list"));
    list->setButtonSize(30, 30);
    presetRow->addWidget(previous);
    presetRow->addWidget(m_presetName, 1);
    presetRow->addWidget(next);
    presetRow->addWidget(list);
    stageLayout->addLayout(presetRow);

    m_gravityReadout = new QLabel(stage);
    m_gravityReadout->setObjectName(QStringLiteral("GravityReadout"));
    m_gravityReadout->setAlignment(Qt::AlignCenter);
    stageLayout->addWidget(m_gravityReadout);

    m_field = new GravityField(stage);
    stageLayout->addWidget(m_field, 1);

    m_drawer = new QWidget(stage);
    m_drawer->setObjectName(QStringLiteral("GravityDrawer"));
    m_drawer->setAccessibleName(tr("Gravity advanced controls"));
    m_drawer->setMinimumHeight(184);
    m_drawer->setMaximumHeight(210);
    auto* drawerLayout = new QVBoxLayout(m_drawer);
    drawerLayout->setContentsMargins(8, 6, 8, 7);
    drawerLayout->setSpacing(0);
    m_drawerTabs = new QTabWidget(m_drawer);
    m_drawerTabs->setObjectName(QStringLiteral("GravityDrawerTabs"));
    m_drawerTabs->setAccessibleName(tr("Gravity advanced sections"));
    drawerLayout->addWidget(m_drawerTabs);

    auto knobTab = [&](const std::initializer_list<std::pair<QString, QString>>& items) {
        auto* tab = new QWidget(m_drawerTabs);
        auto* grid = new QGridLayout(tab);
        grid->setContentsMargins(8, 7, 8, 4);
        grid->setHorizontalSpacing(13);
        grid->setVerticalSpacing(5);
        int index = 0;
        for (const auto& [id, label] : items) {
            grid->addWidget(knobCell(makeKnob(id, 48), label, tab),
                            index / 4, index % 4);
            ++index;
        }
        for (int column = 0; column < 4; ++column) grid->setColumnStretch(column, 1);
        return tab;
    };

    m_drawerTabs->addTab(knobTab({
        {QStringLiteral("density"), tr("Density")},
        {QStringLiteral("motion"), tr("Motion")},
        {QStringLiteral("reverse"), tr("Reverse")},
        {QStringLiteral("transient"), tr("Transient")}}), tr("CLOUD"));
    m_drawerTabs->addTab(knobTab({
        {QStringLiteral("diffusion"), tr("Diffusion")},
        {QStringLiteral("stereo.width"), tr("Width")},
        {QStringLiteral("stereo.input"), tr("Source Stereo")},
        {QStringLiteral("ducking"), tr("Ducking")}}), tr("SPACE"));

    auto* character = knobTab({
        {QStringLiteral("mass"), tr("Mass")},
        {QStringLiteral("damping"), tr("Damping")},
        {QStringLiteral("drive"), tr("Drive")},
        {QStringLiteral("feedback.lowcut"), tr("Low Cut")},
        {QStringLiteral("pitch.spread"), tr("Pitch Spread")}});
    auto* characterGrid = qobject_cast<QGridLayout*>(character->layout());
    m_pitchSnap = new QComboBox(character);
    m_pitchSnap->setAccessibleName(tr("Pitch snap"));
    m_pitchSnap->addItem(tr("Off"), 0);
    m_pitchSnap->addItem(tr("Chromatic"), 1);
    m_pitchSnap->addItem(tr("Perfect"), 2);
    m_pitchSnap->addItem(tr("Octave"), 3);
    m_detectorSource = new QComboBox(character);
    m_detectorSource->setAccessibleName(tr("Detector source"));
    m_detectorSource->addItem(tr("Main"), 0);
    m_detectorSource->addItem(tr("Sidechain"), 1);
    m_detectorSource->addItem(tr("Auto"), 2);
    characterGrid->addWidget(comboCell(m_pitchSnap, tr("Pitch Snap"), character), 1, 1);
    characterGrid->addWidget(comboCell(m_detectorSource, tr("Detector"), character), 1, 2);
    m_drawerTabs->addTab(character, tr("CHARACTER"));

    auto* timing = new QWidget(m_drawerTabs);
    auto* timingGrid = new QGridLayout(timing);
    timingGrid->setContentsMargins(34, 12, 34, 8);
    timingGrid->setHorizontalSpacing(22);
    m_timingMode = new QComboBox(timing);
    m_timingMode->setAccessibleName(tr("Gravity timing mode"));
    m_timingMode->addItem(tr("Sync to tempo"), 1);
    m_timingMode->addItem(tr("Free delay"), 0);
    m_timingDivision = new QComboBox(timing);
    m_timingDivision->setAccessibleName(tr("Gravity sync division"));
    const std::array<std::pair<const char*, int>, 11> divisions{{
        {"1/32", 5}, {"1/16T", 6}, {"1/16", 0}, {"1/8T", 1},
        {"1/8", 2}, {"1/8D", 3}, {"1/4T", 7}, {"1/4", 4},
        {"1/4D", 8}, {"1/2", 9}, {"1/1", 10}}};
    for (const auto& [name, value] : divisions)
        m_timingDivision->addItem(QString::fromLatin1(name), value);
    timingGrid->addWidget(comboCell(m_timingMode, tr("Mode"), timing), 0, 0);
    timingGrid->addWidget(comboCell(m_timingDivision, tr("Division"), timing), 0, 1);
    timingGrid->addWidget(knobCell(makeKnob(QStringLiteral("timing.ms"), 52),
                                   tr("Free Delay"), timing), 0, 2);
    timingGrid->setColumnStretch(0, 1);
    timingGrid->setColumnStretch(1, 1);
    timingGrid->setColumnStretch(2, 1);
    m_drawerTabs->addTab(timing, tr("TIMING"));
    m_drawer->hide();
    stageLayout->addWidget(m_drawer);
    root->addWidget(stage, 1);

    setStyleSheet(QStringLiteral(R"(
#GravityPanel { background: #101113; }
#GravityRail { background: #222325; border-right: 1px solid #34363A; }
#GravityStage { background: #101113; }
#GravityBrand { color: #B3B5BA; font-size: 17px; font-weight: 600; letter-spacing: 1px; }
#GravityKnobCaption, #GravitySectionLabel, #GravityReadout {
    color: #9A9DA3; font-size: 9px; font-weight: 600; letter-spacing: 1px;
}
#GravityReadout { color: #AEB0B5; padding-bottom: 1px; }
#GravityAlgorithm, #GravityPresetName, #GravityDrawer QComboBox {
    color: #D1D3D7; background: #121315; border: 1px solid #44474C;
    border-radius: 7px; min-height: 28px; padding: 0 9px;
    font-size: 10px; font-weight: 600;
}
#GravityPresetName { letter-spacing: 1px; }
#GravityAlgorithm:hover, #GravityPresetName:hover, #GravityDrawer QComboBox:hover { border-color: #8A2356; }
#GravityAlgorithm:focus, #GravityPresetName:focus, #GravityDrawer QComboBox:focus { border: 2px solid #FF26B5; }
#GravityAlgorithm QAbstractItemView {
    color: #D1D3D7; background: #17181A; border: 1px solid #55585E;
    selection-background-color: #76123C; outline: none;
}
QMenu { color: #D1D3D7; background: #202124; border: 1px solid #4B4D52; }
QMenu::item { min-height: 26px; padding: 3px 24px 3px 10px; }
QMenu::item:selected { background: #76123C; }
QSpinBox { color: #D1D3D7; background: #111214; border: 1px solid #55585E;
           border-radius: 4px; padding: 3px; }
#GravityDrawer { background: #17181B; border: 1px solid #34363A; border-radius: 10px; }
#GravityDrawerTabs::pane { border: none; background: transparent; }
#GravityDrawerTabs QTabBar::tab {
    color: #9A9DA3; background: transparent; padding: 6px 13px;
    border-bottom: 2px solid transparent; font-size: 9px; font-weight: 600;
}
#GravityDrawerTabs QTabBar::tab:selected { color: #F1DDE9; border-bottom-color: #FF26B5; }
#GravityDrawerTabs QTabBar::tab:focus { outline: 1px solid #FF26B5; }
)"));

    connect(m_algorithm, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (m_refreshing) return;
                const QString id = QStringLiteral("algorithm");
                beginGesture(id);
                writeParameter(id, index);
                endGesture(id, "Change Gravity Algorithm");
            });
    connect(previous, &QAbstractButton::clicked, this, [this] {
        const int count = int(gravity::factoryPresets().size());
        applyPreset((m_selectedPreset - 1 + count) % count);
    });
    connect(next, &QAbstractButton::clicked, this, [this] {
        applyPreset((m_selectedPreset + 1) % int(gravity::factoryPresets().size()));
    });
    connect(m_presetName, &QPushButton::clicked, this,
            &GravityPanel::showPresetMenu);
    connect(list, &QAbstractButton::clicked, this, &GravityPanel::showPresetMenu);
    connect(m_settings, &QAbstractButton::toggled, this,
            &GravityPanel::setDrawerOpen);

    auto connectStepped = [this](QComboBox* combo, const QString& id,
                                 const char* label) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, combo, id, label](int index) {
            if (m_refreshing || index < 0) return;
            beginGesture(id);
            writeParameter(id, combo->itemData(index).toDouble());
            endGesture(id, label);
        });
    };
    connectStepped(m_pitchSnap, QStringLiteral("pitch.snap"),
                   "Change Gravity Pitch Snap");
    connectStepped(m_detectorSource, QStringLiteral("detector.source"),
                   "Change Gravity Detector");
    connectStepped(m_timingMode, QStringLiteral("timing.sync"),
                   "Set Gravity Timing");
    connectStepped(m_timingDivision, QStringLiteral("timing.division"),
                   "Set Gravity Division");
    auto* closeDrawer = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    closeDrawer->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeDrawer, &QShortcut::activated, this, [this] {
        if (m_settings && m_settings->isChecked()) m_settings->setChecked(false);
    });
    connect(m_power, &QAbstractButton::toggled, this, [this](bool enabled) {
        if (m_refreshing || !m_controller) return;
        m_controller->setInsertBypassed(m_channelKey, m_insertKey, !enabled);
        emit projectEdited();
    });
    connect(m_freeze, &QAbstractButton::toggled, this, [this](bool frozen) {
        if (m_refreshing) return;
        if (gravity::GravityInstance* instance = gravityInstance())
            instance->setFrozen(frozen);
    });
    connect(clear, &QAbstractButton::clicked, this, [this] {
        if (gravity::GravityInstance* instance = gravityInstance())
            instance->clearTail();
        m_field->clearParticles();
    });

    m_field->gestureStarted = [this] { beginXyGesture(); };
    m_field->valuesChanged = [this](double pitchValue, double sizeValue) {
        writeParameter(QStringLiteral("pitch"), pitchValue);
        writeParameter(QStringLiteral("size"), sizeValue);
        if (ui::Knob* knob = m_knobs.value(QStringLiteral("pitch")))
            knob->setValue(pitchValue);
        if (ui::Knob* knob = m_knobs.value(QStringLiteral("size")))
            knob->setValue(sizeValue);
    };
    m_field->gestureFinished = [this] { endXyGesture(); };

    QWidget::setTabOrder(macro, pitch);
    QWidget::setTabOrder(pitch, feedback);
    QWidget::setTabOrder(feedback, decay);
    QWidget::setTabOrder(decay, size);
    QWidget::setTabOrder(size, m_algorithm);
    QWidget::setTabOrder(m_algorithm, m_field);
    QWidget::setTabOrder(m_field, m_presetName);
    QWidget::setTabOrder(m_presetName, m_power);
    QWidget::setTabOrder(m_power, m_settings);
    QWidget::setTabOrder(m_settings, m_drawerTabs);
    QWidget::setTabOrder(m_drawerTabs, m_freeze);
    QWidget::setTabOrder(m_freeze, clear);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &GravityPanel::refresh);
    reloadUserPresets();
    if (qEnvironmentVariableIsSet("DAW_SHOT_GRAVITY_DRAWER"))
        m_settings->setChecked(true);
    refresh();
}

gravity::GravityInstance* GravityPanel::gravityInstance() const {
    if (!m_controller) return nullptr;
    return dynamic_cast<gravity::GravityInstance*>(
        m_controller->insertInstance(m_channelKey, m_insertKey));
}

ui::Knob* GravityPanel::makeKnob(const QString& parameterId, int diameter) {
    auto* control = new ui::Knob({}, this);
    if (const daw::plugins::ParameterInfo* info = parameterInfo(parameterId)) {
        control->setRange(info->minValue, info->maxValue);
        control->setDefaultValue(info->defaultValue);
        control->setStepped(info->isStepped);
        control->setBipolar(info->minValue < 0.0 && info->maxValue > 0.0);
        const std::uint32_t index = info->index;
        control->setFormatter([index](double value) {
            return QString::fromStdString(gravity::parameterText(index, value));
        });
        control->setAccessibleName(QString::fromStdString(info->name));
        control->setToolTip(QString::fromStdString(info->name));
    }
    control->setBare(diameter);
    control->setVisualStyle(ui::Knob::VisualStyle::Gravity);
    control->setAutomatable(true);
    control->setValue(readParameter(parameterId));
    connect(control, &ui::Knob::valueChanged, this,
            [this, parameterId](double value) {
                beginGesture(parameterId);
                writeParameter(parameterId, value);
            });
    connect(control, &ui::Knob::editFinished, this,
            [this, parameterId] { endGesture(parameterId); });
    connect(control, &ui::Knob::automateRequested, this,
            [this, parameterId] { emit automationRequested(parameterId); });
    m_knobs.insert(parameterId, control);
    return control;
}

double GravityPanel::readParameter(const QString& parameterId) const {
    if (!m_controller) return 0.0;
    return m_controller->insertParameter(m_channelKey, m_insertKey,
                                         parameterId.toStdString());
}

void GravityPanel::writeParameter(const QString& parameterId, double value) {
    if (!m_controller) return;
    m_controller->setInsertParameter(m_channelKey, m_insertKey,
                                     parameterId.toStdString(), value);
}

void GravityPanel::beginGesture(const QString& parameterId) {
    if (!m_gestureStart.contains(parameterId))
        m_gestureStart.insert(parameterId, readParameter(parameterId));
}

void GravityPanel::endGesture(const QString& parameterId, const char* label) {
    if (!m_controller || !m_gestureStart.contains(parameterId)) return;
    m_controller->commitInsertParameterEdit(
        m_channelKey, m_insertKey, parameterId.toStdString(),
        m_gestureStart.take(parameterId), label);
    emit projectEdited();
    refresh();
}

void GravityPanel::beginXyGesture() {
    if (m_xyStart) return;
    m_xyStart = std::pair(readParameter(QStringLiteral("pitch")),
                          readParameter(QStringLiteral("size")));
}

void GravityPanel::endXyGesture() {
    if (!m_xyStart || !m_controller) return;
    const auto undo = m_controller->beginUndoGroup();
    m_controller->commitInsertParameterEdit(
        m_channelKey, m_insertKey, "pitch", m_xyStart->first,
        "Move Gravity Attractor");
    m_controller->commitInsertParameterEdit(
        m_channelKey, m_insertKey, "size", m_xyStart->second,
        "Move Gravity Attractor");
    m_controller->collapseUndo(undo, "Move Gravity Attractor");
    m_xyStart.reset();
    emit projectEdited();
    refresh();
}

void GravityPanel::applyValues(const PresetValues& values, const QString& kind,
                               const QString& name, const char* undoLabel) {
    if (!m_controller) return;
    const auto undo = m_controller->beginUndoGroup();
    const auto parameters = gravity::parameterTable();
    for (std::uint32_t i = 0; i < gravity::kParameterCount; ++i) {
        const QString id = QString::fromStdString(parameters[i].id);
        const double before = readParameter(id);
        writeParameter(id, values[i]);
        m_controller->commitInsertParameterEdit(
            m_channelKey, m_insertKey, parameters[i].id, before,
            undoLabel);
    }
    m_controller->collapseUndo(undo, undoLabel);
    if (gravity::GravityInstance* instance = gravityInstance())
        instance->setPresetReference(kind.toStdString(), name.toStdString());
    m_selectedKind = kind;
    m_selectedName = name;
    emit projectEdited();
    refresh();
}

void GravityPanel::applyPreset(int index) {
    const auto presets = gravity::factoryPresets();
    if (!m_controller || index < 0 || index >= int(presets.size())) return;
    const gravity::FactoryPreset& preset = presets[std::size_t(index)];
    applyValues(preset.values, QStringLiteral("factory"),
                QString::fromUtf8(preset.name), "Apply Gravity Preset");
    if (gravity::GravityInstance* instance = gravityInstance())
        instance->setLastPreset(index);
    m_selectedPreset = index;
}

void GravityPanel::applyUserPreset(const QString& name) {
    const QString folded = name.toCaseFolded();
    const auto found = std::find_if(m_userPresets.begin(), m_userPresets.end(),
                                    [&](const UserPreset& preset) {
        return preset.name.toCaseFolded() == folded;
    });
    if (found == m_userPresets.end()) return;
    applyValues(found->values, QStringLiteral("user"), found->name,
                "Apply Gravity User Preset");
}

void GravityPanel::reloadUserPresets() {
    m_userPresets.clear();
    const QByteArray raw = QSettings().value(QLatin1String(kUserPresetKey)).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    if (!document.isObject()) return;
    const QJsonArray stored = document.object().value(QStringLiteral("presets")).toArray();
    const auto parameters = gravity::parameterTable();
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
        for (std::uint32_t i = 0; i < gravity::kParameterCount; ++i)
            preset.values[i] = parameters[i].defaultValue;
        const QJsonObject values = object.value(QStringLiteral("params")).toObject();
        for (const daw::plugins::ParameterInfo& info : parameters) {
            const QJsonValue value = values.value(QString::fromStdString(info.id));
            if (value.isDouble())
                preset.values[info.index] = clampedPresetValue(info, value.toDouble());
        }
        m_userPresets.push_back(std::move(preset));
    }
}

void GravityPanel::storeUserPresets() const {
    QJsonArray presets;
    const auto parameters = gravity::parameterTable();
    for (const UserPreset& preset : m_userPresets) {
        QJsonObject values;
        for (std::uint32_t i = 0; i < gravity::kParameterCount; ++i)
            values.insert(QString::fromStdString(parameters[i].id), preset.values[i]);
        presets.append(QJsonObject{{QStringLiteral("name"), preset.name},
                                   {QStringLiteral("params"), values}});
    }
    const QJsonDocument document(QJsonObject{{QStringLiteral("version"), 1},
                                             {QStringLiteral("presets"), presets}});
    QSettings().setValue(QLatin1String(kUserPresetKey),
                         document.toJson(QJsonDocument::Compact));
}

void GravityPanel::saveCurrentUserPreset() {
    if (!m_controller) return;
    bool accepted = false;
    QString name = QInputDialog::getText(this, tr("Save Gravity Preset"),
                                         tr("Preset name:"), QLineEdit::Normal,
                                         {}, &accepted).trimmed();
    if (!accepted) return;
    if (name.isEmpty() || name.size() > 48 || factoryNameReserved(name)) {
        QMessageBox::warning(this, tr("Invalid preset name"),
                             tr("Use 1–48 characters and a name not reserved by a factory preset."));
        return;
    }
    auto found = std::find_if(m_userPresets.begin(), m_userPresets.end(),
                              [&](const UserPreset& preset) {
        return preset.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (found != m_userPresets.end()) {
        if (QMessageBox::question(this, tr("Replace Gravity Preset"),
                                  tr("Replace “%1”?").arg(found->name),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) return;
    } else {
        if (m_userPresets.size() >= 128) {
            QMessageBox::warning(this, tr("Preset library full"),
                                 tr("Gravity supports up to 128 user presets."));
            return;
        }
        m_userPresets.push_back(UserPreset{});
        found = std::prev(m_userPresets.end());
    }
    found->name = name;
    for (const daw::plugins::ParameterInfo& info : gravity::parameterTable())
        found->values[info.index] = readParameter(QString::fromStdString(info.id));
    storeUserPresets();
    m_selectedKind = QStringLiteral("user");
    m_selectedName = name;
    if (gravity::GravityInstance* instance = gravityInstance())
        instance->setPresetReference("user", name.toStdString());
    refresh();
}

void GravityPanel::renameCurrentUserPreset() {
    const auto current = std::find_if(m_userPresets.begin(), m_userPresets.end(),
                                      [&](const UserPreset& preset) {
        return preset.name.compare(m_selectedName, Qt::CaseInsensitive) == 0;
    });
    if (current == m_userPresets.end()) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename Gravity Preset"),
                                               tr("Preset name:"), QLineEdit::Normal,
                                               current->name, &accepted).trimmed();
    if (!accepted || name == current->name) return;
    const bool duplicate = std::any_of(m_userPresets.begin(), m_userPresets.end(),
                                       [&](const UserPreset& preset) {
        return &preset != &*current &&
               preset.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (name.isEmpty() || name.size() > 48 || factoryNameReserved(name) || duplicate) {
        QMessageBox::warning(this, tr("Invalid preset name"),
                             tr("Choose a unique name of 1–48 characters."));
        return;
    }
    current->name = name;
    m_selectedName = name;
    storeUserPresets();
    if (gravity::GravityInstance* instance = gravityInstance())
        instance->setPresetReference("user", name.toStdString());
    refresh();
}

void GravityPanel::deleteCurrentUserPreset() {
    const auto current = std::find_if(m_userPresets.begin(), m_userPresets.end(),
                                      [&](const UserPreset& preset) {
        return preset.name.compare(m_selectedName, Qt::CaseInsensitive) == 0;
    });
    if (current == m_userPresets.end()) return;
    if (QMessageBox::question(this, tr("Delete Gravity Preset"),
                              tr("Delete “%1”? The current sound will remain unchanged.")
                                  .arg(current->name),
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) return;
    const QString oldName = current->name;
    m_userPresets.erase(current);
    storeUserPresets();
    m_selectedKind = QStringLiteral("custom");
    m_selectedName = oldName;
    if (gravity::GravityInstance* instance = gravityInstance())
        instance->setPresetReference("custom", oldName.toStdString());
    refresh();
}

void GravityPanel::showPresetMenu() {
    QMenu menu(this);
    menu.addSection(tr("Factory Presets"));
    const auto factories = gravity::factoryPresets();
    const int exactFactory = matchingPreset();
    for (int index = 0; index < int(factories.size()); ++index) {
        QAction* action = menu.addAction(QString::fromUtf8(factories[std::size_t(index)].name));
        action->setCheckable(true);
        action->setChecked(index == exactFactory);
        connect(action, &QAction::triggered, this, [this, index] { applyPreset(index); });
    }
    if (!m_userPresets.empty()) {
        menu.addSection(tr("User Presets"));
        const QString exactUser = matchingUserPreset();
        for (const UserPreset& preset : m_userPresets) {
            QAction* action = menu.addAction(preset.name);
            action->setCheckable(true);
            action->setChecked(preset.name == exactUser);
            connect(action, &QAction::triggered, this,
                    [this, name = preset.name] { applyUserPreset(name); });
        }
    }
    menu.addSeparator();
    connect(menu.addAction(tr("Save Current as User Preset…")), &QAction::triggered,
            this, &GravityPanel::saveCurrentUserPreset);
    QAction* rename = menu.addAction(tr("Rename User Preset…"));
    QAction* remove = menu.addAction(tr("Delete User Preset…"));
    const bool userSelected = m_selectedKind == QStringLiteral("user") &&
        std::any_of(m_userPresets.begin(), m_userPresets.end(), [&](const UserPreset& preset) {
            return preset.name.compare(m_selectedName, Qt::CaseInsensitive) == 0;
        });
    rename->setEnabled(userSelected);
    remove->setEnabled(userSelected);
    connect(rename, &QAction::triggered, this, &GravityPanel::renameCurrentUserPreset);
    connect(remove, &QAction::triggered, this, &GravityPanel::deleteCurrentUserPreset);
    menu.exec(m_presetName->mapToGlobal(QPoint(0, m_presetName->height() + 3)));
}

void GravityPanel::setDrawerOpen(bool open) {
    if (!m_drawer || !m_settings) return;
    m_drawer->setVisible(open);
    m_settings->setAccessibleDescription(open
        ? tr("Advanced controls expanded. Press Escape to close.")
        : tr("Advanced controls collapsed."));
    m_settings->setToolTip(open ? tr("Hide Gravity advanced controls")
                                : tr("Show Gravity advanced controls"));
}

int GravityPanel::matchingPreset() const {
    const auto presets = gravity::factoryPresets();
    const auto parameters = gravity::parameterTable();
    for (int presetIndex = 0; presetIndex < int(presets.size()); ++presetIndex) {
        bool matches = true;
        for (std::uint32_t i = 0; i < gravity::kParameterCount; ++i) {
            const double actual = readParameter(
                QString::fromStdString(parameters[i].id));
            if (std::abs(actual - presets[std::size_t(presetIndex)].values[i]) >
                1.0e-6) {
                matches = false;
                break;
            }
        }
        if (matches) return presetIndex;
    }
    return -1;
}

QString GravityPanel::matchingUserPreset() const {
    const auto parameters = gravity::parameterTable();
    for (const UserPreset& preset : m_userPresets) {
        bool matches = true;
        for (std::uint32_t i = 0; i < gravity::kParameterCount; ++i) {
            const double actual = readParameter(QString::fromStdString(parameters[i].id));
            if (std::abs(actual - preset.values[i]) > 1.0e-6) {
                matches = false;
                break;
            }
        }
        if (matches) return preset.name;
    }
    return {};
}

void GravityPanel::refresh() {
    if (!m_controller) return;
    m_refreshing = true;
    for (auto it = m_knobs.begin(); it != m_knobs.end(); ++it) {
        if (!it.value()->isEditing() && !m_gestureStart.contains(it.key()))
            it.value()->setValue(readParameter(it.key()));
    }
    const int algorithm = std::clamp(
        int(std::lround(readParameter(QStringLiteral("algorithm")))), 0, 5);
    if (m_algorithm->currentIndex() != algorithm) {
        const QSignalBlocker blocker(m_algorithm);
        m_algorithm->setCurrentIndex(algorithm);
    }

    auto syncCombo = [&](QComboBox* combo, const QString& id) {
        if (!combo) return;
        const int wanted = combo->findData(int(std::lround(readParameter(id))));
        if (wanted >= 0 && combo->currentIndex() != wanted) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(wanted);
        }
    };
    syncCombo(m_pitchSnap, QStringLiteral("pitch.snap"));
    syncCombo(m_detectorSource, QStringLiteral("detector.source"));
    syncCombo(m_timingMode, QStringLiteral("timing.sync"));
    syncCombo(m_timingDivision, QStringLiteral("timing.division"));
    const bool syncEnabled = readParameter(QStringLiteral("timing.sync")) >= 0.5;
    if (m_timingDivision) m_timingDivision->setEnabled(syncEnabled);
    if (ui::Knob* delay = m_knobs.value(QStringLiteral("timing.ms")))
        delay->setEnabled(!syncEnabled);

    if (const daw::InsertModel* model =
            m_controller->insertModel(m_channelKey, m_insertKey))
        m_power->setChecked(!model->bypassed);

    gravity::Telemetry telemetry;
    if (gravity::GravityInstance* instance = gravityInstance()) {
        telemetry = instance->consumeTelemetry();
        m_freeze->setChecked(instance->frozen());
        m_selectedPreset = std::clamp(instance->lastPreset(), 0,
            int(gravity::factoryPresets().size()) - 1);
        const auto [kind, name] = instance->presetReference();
        m_selectedKind = QString::fromStdString(kind);
        m_selectedName = QString::fromStdString(name);
    }

    const int exactFactory = matchingPreset();
    const QString exactUser = exactFactory < 0 ? matchingUserPreset() : QString{};
    QString preset;
    bool exact = false;
    if (exactFactory >= 0) {
        m_selectedPreset = exactFactory;
        m_selectedKind = QStringLiteral("factory");
        preset = QString::fromUtf8(
            gravity::factoryPresets()[std::size_t(exactFactory)].name);
        m_selectedName = preset;
        exact = true;
    } else if (!exactUser.isEmpty()) {
        m_selectedKind = QStringLiteral("user");
        m_selectedName = exactUser;
        preset = exactUser;
        exact = true;
    } else {
        preset = m_selectedName.isEmpty() ? tr("CUSTOM") : m_selectedName;
    }
    m_presetName->setText(exact ? preset : preset + QStringLiteral("*"));

    const double gravityValue = readParameter(QStringLiteral("gravity"));
    const double pitchValue = readParameter(QStringLiteral("pitch"));
    const double feedbackValue = readParameter(QStringLiteral("feedback"));
    const double decayValue = readParameter(QStringLiteral("decay"));
    const double sizeValue = readParameter(QStringLiteral("size"));
    const double spreadValue = readParameter(QStringLiteral("pitch.spread"));
    const double motionValue = readParameter(QStringLiteral("motion"));
    const double densityValue = readParameter(QStringLiteral("density"));
    m_gravityReadout->setText(
        QStringLiteral("INTENSITY · %1%   /   %2")
            .arg(int(std::lround(gravityValue * 100.0)))
            .arg(QString::fromStdString(gravity::parameterText(
                std::uint32_t(gravity::Param::Algorithm), algorithm)).toUpper()));
    const bool reducedMotion = QSettings().value(
        QStringLiteral("ui/reduceMotion"), false).toBool();
    m_field->setState(gravityValue, pitchValue, feedbackValue, decayValue,
                      sizeValue, spreadValue, motionValue, densityValue,
                      algorithm, telemetry, reducedMotion);
    m_refreshing = false;
}

bool GravityPanel::checkForTest() {
    if (!m_controller || !gravityInstance()) return false;
    const auto oneUndoAdvanced = [this](std::size_t before) {
        const std::size_t after = m_controller->undoDepth();
        return before < m_controller->undoLimit() ? after == before + 1
                                                   : after == before;
    };

    const std::size_t presetDepth = m_controller->undoDepth();
    applyPreset(1);
    const bool presetGrouped = oneUndoAdvanced(presetDepth) && matchingPreset() == 1;

    const QString pitchId = QStringLiteral("pitch");
    beginGesture(pitchId);
    writeParameter(pitchId, readParameter(pitchId) + 0.75);
    endGesture(pitchId);
    const bool presetDirty = m_presetName->text().endsWith(QLatin1Char('*'));

    const std::size_t xyDepth = m_controller->undoDepth();
    beginXyGesture();
    writeParameter(pitchId, readParameter(pitchId) + 0.5);
    writeParameter(QStringLiteral("size"),
                   std::max(0.0, readParameter(QStringLiteral("size")) - 0.1));
    endXyGesture();
    const bool xyGrouped = oneUndoAdvanced(xyDepth);

    m_controller->setInsertBypassed(m_channelKey, m_insertKey, true);
    refresh();
    const bool powerOff = !m_power->isChecked();
    m_controller->setInsertBypassed(m_channelKey, m_insertKey, false);
    refresh();
    const bool powerOn = m_power->isChecked();

    m_settings->setChecked(true);
    const bool drawerExpanded = !m_drawer->isHidden() && m_settings->isChecked();
    m_settings->setChecked(false);
    const bool drawerCollapsed = m_drawer->isHidden();

    QSettings settings;
    const bool presetLibraryExisted = settings.contains(
        QString::fromLatin1(kUserPresetKey));
    const QVariant savedPresetLibrary = settings.value(
        QString::fromLatin1(kUserPresetKey));
    const QString testName = QStringLiteral("__GRAVITY_SELFTEST__");
    std::erase_if(m_userPresets, [&](const UserPreset& preset) {
        return preset.name.compare(testName, Qt::CaseInsensitive) == 0;
    });
    UserPreset testPreset;
    testPreset.name = testName;
    for (const daw::plugins::ParameterInfo& info : gravity::parameterTable())
        testPreset.values[info.index] = readParameter(QString::fromStdString(info.id));
    testPreset.values[std::uint32_t(gravity::Param::Mass)] = 0.73;
    m_userPresets.push_back(testPreset);
    storeUserPresets();
    reloadUserPresets();
    const bool userSaved = std::any_of(m_userPresets.begin(), m_userPresets.end(),
                                       [&](const UserPreset& preset) {
        return preset.name == testName;
    });
    const std::size_t userDepth = m_controller->undoDepth();
    applyUserPreset(testName);
    const bool userGrouped = oneUndoAdvanced(userDepth) &&
                             matchingUserPreset() == testName;
    const QString renamed = testName + QStringLiteral("_RENAMED");
    for (UserPreset& preset : m_userPresets)
        if (preset.name == testName) preset.name = renamed;
    storeUserPresets();
    reloadUserPresets();
    const bool userRenamed = std::any_of(m_userPresets.begin(), m_userPresets.end(),
                                         [&](const UserPreset& preset) {
        return preset.name == renamed;
    });
    std::erase_if(m_userPresets, [&](const UserPreset& preset) {
        return preset.name == renamed;
    });
    storeUserPresets();
    reloadUserPresets();
    const bool userDeleted = std::none_of(m_userPresets.begin(), m_userPresets.end(),
                                          [&](const UserPreset& preset) {
        return preset.name == renamed;
    });
    if (presetLibraryExisted)
        settings.setValue(QString::fromLatin1(kUserPresetKey), savedPresetLibrary);
    else
        settings.remove(QString::fromLatin1(kUserPresetKey));
    settings.sync();
    reloadUserPresets();

    bool knobsAccessible = true;
    for (ui::Knob* knob : std::as_const(m_knobs)) {
        knobsAccessible = knobsAccessible &&
            knob->focusPolicy() == Qt::StrongFocus &&
            !knob->accessibleName().isEmpty();
    }
    const bool keyboardAccessible = knobsAccessible &&
        m_field->focusPolicy() == Qt::StrongFocus &&
        m_algorithm->focusPolicy() == Qt::StrongFocus &&
        m_presetName->focusPolicy() == Qt::StrongFocus &&
        m_pitchSnap->focusPolicy() != Qt::NoFocus &&
        m_detectorSource->focusPolicy() != Qt::NoFocus;
    const bool passed = presetGrouped && presetDirty && xyGrouped && powerOff && powerOn &&
           drawerExpanded && drawerCollapsed && userSaved && userGrouped &&
           userRenamed && userDeleted && keyboardAccessible;
    if (!passed) {
        std::fprintf(stderr,
            "Gravity UI selftest: presetGrouped=%d presetDirty=%d xyGrouped=%d "
            "powerOff=%d powerOn=%d drawerExpanded=%d drawerCollapsed=%d "
            "userSaved=%d userGrouped=%d userRenamed=%d userDeleted=%d "
            "keyboardAccessible=%d\n",
            presetGrouped, presetDirty, xyGrouped, powerOff, powerOn,
            drawerExpanded, drawerCollapsed, userSaved, userGrouped,
            userRenamed, userDeleted, keyboardAccessible);
    }
    return passed;
}

void GravityPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    if (m_timer) m_timer->start();
}

void GravityPanel::hideEvent(QHideEvent* event) {
    if (m_timer) m_timer->stop();
    QWidget::hideEvent(event);
}
