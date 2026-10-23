#include "RackDeviceCard.hpp"
#include "Controls.hpp"
#include "EqualizerStyle.hpp"
#include "Icons.hpp"
#include "RackParameterBinding.hpp"
#include "RackBuiltinView.hpp"
#include "RackModulationPanel.hpp"
#include "ScrollMotion.hpp"
#include "Theme.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
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
#include <array>

namespace {
class PluginLauncher final : public QAbstractButton, public ui::graphics::ScenePaintSource {
public:
    explicit PluginLauncher(QWidget* parent) : QAbstractButton(parent) {
        setObjectName("RackDeviceOpen");
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setMinimumSize(250, 100);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void paintScene(QPainter& p, const QRegion&) override {
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = th();
        const double hover = m_hover.value();
        const QRectF tile = QRectF(rect()).adjusted(8.5, 8.5, -8.5, -8.5);
        p.setPen(QPen(hasFocus() ? t.accent : mixColors(t.separator(), t.accent, isChecked() ? .65 : .25 * hover), 1));
        QLinearGradient fill(tile.topLeft(), tile.bottomRight());
        fill.setColorAt(0, mixColors(t.well(), t.accent, (isChecked() ? .12 : .035) + hover * .035));
        fill.setColorAt(1, t.panelBottom());
        p.setBrush(fill); p.drawRoundedRect(tile, 8, 8);
        const bool compact = height() < 160;
        const QPointF center(tile.center().x(), tile.top() + (compact ? 25 : tile.height() * .36) + (isDown() ? 1 : 0));
        const QRectF window(center.x() - (compact ? 24 : 31), center.y() - (compact ? 17 : 23), compact ? 48 : 62, compact ? 34 : 46);
        p.setPen(QPen(mixColors(t.textSecondary, t.accent, isChecked() ? .8 : .35 + hover * .4), 1.4));
        p.setBrush(mixColors(t.surface, t.accent, isChecked() ? .1 : .025));
        p.drawRoundedRect(window, 5, 5);
        p.drawLine(window.topLeft() + QPointF(1, 11), window.topRight() + QPointF(-1, 11));
        icons::paint(p, isChecked() ? icons::Glyph::Check : icons::Glyph::RackControls,
                     QRectF(center.x() - 11, center.y() - 6, 22, 22), t.accent);
        const double textY = center.y() + (compact ? 23 : 34);
        QFont title = font(); title.setPixelSize(compact ? 12 : 13); title.setWeight(QFont::DemiBold); p.setFont(title);
        p.setPen(t.textPrimary);
        p.drawText(QRectF(tile.left() + 6, textY, tile.width() - 12, 20), Qt::AlignCenter, text());
        title.setPixelSize(10); title.setWeight(QFont::Normal); p.setFont(title); p.setPen(t.textSecondary);
        p.drawText(QRectF(tile.left() + 6, textY + 21, tile.width() - 12, 16), Qt::AlignCenter,
                   isChecked() ? QCoreApplication::translate("RackDeviceCard", "Click to close")
                               : QCoreApplication::translate("RackDeviceCard", "Show the full interface"));
    }
protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); paintScene(p, QRegion(rect())); }
    void enterEvent(QEnterEvent* e) override {
        QAbstractButton::enterEvent(e);
        if (ui::ScrollPreferences::instance().reducedMotion()) m_hover.jumpTo(1); else m_hover.setTarget(1);
    }
    void leaveEvent(QEvent* e) override {
        QAbstractButton::leaveEvent(e);
        if (ui::ScrollPreferences::instance().reducedMotion()) m_hover.jumpTo(0); else m_hover.setTarget(0);
    }
private:
    ui::Fade m_hover{this, 120};
};

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
    m_pageBar = new QWidget(m_header);
    auto* modes = new QHBoxLayout(m_pageBar);
    modes->setContentsMargins(0, 0, 0, 0); modes->setSpacing(1);
    m_pages = new QButtonGroup(this);
    const std::array<std::pair<icons::Glyph, QString>, 3> pages{{
        {icons::Glyph::RackFace, tr("Plugin")}, {icons::Glyph::RackControls, tr("Parameters")},
        {icons::Glyph::RackModulation, tr("Modulation")}}};
    for (int i = 0; i < int(pages.size()); ++i) {
        auto* mode = new ui::IconButton(pages[i].first, pages[i].second, m_pageBar);
        mode->setObjectName(QStringLiteral("RackDevicePage%1").arg(i));
        mode->setButtonSize(24, 24); mode->setCheckable(true); mode->setFocusPolicy(Qt::StrongFocus);
        mode->setAccessibleName(pages[i].second); m_pages->addButton(mode, i); modes->addWidget(mode);
    }
    m_pages->button(0)->setChecked(true);
    connect(m_pages, &QButtonGroup::idClicked, this, [this](int page) { setPage(Page(page)); });
    m_expand = new ui::IconButton(icons::Glyph::RackExpand, tr("Show all parameters"), m_header);
    m_expand->setButtonSize(24, 24);
    m_expand->setObjectName("RackDeviceExpandParameters");
    m_expand->setCheckable(true);
    m_headerEditor = new ui::IconButton(icons::Glyph::Detach, tr("Open full editor"), m_header);
    m_headerEditor->setObjectName("RackDeviceOpenEditor");
    m_headerEditor->setButtonSize(24, 24);
    m_headerEditor->setGlyphSize(16);
    m_headerEditor->setCheckable(true);
    m_headerEditor->setFocusPolicy(Qt::StrongFocus);
    m_headerEditor->setAccessibleName(tr("Open full editor"));
    header->addWidget(m_power);
    header->addWidget(m_title, 1);
    header->addWidget(m_pageBar);
    header->addWidget(m_expand);
    header->addWidget(m_headerEditor);
    header->addWidget(m_fold);
    m_root->addWidget(m_header);
    connect(m_power, &QAbstractButton::clicked, this, [this](bool on) {
        finishEdits();
        m_controller->setInsertBypassed(m_channel.toStdString(), m_slot.toStdString(), !on);
        emit edited(false);
        sync();
    });
    connect(m_expand, &QAbstractButton::clicked, this, [this] {
        setCollapsed(false);
        setExpanded(!m_expanded);
    });
    connect(m_fold, &QAbstractButton::clicked, this, [this] { setCollapsed(false); });
    connect(m_headerEditor, &QAbstractButton::clicked, this, [this] {
        finishEdits(); emit editorToggleRequested(m_channel, m_slot); refresh(false);
    });
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
    m_recent = QSettings().value(expansionKey(c, m_channel, m_slot) + "/recent").toStringList();
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
    if (m_modulation) m_modulation->finishEdits();
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
    m_internal = model->format == daw::PluginFormat::Internal;
    m_headerEditor->setVisible(m_internal && !m_collapsed);
    const auto identity = m_controller->insertIdentity(m_channel.toStdString(), m_slot.toStdString());
    const auto uid = QString::fromStdString(model->uid);
    const auto runtimeState =
        m_controller->insertRuntimeStatus(m_channel.toStdString(), m_slot.toStdString()).state;
    const bool engineOff = !m_controller->audioEnabled();
    const bool rebuild = !m_body || engineOff != m_engineOff || runtimeState != m_runtimeState || identity != m_identity ||
                         m_side != model->editorChannel || uid != m_uid || m_pins != model->rackParameterIds;
    if (rebuild) {
        finishEdits();
        m_runtimeState = runtimeState;
        m_engineOff = engineOff;
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
    m_headerEditor->setEnabled(!m_engineOff && m_binding && m_binding->available());
    m_sidechain->setText(model->sidechainTrackIds.empty() ? tr("SC")
                                                          : tr("SC %1").arg(model->sidechainTrackIds.size()));
    m_sidechain->setToolTip(sidechain ? tr("Choose sidechain sources")
                                      : tr("This device has no sidechain input"));
    m_syncing = false;
}
void RackDeviceCard::refresh(bool synchronize) {
    if (synchronize) sync();
    if (m_collapsed) return;
    const bool opened = editorIsOpen && editorIsOpen();
    m_headerEditor->setChecked(opened);
    m_headerEditor->setToolTip(opened ? tr("Close plugin") : tr("Open full editor"));
    m_headerEditor->setAccessibleName(m_headerEditor->toolTip());
    if (m_openEditor) {
        m_openEditor->setChecked(opened);
        m_openEditor->setText(opened ? tr("Plugin is open") : tr("Open plugin"));
        m_openEditor->setAccessibleName(opened ? tr("Close plugin") : tr("Open plugin"));
    }
    refreshRecentParameters();
    if (m_modulation) m_modulation->refresh();
    if (m_builtin) m_builtin->refresh();
    if (m_binding)
        m_binding->refresh();
}
void RackDeviceCard::setPage(Page page) {
    if (page == m_page) return;
    finishEdits();
    m_page = page;
    m_pages->button(int(page))->setChecked(true);
    rebuildControls();
    emit widthChanged();
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
    m_expand->setVisible(!m_collapsed);
    m_headerEditor->setVisible(m_internal && !m_collapsed);
    m_pageBar->setVisible(!m_collapsed);
    m_fold->setVisible(m_collapsed);
    m_bodyScroll->setVisible(!m_collapsed);
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
    updateWidth();
}
void RackDeviceCard::updateWidth() {
    const int face = m_builtin ? std::max(288, RackBuiltinView::preferredWidth(m_uid) + 24) : 288;
    setFixedWidth(m_collapsed ? 36 : face + (m_expanded ? 272 : 0) + (m_modulation ? m_modulation->width() + 6 : 0));
}
void RackDeviceCard::rebuildControls() {
    if (m_bodyScroll) {
        m_root->removeWidget(m_bodyScroll);
        delete m_bodyScroll;
        m_body = nullptr;
    }
    delete m_binding;
    m_binding = nullptr;
    m_modulation = nullptr; m_builtin = nullptr; m_openEditor = nullptr; m_pinned = nullptr; m_pinnedGrid = nullptr;
    m_displayed.clear();
    m_binding = new RackParameterBinding(m_controller, m_channel, m_slot, this);
    connect(m_binding, &RackParameterBinding::edited, this, [this] { emit edited(false); });
    connect(m_binding, &RackParameterBinding::automationRequested, this,
            [this](const QString& id) { emit automationRequested(m_channel, m_slot, id); });
    m_bodyScroll = new QScrollArea(this);
    m_bodyScroll->setObjectName("RackDeviceBodyScroll");
    m_bodyScroll->setFrameShape(QFrame::NoFrame);
    m_bodyScroll->setWidgetResizable(true);
    m_bodyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_bodyScroll->setMinimumHeight(0);
    m_bodyScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_body = new QWidget;
    m_bodyScroll->setWidget(m_body);
    m_body->setObjectName("RackDeviceBody");
    auto* row = new QHBoxLayout(m_body);
    const int verticalInset = m_page == Page::Editor && m_uid == "daw.equalizer" ? 3 : 1;
    row->setContentsMargins(3, verticalInset, 3, verticalInset);
    row->setSpacing(4);
    m_pages->button(int(Page::Modulation))->setEnabled(!m_controller->hasCloudProjectBinding());
    m_expand->setChecked(m_expanded);
    m_expand->setToolTip(m_expanded ? tr("Hide all parameters") : tr("Show all parameters"));
    if (!m_binding->available() || m_runtimeState != daw::AudioPluginRuntimeState::Local) {
        auto* col = new QVBoxLayout;
        auto* message = new QLabel(m_engineOff ? tr("Engine off") : tr("Device unavailable"), m_body);
        message->setWordWrap(true);
        col->addWidget(message);
        const auto status = m_controller->insertRuntimeStatus(m_channel.toStdString(), m_slot.toStdString());
        auto* detail = new QLabel(m_engineOff ? tr("Enable this project's engine to use this device.")
                                             : QString::fromStdString(status.detail), m_body);
        detail->setWordWrap(true);
        detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(detail, 1);
        auto* retry = new QToolButton(m_body);
        retry->setVisible(!m_engineOff);
        retry->setText(tr("Restore device"));
        retry->setEnabled(status.canRetry);
        col->addWidget(retry);
        connect(retry, &QAbstractButton::clicked, this, [this] {
            m_controller->retryInsertRecovery(m_channel.toStdString(), m_slot.toStdString(), m_identity);
            sync();
            emit edited(true);
        });
        row->addLayout(col);
    } else if (m_page == Page::Editor && RackBuiltinView::supports(m_uid)) {
        m_builtin = new RackBuiltinView(m_binding, m_uid, m_body);
        m_builtin->setObjectName("RackBuiltinFace");
        m_builtin->setFixedWidth(std::max(278, RackBuiltinView::preferredWidth(m_uid)));
        row->addWidget(m_builtin);
    } else if (m_page == Page::Editor) {
        m_openEditor = new PluginLauncher(m_body);
        m_openEditor->setFixedWidth(278);
        m_openEditor->setText(tr("Open plugin"));
        m_openEditor->setAccessibleName(tr("Open plugin"));
        connect(m_openEditor, &QAbstractButton::clicked, this, [this] {
            finishEdits(); emit editorToggleRequested(m_channel, m_slot); refresh(false);
        });
        row->addWidget(m_openEditor);
    } else {
        if (m_page == Page::Modulation) {
            m_body->setProperty("modulationDepthEditing", true);
            m_modulation = new RackModulationPanel(m_controller, m_body, m_slot);
            m_modulation->setChannel(m_channel);
            connect(m_modulation, &RackModulationPanel::edited, this, &RackDeviceCard::edited);
            connect(m_modulation, &RackModulationPanel::layoutChanged, this, [this] { updateWidth(); emit widthChanged(); });
            row->addWidget(m_modulation);
        }
        m_pinned = new QWidget(m_body);
        m_pinned->setObjectName("RackRecentParameters");
        m_pinned->setFixedWidth(278);
        m_pinnedGrid = new QGridLayout(m_pinned);
        m_pinnedGrid->setContentsMargins(2, 2, 2, 0);
        m_pinnedGrid->setSpacing(0); m_pinnedGrid->setRowStretch(2, 1);
        row->addWidget(m_pinned);
        refreshRecentParameters();
    }
    if (m_expanded && m_binding->available()) {
        auto* col = new QVBoxLayout;
        auto* search = new QLineEdit(m_body);
        search->setObjectName("RackParameterSearch");
        search->setPlaceholderText(tr("Find a parameter…"));
        search->setAccessibleName(tr("Find a parameter"));
        auto* grid = new ParameterGrid(m_binding, m_body);
        col->addWidget(search); col->addWidget(grid, 1); row->addLayout(col, 1);
        connect(search, &QLineEdit::textChanged, grid, [grid](const QString& text) { grid->filter(text); });
    }
    m_root->insertWidget(1, m_bodyScroll, 1);
    updateCollapsed();
}
void RackDeviceCard::refreshRecentParameters() {
    if (!m_binding) return;
    for (auto* knob : m_body->findChildren<ui::Knob*>())
        if (knob->isEditing() || knob->property("modulationDepthDragging").toBool()) return;
    const auto& touched = m_controller->recentRackParameters(m_slot.toStdString());
    if (!touched.empty()) {
        QStringList recent;
        for (const auto& id : touched) recent.push_back(QString::fromStdString(id));
        if (recent != m_recent) {
            m_recent = recent;
            QSettings().setValue(expansionKey(m_controller, m_channel, m_slot) + "/recent", m_recent);
        }
    }
    if (!m_pinnedGrid) return;
    QStringList ids;
    const auto append = [&](const QString& id) {
        if (ids.size() < 8 && !ids.contains(id) && m_binding->info(id)) ids.push_back(id);
    };
    if (!m_recent.isEmpty()) append(m_recent.front());
    for (const auto& id : m_pins) append(QString::fromStdString(id));
    for (const auto& id : m_recent) append(id);
    for (const auto& info : m_binding->parameters()) append(QString::fromStdString(info.id));
    if (ids == m_displayed) return;
    m_binding->finishAll();
    const auto* focused = QApplication::focusWidget();
    const QString focusedId = focused && m_pinned->isAncestorOf(focused)
        ? focused->property("modulationParameter").toString() : QString{};
    while (auto* item = m_pinnedGrid->takeAt(0)) { delete item->widget(); delete item; }
    m_displayed = ids;
    for (int i = 0; i < ids.size(); ++i) {
        auto* knob = m_binding->knob(ids[i], m_pinned, 66);
        knob->setFixedSize(66, 58);
        knob->setProperty("recentParameterIndex", i);
        if (i == 0 && !m_recent.isEmpty()) knob->setToolTip(tr("Last touched") + " · " + knob->accessibleName());
        m_pinnedGrid->addWidget(knob, i / 4, i % 4, Qt::AlignTop);
        if (ids[i] == focusedId) { knob->show(); knob->setFocus(Qt::OtherFocusReason); }
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
