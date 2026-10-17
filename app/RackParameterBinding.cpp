#include "RackParameterBinding.hpp"
#include "Controls.hpp"
#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

RackParameterBinding::RackParameterBinding(daw::EngineController* controller, QString channel, QString slot,
                                           QObject* parent)
    : QObject(parent), m_controller(controller), m_channel(channel.toStdString()), m_slot(slot.toStdString()),
      m_identity(controller->insertIdentity(m_channel, m_slot)),
      m_parameters(controller->insertParameters(m_channel, m_slot)) {}
RackParameterBinding::~RackParameterBinding() {
    finishAll();
}
bool RackParameterBinding::available() const {
    return m_identity && m_controller->hasInsert(m_channel, m_slot) &&
           m_controller->insertIdentity(m_channel, m_slot) == m_identity;
}
const daw::plugins::ParameterInfo* RackParameterBinding::info(const QString& id) const {
    const auto key = id.toStdString();
    for (const auto& parameter : m_parameters)
        if (parameter.id == key)
            return &parameter;
    return nullptr;
}
double RackParameterBinding::value(const QString& id) const {
    return available() ? m_controller->insertParameter(m_channel, m_slot, id.toStdString()) : 0.;
}
void RackParameterBinding::write(const QString& id, double next) {
    const auto* parameter = info(id);
    if (!parameter || !available() || !std::isfinite(next))
        return;
    if (!m_gestures.contains(id))
        m_gestures.insert(id, value(id));
    next = std::clamp(next, parameter->minValue, parameter->maxValue);
    if (parameter->isStepped)
        next = std::round(next);
    m_controller->setInsertParameter(m_channel, m_slot, parameter->id, next);
}
void RackParameterBinding::finish(const QString& id) {
    if (!m_gestures.contains(id))
        return;
    const double before = m_gestures.take(id);
    if (!available())
        return;
    m_controller->commitInsertParameterEdit(m_channel, m_slot, id.toStdString(), before,
                                            "Change Rack Parameter");
    emit edited();
}
void RackParameterBinding::finishAll() {
    const auto keys = m_gestures.keys();
    if (keys.isEmpty())
        return;
    const auto group = m_controller->beginUndoGroup();
    for (const auto& key : keys)
        finish(key);
    m_controller->collapseUndo(group, "Change Rack Parameter");
}
void RackParameterBinding::automate(const QString& id) {
    if (const auto* parameter = info(id); parameter && parameter->isAutomatable) {
        finishAll();
        emit automationRequested(id);
    }
}
ui::Knob* RackParameterBinding::knob(const QString& id, QWidget* parent, int width) {
    const auto* parameter = info(id);
    auto* control = new ui::Knob(parameter ? QString::fromStdString(parameter->name) : id, parent);
    control->setObjectName(QStringLiteral("RackParameter.") + id);
    control->setVisualStyle(ui::Knob::VisualStyle::SamplerDigital);
    control->setFixedSize(width, 78);
    control->setFocusPolicy(Qt::StrongFocus);
    if (!parameter) {
        control->setEnabled(false);
        return control;
    }
    control->setAccessibleName(QString::fromStdString(parameter->name));
    control->setToolTip(QString::fromStdString(parameter->name));
    control->setRange(parameter->minValue, parameter->maxValue);
    control->setStepped(parameter->isStepped);
    control->setDefaultValue(parameter->defaultValue);
    control->setAutomatable(parameter->isAutomatable);
    control->setValue(value(id));
    const auto key = parameter->id;
    const auto index = parameter->index;
    control->setValueMapping([this, index](double value) {
        const auto* plugin = available() ? m_controller->insertInstance(m_channel, m_slot) : nullptr;
        return plugin ? plugin->parameterToNormalized(index, value) : 0.;
    }, [this, index](double value) {
        const auto* plugin = available() ? m_controller->insertInstance(m_channel, m_slot) : nullptr;
        return plugin ? plugin->parameterFromNormalized(index, value) : 0.;
    });
    control->setFormatter([this, key, index](double plain) {
        return QString::fromStdString(
            m_controller->insertParameterText(m_channel, m_slot, key, plain, index));
    });
    connect(control, &ui::Knob::valueChanged, this, [this, id](double v) { write(id, v); });
    connect(control, &ui::Knob::editFinished, this, [this, id] { finish(id); });
    connect(control, &ui::Knob::automateRequested, this, [this, id] { automate(id); });
    control->setProperty("modulationChannel", QString::fromStdString(m_channel));
    control->setProperty("modulationSlot", QString::fromStdString(m_slot));
    control->setProperty("modulationParameter", id);
    const auto* eligible = info(id);
    control->setProperty("modulationEligible", eligible && eligible->isAutomatable && !eligible->isReadOnly && eligible->minValue < eligible->maxValue);
    m_controls.push_back({id, control});
    return control;
}
QWidget* RackParameterBinding::slider(const QString& id, QWidget* parent) {
    const auto* parameter = info(id);
    auto* field = new QWidget(parent);
    field->setObjectName(QStringLiteral("RackSliderField.") + id);
    auto* layout = new QVBoxLayout(field);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* caption = new QLabel(parameter ? QString::fromStdString(parameter->name) : id, field);
    auto* control = new ui::GlassSlider(Qt::Horizontal, field);
    auto* readout = new QLabel(field);
    control->setObjectName(QStringLiteral("RackParameter.") + id);
    readout->setObjectName(QStringLiteral("RackParameterValue.") + id);
    control->setFixedHeight(24);
    control->setFocusPolicy(Qt::StrongFocus);
    control->setAccessibleName(caption->text());
    control->setToolTip(caption->text());
    caption->setBuddy(control);
    auto textFont = field->font();
    textFont.setPixelSize(10);
    textFont.setFeature(QFont::Tag("tnum"), 1);
    for (auto* label : {caption, readout}) {
        label->setFont(textFont);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedHeight(12);
    }
    layout->addWidget(caption);
    layout->addWidget(control);
    layout->addWidget(readout);
    field->setMinimumWidth(80);
    if (!parameter) {
        field->setEnabled(false);
        return field;
    }
    // These compact continuous controls retain the DSP's tenths precision.
    control->setRange(int(std::lround(parameter->minValue * 10)),
                      int(std::lround(parameter->maxValue * 10)));
    control->setSingleStep(parameter->isStepped ? 10 : 1);
    control->setPageStep(10);
    control->setValue(int(std::lround(value(id) * 10)));
    const auto format = [this, id, index = parameter->index](double plain) {
        return QString::fromStdString(m_controller->insertParameterText(
            m_channel, m_slot, id.toStdString(), plain, index));
    };
    readout->setText(format(value(id)));
    connect(control, &QSlider::valueChanged, this, [this, id, control, readout, format](int v) {
        write(id, v / 10.);
        readout->setText(format(value(id)));
        if (!control->isSliderDown()) finish(id);
    });
    connect(control, &QSlider::sliderReleased, this, [this, id] { finish(id); });
    control->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(control, &QWidget::customContextMenuRequested, this, [this, control, id](const QPoint& at) {
        QMenu menu(control);
        auto* reset = menu.addAction(tr("Reset"));
        auto* automation = menu.addAction(tr("Create automation"));
        automation->setEnabled(info(id) && info(id)->isAutomatable);
        const auto* selected = menu.exec(control->mapToGlobal(at));
        if (selected == reset && info(id)) control->setValue(int(std::lround(info(id)->defaultValue * 10)));
        else if (selected == automation) automate(id);
    });
    control->setProperty("modulationChannel", QString::fromStdString(m_channel));
    control->setProperty("modulationSlot", QString::fromStdString(m_slot));
    control->setProperty("modulationParameter", id);
    const auto* eligible = info(id);
    control->setProperty("modulationEligible", eligible && eligible->isAutomatable && !eligible->isReadOnly && eligible->minValue < eligible->maxValue);
    m_controls.push_back({id, control, readout});
    return field;
}
QDoubleSpinBox* RackParameterBinding::number(const QString& id, QWidget* parent) {
    auto* control = new QDoubleSpinBox(parent);
    control->setObjectName(QStringLiteral("RackNumber.") + id);
    control->setKeyboardTracking(false);
    control->setMinimumWidth(0);
    const auto* p = info(id);
    if (!p) { control->setEnabled(false); return control; }
    const double scale = p->unit == "%" && p->maxValue <= 1 ? 100. : 1.;
    control->setRange(p->minValue * scale, p->maxValue * scale);
    control->setDecimals(p->isStepped || p->unit == "%" ? 0 : p->unit == "Hz" ? 2 : 1);
    control->setSingleStep(p->unit == "Hz" ? .01 : 1.);
    control->setSuffix(p->unit.empty() ? QString{} : " " + QString::fromStdString(p->unit));
    control->setValue(value(id) * scale);
    control->setAccessibleName(QString::fromStdString(p->name));
    control->setToolTip(control->accessibleName());
    connect(control, &QDoubleSpinBox::valueChanged, this, [this, id, scale](double v) { write(id, v / scale); });
    connect(control, &QDoubleSpinBox::editingFinished, this, [this, id] { finish(id); });
    control->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(control, &QWidget::customContextMenuRequested, this, [this, control, id](const QPoint& at) {
        QMenu menu(control);
        auto* reset = menu.addAction(tr("Reset"));
        auto* automation = menu.addAction(tr("Create automation"));
        automation->setEnabled(info(id) && info(id)->isAutomatable);
        const auto* selected = menu.exec(control->mapToGlobal(at));
        if (selected == reset && info(id)) {
            const auto* p = info(id);
            control->setValue(p->defaultValue * (p->unit == "%" && p->maxValue <= 1 ? 100. : 1.));
            finish(id);
        }
        else if (selected == automation) automate(id);
    });
    control->setProperty("modulationChannel", QString::fromStdString(m_channel));
    control->setProperty("modulationSlot", QString::fromStdString(m_slot));
    control->setProperty("modulationParameter", id);
    const auto* eligible = info(id);
    control->setProperty("modulationEligible", eligible && eligible->isAutomatable && !eligible->isReadOnly && eligible->minValue < eligible->maxValue);
    m_controls.push_back({id, control});
    return control;
}
QComboBox* RackParameterBinding::choice(const QString& id, const QStringList& labels, QWidget* parent) {
    auto* control = new QComboBox(parent);
    control->setObjectName(QStringLiteral("RackChoice.") + id);
    control->addItems(labels);
    if (const auto* p = info(id))
        control->setAccessibleName(QString::fromStdString(p->name));
    control->setEnabled(info(id));
    control->setCurrentIndex(int(value(id)));
    connect(control, &QComboBox::activated, this, [this, id](int v) {
        write(id, v);
        finish(id);
    });
    control->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(control, &QWidget::customContextMenuRequested, this, [this, control, id](const QPoint& at) {
        QMenu menu(control);
        auto* action = menu.addAction(tr("Create automation"));
        action->setEnabled(info(id) && info(id)->isAutomatable);
        if (menu.exec(control->mapToGlobal(at)) == action)
            automate(id);
    });
    control->setProperty("modulationChannel", QString::fromStdString(m_channel));
    control->setProperty("modulationSlot", QString::fromStdString(m_slot));
    control->setProperty("modulationParameter", id);
    const auto* eligible = info(id);
    control->setProperty("modulationEligible", eligible && eligible->isAutomatable && !eligible->isReadOnly && eligible->minValue < eligible->maxValue);
    m_controls.push_back({id, control});
    return control;
}
QAbstractButton* RackParameterBinding::toggle(const QString& id, const QString& text, QWidget* parent) {
    auto* control = new QToolButton(parent);
    control->setText(text);
    control->setCheckable(true);
    control->setChecked(value(id) >= .5);
    control->setEnabled(info(id));
    control->setMinimumSize(24, 24);
    control->setAccessibleName(text);
    connect(control, &QAbstractButton::clicked, this, [this, id](bool on) {
        write(id, on ? 1. : 0.);
        finish(id);
    });
    control->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(control, &QWidget::customContextMenuRequested, this, [this, control, id](const QPoint& at) {
        QMenu menu(control);
        auto* action = menu.addAction(tr("Create automation"));
        action->setEnabled(info(id) && info(id)->isAutomatable);
        if (menu.exec(control->mapToGlobal(at)) == action)
            automate(id);
    });
    control->setProperty("modulationChannel", QString::fromStdString(m_channel));
    control->setProperty("modulationSlot", QString::fromStdString(m_slot));
    control->setProperty("modulationParameter", id);
    const auto* eligible = info(id);
    control->setProperty("modulationEligible", eligible && eligible->isAutomatable && !eligible->isReadOnly && eligible->minValue < eligible->maxValue);
    m_controls.push_back({id, control});
    return control;
}
void RackParameterBinding::refresh() {
    std::erase_if(m_controls, [](const auto& c) { return c.widget.isNull(); });
    const bool live = available();
    const auto currentValue = [this, live](const QString& id) {
        return live ? m_controller->insertParameter(m_channel, m_slot, id.toStdString()) : 0.;
    };
    for (const auto& control : m_controls) {
        if (!control.widget->isVisible())
            continue;
        control.widget->setEnabled(live);
        const QSignalBlocker block(control.widget);
        if (auto* knob = qobject_cast<ui::Knob*>(control.widget.data())) {
            refreshModulation(knob, m_controller, m_channel, m_slot, control.id.toStdString());
            if (!knob->isEditing() && !m_gestures.contains(control.id))
                knob->setValue(currentValue(control.id));
        } else if (auto* slider = qobject_cast<QSlider*>(control.widget.data())) {
            if (!slider->isSliderDown() && !m_gestures.contains(control.id)) {
                const double plain = currentValue(control.id);
                slider->setValue(int(std::lround(plain * 10)));
                if (const auto* parameter = info(control.id); control.readout && parameter)
                    control.readout->setText(QString::fromStdString(m_controller->insertParameterText(
                        m_channel, m_slot, parameter->id, plain, parameter->index)));
            }
        } else if (auto* number = qobject_cast<QDoubleSpinBox*>(control.widget.data())) {
            if (!number->hasFocus()) {
                const auto* p = info(control.id);
                number->setValue(currentValue(control.id) * (p && p->unit == "%" && p->maxValue <= 1 ? 100. : 1.));
            }
        } else if (auto* choice = qobject_cast<QComboBox*>(control.widget.data())) {
            if (!choice->view()->isVisible())
                choice->setCurrentIndex(int(currentValue(control.id)));
        } else if (auto* toggle = qobject_cast<QAbstractButton*>(control.widget.data()))
            toggle->setChecked(currentValue(control.id) >= .5);
    }
}

void RackParameterBinding::refreshModulation(ui::Knob* knob, daw::EngineController* controller, const std::string& channel, const std::string& slot, const std::string& parameter) {
            daw::PluginParameterReadout readout; readout.id = parameter;
            controller->readInsertParameters(channel, slot, std::span(&readout, 1));
            QString selected;
            for (auto* parent = knob->parentWidget(); parent; parent = parent->parentWidget())
                if (parent->property("modulationSource").isValid()) { selected = parent->property("modulationSource").toString(); break; }
            double low = readout.baseNormalized, high = low;
            for (const auto& route : controller->rackModulation(channel).routes) {
                if (route.slotId != slot || route.parameterId != readout.id || (!selected.isEmpty() && route.sourceId != selected.toStdString())) continue;
                daw::AutomationTarget target{daw::AutomationTargetKind::ModulationDepth, channel, route.id, "depth", {}};
                const auto depth = controller->automationValueAtPlayhead(target).value_or(route.depth);
                low += route.bipolar ? -std::abs(depth) : std::min(0., depth);
                high += route.bipolar ? std::abs(depth) : std::max(0., depth);
            }
            knob->setModulation(readout.modulated, low, high, readout.effectiveNormalized);
}
