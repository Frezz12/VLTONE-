#include "RackWidget.hpp"
#include "Controls.hpp"
#include "Icons.hpp"
#include "MiniModuleRack.hpp"
#include "PluginPickerMenu.hpp"
#include "RackDeviceCard.hpp"
#include "RackDrag.hpp"
#include "Theme.hpp"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDataStream>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {
class GroupHeader final : public QLabel {
  public:
    GroupHeader(const QString& text, QWidget* parent) : QLabel(text, parent) {
        setMinimumHeight(26);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::OpenHandCursor);
    }
    std::function<void(Qt::KeyboardModifiers)> select;
    std::function<void(Qt::KeyboardModifiers)> click;
    std::function<void()> drag;
    std::function<void(QPoint)> menu;
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) {
            if (menu)
                menu(e->globalPosition().toPoint());
            return;
        }
        if (e->button() == Qt::LeftButton) {
            start = e->position().toPoint();
            pressed = true;
            setFocus();
            if (select)
                select(e->modifiers());
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (pressed &&
            (e->position().toPoint() - start).manhattanLength() >= QApplication::startDragDistance()) {
            pressed = false;
            if (drag)
                drag();
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (pressed && click)
            click(e->modifiers());
        pressed = false;
    }

  private:
    QPoint start;
    bool pressed = false;
};
bool textInput(QWidget* w) {
    for (auto* p = w; p; p = p->parentWidget())
        if (qobject_cast<QLineEdit*>(p) || qobject_cast<QTextEdit*>(p) || qobject_cast<QPlainTextEdit*>(p) ||
            qobject_cast<QAbstractSpinBox*>(p) || qobject_cast<QComboBox*>(p))
            return true;
    return false;
}
QString keyCommand(QKeyEvent* e) {
    const bool control = e->modifiers().testFlag(Qt::ControlModifier),
               shift = e->modifiers().testFlag(Qt::ShiftModifier);
    if (control) {
        switch (e->key()) {
        case Qt::Key_C:
            return "copy";
        case Qt::Key_X:
            return "cut";
        case Qt::Key_V:
            return "paste";
        case Qt::Key_D:
            return "duplicate";
        case Qt::Key_A:
            return "all";
        case Qt::Key_G:
            return shift ? "ungroup" : "group";
        case Qt::Key_Z:
            return shift ? "redo" : "undo";
        case Qt::Key_Y:
            return "redo";
        default:
            break;
        }
    }
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
        return "delete";
    return {};
}
} // namespace
RackWidget::RackWidget(daw::EngineController* c, QWidget* parent) : QWidget(parent), m_controller(c) {
    setObjectName("RackPanel");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(0);
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    m_left = new QWidget(this);
    m_left->setObjectName("RackMiniModules");
    m_left->setFixedWidth(176);
    auto* left = new QVBoxLayout(m_left);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(0);
    root->addWidget(m_left);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("RackChainScroll");
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setMinimumWidth(80);
    m_scroll->viewport()->setAcceptDrops(true);
    m_scroll->viewport()->installEventFilter(this);
    root->addWidget(m_scroll, 1);
    m_chain = new QWidget;
    m_chain->setObjectName("RackChain");
    m_chain->setAcceptDrops(true);
    m_chain->installEventFilter(this);
    m_row = new QHBoxLayout(m_chain);
    m_row->setContentsMargins(0, 0, 0, 0);
    m_row->setSpacing(6);
    m_scroll->setWidget(m_chain);
    m_marker = new QWidget(m_chain);
    m_marker->setObjectName("RackInsertionMarker");
    m_marker->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_marker->hide();
    m_sends = new QWidget(this);
    m_sends->setObjectName("RackSends");
    m_sends->setFixedWidth(156);
    auto* sends = new QVBoxLayout(m_sends);
    sends->setContentsMargins(0, 0, 0, 0);
    sends->setSpacing(3);
    auto* caption = new QLabel(tr("SENDS"), m_sends);
    caption->setFixedHeight(18);
    sends->addWidget(caption);
    m_sendScroll = new QScrollArea(m_sends);
    m_sendScroll->setFrameShape(QFrame::NoFrame);
    m_sendScroll->setWidgetResizable(true);
    m_sendScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sends->addWidget(m_sendScroll, 1);
    root->addWidget(m_sends);
    m_rubber = new QRubberBand(QRubberBand::Rectangle, m_chain);
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &RackWidget::refresh);
    m_scrollTimer = new QTimer(this);
    m_scrollTimer->setInterval(16);
    connect(m_scrollTimer, &QTimer::timeout, this, [this] {
        auto* bar = m_scroll->horizontalScrollBar();
        const int x = m_scroll->viewport()->mapFromGlobal(m_dragPosition).x();
        const int delta = x < 30 ? -std::max(4, (30 - x) / 2)
                          : x > m_scroll->viewport()->width() - 30
                              ? std::max(4, (x - m_scroll->viewport()->width() + 30) / 2)
                              : 0;
        if (delta) {
            bar->setValue(bar->value() + delta);
            updateDrop(m_dragPosition);
        }
    });
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { refresh(); });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        m_marker->setStyleSheet("background:" + th().accent.name());
        m_structure.clear();
        sync();
    });
    qApp->installEventFilter(this);
    m_marker->setStyleSheet("background:" + th().accent.name());
}
RackWidget::~RackWidget() {
    saveView();
    finishEdits();
    qApp->removeEventFilter(this);
}
QString RackWidget::settingsKey(const QString& suffix) const {
    return "rack/" + m_project + "/" + m_channel + "/" + suffix;
}
void RackWidget::saveView() {
    if (!m_project.isEmpty()) {
        QSettings().setValue(settingsKey("scroll"), m_scroll->horizontalScrollBar()->value());
        m_channelSelection[m_channel] = m_selection;
    }
}
void RackWidget::setChannel(const QString& channel) {
    QString next = channel;
    const auto* track = m_controller->project().findTrack(channel.toStdString());
    if (track && track->kind == daw::TrackKind::Master)
        next = QString::fromUtf8(daw::EngineController::kMasterChannelId);
    const auto project = QString::fromStdString(m_controller->project().miniModuleProjectId);
    if (next == m_channel && project == m_project) {
        sync();
        return;
    }
    saveView();
    finishEdits();
    if (project != m_project)
        m_channelSelection.clear();
    m_project = project;
    m_channel = next;
    m_selection = m_channelSelection.value(next);
    m_anchor.clear();
    m_insertAt = -1;
    m_structure.clear();
    // Even an empty destination needs to clear the preceding channel's sends.
    m_sendStructure = QByteArray(1, '\xff');
    for (auto card : m_cards)
        if (card)
            delete card;
    m_cards.clear();
    delete m_mini;
    m_mini = nullptr;
    const bool audio =
        next == daw::EngineController::kMasterChannelId || (track && daw::carriesAudio(*track));
    m_left->setVisible(audio);
    m_sends->setVisible(audio && next != daw::EngineController::kMasterChannelId);
    if (audio) {
        m_mini = new ui::MiniModuleRack(m_controller, next, m_left, true);
        m_mini->setStripWidth(m_left->width());
        m_left->layout()->addWidget(m_mini);
        static_cast<QVBoxLayout*>(m_left->layout())->setAlignment(m_mini, Qt::AlignTop);
        connect(m_mini, &ui::MiniModuleRack::edited, this, [this](bool undoable) { changed(undoable); });
        connect(m_mini, &ui::MiniModuleRack::automateRequested, this,
                [this](const QString& id, const QString& parameter) {
                    emit automationRequested(m_channel, id, parameter);
                });
    }
    sync();
    const int scroll = QSettings().value(settingsKey("scroll"), 0).toInt();
    QTimer::singleShot(0, this, [this, scroll] { m_scroll->horizontalScrollBar()->setValue(scroll); });
}
QString RackWidget::channelName() const {
    if (m_channel == daw::EngineController::kMasterChannelId)
        return tr("Master");
    if (const auto* t = m_controller->project().findTrack(m_channel.toStdString()))
        return QString::fromStdString(t->name);
    return tr("No channel selected");
}
void RackWidget::sync() {
    if (m_rebuilding)
        return;
    const auto* selectedTrack = m_controller->project().findTrack(m_channel.toStdString());
    const bool audio = m_channel == daw::EngineController::kMasterChannelId ||
                       (selectedTrack && daw::carriesAudio(*selectedTrack));
    m_left->setVisible(audio);
    m_sends->setVisible(audio && m_channel != daw::EngineController::kMasterChannelId);
    QByteArray structure, sends;
    QDataStream s(&structure, QIODevice::WriteOnly), out(&sends, QIODevice::WriteOnly);
    const auto slot = [&](const daw::InsertModel& m) {
        s << QString::fromStdString(m.id) << QString::fromStdString(m.uid) << int(m.format);
    };
    if (const auto* track = m_controller->project().findTrack(m_channel.toStdString())) {
        s << int(track->kind);
        if (track->instrument.isLoaded())
            slot(track->instrument);
        for (const auto& send : track->sends) {
            out << QString::fromStdString(send.id) << QString::fromStdString(send.destinationTrackId)
                << send.preFader << send.enabled;
            if (const auto* d = m_controller->project().findTrack(send.destinationTrackId))
                out << QString::fromStdString(d->name);
        }
    }
    if (const auto* chain = m_controller->channelInserts(m_channel.toStdString()))
        for (const auto& model : *chain)
            slot(model);
    for (const auto& g : m_controller->rackGroups(m_channel.toStdString())) {
        s << QString::fromStdString(g.id) << QString::fromStdString(g.name);
        for (const auto& id : g.insertIds)
            s << QString::fromStdString(id);
    }
    if (structure != m_structure) {
        m_structure = structure;
        rebuild();
    }
    if (sends != m_sendStructure) {
        m_sendStructure = sends;
        rebuildSends();
    }
    for (auto card : m_cards)
        if (card)
            card->sync();
    if (m_mini)
        m_mini->sync(true);
}
void RackWidget::rebuild() {
    if (m_rebuilding)
        return;
    m_rebuilding = true;
    QPointer<QWidget> focused = QApplication::focusWidget();
    const bool hadFocus = focused && (focused == this || isAncestorOf(focused));
    const int scroll = m_scroll->horizontalScrollBar()->value();
    for (auto card : m_cards)
        if (card) {
            card->hide();
            card->setParent(this);
        }
    while (auto* item = m_row->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_groups.clear();
    QSet<QString> keep;
    const auto addCard = [&](const daw::InsertModel& model, QHBoxLayout* layout) {
        const auto id = QString::fromStdString(model.id);
        keep.insert(id);
        auto* card = m_cards.value(id).data();
        if (!card) {
            card = new RackDeviceCard(m_controller, m_channel, id, m_chain);
            m_cards[id] = card;
            connect(card, &RackDeviceCard::selected, this,
                    [this](const QString& id, auto mods) { select(id, mods); });
            connect(card, &RackDeviceCard::clicked, this, [this](const QString& id, auto mods) {
                if (!(mods & (Qt::ControlModifier | Qt::ShiftModifier)))
                    selectDevices({id});
            });
            connect(card, &RackDeviceCard::dragRequested, this, &RackWidget::startDrag);
            connect(card, &RackDeviceCard::contextRequested, this,
                    [this](const QString& id, const QPoint& at) { contextMenu(id, at); });
            connect(card, &RackDeviceCard::editorRequested, this, &RackWidget::editorRequested);
            connect(card, &RackDeviceCard::automationRequested, this, &RackWidget::automationRequested);
            connect(card, &RackDeviceCard::edited, this, &RackWidget::changed);
            connect(card, &RackDeviceCard::widthChanged, this, [this] {
                m_row->activate();
                m_chain->adjustSize();
            });
        }
        layout->addWidget(card);
        card->show();
    };
    const auto* track = m_controller->project().findTrack(m_channel.toStdString());
    const bool audio =
        m_channel == daw::EngineController::kMasterChannelId || (track && daw::carriesAudio(*track));
    if (!audio) {
        auto* empty = new QLabel(
            tr("This track has no audio channel.\nSelect an audio, instrument, bus or master channel."),
            m_chain);
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        m_row->addWidget(empty, 1);
    } else {
        if (track && track->instrument.isLoaded())
            addCard(track->instrument, m_row);
        else if (track && daw::trackAccepts(track->kind, daw::ClipKind::Midi)) {
            auto* instrument = new QPushButton(tr("+ Instrument"), m_chain);
            instrument->setFixedWidth(150);
            m_row->addWidget(instrument);
            connect(instrument, &QPushButton::clicked, this, [this, instrument] {
                addDevice(instrument->mapToGlobal(QPoint(0, instrument->height())), true);
            });
        }
        const auto* chain = m_controller->channelInserts(m_channel.toStdString());
        if (chain)
            for (std::size_t i = 0; i < chain->size();) {
                const daw::RackGroupModel* group = nullptr;
                for (const auto& g : m_controller->rackGroups(m_channel.toStdString()))
                    if (!g.insertIds.empty() && g.insertIds.front() == (*chain)[i].id) {
                        group = &g;
                        break;
                    }
                if (!group) {
                    addCard((*chain)[i++], m_row);
                    continue;
                }
                const auto id = QString::fromStdString(group->id);
                auto* frame = new QFrame(m_chain);
                frame->setObjectName("RackGroup");
                frame->setProperty("groupId", id);
                frame->setFrameShape(QFrame::StyledPanel);
                frame->setStyleSheet("#RackGroup{border:1px solid " +
                                     mixColors(th().accent, th().separator(), .55).name() +
                                     ";border-radius:6px;background:" + th().well().name() + ";}");
                m_groups[id] = frame;
                auto* col = new QVBoxLayout(frame);
                col->setContentsMargins(3, 1, 3, 3);
                col->setSpacing(2);
                auto* bar = new QHBoxLayout;
                bar->setContentsMargins(0, 0, 0, 0);
                bar->setSpacing(3);
                const bool collapsed = QSettings().value(settingsKey("group/" + id), false).toBool();
                auto* fold =
                    new ui::IconButton(collapsed ? icons::Glyph::ChevronRight : icons::Glyph::Chevron,
                                       tr("Expand or collapse group"), frame);
                fold->setButtonSize(24, 24);
                fold->setCheckable(true);
                fold->setChecked(!collapsed);
                bar->addWidget(fold);
                auto* title = new GroupHeader(QString::fromStdString(group->name) + " · " +
                                                  QString::number(group->insertIds.size()),
                                              frame);
                title->setToolTip(QString::fromStdString(group->name));
                bar->addWidget(title, 1);
                title->select = [this, id](auto mods) { select(id, mods, true); };
                title->click = [this, id](auto mods) {
                    if (!(mods & (Qt::ControlModifier | Qt::ShiftModifier))) {
                        m_selection.clear();
                        select(id, Qt::NoModifier, true);
                    }
                };
                title->drag = [this, id] {
                    select(id, Qt::NoModifier, true);
                    startDrag({});
                };
                title->menu = [this, id](auto at) { contextMenu(id, at, true); };
                auto* power = new ui::IconButton(icons::Glyph::Power, tr("Enable or bypass group"), frame);
                power->setButtonSize(24, 24);
                power->setCheckable(true);
                power->setObjectName("RackGroupPower");
                bool active = false;
                for (const auto& slotId : group->insertIds)
                    if (const auto* m = m_controller->insertModel(m_channel.toStdString(), slotId))
                        active |= !m->bypassed;
                power->setChecked(active);
                bar->addWidget(power);
                col->addLayout(bar);
                const auto ids = group->insertIds;
                connect(power, &QAbstractButton::clicked, this, [this, ids](bool on) {
                    finishEdits();
                    m_controller->bypassRackSelection(m_channel.toStdString(), ids, !on);
                    changed(false);
                });
                connect(fold, &QAbstractButton::clicked, this, [this, id, collapsed] {
                    QSettings().setValue(settingsKey("group/" + id), !collapsed);
                    QTimer::singleShot(0, this, &RackWidget::rebuild);
                });
                auto* devices = new QHBoxLayout;
                devices->setContentsMargins(0, 0, 0, 0);
                devices->setSpacing(4);
                col->addLayout(devices, 1);
                for (std::size_t n = 0; n < ids.size() && i < chain->size(); ++n, ++i) {
                    if (collapsed)
                        keep.insert(QString::fromStdString((*chain)[i].id));
                    else
                        addCard((*chain)[i], devices);
                }
                if (collapsed) {
                    frame->setFixedWidth(190);
                    auto* label = new QLabel(tr("%1 effects").arg(ids.size()), frame);
                    label->setWordWrap(true);
                    label->setAlignment(Qt::AlignCenter);
                    devices->addWidget(label);
                    title->setContextMenuPolicy(Qt::DefaultContextMenu);
                }
                m_row->addWidget(frame);
            }
        auto* add = new QPushButton(tr("+ Effect"), m_chain);
        add->setObjectName("RackAddDevice");
        add->setFixedWidth(100);
        add->setMinimumHeight(48);
        m_row->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this, add] {
            m_insertAt = -1;
            addDevice(add->mapToGlobal(QPoint(0, add->height())));
        });
        m_row->addStretch(1);
    }
    for (auto it = m_cards.begin(); it != m_cards.end();) {
        if (!keep.contains(it.key())) {
            delete it.value();
            m_selection.remove(it.key());
            it = m_cards.erase(it);
        } else
            ++it;
    }
    m_row->activate();
    m_scroll->horizontalScrollBar()->setValue(scroll);
    updateSelection();
    if (hadFocus) {
        if (focused && focused->isVisible())
            focused->setFocus(Qt::OtherFocusReason);
        else
            setFocus(Qt::OtherFocusReason);
    }
    m_rebuilding = false;
}
void RackWidget::finishEdits() {
    for (auto card : m_cards)
        if (card)
            card->finishEdits();
    if (m_mini)
        m_mini->finishEdits();
    for (auto knob : m_sendKnobs)
        if (knob)
            knob->finishEditing();
}
void RackWidget::refresh() {
    if (!isVisible() || m_rebuilding)
        return;
    sync();
    const auto visible = m_scroll->viewport()->rect();
    for (auto card : m_cards)
        if (card && card->isVisible() &&
            visible.intersects(QRect(card->mapTo(m_scroll->viewport(), QPoint()), card->size())))
            card->refresh();
    if (const auto* track = m_controller->project().findTrack(m_channel.toStdString()))
        for (const auto& send : track->sends) {
            auto* knob = m_sendKnobs.value(QString::fromStdString(send.id)).data();
            if (knob && !knob->isEditing()) {
                QSignalBlocker block(knob);
                double value = send.level;
                daw::AutomationTarget target;
                target.kind = daw::AutomationTargetKind::SendLevel;
                target.channelId = track->id;
                target.sendId = send.id;
                if (auto playing = m_controller->automationValueAtPlayhead(target))
                    value = *playing;
                knob->setValue(value);
            }
        }
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
        if (it.value())
            if (auto* power = it.value()->findChild<ui::IconButton*>("RackGroupPower")) {
                bool active = false;
                for (const auto& g : m_controller->rackGroups(m_channel.toStdString()))
                    if (g.id == it.key().toStdString())
                        for (const auto& id : g.insertIds)
                            if (const auto* model = m_controller->insertModel(m_channel.toStdString(), id))
                                active |= !model->bypassed;
                power->setChecked(active);
            }
}
void RackWidget::rebuildSends() {
    for (auto knob : m_sendKnobs)
        if (knob)
            knob->finishEditing();
    m_sendKnobs.clear();
    m_sendBefore.clear();
    delete m_sendScroll->takeWidget();
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(1, 0, 1, 0);
    column->setSpacing(3);
    if (const auto* track = m_controller->project().findTrack(m_channel.toStdString()))
        for (const auto& send : track->sends) {
            const auto id = QString::fromStdString(send.id);
            auto* frame = new QFrame(page);
            frame->setFrameShape(QFrame::StyledPanel);
            auto* row = new QHBoxLayout(frame);
            row->setContentsMargins(2, 2, 2, 2);
            row->setSpacing(2);
            auto* level = new ui::Knob({}, frame);
            level->setBare(36);
            level->setRange(0, daw::EngineController::kMaxSendLevel);
            level->setDefaultValue(.5);
            level->setValue(send.level);
            level->setFormatter([](double v) { return ui::formatGainDb(v); });
            level->setAutomatable(true);
            level->setAccessibleName(tr("Send level"));
            m_sendKnobs[id] = level;
            auto before = std::make_shared<std::optional<float>>();
            m_sendBefore[id] = before;
            row->addWidget(level);
            auto* col = new QVBoxLayout;
            col->setSpacing(0);
            auto* destination = new QToolButton(frame);
            const auto* target = m_controller->project().findTrack(send.destinationTrackId);
            destination->setText(target ? QString::fromStdString(target->name) : tr("Missing"));
            destination->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            destination->setMinimumHeight(24);
            destination->setToolTip(tr("Change send destination"));
            col->addWidget(destination);
            auto* switches = new QHBoxLayout;
            switches->setSpacing(1);
            auto* power = new ui::IconButton(icons::Glyph::Power, tr("Enable send"), frame);
            power->setButtonSize(24, 24);
            power->setCheckable(true);
            power->setChecked(send.enabled);
            auto* pre = new QToolButton(frame);
            pre->setText(send.preFader ? tr("Pre") : tr("Post"));
            pre->setMinimumSize(30, 24);
            pre->setToolTip(tr("Pre / post fader"));
            auto* remove = new ui::IconButton(icons::Glyph::Close, tr("Delete send"), frame);
            remove->setButtonSize(24, 24);
            switches->addWidget(power);
            switches->addWidget(pre);
            switches->addWidget(remove);
            col->addLayout(switches);
            row->addLayout(col, 1);
            column->addWidget(frame);
            connect(level, &ui::Knob::valueChanged, this, [this, id, before](double value) {
                if (!*before)
                    if (const auto* t = m_controller->project().findTrack(m_channel.toStdString()))
                        for (const auto& s : t->sends)
                            if (s.id == id.toStdString())
                                *before = s.level;
                m_controller->setSendLevel(m_channel.toStdString(), id.toStdString(), float(value));
            });
            connect(level, &ui::Knob::editFinished, this, [this, id, before] {
                if (*before) {
                    m_controller->commitSendLevelEdit(m_channel.toStdString(), id.toStdString(), **before);
                    before->reset();
                    changed(false);
                }
            });
            connect(level, &ui::Knob::automateRequested, this,
                    [this, id] { emit sendAutomationRequested(m_channel, id); });
            connect(destination, &QAbstractButton::clicked, this, [this, id, destination] {
                addSend(destination->mapToGlobal(QPoint(0, destination->height())), id);
            });
            connect(power, &QAbstractButton::clicked, this, [this, id](bool on) {
                m_controller->setSendEnabled(m_channel.toStdString(), id.toStdString(), on);
                changed();
            });
            connect(pre, &QAbstractButton::clicked, this, [this, id, pre] {
                if (const auto* t = m_controller->project().findTrack(m_channel.toStdString()))
                    for (const auto& s : t->sends)
                        if (s.id == id.toStdString()) {
                            m_controller->setSendPreFader(m_channel.toStdString(), id.toStdString(),
                                                          !s.preFader);
                            break;
                        }
                changed();
                m_sendStructure.clear();
            });
            connect(remove, &QAbstractButton::clicked, this, [this, id] {
                m_controller->removeSend(m_channel.toStdString(), id.toStdString());
                changed();
            });
        }
    auto* add = new QPushButton(tr("+ Send"), page);
    add->setMinimumHeight(30);
    column->addWidget(add);
    column->addStretch(1);
    connect(add, &QPushButton::clicked, this,
            [this, add] { addSend(add->mapToGlobal(QPoint(0, add->height()))); });
    m_sendScroll->setWidget(page);
}
void RackWidget::addSend(const QPoint& point, const QString& replace) {
    QMenu menu(this);
    const auto choose = [this, replace](const std::string& destination) {
        if (replace.isEmpty())
            m_controller->addSend(m_channel.toStdString(), destination);
        else
            m_controller->setSendDestination(m_channel.toStdString(), replace.toStdString(), destination);
        changed();
    };
    for (const auto& track : m_controller->project().tracks)
        if (track.kind == daw::TrackKind::Aux && track.id != m_channel.toStdString()) {
            auto* action = menu.addAction(QString::fromStdString(track.name));
            connect(action, &QAction::triggered, this, [choose, id = track.id] { choose(id); });
        }
    menu.addSeparator();
    connect(menu.addAction(tr("Create send track")), &QAction::triggered, this, [this, choose] {
        const auto undo = m_controller->beginUndoGroup();
        const auto id = m_controller->addTrack(daw::TrackKind::Aux, tr("Send").toStdString());
        if (!id.empty()) {
            choose(id);
            emit trackCreated();
        }
        m_controller->collapseUndo(undo, "Add Send");
    });
    menu.exec(point);
}
void RackWidget::changed(bool structural) {
    if (structural)
        QTimer::singleShot(0, this, &RackWidget::sync);
    emit edited(structural);
}
bool RackWidget::report(const audio::Result& result) {
    if (result)
        return true;
    QToolTip::showText(QCursor::pos(), QString::fromStdString(result.message()), this);
    return false;
}
void RackWidget::addDevice(const QPoint& at, bool instrument) {
    finishEdits();
    auto* menu = ui::buildPluginMenu(
        this, m_controller, instrument, [this, instrument](const daw::plugins::PluginDescriptor& descriptor) {
            if (instrument)
                m_controller->setTrackInstrumentPlugin(m_channel.toStdString(), descriptor);
            else {
                const auto* chain = m_controller->channelInserts(m_channel.toStdString());
                const auto index = m_insertAt >= 0 ? std::size_t(m_insertAt) : chain ? chain->size() : 0;
                const auto id = m_controller->addInsert(m_channel.toStdString(), descriptor, index);
                if (!id.empty())
                    selectDevices({QString::fromStdString(id)});
            }
            ui::rememberRecentPlugin(descriptor);
            changed();
        });
    menu->exec(at);
    menu->deleteLater();
}
QStringList RackWidget::selectedDevices() const {
    QStringList ids;
    if (const auto* track = m_controller->project().findTrack(m_channel.toStdString());
        track && track->instrument.isLoaded() &&
        m_selection.contains(QString::fromStdString(track->instrument.id)))
        ids.push_back(QString::fromStdString(track->instrument.id));
    if (const auto* chain = m_controller->channelInserts(m_channel.toStdString()))
        for (const auto& model : *chain)
            if (m_selection.contains(QString::fromStdString(model.id)))
                ids.push_back(QString::fromStdString(model.id));
    return ids;
}
void RackWidget::selectDevices(const QStringList& ids) {
    m_selection = QSet<QString>(ids.begin(), ids.end());
    updateSelection();
}
void RackWidget::select(const QString& id, Qt::KeyboardModifiers mods, bool group) {
    QStringList chosen{id};
    if (group)
        for (const auto& g : m_controller->rackGroups(m_channel.toStdString()))
            if (g.id == id.toStdString()) {
                chosen.clear();
                for (const auto& member : g.insertIds)
                    chosen.push_back(QString::fromStdString(member));
            }
    if (mods.testFlag(Qt::ShiftModifier) && !m_anchor.isEmpty()) {
        QStringList order;
        if (const auto* track = m_controller->project().findTrack(m_channel.toStdString());
            track && track->instrument.isLoaded())
            order.push_back(QString::fromStdString(track->instrument.id));
        if (const auto* chain = m_controller->channelInserts(m_channel.toStdString()))
            for (const auto& slot : *chain)
                order.push_back(QString::fromStdString(slot.id));
        const int a = order.indexOf(m_anchor), b = order.indexOf(chosen.value(0));
        if (a >= 0 && b >= 0) {
            m_selection.clear();
            for (int i = std::min(a, b); i <= std::max(a, b); ++i)
                m_selection.insert(order[i]);
            for (const auto& member : chosen)
                m_selection.insert(member);
        }
    } else if (mods.testFlag(Qt::ControlModifier)) {
        const bool groupSelected =
            group && std::all_of(chosen.begin(), chosen.end(),
                                 [this](const auto& member) { return m_selection.contains(member); });
        for (const auto& member : chosen) {
            if (group ? groupSelected : m_selection.contains(member))
                m_selection.remove(member);
            else
                m_selection.insert(member);
        }
        m_anchor = chosen.value(0);
    } else {
        const bool all = std::all_of(chosen.begin(), chosen.end(),
                                     [this](const auto& member) { return m_selection.contains(member); });
        if (!all)
            m_selection = QSet<QString>(chosen.begin(), chosen.end());
        m_anchor = chosen.value(0);
    }
    m_insertAt = -1;
    updateSelection();
}
void RackWidget::updateSelection() {
    for (auto it = m_cards.begin(); it != m_cards.end(); ++it)
        if (it.value())
            it.value()->setSelected(m_selection.contains(it.key()));
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
        if (it.value()) {
            bool all = true;
            for (const auto& g : m_controller->rackGroups(m_channel.toStdString()))
                if (g.id == it.key().toStdString())
                    for (const auto& id : g.insertIds)
                        all &= m_selection.contains(QString::fromStdString(id));
            it.value()->setProperty("selected", all);
        }
    updateGroupAppearance();
}
void RackWidget::updateGroupAppearance() {
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        auto* frame = it.value().data();
        if (!frame)
            continue;
        const bool target = it.key() == m_dropGroup;
        const bool selected = frame->property("selected").toBool();
        const auto border = target || selected ? th().accent : mixColors(th().accent, th().separator(), .55);
        const auto background = target ? mixColors(th().accent, th().well(), .13) : th().well();
        const QString style = "#RackGroup{border:1px solid " + border.name() +
                              ";border-radius:6px;background:" + background.name() + ";}";
        if (frame->styleSheet() != style)
            frame->setStyleSheet(style);
        frame->setToolTip(target ? tr("Insert into this group") : QString{});
    }
}
void RackWidget::clearDrop() {
    m_marker->hide();
    m_scrollTimer->stop();
    m_dropGroup.clear();
    updateGroupAppearance();
}
bool RackWidget::ownsFocus() const {
    auto* focus = QApplication::focusWidget();
    return isVisible() && focus && (focus == this || isAncestorOf(focus)) && !textInput(focus);
}
bool RackWidget::command(const QString& action) {
    if (!ownsFocus())
        return false;
    finishEdits();
    const auto selected = selectedDevices();
    const auto ids = ui::rack::ids(selected);
    const auto channel = m_channel.toStdString();
    const auto* chain = m_controller->channelInserts(channel);
    if (action == "all") {
        QStringList all;
        if (const auto* track = m_controller->project().findTrack(channel);
            track && track->instrument.isLoaded())
            all.push_back(QString::fromStdString(track->instrument.id));
        if (chain)
            for (const auto& slot : *chain)
                all.push_back(QString::fromStdString(slot.id));
        selectDevices(all);
        return true;
    }
    if (action == "undo" || action == "redo") {
        if (action == "undo")
            m_controller->undo();
        else
            m_controller->redo();
        changed();
        return true;
    }
    if (action == "group" || action == "ungroup") {
        groupSelection(action == "ungroup");
        return true;
    }
    if (action == "copy" || action == "cut" || action == "duplicate") {
        if (ids.empty())
            return true;
        daw::EngineController::ChannelSnapshot snapshot;
        if (!report(m_controller->captureRackSelection(channel, ids, snapshot)))
            return true;
        if (action == "duplicate") {
            if (snapshot.instrument) {
                QToolTip::showText(QCursor::pos(),
                                   tr("A channel holds one instrument. Use Copy to channel to duplicate it."),
                                   this);
                return true;
            }
            std::size_t at = 0;
            if (chain)
                for (std::size_t i = 0; i < chain->size(); ++i)
                    if (m_selection.contains(QString::fromStdString((*chain)[i].id)))
                        at = i + 1;
            std::vector<std::string> added;
            if (report(m_controller->pasteRackSelection(channel, snapshot, at, {}, &added))) {
                QStringList selected;
                for (const auto& id : added)
                    selected.push_back(QString::fromStdString(id));
                selectDevices(selected);
                changed();
            }
        } else {
            if (action == "cut") {
                if (m_controller->removeRackSelection(channel, ids)) {
                    snapshot.rackCutSource = channel;
                    m_controller->setChannelClipboard(std::move(snapshot));
                    selectDevices({});
                    changed();
                }
            } else
                m_controller->setChannelClipboard(std::move(snapshot));
        }
        return true;
    }
    if (action == "paste") {
        std::size_t at = chain ? chain->size() : 0;
        if (m_insertAt >= 0)
            at = std::size_t(m_insertAt);
        else if (!selected.isEmpty() && chain)
            for (std::size_t i = 0; i < chain->size(); ++i)
                if (m_selection.contains(QString::fromStdString((*chain)[i].id)))
                    at = i + 1;
        std::vector<std::string> added;
        if (!m_controller->channelClipboard().empty() &&
            report(m_controller->pasteRackSelection(channel, m_controller->channelClipboard(), at, {},
                                                    &added))) {
            QStringList selection;
            for (const auto& id : added)
                selection.push_back(QString::fromStdString(id));
            selectDevices(selection);
            changed();
        }
        return true;
    }
    if (action == "delete") {
        if (!ids.empty() && m_controller->removeRackSelection(channel, ids)) {
            selectDevices({});
            changed();
        }
        return true;
    }
    return false;
}
void RackWidget::groupSelection(bool ungroup) {
    if (m_controller->hasCloudProjectBinding()) {
        QToolTip::showText(QCursor::pos(), tr("Rack groups are available in local projects."), this);
        return;
    }
    if (ungroup) {
        const auto groups = m_controller->rackGroups(m_channel.toStdString());
        const auto undo = m_controller->beginUndoGroup();
        bool edited = false;
        for (const auto& group : groups)
            if (std::all_of(group.insertIds.begin(), group.insertIds.end(), [this](const auto& id) {
                    return m_selection.contains(QString::fromStdString(id));
                }))
                edited |= m_controller->removeRackGroup(m_channel.toStdString(), group.id);
        m_controller->collapseUndo(undo, "Ungroup Plugins");
        if (edited)
            changed();
    } else {
        if (m_controller->createRackGroup(m_channel.toStdString(), ui::rack::ids(selectedDevices()),
                                          tr("Group").toStdString()))
            changed();
        else
            QToolTip::showText(QCursor::pos(), tr("Select at least two adjacent, ungrouped effects."), this);
    }
}
void RackWidget::transferTo(const QString& destination, bool copy) {
    finishEdits();
    const auto selected = selectedDevices();
    const auto* chain = m_controller->channelInserts(destination.toStdString());
    std::vector<std::string> landed;
    if (report(m_controller->transferRackSelection(m_channel.toStdString(), ui::rack::ids(selected),
                                                   destination.toStdString(), chain ? chain->size() : 0, copy,
                                                   {}, &landed))) {
        QStringList ids;
        for (const auto& id : landed)
            ids.push_back(QString::fromStdString(id));
        setChannel(destination);
        selectDevices(ids);
        emit channelRequested(destination);
        changed();
    }
}
void RackWidget::contextMenu(const QString& id, const QPoint& at, bool group) {
    if (!id.isEmpty())
        select(id, Qt::NoModifier, group);
    setFocus();
    QMenu menu(this);
    const auto add = [&](const QString& label, const QString& action) {
        auto* a = menu.addAction(label);
        connect(a, &QAction::triggered, this, [this, action] {
            setFocus();
            command(action);
        });
        return a;
    };
    add(tr("Copy"), "copy");
    add(tr("Cut"), "cut");
    add(tr("Paste"), "paste");
    add(tr("Duplicate"), "duplicate");
    menu.addSeparator();
    const auto* channelTrack = m_controller->project().findTrack(m_channel.toStdString());
    const bool instrumentSelected = channelTrack && channelTrack->instrument.isLoaded() &&
                                    m_selection.contains(QString::fromStdString(channelTrack->instrument.id));
    add(tr("Group effects"), "group")
        ->setEnabled(!m_controller->hasCloudProjectBinding() && selectedDevices().size() >= 2);
    add(tr("Ungroup"), "ungroup")->setEnabled(!m_controller->hasCloudProjectBinding());
    if (group) {
        auto* rename = menu.addAction(tr("Rename group…"));
        connect(rename, &QAction::triggered, this, [this, id] {
            QString old;
            for (const auto& g : m_controller->rackGroups(m_channel.toStdString()))
                if (g.id == id.toStdString())
                    old = QString::fromStdString(g.name);
            bool accepted = false;
            const auto name =
                QInputDialog::getText(this, tr("Group name"), tr("Name"), QLineEdit::Normal, old, &accepted);
            if (accepted && m_controller->renameRackGroup(m_channel.toStdString(), id.toStdString(),
                                                          name.trimmed().toStdString()))
                changed();
        });
    }
    if (!selectedDevices().isEmpty())
        for (bool copy : {false, true}) {
            auto* channels = menu.addMenu(copy ? tr("Copy to channel") : tr("Move to channel"));
            for (const auto& track : m_controller->project().tracks)
                if (daw::carriesAudio(track) && track.id != m_channel.toStdString() &&
                    (!instrumentSelected || daw::trackAccepts(track.kind, daw::ClipKind::Midi))) {
                    auto* a = channels->addAction(QString::fromStdString(track.name));
                    connect(a, &QAction::triggered, this,
                            [this, copy, destination = QString::fromStdString(track.id)] {
                                transferTo(destination, copy);
                            });
                }
            if (!instrumentSelected && m_channel != daw::EngineController::kMasterChannelId)
                connect(channels->addAction(tr("Master")), &QAction::triggered, this,
                        [this, copy] { transferTo(daw::EngineController::kMasterChannelId, copy); });
        }
    menu.addSeparator();
    add(tr("Delete"), "delete");
    if (!group && !id.isEmpty()) {
        connect(menu.addAction(tr("Replace device…")), &QAction::triggered, this,
                [this, id, at, instrumentSelected] {
                    finishEdits();
                    auto* picker = ui::buildPluginMenu(
                        this, m_controller, instrumentSelected,
                        [this, id, instrumentSelected](const daw::plugins::PluginDescriptor& descriptor) {
                            if (instrumentSelected)
                                m_controller->setTrackInstrumentPlugin(m_channel.toStdString(), descriptor);
                            else
                                m_controller->replaceInsert(m_channel.toStdString(), id.toStdString(),
                                                            descriptor);
                            changed();
                        });
                    picker->exec(at);
                    picker->deleteLater();
                });
    }
    connect(menu.addAction(tr("Add effect…")), &QAction::triggered, this, [this, at] { addDevice(at); });
    menu.exec(at);
}
void RackWidget::startDrag(const QString& id) {
    if (!id.isEmpty() && !m_selection.contains(id))
        selectDevices({id});
    finishEdits();
    const auto ids = selectedDevices();
    if (ids.isEmpty())
        return;
    auto* drag = new QDrag(this);
    auto* data = new QMimeData;
    data->setData(ui::rack::mime, ui::rack::encode(m_channel, ids));
    drag->setMimeData(data);
    QPixmap pixmap(160, 32);
    pixmap.fill(th().surface);
    QPainter p(&pixmap);
    p.setPen(th().accent);
    p.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    p.drawText(pixmap.rect(), Qt::AlignCenter, tr("%1 devices").arg(ids.size()));
    p.end();
    drag->setPixmap(pixmap);
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    clearDrop();
    sync();
}
void RackWidget::updateDrop(const QPoint& global) {
    m_dragPosition = global;
    const auto point = m_chain->mapFromGlobal(global);
    const auto* chain = m_controller->channelInserts(m_channel.toStdString());
    m_dropAt = chain ? int(chain->size()) : 0;
    m_dropGroup.clear();
    int markerX = 0;
    if (chain)
        for (int i = 0; i < int(chain->size()); ++i) {
            const auto id = QString::fromStdString((*chain)[i].id);
            QWidget* widget = m_cards.value(id).data();
            QString groupId;
            for (const auto& g : m_controller->rackGroups(m_channel.toStdString()))
                if (std::find(g.insertIds.begin(), g.insertIds.end(), (*chain)[i].id) != g.insertIds.end()) {
                    groupId = QString::fromStdString(g.id);
                    if (!widget || !widget->isVisible())
                        widget = m_groups.value(groupId);
                    break;
                }
            if (!widget || !widget->isVisible())
                continue;
            const QRect r(widget->mapTo(m_chain, QPoint()), widget->size());
            if (point.x() < r.center().x()) {
                m_dropAt = i;
                markerX = r.left() - 3;
                if (!groupId.isEmpty() && point.x() > r.left() + 8)
                    m_dropGroup = groupId;
                break;
            }
            markerX = r.right() + 3;
            if (r.contains(point) && !groupId.isEmpty()) {
                m_dropGroup = groupId;
                m_dropAt = i + 1;
            }
        }
    m_marker->setGeometry(markerX, 2, 3, std::max(10, m_chain->height() - 4));
    m_marker->show();
    m_marker->raise();
    updateGroupAppearance();
}
bool RackWidget::eventFilter(QObject* object, QEvent* event) {
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) && ownsFocus()) {
        auto* key = static_cast<QKeyEvent*>(event);
        const auto action = keyCommand(key);
        if (!action.isEmpty()) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            command(action);
            return true;
        }
    }
    if (object != m_chain && object != m_scroll->viewport())
        return QWidget::eventFilter(object, event);
    auto* surface = static_cast<QWidget*>(object);
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* e = static_cast<QDragMoveEvent*>(event);
        if (ui::rack::decode(e->mimeData()).valid) {
            updateDrop(surface->mapToGlobal(e->position().toPoint()));
            e->setDropAction(e->modifiers().testFlag(Qt::AltModifier) ? Qt::CopyAction : Qt::MoveAction);
            e->accept();
            m_scrollTimer->start();
            return true;
        }
    }
    if (event->type() == QEvent::DragLeave) {
        clearDrop();
        return true;
    }
    if (event->type() == QEvent::Drop) {
        auto* e = static_cast<QDropEvent*>(event);
        const auto drag = ui::rack::decode(e->mimeData());
        if (drag.valid) {
            updateDrop(surface->mapToGlobal(e->position().toPoint()));
            std::vector<std::string> landed;
            const bool copy = e->modifiers().testFlag(Qt::AltModifier);
            if (report(m_controller->transferRackSelection(
                    drag.channel.toStdString(), ui::rack::ids(drag.ids), m_channel.toStdString(),
                    std::size_t(std::max(0, m_dropAt)), copy, m_dropGroup.toStdString(), &landed))) {
                QStringList ids;
                for (const auto& id : landed)
                    ids.push_back(QString::fromStdString(id));
                selectDevices(ids);
                setFocus();
                changed();
                e->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
                e->accept();
            }
            clearDrop();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        auto* e = static_cast<QMouseEvent*>(event);
        if (e->button() == Qt::RightButton) {
            updateDrop(surface->mapToGlobal(e->position().toPoint()));
            m_insertAt = m_dropAt;
            contextMenu({}, e->globalPosition().toPoint());
            clearDrop();
            return true;
        }
        if (e->button() == Qt::LeftButton) {
            setFocus();
            m_rubberOrigin = m_chain->mapFromGlobal(e->globalPosition().toPoint());
            m_rubberBefore = e->modifiers().testFlag(Qt::ControlModifier) ? m_selection : QSet<QString>{};
            m_selection = m_rubberBefore;
            updateDrop(e->globalPosition().toPoint());
            m_insertAt = m_dropAt;
            clearDrop();
            m_rubberActive = true;
            m_rubber->setGeometry(QRect(m_rubberOrigin, QSize()));
            m_rubber->show();
            updateSelection();
            return true;
        }
    }
    if (event->type() == QEvent::MouseMove && m_rubberActive) {
        auto* e = static_cast<QMouseEvent*>(event);
        const QRect rect =
            QRect(m_rubberOrigin, m_chain->mapFromGlobal(e->globalPosition().toPoint())).normalized();
        m_rubber->setGeometry(rect);
        m_selection = m_rubberBefore;
        for (auto it = m_cards.begin(); it != m_cards.end(); ++it)
            if (it.value() && it.value()->isVisible() &&
                rect.intersects(QRect(it.value()->mapTo(m_chain, QPoint()), it.value()->size())))
                m_selection.insert(it.key());
        for (const auto& group : m_controller->rackGroups(m_channel.toStdString())) {
            const auto id = QString::fromStdString(group.id);
            auto* frame = m_groups.value(id).data();
            if (frame && QSettings().value(settingsKey("group/" + id), false).toBool() &&
                rect.intersects(QRect(frame->mapTo(m_chain, QPoint()), frame->size())))
                for (const auto& member : group.insertIds)
                    m_selection.insert(QString::fromStdString(member));
        }
        updateSelection();
        return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && m_rubberActive) {
        m_rubberActive = false;
        m_rubber->hide();
        return true;
    }
    return QWidget::eventFilter(object, event);
}
void RackWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    sync();
    m_timer->start();
    refresh();
}
void RackWidget::hideEvent(QHideEvent* e) {
    saveView();
    finishEdits();
    m_timer->stop();
    m_scrollTimer->stop();
    QWidget::hideEvent(e);
}
void RackWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    const bool narrow = width() < 850;
    m_left->setFixedWidth(narrow ? 148 : 176);
    m_sends->setFixedWidth(narrow ? 132 : 156);
    if (m_mini)
        m_mini->setStripWidth(m_left->width());
}
