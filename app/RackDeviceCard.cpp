#include "RackDeviceCard.hpp"
#include "Controls.hpp"
#include "EqualizerStyle.hpp"
#include "Icons.hpp"
#include "RackBuiltinView.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
class DeviceTitle final : public QLabel {
  public:
    using QLabel::QLabel;
    bool vertical = false;

  protected:
    void paintEvent(QPaintEvent* event) override {
        if (!vertical) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                      QPalette::WindowText));
        painter.translate(0, height());
        painter.rotate(-90);
        painter.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, height()));
    }
};
// Only visible rows exist. Neither a 1,000-parameter catalog nor scrolling it
// causes 1,000 QWidget allocations or parameter reads on every display frame.
class ParameterGrid final : public QScrollArea {
  public:
    ParameterGrid(RackParameterBinding* binding, QWidget* parent) : QScrollArea(parent), binding(binding) {
        setObjectName("RackAllParameters");
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        page = new QWidget;
        setWidget(page);
        setWidgetResizable(false);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { materialize(); });
        filter({});
    }
    void filter(const QString& text) {
        binding->finishAll();
        for (auto* control : controls)
            delete control;
        controls.clear();
        first = -1;
        ids.clear();
        for (const auto& info : binding->parameters()) {
            const auto name = QString::fromStdString(info.name), id = QString::fromStdString(info.id);
            if (name.contains(text, Qt::CaseInsensitive) || id.contains(text, Qt::CaseInsensitive))
                ids.push_back(id);
        }
        verticalScrollBar()->setValue(0);
        materialize();
    }

  protected:
    void resizeEvent(QResizeEvent* e) override {
        QScrollArea::resizeEvent(e);
        materialize();
    }
    void showEvent(QShowEvent* e) override {
        QScrollArea::showEvent(e);
        materialize();
    }

  private:
    void materialize() {
        constexpr int columns = 3, rowHeight = 80;
        page->resize(viewport()->width(),
                     std::max(viewport()->height(), int((ids.size() + columns - 1) / columns) * rowHeight));
        const int nextFirst = std::max(0, verticalScrollBar()->value() / rowHeight - 1);
        const int rows = viewport()->height() / rowHeight + 3;
        if (first != nextFirst || visibleRows != rows) {
            binding->finishAll();
            for (auto* control : controls)
                delete control;
            controls.clear();
            first = nextFirst;
            visibleRows = rows;
            for (int row = first; row < first + rows; ++row)
                for (int col = 0; col < columns; ++col) {
                    const int at = row * columns + col;
                    if (at >= ids.size())
                        break;
                    auto* knob = binding->knob(ids[at], page, 76);
                    controls.push_back(knob);
                    knob->show();
                }
        }
        for (int i = 0; i < controls.size(); ++i) {
            const int col = i % columns, row = first + i / columns;
            controls[i]->move(col * page->width() / columns +
                                  (page->width() / columns - controls[i]->width()) / 2,
                              row * rowHeight);
        }
    }
    RackParameterBinding* binding;
    QWidget* page;
    QStringList ids;
    QList<QWidget*> controls;
    int first = -1, visibleRows = 0;
};
QString expansionKey(daw::EngineController* c, const QString& channel, const QString& slot) {
    return "rack/" + QString::fromStdString(c->project().miniModuleProjectId) + "/" + channel + "/expanded/" +
           slot;
}
QString collapseKey(daw::EngineController* c, const QString& channel, const QString& slot) {
    return "rack/" + QString::fromStdString(c->project().miniModuleProjectId) + "/" + channel + "/collapsed/" +
           slot;
}
} // namespace
RackDeviceCard::RackDeviceCard(daw::EngineController* c, QString channel, QString slot, QWidget* parent)
    : QFrame(parent), m_controller(c), m_channel(std::move(channel)), m_slot(std::move(slot)) {
    setObjectName("RackDeviceCard");
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(1, 1, 1, 1);
    m_root->setSpacing(0);
    m_header = new QWidget(this);
    m_header->setObjectName("RackDeviceHeader");
    m_header->setFixedHeight(30);
    m_header->installEventFilter(this);
    auto* header = m_headerLayout = new QHBoxLayout(m_header);
    header->setContentsMargins(3, 0, 3, 0);
    header->setSpacing(2);
    m_power = new ui::IconButton(icons::Glyph::Power, tr("Enable device"), m_header);
    m_power->setCheckable(true);
    m_power->setButtonSize(24, 24);
    m_power->setObjectName("RackDevicePower");
    m_fold = new ui::IconButton(icons::Glyph::Chevron, tr("Collapse device"), m_header);
    m_fold->setObjectName("RackDeviceFold");
    m_fold->setButtonSize(24, 24);
    m_fold->setCheckable(true);
    m_fold->setFocusPolicy(Qt::StrongFocus);
    m_title = new DeviceTitle(m_header);
    m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto font = m_title->font();
    font.setBold(true);
    font.setPixelSize(11);
    m_title->setFont(font);
    auto* open = new ui::IconButton(icons::Glyph::WindowMaximize, tr("Open full editor"), m_header);
    open->setObjectName("RackDeviceOpen");
    open->setButtonSize(24, 24);
    m_configure = new ui::IconButton(icons::Glyph::Gear, tr("Configure eight controls"), m_header);
    m_configure->setObjectName("RackDeviceConfigure");
    m_configure->setButtonSize(24, 24);
    m_expand = new ui::IconButton(icons::Glyph::ChevronRight, tr("Show all parameters"), m_header);
    m_expand->setButtonSize(24, 24);
    m_expand->setObjectName("RackDeviceExpandParameters");
    m_expand->setCheckable(true);
    header->addWidget(m_power);
    header->addWidget(open);
    header->addWidget(m_title, 1);
    header->addWidget(m_configure);
    header->addWidget(m_expand);
    header->addWidget(m_fold);
    m_root->addWidget(m_header);
    connect(m_power, &QAbstractButton::clicked, this, [this](bool on) {
        finishEdits();
        m_controller->setInsertBypassed(m_channel.toStdString(), m_slot.toStdString(), !on);
        emit edited(false);
        sync();
    });
    connect(open, &QAbstractButton::clicked, this, [this] {
        finishEdits();
        emit editorRequested(m_channel, m_slot);
    });
    connect(m_expand, &QAbstractButton::clicked, this, [this] {
        setCollapsed(false);
        setExpanded(!m_expanded);
    });
    connect(m_fold, &QAbstractButton::clicked, this, [this] { setCollapsed(false); });
    connect(m_configure, &QAbstractButton::clicked, this, &RackDeviceCard::configurePins);
    auto* footer = m_footer = new QWidget(this);
    footer->setObjectName("RackDeviceFooter");
    footer->setFixedHeight(30);
    auto* foot = new QHBoxLayout(footer);
    foot->setContentsMargins(5, 1, 5, 1);
    foot->setSpacing(3);
    m_mix = new QDoubleSpinBox(footer);
    m_mix->setObjectName("RackMix");
    m_mix->setRange(0, 100);
    m_mix->setDecimals(0);
    m_mix->setSuffix("%");
    m_mix->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_mix->setFixedWidth(44);
    m_mix->setToolTip(tr("Dry / wet"));
    m_mix->setAccessibleName(tr("Dry / wet"));
    m_mode = new QComboBox(footer);
    m_mode->setObjectName("RackChannelMode");
    m_mode->addItems({tr("Auto"), tr("Mono"), tr("Stereo"), tr("Dual Mono")});
    m_mode->setMinimumWidth(64);
    m_mode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_mode->setToolTip(tr("Processing channels"));
    m_mode->setAccessibleName(tr("Processing channels"));
    m_channelSide = new QComboBox(footer);
    m_channelSide->addItems({"L", "R"});
    m_channelSide->setFixedWidth(40);
    m_channelSide->setAccessibleName(tr("Edited side"));
    m_sidechain = new QToolButton(footer);
    m_sidechain->setText(tr("SC"));
    m_sidechain->setMinimumSize(26, 24);
    m_sidechain->setAccessibleName(tr("Sidechain sources"));
    foot->addWidget(new QLabel(tr("Mix"), footer));
    foot->addWidget(m_mix);
    foot->addWidget(m_mode, 1);
    foot->addWidget(m_channelSide);
    foot->addWidget(m_sidechain);
    m_root->addWidget(footer);
    connect(m_mix, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_syncing)
            return;
        const auto* model = m_controller->insertModel(m_channel.toStdString(), m_slot.toStdString());
        if (!model)
            return;
        if (!m_mixBefore)
            m_mixBefore = model->mix;
        m_controller->setInsertMix(m_channel.toStdString(), m_slot.toStdString(), float(v / 100.));
    });
    connect(m_mix, &QDoubleSpinBox::editingFinished, this, [this] { finishEdits(); });
    connect(m_mode, &QComboBox::activated, this, [this](int mode) {
        finishEdits();
        m_controller->setInsertChannelMode(m_channel.toStdString(), m_slot.toStdString(),
                                           daw::PluginChannelMode(mode));
        sync();
        emit edited(true);
    });
    connect(m_channelSide, &QComboBox::activated, this, [this](int side) {
        finishEdits();
        m_controller->setInsertEditorChannel(m_channel.toStdString(), m_slot.toStdString(),
                                             daw::PluginEditorChannel(side));
        sync();
        emit edited(false);
    });
    connect(m_sidechain, &QAbstractButton::clicked, this, &RackDeviceCard::sidechainMenu);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, &RackDeviceCard::updateTheme);
    m_expanded = QSettings().value(expansionKey(c, m_channel, m_slot), false).toBool();
    m_collapsed = QSettings().value(collapseKey(c, m_channel, m_slot), false).toBool();
    updateTheme();
    sync();
}
RackDeviceCard::~RackDeviceCard() {
    finishEdits();
}
void RackDeviceCard::updateTheme() {
    const auto& t = th();
    setStyleSheet(
        QStringLiteral(
            "#RackDeviceCard{background:%1;border:0;border-radius:9px;} #RackDeviceFooter{border-top:1px "
            "solid %2;} #RackDeviceCard QComboBox{font-size:10px;min-height:22px;padding:0 3px;} "
            "#RackDeviceCard QDoubleSpinBox{font-size:10px;min-height:22px;padding:0 2px;}")
            .arg(t.surface.name(), t.separator().name()));
    update();
}
void RackDeviceCard::finishEdits() {
    if (m_binding)
        m_binding->finishAll();
    if (m_mixBefore) {
        const auto before = *m_mixBefore;
        m_mixBefore.reset();
        m_controller->commitInsertMixEdit(m_channel.toStdString(), m_slot.toStdString(), before,
                                          "Change Plugin Mix");
        emit edited(false);
    }
}
void RackDeviceCard::sync() {
    const auto* model = m_controller->insertModel(m_channel.toStdString(), m_slot.toStdString());
    if (!model)
        return;
    m_syncing = true;
    const auto identity = m_controller->insertIdentity(m_channel.toStdString(), m_slot.toStdString());
    const auto uid = QString::fromStdString(model->uid);
    const auto runtimeState =
        m_controller->insertRuntimeStatus(m_channel.toStdString(), m_slot.toStdString()).state;
    const bool rebuild = !m_body || runtimeState != m_runtimeState || identity != m_identity ||
                         m_side != model->editorChannel || uid != m_uid || m_pins != model->rackParameterIds;
    if (rebuild) {
        finishEdits();
        m_runtimeState = runtimeState;
        m_uid = uid;
        m_identity = identity;
        m_side = model->editorChannel;
        m_pins = model->rackParameterIds;
        rebuildControls();
    }
    m_title->setText(eqUi::displayName(model->uid, model->name));
    m_title->setToolTip(m_title->text());
    setAccessibleName(m_title->text());
    m_power->setChecked(!model->bypassed);
    m_power->setToolTip(model->bypassed ? tr("Enable device") : tr("Bypass device"));
    m_title->setEnabled(!model->bypassed);
    m_mode->setCurrentIndex(int(model->channelMode));
    m_channelSide->setVisible(model->channelMode == daw::PluginChannelMode::DualMono);
    m_channelSide->setCurrentIndex(int(model->editorChannel));
    m_channelSide->setToolTip(model->editorChannel == daw::PluginEditorChannel::Left
                                  ? tr("Editing left channel")
                                  : tr("Editing right channel"));
    if (auto* choices = qobject_cast<QStandardItemModel*>(m_mode->model())) {
        // Auto, mono and stereo share the runtime's availability gate; only
        // dual mono asks for an additional capability. Query live capabilities
        // each refresh so plugin I/O changes cannot leave stale menu state.
        const auto channel = m_channel.toStdString();
        const auto slot = m_slot.toStdString();
        const bool available = m_controller->insertSupportsChannelMode(
            channel, slot, daw::PluginChannelMode::Auto);
        const bool dualMono = available && m_controller->insertSupportsChannelMode(
            channel, slot, daw::PluginChannelMode::DualMono);
        for (int i = 0; i < 4; ++i)
            choices->item(i)->setEnabled(daw::PluginChannelMode(i) == daw::PluginChannelMode::DualMono
                ? dualMono : available);
    }
    if (!m_mix->hasFocus() && !m_mixBefore)
        m_mix->setValue(model->mix * 100.);
    const bool sidechain =
        m_controller->insertSupportsSidechain(m_channel.toStdString(), m_slot.toStdString());
    m_sidechain->setEnabled(sidechain);
    m_sidechain->setText(model->sidechainTrackIds.empty() ? tr("SC")
                                                          : tr("SC %1").arg(model->sidechainTrackIds.size()));
    m_sidechain->setToolTip(sidechain ? tr("Choose sidechain sources")
                                      : tr("This device has no sidechain input"));
    m_syncing = false;
}
void RackDeviceCard::refresh(bool synchronize) {
    if (synchronize) sync();
    if (m_collapsed) return;
    if (m_binding)
        m_binding->refresh();
    if (m_builtin)
        m_builtin->refresh();
}
void RackDeviceCard::setSelected(bool on) {
    if (m_selected == on)
        return;
    m_selected = on;
    update();
}
void RackDeviceCard::setExpanded(bool on) {
    if (m_expanded == on)
        return;
    finishEdits();
    m_expanded = on;
    QSettings().setValue(expansionKey(m_controller, m_channel, m_slot), on);
    rebuildControls();
    emit widthChanged();
}
void RackDeviceCard::setCollapsed(bool on) {
    if (m_collapsed == on) return;
    finishEdits();
    const bool focusControls = on && ((m_body && m_body->isAncestorOf(QApplication::focusWidget())) ||
                                      m_footer->isAncestorOf(QApplication::focusWidget()));
    const bool focusFold = !on && m_fold->hasFocus();
    m_pressed = false;
    m_collapsed = on;
    QSettings().setValue(collapseKey(m_controller, m_channel, m_slot), on);
    updateCollapsed();
    if (focusControls) m_fold->setFocus();
    else if (focusFold) setFocus();
    emit widthChanged();
}
void RackDeviceCard::updateCollapsed() {
    const bool native = RackBuiltinView::supports(m_uid);
    m_expand->setVisible(!native && !m_collapsed);
    m_fold->setVisible(m_collapsed);
    m_body->setVisible(!m_collapsed);
    m_footer->setVisible(!m_collapsed);
    m_headerLayout->setDirection(m_collapsed ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_headerLayout->setContentsMargins(3, m_collapsed ? 3 : 0, 3, m_collapsed ? 3 : 0);
    m_header->setMinimumHeight(m_collapsed ? 0 : 30);
    m_header->setMaximumHeight(m_collapsed ? QWIDGETSIZE_MAX : 30);
    m_header->setSizePolicy(QSizePolicy::Preferred, m_collapsed ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    static_cast<DeviceTitle*>(m_title)->vertical = m_collapsed;
    m_title->setSizePolicy(QSizePolicy::Ignored, m_collapsed ? QSizePolicy::Ignored : QSizePolicy::Preferred);
    m_title->update();
    m_fold->setChecked(!m_collapsed);
    m_fold->setGlyph(m_collapsed ? icons::Glyph::ChevronRight : icons::Glyph::Chevron);
    const auto action = m_collapsed ? tr("Expand device") : tr("Collapse device");
    m_fold->setToolTip(action);
    m_fold->setAccessibleName(action);
    m_header->setToolTip(m_collapsed ? tr("Double-click to expand device") : tr("Double-click to collapse device"));
    setFixedWidth(m_collapsed ? 36 : (native ? RackBuiltinView::preferredWidth(m_uid) : (m_expanded ? 560 : 288)));
}
void RackDeviceCard::rebuildControls() {
    if (m_body) {
        m_root->removeWidget(m_body);
        delete m_body;
        m_body = nullptr;
    }
    delete m_binding;
    m_binding = nullptr;
    m_builtin = nullptr;
    m_binding = new RackParameterBinding(m_controller, m_channel, m_slot, this);
    connect(m_binding, &RackParameterBinding::edited, this, [this] { emit edited(false); });
    connect(m_binding, &RackParameterBinding::automationRequested, this,
            [this](const QString& id) { emit automationRequested(m_channel, m_slot, id); });
    m_body = new QWidget(this);
    m_body->setObjectName("RackDeviceBody");
    auto* row = new QHBoxLayout(m_body);
    row->setContentsMargins(3, 3, 3, 3);
    row->setSpacing(4);
    const bool native = RackBuiltinView::supports(m_uid);
    m_configure->setVisible(!native);
    m_configure->setEnabled(!m_controller->hasCloudProjectBinding());
    m_expand->setChecked(m_expanded);
    m_expand->setToolTip(m_expanded ? tr("Hide all parameters") : tr("Show all parameters"));
    if (!m_binding->available() || m_runtimeState != daw::AudioPluginRuntimeState::Local) {
        auto* col = new QVBoxLayout;
        auto* message = new QLabel(tr("Device unavailable"), m_body);
        message->setWordWrap(true);
        col->addWidget(message);
        const auto status = m_controller->insertRuntimeStatus(m_channel.toStdString(), m_slot.toStdString());
        auto* detail = new QLabel(QString::fromStdString(status.detail), m_body);
        detail->setWordWrap(true);
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(detail, 1);
        auto* retry = new QToolButton(m_body);
        retry->setText(tr("Restore device"));
        retry->setEnabled(status.canRetry);
        col->addWidget(retry);
        connect(retry, &QAbstractButton::clicked, this, [this] {
            m_controller->retryInsertRecovery(m_channel.toStdString(), m_slot.toStdString(), m_identity);
            sync();
            emit edited(true);
        });
        row->addLayout(col);
    } else if (native) {
        m_builtin = new RackBuiltinView(m_binding, m_uid, m_body);
        row->addWidget(m_builtin);
    } else {
        auto* pinned = new QWidget(m_body);
        pinned->setFixedWidth(278);
        auto* grid = new QGridLayout(pinned);
        grid->setContentsMargins(2, 0, 2, 0);
        grid->setSpacing(0);
        QStringList ids;
        for (const auto& id : m_pins)
            ids.push_back(QString::fromStdString(id));
        if (ids.isEmpty())
            for (const auto& p : m_binding->parameters()) {
                if (ids.size() == 8)
                    break;
                ids.push_back(QString::fromStdString(p.id));
            }
        for (int i = 0; i < 8; ++i) {
            if (i < ids.size() && m_binding->info(ids[i])) {
                auto* knob = m_binding->knob(ids[i], pinned, 66);
                knob->setMinimumHeight(60);
                knob->setMaximumHeight(78);
                knob->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
                grid->addWidget(knob, i / 4, i % 4);
            } else {
                auto* empty = new QLabel(QStringLiteral("—"), pinned);
                empty->setAlignment(Qt::AlignCenter);
                empty->setMinimumSize(60, 60);
                empty->setEnabled(false);
                grid->addWidget(empty, i / 4, i % 4);
            }
        }
        row->addWidget(pinned);
        if (m_expanded) {
            auto* col = new QVBoxLayout;
            auto* search = new QLineEdit(m_body);
            search->setObjectName("RackParameterSearch");
            search->setPlaceholderText(tr("Find a parameter…"));
            search->setAccessibleName(tr("Find a parameter"));
            auto* grid = new ParameterGrid(m_binding, m_body);
            col->addWidget(search);
            col->addWidget(grid, 1);
            row->addLayout(col, 1);
            connect(search, &QLineEdit::textChanged, grid,
                    [grid](const QString& text) { grid->filter(text); });
        }
    }
    m_root->insertWidget(1, m_body, 1);
    updateCollapsed();
}
void RackDeviceCard::configurePins() {
    finishEdits();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Rack controls"));
    dialog.resize(420, 360);
    auto* form = new QFormLayout(&dialog);
    QList<QComboBox*> cells;
    for (int i = 0; i < 8; ++i) {
        auto* box = new QComboBox(&dialog);
        box->setEditable(true);
        box->setInsertPolicy(QComboBox::NoInsert);
        box->addItem(tr("Empty"), QString{});
        for (const auto& p : m_binding->parameters())
            box->addItem(QString::fromStdString(p.name), QString::fromStdString(p.id));
        int selected = 0;
        if (!m_pins.empty() && i < int(m_pins.size()))
            selected = box->findData(QString::fromStdString(m_pins[i]));
        else if (m_pins.empty() && i < int(m_binding->parameters().size()))
            selected = i + 1;
        box->setCurrentIndex(std::max(0, selected));
        form->addRow(tr("Control %1").arg(i + 1), box);
        cells.push_back(box);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        std::vector<std::string> ids;
        for (auto* cell : cells)
            ids.push_back(cell->currentData().toString().toStdString());
        if (m_controller->setRackParameters(m_channel.toStdString(), m_slot.toStdString(), ids)) {
            sync();
            emit edited(true);
        }
    }
}
void RackDeviceCard::sidechainMenu() {
    finishEdits();
    QMenu menu(this);
    const auto* model = m_controller->insertModel(m_channel.toStdString(), m_slot.toStdString());
    if (!model)
        return;
    auto selected = model->sidechainTrackIds;
    auto* none = menu.addAction(tr("No sidechain"));
    none->setCheckable(true);
    none->setChecked(selected.empty());
    connect(none, &QAction::triggered, this, [this] {
        m_controller->setInsertSidechainSources(m_channel.toStdString(), m_slot.toStdString(), {});
        sync();
        emit edited(true);
    });
    menu.addSeparator();
    for (const auto& source : m_controller->insertSidechainSources(m_channel.toStdString())) {
        auto* action = menu.addAction(QString::fromStdString(source.name));
        action->setCheckable(true);
        const bool checked = std::find(selected.begin(), selected.end(), source.id) != selected.end();
        action->setChecked(checked);
        action->setEnabled(checked || selected.size() < daw::kMaxPluginSidechainSources);
        connect(action, &QAction::triggered, this, [this, id = source.id, selected](bool on) mutable {
            std::erase(selected, id);
            if (on)
                selected.push_back(id);
            m_controller->setInsertSidechainSources(m_channel.toStdString(), m_slot.toStdString(), selected);
            sync();
            emit edited(true);
        });
    }
    menu.exec(m_sidechain->mapToGlobal(QPoint(0, m_sidechain->height())));
}
bool RackDeviceCard::eventFilter(QObject* object, QEvent* event) {
    if (object == m_header) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* e = static_cast<QMouseEvent*>(event);
            if (e->button() == Qt::LeftButton) {
                setFocus();
                m_press = e->position().toPoint();
                m_pressed = true;
                emit selected(m_slot, e->modifiers());
                return true;
            }
            if (e->button() == Qt::RightButton) {
                emit contextRequested(m_slot, e->globalPosition().toPoint());
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && m_pressed) {
            auto* e = static_cast<QMouseEvent*>(event);
            if ((e->position().toPoint() - m_press).manhattanLength() >= QApplication::startDragDistance()) {
                m_pressed = false;
                finishEdits();
                emit dragRequested(m_slot);
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            if (m_pressed)
                emit clicked(m_slot, static_cast<QMouseEvent*>(event)->modifiers());
            m_pressed = false;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                setCollapsed(!m_collapsed);
                return true;
            }
        }
    }
    return QFrame::eventFilter(object, event);
}
void RackDeviceCard::paintEvent(QPaintEvent* e) {
    QFrame::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(m_selected ? th().accent : th().separator(), m_selected ? 2 : 1));
    p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
}
void RackDeviceCard::hideEvent(QHideEvent* e) {
    finishEdits();
    QFrame::hideEvent(e);
}
