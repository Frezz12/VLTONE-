#include "ModulationPanel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#elif defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#endif

namespace mod = daw::plugins::modulation;
namespace {
const Theme &theme() {
    return ThemeManager::instance().theme();
}
QColor alpha(QColor c, int a) {
    c.setAlpha(a);
    return c;
}
bool systemReducedMotion() {
#ifdef Q_OS_MACOS
    CFPropertyListRef value =
        CFPreferencesCopyAppValue(CFSTR("reduceMotion"), CFSTR("com.apple.universalaccess"));
    const bool reduced = value && CFGetTypeID(value) == CFBooleanGetTypeID() &&
                         CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    if (value)
        CFRelease(value);
    return reduced;
#elif defined(Q_OS_WIN)
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled;
#else
    return false;
#endif
}
QString caption(const std::string &id) {
    if (id == "width")
        return ModulationPanel::tr("Width");
    if (id == "humanize")
        return ModulationPanel::tr("Humanize");
    if (id == "softness")
        return ModulationPanel::tr("Softness");
    if (id == "rate")
        return ModulationPanel::tr("Rate");
    if (id == "depth")
        return ModulationPanel::tr("Depth");
    if (id == "delay") return ModulationPanel::tr("Delay");
    if (id == "detune") return ModulationPanel::tr("Detune");
    if (id == "body") return ModulationPanel::tr("Body");
    return ModulationPanel::tr("Amount");
}
double displayScale(const std::string &id) { return id == "rate" || id == "detune" ? 1 : 100; }
QString help(const std::string &id) {
    if (id == "width") return ModulationPanel::tr("Spread the doubles around the original vocal. Zero keeps the widening off.");
    if (id == "humanize") return ModulationPanel::tr("Add natural variations in the doubles' timing and pitch.");
    if (id == "softness") return ModulationPanel::tr("Soften the added voices and tame bright consonants.");
    if (id == "delay") return ModulationPanel::tr("Blend a short echo synced to 1/32 note of the project tempo, up to 375 ms. Zero removes the echo.");
    if (id == "detune") return ModulationPanel::tr("Add opposite pitch offsets to the doubles, up to 16 cents each side. Zero removes this layer.");
    if (id == "body") return ModulationPanel::tr("Add a soft double in the centre for more vocal density, including in mono.");
    if (id == "rate") return ModulationPanel::tr("Set how quickly the effect moves.");
    if (id == "depth") return ModulationPanel::tr("Set the range of pitch or filter movement.");
    return ModulationPanel::tr("Blend the effect with the original signal.");
}
} // namespace

ModulationField::ModulationField(mod::Kind kind, QWidget *parent)
    : FrameWidget(parent), m_kind(kind) {
    setObjectName(QStringLiteral("ModulationField"));
    setAccessibleName(ModulationPanel::tr("Modulation visualization"));
    setMinimumHeight(kind == mod::Kind::Doubler ? 170 : kind == mod::Kind::DoublerPro ? 110 : 92);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
void ModulationField::present(const mod::Telemetry &t, double dt, bool reduced) {
    m_reading = t;
    m_reduced = reduced;
    if (reduced && m_kind == mod::Kind::Flanger)
        m_reading.positions = {3.5f, 3.5f, 0, 0};
    if (reduced && m_kind == mod::Kind::Phaser)
        m_reading.positions = {429.f, 550.f, 706.f, 906.f};
    dt = std::clamp(dt, 0., .1);
    const double blend = 1 - std::exp(-dt / .12);
    m_level += (std::clamp(double(t.level) * 4, 0., 1.) - m_level) * blend;
    m_width += (std::clamp(double(t.width), 0., 1.4) - m_width) * blend;
    if (!reduced)
        m_time += dt;
    update();
}
void ModulationField::paintEvent(QPaintEvent *) {
    QPainter p(this);
    paintScene(p, rect());
}
void ModulationField::paintScene(QPainter &p, const QRegion &) {
    p.setRenderHint(QPainter::Antialiasing);
    const auto &th = theme();
    const QRectF field = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setPen(QPen(th.separator(), 1));
    p.setBrush(th.well());
    p.drawRoundedRect(field, 12, 12);
    p.save();
    p.setClipRect(field.adjusted(8, 8, -8, -8));
    const QPointF c = field.center();
    const double w = field.width() * .43, h = field.height() * .36;
    p.setPen(QPen(alpha(th.textSecondary, 35), 1));
    p.setBrush(Qt::NoBrush);
    if (mod::isDoubler(m_kind)) {
        for (int i = 1; i <= 3; ++i)
            p.drawEllipse(c, w * i / 3, h * i / 3);
        p.drawLine(QPointF(c.x(), c.y() - h - 10), QPointF(c.x(), c.y() + h + 10));
        // Fixed particle identities: only the measured envelope and each
        // voice's trajectory move them. No allocation or random repaint noise.
        for (int i = 0; i < 64; ++i) {
            const double angle = i * 2.399963229728653;
            const double radius = std::sqrt((i + .5) / 64.);
            const double drift =
                m_reduced ? 0 : .10 * std::sin(m_time * .65 + i) * m_reading.positions[i % 4];
            const double spread = .045 + std::min(m_width, 1.0) * 1.25;
            const QPointF point(c.x() + std::cos(angle + drift) * radius * w * spread,
                                c.y() +
                                    std::sin(angle + drift) * radius * h * (.30 + .7 * m_level));
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(th.accent, int(m_level * (85 + 130 * radius))));
            p.drawEllipse(point, 1.6 + 1.3 * radius, 1.6 + 1.3 * radius);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(th.textPrimary, 130 + int(100 * m_level)));
        p.drawEllipse(c, 3.5, 3.5);
    } else if (m_kind == mod::Kind::Chorus) {
        for (int voice = 0; voice < 4; ++voice) {
            QPainterPath path;
            for (int i = 0; i <= 100; ++i) {
                const double x = double(i) / 100;
                const double y = std::sin(x * 2 * mod::dsp::pi + voice * 1.1 +
                                          (m_reduced ? 0 : m_reading.positions[voice])) *
                                 h * .35;
                const QPointF point(c.x() - w + 2 * w * x, c.y() + (voice - 1.5) * h * .35 + y);
                if (!i)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            p.setPen(QPen(alpha(th.accent, int(30 + m_level * (100 + voice * 25))), 1.4));
            p.drawPath(path);
        }
    } else if (m_kind == mod::Kind::Flanger) {
        for (int i = 0; i < 16; ++i) {
            const double x = c.x() - w + 2 * w * i / 15;
            p.setPen(QPen(alpha(th.textSecondary, 35), 1));
            p.drawLine(QPointF(x, c.y() - h), QPointF(x, c.y() + h));
        }
        for (unsigned i = 0; i < 2; ++i) {
            const double x =
                c.x() - w + 2 * w * std::clamp(double(m_reading.positions[i]) / 8.5, 0., 1.);
            p.setPen(QPen(alpha(th.accent, 65 + int(170 * m_level)), i ? 2 : 3));
            p.drawLine(QPointF(x, c.y() - h), QPointF(x, c.y() + h));
        }
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            const double hz = std::max(120.f, m_reading.positions[i]);
            const double x =
                c.x() - w + 2 * w * std::clamp(std::log(hz / 120.) / std::log(50.), 0., 1.);
            p.setPen(QPen(alpha(th.accent, 45 + int(155 * m_level)), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(x, c.y()), 14 + i * 5, h * (.4 + .15 * i));
        }
    }
    p.restore();
    p.setPen(th.textSecondary);
    p.drawText(field.adjusted(14, 10, -14, -10), Qt::AlignLeft | Qt::AlignBottom,
               mod::isDoubler(m_kind) ? ModulationPanel::tr("Vocal width")
                                            : ModulationPanel::tr("Motion"));
}

ModulationPanel::ModulationPanel(daw::EngineController *controller, QString channel, QString insert,
                                 Kind kind, QWidget *parent)
    : FrameWidget(parent), m_controller(controller), m_channel(channel.toStdString()),
      m_insert(insert.toStdString()), m_kind(kind) {
    setObjectName(QStringLiteral("ModulationPanel"));
    setMinimumSize(440, kind == Kind::DoublerPro ? 560 : kind == Kind::Doubler ? 460 : 500);
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(20, 14, 20, 16);
    column->setSpacing(10);
    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    auto *previous = new QPushButton(QString::fromUtf8("‹"), this);
    previous->setFixedSize(30, 30);
    previous->setAccessibleName(tr("Previous preset"));
    auto *next = new QPushButton(QString::fromUtf8("›"), this);
    next->setFixedSize(30, 30);
    next->setAccessibleName(tr("Next preset"));
    m_preset = new QPushButton(this);
    m_preset->setObjectName(QStringLiteral("ModulationPreset"));
    m_preset->setMinimumHeight(32);
    m_preset->setAccessibleName(tr("Preset browser"));
    header->addWidget(previous);
    header->addWidget(m_preset, 1);
    header->addWidget(next);
    column->addLayout(header);
    m_field = new ModulationField(kind, this);
    column->addWidget(m_field, 1);
    m_meter = new QLabel(this);
    m_meter->setObjectName(QStringLiteral("ModulationMeters"));
    m_meter->setAlignment(Qt::AlignCenter);
    column->addWidget(m_meter);
    m_mono = new QLabel(tr("Stereo output is required for widening"), this);
    m_mono->setObjectName(QStringLiteral("ModulationMonoNotice"));
    m_mono->setAlignment(Qt::AlignCenter);
    m_mono->setWordWrap(true);
    column->addWidget(m_mono);
    m_controls = new QWidget(this);
    m_grid = new QGridLayout(m_controls);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(8);
    column->addWidget(m_controls);
    for (const auto &info : mod::parameterTable(kind)) {
        const auto i = info.index;
        auto *cell = new QWidget(m_controls);
        m_cells[i] = cell;
        auto *layout = new QVBoxLayout(cell);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        auto *label = new QLabel(info.id == "delay" ? tr("Delay · 1/32") : caption(info.id), cell);
        cell->setToolTip(help(info.id));
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
        auto *knob = new ui::Knob({}, cell);
        m_knobs[i] = knob;
        knob->setObjectName(QString::fromStdString("ModulationKnob_" + info.id));
        knob->setAccessibleName(caption(info.id));
        knob->setAccessibleDescription(help(info.id));
        knob->setVisualStyle(ui::Knob::VisualStyle::Graphite);
        knob->setBare(kind == Kind::DoublerPro ? 86 : 98);
        knob->setRange(info.minValue, info.maxValue);
        knob->setDefaultValue(info.defaultValue);
        knob->setLogarithmic(info.id == "rate");
        knob->setAutomatable(true);
        knob->setFormatter([kind, i](double value) {
            const auto &p = mod::parameterTable(kind)[i];
            return p.id == "rate" ? tr("%1 Hz").arg(value, 0, 'g', 3)
                   : p.id == "detune" ? tr("%1 cents").arg(value, 0, 'f', 1)
                                  : tr("%1%").arg(value * 100, 0, 'f', 0);
        });
        layout->addWidget(knob, 0, Qt::AlignCenter);
        auto *number = new QDoubleSpinBox(cell);
        m_numbers[i] = number;
        number->setObjectName(QString::fromStdString("ModulationValue_" + info.id));
        number->setAccessibleName(caption(info.id));
        number->setAccessibleDescription(help(info.id));
        const double scale = displayScale(info.id);
        number->setRange(info.minValue * scale, info.maxValue * scale);
        number->setDecimals(info.id == "rate" ? 3 : 1);
        number->setSingleStep(info.id == "rate" ? .01 : info.id == "detune" ? .1 : 1);
        number->setSuffix(info.id == "rate" ? tr(" Hz") : info.id == "detune" ? tr(" ct") : QStringLiteral("%"));
        number->setButtonSymbols(QAbstractSpinBox::NoButtons);
        number->setAlignment(Qt::AlignCenter);
        number->setFixedWidth(86);
        number->setMinimumHeight(25);
        number->setKeyboardTracking(false);
        layout->addWidget(number, 0, Qt::AlignCenter);
        label->setBuddy(number);
        connect(knob, &ui::Knob::valueChanged, this, [this, i](double v) { write(i, v); });
        connect(knob, &ui::Knob::editFinished, this, [this, i] { finishGesture(i); });
        connect(knob, &ui::Knob::automateRequested, this, [this, i] {
            emit automationRequested(QString::fromStdString(mod::parameterTable(m_kind)[i].id));
        });
        connect(number, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, i, scale](double v) {
                    write(i, v / scale);
                    finishGesture(i);
                });
    }
    connect(previous, &QPushButton::clicked, this,
            [this] { applyFactoryPreset((m_lastFactory + 9) % 10); });
    connect(next, &QPushButton::clicked, this,
            [this] { applyFactoryPreset((m_lastFactory + 1) % 10); });
    connect(m_preset, &QPushButton::clicked, this, &ModulationPanel::showPresetMenu);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &ModulationPanel::applyTheme);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(33);
    connect(m_refreshTimer, &QTimer::timeout, this, &ModulationPanel::refresh);
    m_visualTimer = new ui::FrameTimer(this);
    connect(m_visualTimer, &ui::FrameTimer::timeout, this, &ModulationPanel::refreshVisual);
    loadUserPresets();
    arrangeControls();
    applyTheme();
    refresh();
}
ModulationPanel::~ModulationPanel() {
    for (unsigned i = 0; i < m_gestures.size(); ++i)
        finishGesture(i);
}
mod::ModulationInstance *ModulationPanel::instance() const {
    auto *plugin = m_controller ? dynamic_cast<mod::ModulationInstance *>(
                                      m_controller->insertInstance(m_channel, m_insert))
                                : nullptr;
    return plugin && plugin->kind() == m_kind ? plugin : nullptr;
}
double ModulationPanel::read(unsigned i) const {
    const auto p = mod::parameterTable(m_kind);
    return m_controller && i < p.size()
               ? m_controller->insertParameter(m_channel, m_insert, p[i].id)
               : 0;
}
ModulationPanel::Values ModulationPanel::values() const {
    Values v{};
    for (const auto &p : mod::parameterTable(m_kind))
        v[p.index] = read(p.index);
    return v;
}
void ModulationPanel::write(unsigned i, double value) {
    if (m_refreshing || !instance())
        return;
    if (!m_gestures[i])
        m_gestures[i] = read(i);
    m_controller->setInsertParameter(m_channel, m_insert, mod::parameterTable(m_kind)[i].id, value);
    refresh();
}
void ModulationPanel::finishGesture(unsigned i) {
    if (!m_gestures[i])
        return;
    if (instance()) {
        m_controller->commitInsertParameterEdit(m_channel, m_insert,
                                                mod::parameterTable(m_kind)[i].id, *m_gestures[i],
                                                "Change Modulation Parameter");
        emit projectEdited();
    }
    m_gestures[i].reset();
}
void ModulationPanel::applyValues(const Values &v, const QString &kind, const QString &name) {
    if (!instance())
        return;
    for (unsigned i = 0; i < m_gestures.size(); ++i)
        finishGesture(i);
    const auto group = m_controller->beginUndoGroup();
    for (const auto &p : mod::parameterTable(m_kind)) {
        const double before = read(p.index);
        m_controller->setInsertParameter(m_channel, m_insert, p.id, v[p.index]);
        m_controller->commitInsertParameterEdit(m_channel, m_insert, p.id, before,
                                                "Apply Modulation Preset");
    }
    m_controller->collapseUndo(group, "Apply Modulation Preset");
    if (auto *plugin = instance())
        plugin->setPresetReference(kind.toStdString(), name.toStdString());
    emit projectEdited();
    refresh();
}
void ModulationPanel::applyFactoryPreset(int index) {
    const auto presets = mod::factoryPresets(m_kind);
    if (index < 0 || index >= int(presets.size()))
        return;
    m_lastFactory = index;
    applyValues(presets[index].values, QStringLiteral("factory"),
                QString::fromUtf8(presets[index].name.data(), int(presets[index].name.size())));
}
void ModulationPanel::refresh() {
    auto *plugin = instance();
    m_controls->setEnabled(plugin);
    m_preset->setEnabled(plugin);
    if (!plugin) {
        m_mono->hide();
        m_visualTimer->stop();
        return;
    }
    m_refreshing = true;
    const auto current = values();
    for (const auto &p : mod::parameterTable(m_kind)) {
        const auto i = p.index;
        if (!m_knobs[i]->isEditing()) {
            QSignalBlocker b(m_knobs[i]);
            m_knobs[i]->setValue(current[i]);
        }
        if (!m_numbers[i]->hasFocus()) {
            QSignalBlocker b(m_numbers[i]);
            m_numbers[i]->setValue(current[i] * displayScale(p.id));
        }
    }
    const auto reference = plugin->presetReference();
    m_selectedKind = QString::fromStdString(reference.first);
    m_selectedName = QString::fromStdString(reference.second);
    const auto matches = [&](const Values &v) {
        for (const auto &p : mod::parameterTable(m_kind))
            if (std::abs(v[p.index] - current[p.index]) > 1.e-6)
                return false;
        return true;
    };
    bool exact = false;
    // A named user preset may intentionally duplicate factory settings.
    // Preserve that reference when it still matches; undo falls back to values.
    if (m_selectedKind == QStringLiteral("user"))
        for (const auto &p : m_users)
            if (p.name == m_selectedName && matches(p.values)) {
                exact = true;
                break;
            }
    const auto presets = mod::factoryPresets(m_kind);
    for (unsigned i = 0; !exact && i < presets.size(); ++i)
        if (matches(presets[i].values)) {
            m_selectedKind = QStringLiteral("factory");
            m_selectedName = QString::fromUtf8(presets[i].name.data(), int(presets[i].name.size()));
            m_lastFactory = int(i);
            exact = true;
            break;
        }
    if (!exact)
        for (const auto &p : m_users)
            if (matches(p.values)) {
                m_selectedKind = QStringLiteral("user");
                m_selectedName = p.name;
                exact = true;
                break;
            }
    m_preset->setText((m_selectedName.isEmpty() ? tr("Custom") : m_selectedName) +
                      (exact ? QString() : QStringLiteral(" *")) + QString::fromUtf8("  ▾"));
    m_mono->setVisible(mod::isDoubler(m_kind) && plugin->busLayout().outputs.front() == 1);
    m_refreshing = false;
}
void ModulationPanel::refreshVisual() {
    auto *plugin = instance();
    mod::Telemetry t;
    if (plugin)
        t = plugin->telemetry();
    const auto *model = m_controller ? m_controller->insertModel(m_channel, m_insert) : nullptr;
    // Monitoring can produce audio while transport is stopped. Use freshness
    // of audio telemetry rather than transport state to fade a stopped meter.
    if (t.serial != m_lastTelemetrySerial)
        m_telemetryIdleSeconds = 0;
    else
        m_telemetryIdleSeconds += m_visualTimer->deltaSeconds();
    m_lastTelemetrySerial = t.serial;
    if (!model || model->bypassed || m_telemetryIdleSeconds > .25)
        t.level = 0;
    m_field->setAccessibleDescription(
        tr("Signal level %1 dBFS")
            .arg(t.level > 1.e-6f ? 20 * std::log10(t.level) : -120., 0, 'f', 1));
    const bool reduced = m_reduced || qApp->property("vlt.modulationReduceMotion").toBool();
    m_field->present(t, m_visualTimer->deltaSeconds(), reduced);
    m_meter->setText(mod::isDoubler(m_kind) ? tr("Width %1%   ·   Correlation %2")
                                                   .arg(std::min(999, int(t.width * 100)))
                                                   .arg(t.correlation, 0, 'f', 2)
                                             : tr("Soft modulation"));
}
bool ModulationPanel::visualUpdatesActive() const {
    return m_visualTimer->isActive();
}
void ModulationPanel::showEvent(QShowEvent *e) {
    FrameWidget::showEvent(e);
    m_reduced = systemReducedMotion();
    qApp->setProperty(
        "vlt.modulationReduceMotion",
        QSettings().value(QStringLiteral("ui/modulationReduceMotion"), false).toBool());
    refresh();
    m_refreshTimer->start();
    if (instance())
        m_visualTimer->start();
}
void ModulationPanel::hideEvent(QHideEvent *e) {
    for (unsigned i = 0; i < m_gestures.size(); ++i)
        finishGesture(i);
    m_refreshTimer->stop();
    m_visualTimer->stop();
    FrameWidget::hideEvent(e);
}
void ModulationPanel::resizeEvent(QResizeEvent *e) {
    FrameWidget::resizeEvent(e);
    arrangeControls();
}
void ModulationPanel::arrangeControls() {
    const int count = int(mod::parameterTable(m_kind).size()),
              columns = count > 4 ? 3 : count == 4 && width() < 500 ? 2 : count;
    if (columns == m_columns)
        return;
    m_columns = columns;
    for (int i = 0; i < count; ++i) {
        m_grid->removeWidget(m_cells[i]);
        m_grid->addWidget(m_cells[i], i / columns, i % columns);
    }
}
void ModulationPanel::applyTheme() {
    const auto &t = theme();
    setStyleSheet(
        QStringLiteral(
            "QWidget#ModulationPanel QLabel{color:%1;}"
            "QWidget#ModulationPanel QPushButton,QWidget#ModulationPanel "
            "QDoubleSpinBox{color:%1;background:%2;border:1px solid "
            "%3;border-radius:5px;padding:3px;}"
            "QWidget#ModulationPanel QPushButton:focus,QWidget#ModulationPanel "
            "QDoubleSpinBox:focus{border:1px solid %4;}"
            "QWidget#ModulationPanel QPushButton:hover{border-color:%4;}"
            "QWidget#ModulationPanel QPushButton:pressed{background:%3;}"
            "QWidget#ModulationPanel QPushButton#ModulationPreset{border-bottom:2px solid %4;}"
            "QWidget#ModulationPanel QLabel#ModulationMeters{color:%5;}")
            .arg(t.textPrimary.name(), t.well().name(), t.separator().name(), t.accent.name(),
                 t.textSecondary.name()));
    for (auto *k : m_knobs)
        if (k) {
            k->setArcColor(t.accent);
            k->update();
        }
    m_field->update();
    update();
}
void ModulationPanel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    paintScene(p, rect());
}
void ModulationPanel::paintScene(QPainter &p, const QRegion &) {
    p.fillRect(rect(), theme().surface);
}

QString ModulationPanel::settingsKey() const {
    return QString::fromStdString(mod::descriptorFor(m_kind).uid) +
           QStringLiteral("/userPresets.v1");
}
void ModulationPanel::loadUserPresets() {
    m_users.clear();
    const auto root =
        QJsonDocument::fromJson(QSettings().value(settingsKey()).toByteArray()).object();
    for (const auto &item : root.value(QStringLiteral("presets")).toArray()) {
        if (m_users.size() >= 128)
            break;
        const auto obj = item.toObject();
        const auto name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (!validPresetName(name))
            continue;
        UserPreset preset;
        preset.name = name;
        const auto params = obj.value(QStringLiteral("params")).toObject();
        for (const auto &p : mod::parameterTable(m_kind)) {
            const double v = params.value(QString::fromStdString(p.id)).toDouble(p.defaultValue);
            preset.values[p.index] =
                std::isfinite(v) ? std::clamp(v, p.minValue, p.maxValue) : p.defaultValue;
        }
        m_users.push_back(preset);
    }
}
void ModulationPanel::storeUserPresets() {
    QJsonArray all;
    for (const auto &preset : m_users) {
        QJsonObject params;
        for (const auto &p : mod::parameterTable(m_kind))
            params.insert(QString::fromStdString(p.id), preset.values[p.index]);
        all.append(
            QJsonObject{{QStringLiteral("name"), preset.name}, {QStringLiteral("params"), params}});
    }
    QSettings().setValue(settingsKey(), QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                                  {QStringLiteral("presets"), all}})
                                            .toJson(QJsonDocument::Compact));
}
bool ModulationPanel::validPresetName(const QString &name, const QString &except) const {
    if (name.isEmpty() || name.size() > 48)
        return false;
    for (const auto &p : mod::factoryPresets(m_kind))
        if (name.compare(QString::fromUtf8(p.name.data(), int(p.name.size())),
                         Qt::CaseInsensitive) == 0)
            return false;
    for (const auto &p : m_users)
        if (p.name != except && p.name.compare(name, Qt::CaseInsensitive) == 0)
            return false;
    return true;
}
void ModulationPanel::saveUserPreset() {
    if (!instance())
        return;
    loadUserPresets();
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Save preset"), tr("Preset name:"),
                                            QLineEdit::Normal, {}, &accepted)
                          .trimmed();
    if (!accepted)
        return;
    auto found = std::find_if(m_users.begin(), m_users.end(), [&](const auto &p) {
        return p.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    const QString except = found == m_users.end() ? QString() : found->name;
    if (!validPresetName(name, except)) {
        QMessageBox::warning(this, tr("Invalid name"),
                             tr("Choose a unique name of 1–48 characters."));
        return;
    }
    if (found != m_users.end()) {
        if (QMessageBox::question(this, tr("Replace preset"), tr("Replace “%1”?").arg(found->name),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        found->values = values();
        found->name = name;
    } else {
        if (m_users.size() >= 128) {
            QMessageBox::warning(this, tr("Preset library full"),
                                 tr("Up to 128 user presets are supported."));
            return;
        }
        m_users.push_back({name, values()});
    }
    storeUserPresets();
    if (auto *p = instance())
        p->setPresetReference("user", name.toStdString());
    emit projectEdited();
    refresh();
}
void ModulationPanel::renameUserPreset() {
    loadUserPresets();
    auto found = std::find_if(m_users.begin(), m_users.end(),
                              [&](const auto &p) { return p.name == m_selectedName; });
    if (found == m_users.end())
        return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, tr("Rename preset"), tr("Preset name:"),
                                            QLineEdit::Normal, found->name, &accepted)
                          .trimmed();
    if (!accepted || name == found->name)
        return;
    if (!validPresetName(name, found->name)) {
        QMessageBox::warning(this, tr("Invalid name"),
                             tr("Choose a unique name of 1–48 characters."));
        return;
    }
    found->name = name;
    storeUserPresets();
    if (auto *p = instance())
        p->setPresetReference("user", name.toStdString());
    emit projectEdited();
    refresh();
}
void ModulationPanel::deleteUserPreset() {
    const auto name = m_selectedName;
    if (QMessageBox::question(
            this, tr("Delete preset"), tr("Delete “%1”? The current sound is kept.").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    loadUserPresets();
    std::erase_if(m_users, [&](const auto &p) { return p.name == name; });
    storeUserPresets();
    if (auto *p = instance())
        p->setPresetReference("custom", name.toStdString());
    emit projectEdited();
    refresh();
}
void ModulationPanel::showPresetMenu() {
    loadUserPresets();
    refresh();
    QMenu menu(this);
    menu.addSection(tr("Factory presets"));
    int i = 0;
    for (const auto &preset : mod::factoryPresets(m_kind)) {
        auto *action =
            menu.addAction(QString::fromUtf8(preset.name.data(), int(preset.name.size())));
        connect(action, &QAction::triggered, this,
                [this, index = i++] { applyFactoryPreset(index); });
    }
    if (!m_users.empty())
        menu.addSection(tr("User presets"));
    for (const auto &p : m_users)
        connect(menu.addAction(p.name), &QAction::triggered, this,
                [this, p] { applyValues(p.values, QStringLiteral("user"), p.name); });
    menu.addSeparator();
    connect(menu.addAction(tr("Save preset…")), &QAction::triggered, this,
            &ModulationPanel::saveUserPreset);
    auto *rename = menu.addAction(tr("Rename preset…"));
    auto *remove = menu.addAction(tr("Delete preset…"));
    rename->setEnabled(m_selectedKind == QStringLiteral("user"));
    remove->setEnabled(rename->isEnabled());
    connect(rename, &QAction::triggered, this, &ModulationPanel::renameUserPreset);
    connect(remove, &QAction::triggered, this, &ModulationPanel::deleteUserPreset);
    menu.addSeparator();
    auto *reduced = menu.addAction(tr("Reduce motion"));
    reduced->setCheckable(true);
    reduced->setChecked(m_reduced || qApp->property("vlt.modulationReduceMotion").toBool());
    connect(reduced, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("ui/modulationReduceMotion"), on);
        qApp->setProperty("vlt.modulationReduceMotion", on);
    });
    menu.exec(m_preset->mapToGlobal(QPoint(0, m_preset->height())));
}
