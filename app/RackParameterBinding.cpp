#include "RackParameterBinding.hpp"
#include "Controls.hpp"
#include <QAbstractItemView>
#include <QComboBox>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>
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
    control->setFormatter([this, key, index](double plain) {
        return QString::fromStdString(
            m_controller->insertParameterText(m_channel, m_slot, key, plain, index));
    });
    connect(control, &ui::Knob::valueChanged, this, [this, id](double v) { write(id, v); });
    connect(control, &ui::Knob::editFinished, this, [this, id] { finish(id); });
    connect(control, &ui::Knob::automateRequested, this, [this, id] { automate(id); });
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
    m_controls.push_back({id, control});
    return control;
}
void RackParameterBinding::refresh() {
    std::erase_if(m_controls, [](const auto& c) { return c.widget.isNull(); });
    for (const auto& control : m_controls) {
        if (!control.widget->isVisible() || m_gestures.contains(control.id))
            continue;
        control.widget->setEnabled(available());
        const QSignalBlocker block(control.widget);
        if (auto* knob = qobject_cast<ui::Knob*>(control.widget.data())) {
            if (!knob->isEditing())
                knob->setValue(value(control.id));
        } else if (auto* choice = qobject_cast<QComboBox*>(control.widget.data())) {
            if (!choice->view()->isVisible())
                choice->setCurrentIndex(int(value(control.id)));
        } else if (auto* toggle = qobject_cast<QAbstractButton*>(control.widget.data()))
            toggle->setChecked(value(control.id) >= .5);
    }
}
