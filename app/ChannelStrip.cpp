#include "ChannelStrip.hpp"
#include "ChannelStripPresets.hpp"
#include "PluginPickerMenu.hpp"
#include "Controls.hpp"
#include "FileTypes.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QContextMenuEvent>
#include <QApplication>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QHBoxLayout>
#include <QHash>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace {

constexpr int kStripWidth = 112;

// Fallback until the strip has been laid out and its real height measured.
constexpr int kFallbackHeight = 420;
constexpr int kInsertSlots = 2;
constexpr int kMinSendSlots = 2;

// Slot rows are deliberately tight: a strip with a full chain of plugins has to
// fit the mixer pane along with everything below it, and a slot only ever holds
// one short line of 9px text. The row also has to hold the hover actions, which
// need more than the text does — 19px is where those two stop fighting. Every
// pixel above it is multiplied by the number of slots in the chain, and paid
// for by the fader.
constexpr int kSlotHeight = 19;
/// The I/O buttons are one line of text with nothing overlaid on them, so they
/// keep the tighter height the slots grew out of: every pixel spent up here is
/// a pixel the fader does not get.
constexpr int kRoutingHeight = 18;

QString panText(double pan) {
    if (std::abs(pan) < 0.01) return QStringLiteral("C");
    const int amount = int(std::round(std::abs(pan) * 100.0));
    return (pan < 0 ? QStringLiteral("L") : QStringLiteral("R")) +
           QString::number(amount);
}

// Size of the actions that appear on a hovered slot, and the gaps around them.
// Big enough to hit without aiming: these are the controls the chain is worked
// with, and a 14-pixel target inside a 17-pixel row was a dot.
constexpr int kActionSide = 17;
constexpr int kActionGap = 2;
constexpr int kActionMargin = 2;
/// A pre/post tap button carries three letters, so it is wider than the square
/// icon actions beside it.
constexpr int kTapWidth = 24;
/// Diameter of the round level control at the right of a send row.
constexpr int kSendKnobSide = 17;
/// Padding a slot button puts either side of its text (see #SlotButton).
constexpr int kSlotTextPad = 5;
/// Border + padding the slot button spends on its own text, both sides.
constexpr int kSlotTextInset = 2 + kSlotTextPad * 2;

// ── Drag and drop between strips ───────────────────────────────────────────
//
// Three payloads, all of them plain text so the same two helpers serve every
// case:
//   x-daw-insert — one plugin slot: "channel\tslot\tindex". Dragged onto
//                  another slot to reorder, or onto another strip to move it
//                  there.
//   x-daw-chain  — a whole Audio FX chain: "channel". This is what dragging the
//                  section's own title does.
//   x-daw-sends  — a whole send set: "track".
// Holding Alt (Option) at the drop copies instead of moving, which is the
// convention the Finder and every DAW already taught the user.
constexpr auto kInsertMime = "application/x-daw-insert";
constexpr auto kChainMime = "application/x-daw-chain";
constexpr auto kSendsMime = "application/x-daw-sends";

QMimeData* dragPayload(const char* mime, const QString& text) {
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(mime), text.toUtf8());
    return data;
}

QString payloadOf(const QMimeData* mime, const char* type) {
    if (!mime) return {};
    const QString key = QString::fromLatin1(type);
    if (!mime->hasFormat(key)) return {};
    return QString::fromUtf8(mime->data(key));
}

/// A section title you can pick up. Dragging "AUDIO FX" or "SENDS" onto another
/// strip takes what is under that title with it — the whole point being that
/// the thing you grab is the thing that moves.
class DragTitle : public QLabel {
public:
    DragTitle(const QString& text, const char* mime, QString payload,
              QWidget* parent)
        : QLabel(text, parent), m_mime(mime), m_payload(std::move(payload)) {
        setCursor(Qt::OpenHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton) m_press = ev->pos();
        QLabel::mousePressEvent(ev);
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (!(ev->buttons() & Qt::LeftButton) || m_payload.isEmpty()) return;
        if ((ev->pos() - m_press).manhattanLength() <
            QApplication::startDragDistance())
            return;
        auto* drag = new QDrag(this);
        drag->setMimeData(dragPayload(m_mime, m_payload));
        drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    }

private:
    const char* m_mime;
    QString m_payload;
    QPoint m_press;
};

/// One slot row: the name button, plus the actions that show only while the
/// pointer is on the row — for a plugin, bypass / open / replace; for a send,
/// enable / pre-post / remove.
///
/// The actions are spread **across** the row rather than stacked at one end:
/// first against the left edge, last against the right, the rest evenly
/// between. Three buttons that far apart cannot be mis-clicked for one another,
/// which matters more here than keeping the name legible under them — so with
/// three actions the name steps aside for as long as the pointer is on the row,
/// and comes straight back when it leaves. The tooltip still names the plugin.
///
/// The actions are children of the row rather than of the name button, so
/// moving the pointer onto one is not "leaving the slot". Qt sends a widget a
/// Leave when the pointer crosses onto its own child, so hover is decided by
/// hit-testing the row instead of trusting Enter/Leave on their own.
class SlotRow : public QWidget {
public:
    SlotRow(QToolButton* slot, QWidget* parent)
        : QWidget(parent), m_slot(slot), m_fullText(slot->text()) {
        m_slot->setParent(this);
        setFixedHeight(kSlotHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_slot->installEventFilter(this);
    }

    /// Actions are laid out in the order they are added, left to right. Not
    /// named addAction: that would hide QWidget's. `width` overrides the square
    /// default for an action that carries text instead of a glyph.
    void addSlotAction(QAbstractButton* button, int width = kActionSide) {
        button->setParent(this);
        button->setFixedSize(width, kActionSide);
        button->hide();
        button->installEventFilter(this);
        m_actions.push_back(button);
        layoutRow();
    }

    /// Make this row a drag source carrying `payload`, and — with `takes` set —
    /// a target for the same kind of drag. `onDrop` is given the payload and
    /// the modifiers held at the drop, so Alt can mean "copy".
    void setDragPayload(const char* mime, QString payload) {
        m_dragMime = mime;
        m_dragPayload = std::move(payload);
        m_slot->setCursor(Qt::OpenHandCursor);
    }
    void setDropTarget(const char* mime,
                       std::function<void(const QString&, Qt::KeyboardModifiers)> onDrop) {
        m_dropMime = mime;
        m_onDrop = std::move(onDrop);
        setAcceptDrops(true);
    }

    /// A control that is always visible, pinned to one edge of the row — the
    /// send level. Unlike the actions it is not a hover reveal: a send's amount
    /// is the thing you look at without touching anything.
    void setTrailing(QWidget* widget) {
        widget->setParent(this);
        widget->show();
        widget->installEventFilter(this);
        m_trailing = widget;
        layoutRow();
    }
    /// The same, on the left. A send reads as "this much, to there", and the
    /// amount belongs where reading starts.
    void setLeading(QWidget* widget) {
        widget->setParent(this);
        widget->show();
        widget->installEventFilter(this);
        m_leading = widget;
        layoutRow();
    }

protected:
    void enterEvent(QEnterEvent*) override { refreshHover(); }
    void leaveEvent(QEvent*) override { refreshHover(); }
    void resizeEvent(QResizeEvent*) override {
        layoutRow();
        // The row can appear under a pointer that never moved — bypassing a
        // plugin rebuilds the strip beneath the finger that clicked it — so
        // hover is re-read once the row knows where it is, not only on Enter.
        refreshHover();
    }

    bool eventFilter(QObject* watched, QEvent* ev) override {
        if (ev->type() == QEvent::Enter || ev->type() == QEvent::Leave) {
            refreshHover();
        }
        // The name button fills the row, so a drag has to start from *its*
        // events; the row itself never sees them.
        if (watched == m_slot && !m_dragPayload.isEmpty()) {
            if (ev->type() == QEvent::MouseButtonPress) {
                m_press = static_cast<QMouseEvent*>(ev)->pos();
            } else if (ev->type() == QEvent::MouseMove) {
                auto* me = static_cast<QMouseEvent*>(ev);
                if ((me->buttons() & Qt::LeftButton) &&
                    (me->pos() - m_press).manhattanLength() >=
                        QApplication::startDragDistance()) {
                    startDrag();
                    return true;    // this move belongs to the drag, not the button
                }
            }
        }
        return false;   // never consume; the child still gets its event
    }

    void dragEnterEvent(QDragEnterEvent* ev) override {
        if (!m_onDrop || payloadOf(ev->mimeData(), m_dropMime).isEmpty()) return;
        m_dropHover = true;
        update();
        ev->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* ev) override {
        if (!m_onDrop || payloadOf(ev->mimeData(), m_dropMime).isEmpty()) return;
        ev->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override {
        m_dropHover = false;
        update();
    }
    void dropEvent(QDropEvent* ev) override {
        m_dropHover = false;
        update();
        const QString payload = payloadOf(ev->mimeData(), m_dropMime);
        if (payload.isEmpty() || !m_onDrop) return;
        ev->acceptProposedAction();
        m_onDrop(payload, ev->modifiers());
    }
    void paintEvent(QPaintEvent* ev) override {
        QWidget::paintEvent(ev);
        if (!m_dropHover) return;
        // A line where the dragged plugin would land, rather than a box around
        // the row: the drop *replaces this position in the chain*, and a filled
        // outline reads as "into this slot".
        QPainter p(this);
        p.setPen(QPen(th().accent, 2.0));
        p.drawLine(0, 1, width(), 1);
    }

private:
    void startDrag() {
        // The button keeps the mouse grab through the drag and would otherwise
        // come back pressed — and fire a click nobody asked for.
        m_slot->setDown(false);
        auto* drag = new QDrag(this);
        drag->setMimeData(dragPayload(m_dragMime, m_dragPayload));
        drag->setPixmap(grab());
        drag->setHotSpot(m_press);
        drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    }

    void refreshHover() {
        // A grab cannot hold a pointer over a row, so a screenshot check asks
        // for the hovered state outright. Set once per process.
        static const bool forced = qEnvironmentVariableIsSet("DAW_SHOT_SLOT_HOVER");
        const bool on =
            forced || rect().contains(mapFromGlobal(QCursor::pos()));
        if (on == m_hovered) return;
        m_hovered = on;
        for (QAbstractButton* b : m_actions) b->setVisible(on);
        relabel();
    }

    /// Width the actions take when they are stacked at the right edge — the
    /// single-action case, which is the only one that still reserves padding.
    int actionsWidth() const {
        int total = kActionMargin * 2;
        for (const QAbstractButton* b : m_actions) total += b->width() + kActionGap;
        return m_actions.empty() ? 0 : total - kActionGap;
    }

    /// Width the name button gets: the row, less anything pinned to its right.
    /// Where the name button starts — past a leading control, if there is one.
    int slotLeft() const {
        return m_leading ? m_leading->width() + kActionGap * 2 : 0;
    }
    int slotWidth() const {
        return width() - slotLeft() -
               (m_trailing ? m_trailing->width() + kActionGap * 2 : 0);
    }

    void layoutRow() {
        m_slot->setGeometry(slotLeft(), 0, slotWidth(), height());
        if (m_leading) {
            m_leading->move(0, (height() - m_leading->height()) / 2);
            m_leading->raise();
        }
        if (m_trailing) {
            m_trailing->move(width() - m_trailing->width(),
                             (height() - m_trailing->height()) / 2);
            m_trailing->raise();
        }

        const int count = int(m_actions.size());
        const int top = (height() - kActionSide) / 2;
        if (count == 1) {
            // One action has no spread to speak of: it sits where a single
            // affordance is expected, at the end of the row.
            m_actions.front()->move(slotLeft() + slotWidth() - kActionMargin -
                                        m_actions.front()->width(),
                                    top);
            m_actions.front()->raise();
        } else if (count > 1) {
            int occupied = 0;
            for (const QAbstractButton* b : m_actions) occupied += b->width();
            const int span = slotWidth() - kActionMargin * 2 - occupied;
            const int gap = std::max(kActionGap, span / (count - 1));
            int x = slotLeft() + kActionMargin;
            for (QAbstractButton* b : m_actions) {
                b->move(x, top);
                b->raise();
                x += b->width() + gap;
            }
        }
        relabel();
    }

    /// The name is elided against whatever the actions leave it, so a hovered
    /// slot shows a shortened name rather than one running under the buttons.
    ///
    /// Eliding alone is not enough: the style centres the label in the button,
    /// so a shorter string just drifts rightwards into the buttons. Reserving
    /// the strip they cover as right padding moves the centre with it, and the
    /// slot keeps its full-width box instead of visibly shrinking on hover.
    void relabel() {
        const bool spread = m_actions.size() > 1;
        // Only the stacked single-action case needs the padding trick: with the
        // actions spread symmetrically the label is already centred between
        // them, and with three of them there is no label to place.
        const int reserve = (m_hovered && !spread) ? actionsWidth() : 0;
        if (reserve != m_reserve) {
            m_reserve = reserve;
            m_slot->setStyleSheet(
                reserve ? QStringLiteral("#SlotButton { padding-right: %1px; }")
                              .arg(reserve + kSlotTextPad)
                        : QString());
        }

        int available = slotWidth() - kSlotTextInset - reserve;
        if (m_hovered && spread) {
            if (m_actions.size() >= 3) {
                available = 0;   // the middle action stands where the name was
            } else {
                available -= 2 * (kActionMargin + m_actions.front()->width() +
                                  kActionGap);
            }
        }
        // fontMetrics(), not font(): the size comes from the stylesheet and is
        // only resolved once the widget has been polished.
        m_slot->setText(m_slot->fontMetrics().elidedText(
            m_fullText, Qt::ElideRight, std::max(0, available)));
    }

    QToolButton* m_slot = nullptr;
    QWidget* m_trailing = nullptr;
    QWidget* m_leading = nullptr;
    QString m_fullText;
    std::vector<QAbstractButton*> m_actions;
    bool m_hovered = false;
    /// Drag source: what this row hands over when it is picked up.
    const char* m_dragMime = nullptr;
    QString m_dragPayload;
    QPoint m_press;
    /// Drop target: what it takes, and what to do with it.
    const char* m_dropMime = nullptr;
    std::function<void(const QString&, Qt::KeyboardModifiers)> m_onDrop;
    bool m_dropHover = false;
    /// Width currently reserved for the actions, so the stylesheet is only
    /// rewritten when it actually changes — relabel runs on every resize.
    int m_reserve = 0;
};

} // namespace

/// One I/O plate: a recessed field with a micro-caption on the left, the
/// destination named across the middle and a caret at the right.
///
/// A QToolButton underneath, so the QSS box, the hover and the menu behave like
/// every other control on the strip — everything above is painted over it. The
/// caption is what tells the two rows apart now that neither carries an icon:
/// the arrow glyphs they used read as a symbol nobody could name, and at 11px
/// they cost more width than they returned.
class RoutingField : public QToolButton {
public:
    explicit RoutingField(QWidget* parent, QString caption)
        : QToolButton(parent), m_caption(std::move(caption)) {
        setObjectName("RoutingButton");
        setPopupMode(QToolButton::InstantPopup);
        setToolButtonStyle(Qt::ToolButtonTextOnly);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(kRoutingHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    /// The destination, elided to whatever the caption and the caret leave.
    void setFieldText(const QString& text) {
        m_full = text;
        relabel();
    }
    const QString& fieldText() const { return m_full; }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QToolButton::resizeEvent(event);
        relabel();
    }

    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);   // the styled plate and its centred text

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        const QRectF r(rect());

        // The lip of the recess: one lit hairline along the top edge. It is the
        // whole reason the plate reads as cut into the strip rather than laid
        // on it.
        p.setPen(QPen(t.ink(t.dark ? 16 : 34), 1.0));
        p.drawLine(QPointF(r.left() + 4.0, r.top() + 1.0),
                   QPointF(r.right() - 4.0, r.top() + 1.0));

        QFont f = font();
        f.setPixelSize(7);
        f.setBold(true);
        f.setLetterSpacing(QFont::PercentageSpacing, 108);
        p.setFont(f);
        p.setPen(mixColors(t.textSecondary, t.background, isEnabled() ? 0.2 : 0.5));
        p.drawText(QRectF(r.left() + 4.0, r.top(), kCaptionWidth, r.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_caption);

        if (!menu()) return;
        // The caret: the plate opens a menu, and nothing else on the strip says
        // so — the slot rows all act on a click.
        const double x = r.right() - 8.0;
        const double y = r.center().y() - 0.5;
        QColor caret = t.accent;
        caret.setAlphaF(underMouse() ? 0.95 : 0.62);
        p.setPen(QPen(caret, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF({QPointF(x - 2.6, y - 1.2), QPointF(x, y + 1.6),
                                  QPointF(x + 2.6, y - 1.2)}));
    }

private:
    void relabel() {
        const int available =
            width() - int(kCaptionWidth) - kCaretWidth - kFieldPad * 2;
        setText(fontMetrics().elidedText(m_full, Qt::ElideRight,
                                         std::max(0, available)));
        setToolTip(m_full);
    }

    static constexpr double kCaptionWidth = 17.0;
    static constexpr int kCaretWidth = 10;
    static constexpr int kFieldPad = 4;

    QString m_caption;
    QString m_full;
};

ChannelStrip::ChannelStrip(daw::EngineController* controller,
                           const QString& trackId, bool master,
                           QWidget* parent)
    : QWidget(parent), m_controller(controller), m_trackId(trackId),
      m_master(master) {
    setFixedWidth(kStripWidth);
    // The strip never goes below the height at which the whole console is
    // visible (set once it has been measured, below). A mixer pane shorter than
    // that scrolls; a taller one hands the extra height to the fader and meter.
    setMinimumHeight(kFallbackHeight);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_StyledBackground, true);
    // A chain or a send set dragged off another strip lands anywhere on this
    // one: the target is the channel, not a particular well inside it.
    setAcceptDrops(true);

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(8, 7, 8, 7);
    col->setSpacing(5);

    col->addWidget(buildHeader());
    col->addWidget(buildRouting());
    // The instrument comes before the inserts because that is the signal order:
    // it makes the sound, the inserts then treat it.
    if (QWidget* instrument = buildInstrument()) col->addWidget(instrument);
    col->addWidget(buildInserts());
    if (!m_master) col->addWidget(buildSends());

    auto* panSection = new QWidget(this);
    auto* panColumn = new QVBoxLayout(panSection);
    panColumn->setContentsMargins(0, 0, 0, 0);
    panColumn->setSpacing(2);
    m_pan = new ui::PanKnob(panSection);
    if (!m_master) {
        m_pan->setAutomatable(true);
        connect(m_pan, &ui::PanKnob::automateRequested, this,
                [this] { emit automateControlRequested(m_trackId, true); });
    }
    m_panLabel = new QLabel(QStringLiteral("C"), panSection);
    m_panLabel->setAlignment(Qt::AlignCenter);
    panColumn->addWidget(m_pan, 0, Qt::AlignHCenter);
    panColumn->addWidget(m_panLabel);
    if (!m_master) {
        // Mono (one ring) / stereo (two rings) fold for the whole channel.
        m_monoButton = new ui::IconButton(icons::Glyph::StereoRings,
                                          tr("Stereo"), panSection);
        m_monoButton->setCheckable(true);
        m_monoButton->setButtonSize(28, 20);
        panColumn->addWidget(m_monoButton, 0, Qt::AlignHCenter);
        connect(m_monoButton, &QAbstractButton::clicked, this, [this] {
            const bool mono = m_monoButton->isChecked();
            m_controller->setTrackMono(m_trackId.toStdString(), mono);
            m_monoButton->setGlyph(mono ? icons::Glyph::MonoRing
                                        : icons::Glyph::StereoRings);
            m_monoButton->setToolTip(mono ? tr("Mono") : tr("Stereo"));
            emit edited();
        });
    }
    col->addWidget(panSection);
    // The fader row carries the stretch: extra height goes to the fader and
    // the meter, which is what makes a tall mixer worth having.
    col->addWidget(buildFaderRow(), 1);
    m_gainLabel = new QLabel(QStringLiteral("0.0 dB"), this);
    m_gainLabel->setAlignment(Qt::AlignCenter);
    col->addWidget(m_gainLabel);
    col->addWidget(buildButtons(), 0, Qt::AlignHCenter);
    col->addWidget(buildNamePlate());

    // Several wells accept their own internal drag formats. Without this
    // forwarding layer they become holes in the larger channel-strip target:
    // a browser plugin works over the name plate, then appears to fail over an
    // insert row. Inspect every child event before its specialised well does.
    for (QWidget* child : findChildren<QWidget*>())
        child->installEventFilter(this);

    connect(m_pan, &ui::PanKnob::panChanged, this, [this](double v) {
        if (m_master) {
            if (!m_panGestureStart)
                m_panGestureStart = m_controller->masterPan();
            m_controller->setMasterPanLive(float(v));
        } else {
            const std::string id = m_trackId.toStdString();
            if (!m_panGestureStart) {
                if (const auto* track = m_controller->project().findTrack(id))
                    m_panGestureStart = track->pan;
            }
            m_controller->setTrackPanLive(id, float(v));
        }
        m_panLabel->setText(panText(v));
    });
    connect(m_pan, &ui::PanKnob::editFinished, this, [this] {
        if (m_panGestureStart) {
            if (m_master) {
                m_controller->commitMasterPanEdit(*m_panGestureStart);
            } else {
                m_controller->commitTrackPanEdit(
                    {{m_trackId.toStdString(), *m_panGestureStart}});
            }
            m_panGestureStart.reset();
        }
        emit edited();
    });

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &ChannelStrip::applyTheme);
    applyTheme();
    syncFromModel();

    // Measure what the console actually needs and make that the floor. Adding
    // inserts or sends grows it, and the mixer starts scrolling instead of
    // clipping the strip.
    m_naturalHeight = std::max(kFallbackHeight, sizeHint().height());
    setMinimumHeight(m_naturalHeight);
}

QWidget* ChannelStrip::buildHeader() {
    auto* box = new QWidget(this);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(5);
    box->setFixedHeight(20);

    auto* swatch = new QWidget(box);
    swatch->setObjectName("ColorSwatch");
    swatch->setFixedSize(5, 18);
    uint32_t color = 0x888888;
    if (const auto* t = m_controller->project().findTrack(m_trackId.toStdString()))
        color = t->color;
    swatch->setStyleSheet(QString("background: %1; border-radius: 2px;")
                              .arg(colorFromRgb(color).name()));

    QString name = tr("Master");
    if (!m_master) {
        if (const auto* t =
                m_controller->project().findTrack(m_trackId.toStdString()))
            name = QString::fromStdString(t->name);
    }
    auto* label = new QLabel(name, box);
    label->setObjectName("StripName");

    row->addWidget(swatch);
    row->addWidget(label, 1);
    return box;
}

QToolButton* ChannelStrip::makeSlotButton(const QString& text, bool active) {
    auto* b = new QToolButton(this);
    b->setObjectName("SlotButton");
    b->setProperty("active", active);
    b->setText(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(kSlotHeight);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

QWidget* ChannelStrip::buildSlotRow(QToolButton* slot, const QString& channel,
                                    const QString& slotId, bool bypassed,
                                    bool instrument) {
    auto* row = new SlotRow(slot, this);

    auto action = [row](icons::Glyph glyph, const QString& tip) {
        auto* b = new ui::IconButton(glyph, tip, row);
        b->setButtonSize(kActionSide, kActionSide);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };

    // Bypass lights red rather than in the accent: a bypassed slot is a hole in
    // the chain, which is the same news a muted channel is.
    auto* power = action(icons::Glyph::Power, tr("Bypass this plugin"));
    power->setCheckable(true);
    power->setChecked(bypassed);
    power->setActiveColor(Theme::mute());
    // clicked, not toggled: setChecked above must not fire the handler.
    connect(power, &QAbstractButton::clicked, this,
            [this, channel, slotId](bool on) {
                m_controller->setInsertBypassed(channel.toStdString(),
                                                slotId.toStdString(), on);
                emit edited();
                emit structureChanged();
            });
    row->addSlotAction(power);

    auto* open = action(icons::Glyph::Detach, tr("Open the plugin window"));
    connect(open, &QAbstractButton::clicked, this, [this, channel, slotId] {
        emit editorRequested(channel, slotId);
    });
    row->addSlotAction(open);

    auto* swap = action(icons::Glyph::Chevron, tr("Replace this plugin"));
    connect(swap, &QAbstractButton::clicked, this,
            [this, swap, channel, slotId, instrument] {
                QMenu* menu = ui::buildPluginMenu(
                    swap, m_controller, instrument,
                    [this, channel, slotId,
                     instrument](const daw::plugins::PluginDescriptor& d) {
                        const bool loaded =
                            instrument
                                ? m_controller->setTrackInstrumentPlugin(
                                      channel.toStdString(), d)
                                : m_controller->replaceInsert(
                                      channel.toStdString(), slotId.toStdString(), d);
                        if (!loaded) reportPluginFailure(d);
                        emit edited();
                        emit structureChanged();
                    });
                menu->exec(swap->mapToGlobal(QPoint(0, swap->height())));
                menu->deleteLater();
            });
    row->addSlotAction(swap);

    return row;
}

QWidget* ChannelStrip::buildRouting() {
    auto* box = new QWidget(this);
    auto* col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);
    col->addWidget(ui::sectionLabel(m_master ? tr("Output") : tr("I/O"), box));

    auto routingButton = [this](const QString& caption, const QString& text) {
        auto* b = new RoutingField(this, caption);
        b->setFieldText(text);
        return b;
    };

    if (!m_master) {
        m_inputButton = routingButton(tr("IN"), tr("No Input"));
        auto* inputMenu = new QMenu(m_inputButton);
        connect(inputMenu, &QMenu::aboutToShow, this,
                [this, inputMenu] { populateInputMenu(inputMenu); });
        m_inputButton->setMenu(inputMenu);
        col->addWidget(m_inputButton);
    }

    m_outputButton =
        routingButton(tr("OUT"), m_master ? tr("Main Out") : tr("Master"));
    if (!m_master) {
        auto* outputMenu = new QMenu(m_outputButton);
        connect(outputMenu, &QMenu::aboutToShow, this,
                [this, outputMenu] { populateOutputMenu(outputMenu); });
        m_outputButton->setMenu(outputMenu);
    } else {
        m_outputButton->setPopupMode(QToolButton::DelayedPopup);
        m_outputButton->setEnabled(false);
    }
    col->addWidget(m_outputButton);
    return box;
}

namespace {

/// A slot well that accepts a dropped file.
///
/// The strip decides what a drop *means*; this only reports one, so the same
/// widget can serve the instrument slot now and the inserts later. It takes
/// plain file URLs, which is what the browser's drag and the desktop both send.
class DropWell : public QWidget {
public:
    using QWidget::QWidget;

    std::function<bool(const QString&)> accepts;
    std::function<void(const QString&)> onDrop;
    /// A drag from inside the program — a plugin slot looking for a new place
    /// in the chain. The empty space below the loaded slots is a legitimate
    /// target: it means "put it at the end".
    const char* mimeType = nullptr;
    std::function<void(const QString&, Qt::KeyboardModifiers)> onMimeDrop;

protected:
    /// The first acceptable local file in the drag, or empty.
    QString candidate(const QMimeData* mime) const {
        if (!mime || !mime->hasUrls() || !accepts) return {};
        for (const QUrl& url : mime->urls()) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            if (accepts(path)) return path;
        }
        return {};
    }
    QString internal(const QMimeData* mime) const {
        if (!mimeType || !onMimeDrop) return {};
        return payloadOf(mime, mimeType);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (candidate(event->mimeData()).isEmpty() &&
            internal(event->mimeData()).isEmpty())
            return;
        m_hover = true;
        update();
        event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (candidate(event->mimeData()).isEmpty() &&
            internal(event->mimeData()).isEmpty())
            return;
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override {
        m_hover = false;
        update();
    }
    void dropEvent(QDropEvent* event) override {
        m_hover = false;
        update();
        if (const QString payload = internal(event->mimeData()); !payload.isEmpty()) {
            event->acceptProposedAction();
            onMimeDrop(payload, event->modifiers());
            return;
        }
        const QString path = candidate(event->mimeData());
        if (path.isEmpty() || !onDrop) return;
        event->acceptProposedAction();
        onDrop(path);
    }
    void paintEvent(QPaintEvent* event) override {
        QWidget::paintEvent(event);
        if (!m_hover) return;
        // An accent outline over the whole well: the slot inside it may be one
        // of several rows, and the drop lands on the slot, not on a row.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(th().accent, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75), 4.0, 4.0);
    }

private:
    bool m_hover = false;
};

} // namespace

QWidget* ChannelStrip::buildSlotWell(const QString& title, QWidget* addButton,
                                     const std::vector<QWidget*>& rows,
                                     std::function<bool(const QString&)> accepts,
                                     std::function<void(const QString&)> onDrop,
                                     WellDrag drag) {
    auto* box = new QWidget(this);
    auto* col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    QLabel* caption = nullptr;
    if (drag.dragMime) {
        // The title is the handle for everything under it: grab "AUDIO FX" and
        // the chain comes with you.
        // Same look as any other section title — it is one, it just happens to
        // be a handle as well.
        auto* handle = new DragTitle(title.toUpper(), drag.dragMime,
                                     drag.dragPayload, box);
        handle->setProperty("role", "section");
        QFont f = handle->font();
        f.setPixelSize(9);
        f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
        handle->setFont(f);
        caption = handle;
    } else {
        caption = ui::sectionLabel(title, box);
    }
    if (!drag.titleTip.isEmpty()) caption->setToolTip(drag.titleTip);
    head->addWidget(caption);
    head->addStretch(1);
    if (addButton) head->addWidget(addButton);
    col->addLayout(head);

    QWidget* well = nullptr;
    if ((accepts && onDrop) || drag.dropMime) {
        auto* dropWell = new DropWell(box);
        dropWell->setAcceptDrops(true);
        dropWell->accepts = std::move(accepts);
        dropWell->onDrop = std::move(onDrop);
        dropWell->mimeType = drag.dropMime;
        dropWell->onMimeDrop = std::move(drag.onMimeDrop);
        well = dropWell;
    } else {
        well = new QWidget(box);
    }
    well->setObjectName("SlotWell");
    auto* wellCol = new QVBoxLayout(well);
    // Tight enough that the well reads as a rack of slots rather than a box
    // with slots floating in it; the border still separates it from the strip.
    wellCol->setContentsMargins(3, 3, 3, 3);
    wellCol->setSpacing(2);
    for (QWidget* row : rows) {
        row->setParent(well);
        wellCol->addWidget(row);
    }
    col->addWidget(well);
    return box;
}

void ChannelStrip::dropSampleOnInstrument(const QString& trackId,
                                          const QString& path) {
    if (!m_controller->loadInstrumentSampler(trackId.toStdString(),
                                             path.toStdString())) {
        return;
    }
    emit edited();
    // The strip rebuilds itself from this, which deletes the widget the drop
    // arrived on — the signal is queued for exactly that reason.
    emit structureChanged();
}

QString ChannelStrip::channelId() const {
    // The master addresses its inserts through a reserved channel id, so every
    // path below is identical for a track and for the master bus.
    return m_master ? QString::fromLatin1(daw::EngineController::kMasterChannelId)
                    : m_trackId;
}

void ChannelStrip::reportPluginFailure(const daw::plugins::PluginDescriptor& descriptor) {
    // A plugin that will not load used to be perfectly silent: the menu closed
    // and nothing happened, which reads as a broken program rather than as a
    // broken plugin. The commonest cause by far is a scan that has gone stale —
    // a licence that lapsed, a module updated since, a plugin uninstalled — so
    // the message says where to look.
    QMessageBox::warning(
        this, tr("Plugin"),
        tr("%1 could not be loaded.\n\nThe plugin is still listed from an "
           "earlier scan but its module no longer offers it — it may have been "
           "moved, uninstalled, or its licence may have expired. Rescanning in "
           "the Plugin Manager will drop what is no longer there.")
            .arg(QString::fromStdString(descriptor.name)));
}

QWidget* ChannelStrip::buildInserts() {
    const QString channel = channelId();
    const std::vector<daw::InsertModel>* inserts =
        m_controller->channelInserts(channel.toStdString());
    const std::size_t loaded = inserts ? inserts->size() : 0;

    std::vector<QWidget*> rows;
    // Every loaded plugin, plus the empty slots that make the well look like a
    // console rather than a list that collapses to nothing.
    const int slotCount = std::max<int>(kInsertSlots, int(loaded) + 1);
    for (int i = 0; i < slotCount; ++i) {
        const bool filled = i < int(loaded);
        const daw::InsertModel* model = filled ? &(*inserts)[std::size_t(i)] : nullptr;

        auto* b = makeSlotButton(
            filled ? QString::fromStdString(model->name) : tr("INSERT %1").arg(i + 1),
            filled);
        // A slot can name a plugin and have nothing behind it: a project made
        // on another machine, a plugin uninstalled, a licence that lapsed. The
        // document still says what belongs here — the strip has to say that it
        // is not there.
        const bool missing =
            filled && !m_controller->insertInstance(channel.toStdString(), model->id);
        if (filled) {
            b->setProperty("bypassed", model->bypassed);
            b->setProperty("missing", missing);
            if (missing) {
                b->setToolTip(
                    tr("%1 is not loaded — the plugin could not be found on "
                       "this machine. Audio passes through this slot untouched.")
                        .arg(QString::fromStdString(model->name)));
            } else {
                b->setToolTip(model->bypassed
                                  ? tr("%1 — bypassed. Click to edit, right-click for options.")
                                        .arg(QString::fromStdString(model->name))
                                  : tr("%1 — click to edit, right-click for options.")
                                        .arg(QString::fromStdString(model->name)));
            }
        } else {
            b->setToolTip(tr("Empty insert slot — click to load a plugin."));
        }

        if (!filled) {
            // An empty slot opens the picker directly: one click to a plugin.
            b->setMenu(ui::buildLazyPluginMenu(
                b, m_controller, /*instruments=*/false,
                [this, channel](const daw::plugins::PluginDescriptor& descriptor) {
                    const std::string id =
                        m_controller->addInsert(channel.toStdString(), descriptor);
                    if (id.empty()) {
                        reportPluginFailure(descriptor);
                        return;
                    }
                    emit editorRequested(channel, QString::fromStdString(id));
                    emit edited();
                    emit structureChanged();
                }));
            rows.push_back(b);
            continue;
        }

        // A loaded slot behaves the way it does in every other DAW: click
        // opens the plugin, right-click offers the slot's options. Leaving
        // the options on left-click would put a menu between the user and
        // the thing they actually want to reach.
        const QString insertId = QString::fromStdString(model->id);
        b->setPopupMode(QToolButton::DelayedPopup);
        connect(b, &QToolButton::clicked, this, [this, channel, insertId] {
            emit editorRequested(channel, insertId);
        });
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        const std::size_t index = std::size_t(i);
        connect(b, &QWidget::customContextMenuRequested, this,
                [this, b, insertId, index](const QPoint& at) {
                    QMenu* menu = buildInsertMenu(b, insertId, index);
                    menu->exec(b->mapToGlobal(at));
                    menu->deleteLater();
                });
        // The hover actions are the shortcuts to the three things that menu
        // offers most often; the menu keeps the rest.
        auto* row = buildSlotRow(b, channel, insertId, model->bypassed,
                                 /*instrument=*/false);
        // Picked up by its name, dropped on another slot to take that place in
        // the chain — the reorder every other DAW does by dragging, instead of
        // the two "Move Up" menu items it used to be.
        // buildSlotRow always makes a SlotRow; the header cannot name the type
        // because it lives in this file's anonymous namespace.
        auto* slotRow = static_cast<SlotRow*>(row);
        slotRow->setDragPayload(kInsertMime, channel + "\t" + insertId);
        slotRow->setDropTarget(
            kInsertMime, [this, channel, index](const QString& payload,
                                                Qt::KeyboardModifiers mods) {
                dropInsertAt(channel, payload, index, mods);
            });
        rows.push_back(row);
    }

    auto* head = new QWidget(this);
    auto* headRow = new QHBoxLayout(head);
    headRow->setContentsMargins(0, 0, 0, 0);
    headRow->setSpacing(2);

    // One switch for the whole chain, next to the slots it acts on: the A/B
    // every engineer does by hand otherwise, slot by slot. It reads the chain
    // rather than remembering — "all bypassed" is the only state that lights.
    const bool allBypassed =
        loaded > 0 && std::all_of(inserts->begin(), inserts->end(),
                                  [](const daw::InsertModel& in) { return in.bypassed; });
    auto* bypassAll =
        new ui::IconButton(icons::Glyph::Power, tr("Bypass every plugin on this channel"), this);
    bypassAll->setButtonSize(16, 14);
    bypassAll->setCheckable(true);
    bypassAll->setChecked(allBypassed);
    bypassAll->setActiveColor(Theme::mute());
    bypassAll->setEnabled(loaded > 0);
    connect(bypassAll, &QAbstractButton::clicked, this, [this, channel](bool on) {
        m_controller->setAllInsertsBypassed(channel.toStdString(), on);
        emit edited();
        emit structureChanged();
    });
    headRow->addWidget(bypassAll);

    // Where the bare "+" used to be. Adding a plugin is still the first item,
    // but the chain as a whole is a thing you work with — copy it, paste it,
    // clear it — and a plus sign could only ever say "add".
    auto* more = new ui::IconButton(icons::Glyph::Gear,
                                    tr("Plugins and channel strip: add, copy, "
                                       "paste"), this);
    more->setButtonSize(16, 14);
    connect(more, &QAbstractButton::clicked, this, [this, more] {
        QMenu* menu = buildChainMenu(more);
        menu->exec(more->mapToGlobal(QPoint(0, more->height())));
        menu->deleteLater();
    });
    headRow->addWidget(more);

    WellDrag drag;
    drag.dragMime = kChainMime;
    drag.dragPayload = channel;
    drag.titleTip = tr("Drag onto another channel to move these plugins there. "
                       "Hold Alt to copy them instead.");
    drag.dropMime = kInsertMime;
    // The empty space under the slots is the end of the chain.
    const std::size_t end = loaded;
    drag.onMimeDrop = [this, channel, end](const QString& payload,
                                           Qt::KeyboardModifiers mods) {
        dropInsertAt(channel, payload, end, mods);
    };
    return buildSlotWell(tr("Audio FX"), head, rows, {}, {}, std::move(drag));
}

void ChannelStrip::dropInsertAt(const QString& channel, const QString& payload,
                                std::size_t index, Qt::KeyboardModifiers mods) {
    const QStringList parts = payload.split('\t');
    if (parts.size() < 2) return;
    const QString sourceChannel = parts[0];
    const QString slotId = parts[1];

    if (sourceChannel == channel) {
        // Within one chain a drag is a reorder; Alt has nothing to copy to.
        m_controller->moveInsert(channel.toStdString(), slotId.toStdString(),
                                 index);
    } else if (!m_controller->moveInsertBetweenChannels(
                   sourceChannel.toStdString(), slotId.toStdString(),
                   channel.toStdString(), index, mods & Qt::AltModifier)) {
        return;
    }
    emit edited();
    emit structureChanged();
}

QMenu* ChannelStrip::buildChainMenu(QWidget* parent) {
    const QString channel = channelId();
    const std::vector<daw::InsertModel>* inserts =
        m_controller->channelInserts(channel.toStdString());
    const bool hasPlugins = inserts && !inserts->empty();
    const auto& clipboard = m_controller->channelClipboard();

    auto* menu = new QMenu(parent);

    QMenu* add = ui::buildPluginMenu(
        menu, m_controller, /*instruments=*/false,
        [this, channel](const daw::plugins::PluginDescriptor& descriptor) {
            const std::string id =
                m_controller->addInsert(channel.toStdString(), descriptor);
            if (id.empty()) {
                reportPluginFailure(descriptor);
                return;
            }
            emit editorRequested(channel, QString::fromStdString(id));
            emit edited();
            emit structureChanged();
        });
    add->setTitle(tr("Add Plugin"));
    menu->addMenu(add);

    menu->addSeparator();
    auto* presets = menu->addMenu(tr("Channel Strip Presets"));
    const QStringList presetFiles = ui::channelstrippresets::files();
    for (const QString& file : presetFiles) {
        QAction* apply =
            presets->addAction(ui::channelstrippresets::displayName(file));
        apply->setToolTip(tr("Apply %1 to this channel")
                              .arg(QFileInfo(file).fileName()));
        connect(apply, &QAction::triggered, this, [this, channel, file] {
            const audio::Result result = m_controller->applyChannelStripPreset(
                channel.toStdString(), file.toStdString());
            if (!result) {
                QMessageBox::warning(
                    this, tr("Channel Strip Preset"),
                    tr("%1 could not be loaded.\n\n%2")
                        .arg(QFileInfo(file).fileName(),
                             QString::fromStdString(result.message())));
                return;
            }
            emit edited();
            emit structureChanged();
            QToolTip::showText(QCursor::pos(),
                               tr("Applied %1")
                                   .arg(ui::channelstrippresets::displayName(file)),
                               this);
        });
    }
    if (presetFiles.isEmpty()) {
        QAction* none = presets->addAction(tr("No saved presets"));
        none->setEnabled(false);
    }
    presets->addSeparator();
    QAction* savePreset = presets->addAction(tr("Save New Template…"));
    connect(savePreset, &QAction::triggered, this, [this, channel] {
        const auto snapshot = m_controller->copyChannelStrip(
            channel.toStdString(), /*withSettings=*/true);
        QString suggestion = QString::fromStdString(snapshot.sourceName).trimmed();
        if (suggestion.isEmpty()) suggestion = tr("Channel Strip");

        for (;;) {
            bool accepted = false;
            const QString name = QInputDialog::getText(
                this, tr("Save Channel Strip Template"), tr("Template name:"),
                QLineEdit::Normal, suggestion, &accepted);
            if (!accepted) return;
            suggestion = name;
            const QString file =
                ui::channelstrippresets::filePathForName(name);
            if (file.isEmpty()) {
                QMessageBox::warning(
                    this, tr("Channel Strip Preset"),
                    tr("Use a file-safe name without / \\ : * ? \" < > or |."));
                continue;
            }
            if (QFileInfo::exists(file) &&
                QMessageBox::question(
                    this, tr("Replace Channel Strip Preset"),
                    tr("A template named “%1” already exists. Replace it?")
                        .arg(ui::channelstrippresets::displayName(file)),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Yes) {
                continue;
            }

            const audio::Result result = m_controller->saveChannelStripPreset(
                channel.toStdString(), file.toStdString());
            if (!result) {
                QMessageBox::warning(
                    this, tr("Channel Strip Preset"),
                    tr("The template could not be saved.\n\n%1")
                        .arg(QString::fromStdString(result.message())));
                return;
            }
            QToolTip::showText(
                QCursor::pos(),
                tr("Saved %1.vlts")
                    .arg(ui::channelstrippresets::displayName(file)),
                this);
            return;
        }
    });

    menu->addSeparator();
    QAction* copyPlugins = menu->addAction(tr("Copy Plugins"));
    copyPlugins->setEnabled(hasPlugins);
    connect(copyPlugins, &QAction::triggered, this, [this, channel] {
        m_controller->setChannelClipboard(m_controller->copyChannelStrip(
            channel.toStdString(), /*withSettings=*/false));
    });
    QAction* copyStrip = menu->addAction(tr("Copy Channel Strip"));
    connect(copyStrip, &QAction::triggered, this, [this, channel] {
        m_controller->setChannelClipboard(m_controller->copyChannelStrip(
            channel.toStdString(), /*withSettings=*/true));
    });

    menu->addSeparator();
    // The source is named in the item itself: a clipboard you cannot see is
    // otherwise a guess about what the last copy was.
    const QString from = QString::fromStdString(clipboard.sourceName);
    QAction* pastePlugins =
        menu->addAction(from.isEmpty() ? tr("Paste Plugins")
                                       : tr("Paste Plugins from %1").arg(from));
    pastePlugins->setEnabled(!clipboard.inserts.empty());
    connect(pastePlugins, &QAction::triggered, this, [this, channel] {
        if (!m_controller->pasteChannelInserts(channel.toStdString(),
                                               m_controller->channelClipboard()))
            return;
        emit edited();
        emit structureChanged();
    });
    QAction* pasteStrip =
        menu->addAction(from.isEmpty() ? tr("Paste Channel Strip")
                                       : tr("Paste Channel Strip from %1").arg(from));
    pasteStrip->setEnabled(clipboard.hasSettings);
    connect(pasteStrip, &QAction::triggered, this, [this, channel] {
        if (!m_controller->pasteChannelStrip(channel.toStdString(),
                                             m_controller->channelClipboard()))
            return;
        emit edited();
        emit structureChanged();
    });

    menu->addSeparator();
    QAction* clear = menu->addAction(tr("Clear All Plugins"));
    clear->setEnabled(hasPlugins);
    connect(clear, &QAction::triggered, this, [this, channel] {
        if (!m_controller->pasteChannelInserts(channel.toStdString(), {})) return;
        emit edited();
        emit structureChanged();
    });
    return menu;
}

QMenu* ChannelStrip::buildInsertMenu(QWidget* parent, const QString& insertId,
                                     std::size_t index) {
    const QString channel = channelId();
    const std::vector<daw::InsertModel>* inserts =
        m_controller->channelInserts(channel.toStdString());
    const bool bypassed =
        inserts && index < inserts->size() && (*inserts)[index].bypassed;

    auto* menu = new QMenu(parent);

    connect(menu->addAction(tr("Open Editor")), &QAction::triggered, this,
            [this, channel, insertId] { emit editorRequested(channel, insertId); });
    menu->addSeparator();

    QAction* bypass = menu->addAction(tr("Bypass"));
    bypass->setCheckable(true);
    bypass->setChecked(bypassed);
    connect(bypass, &QAction::triggered, this, [this, channel, insertId](bool on) {
        m_controller->setInsertBypassed(channel.toStdString(), insertId.toStdString(), on);
        emit edited();
        emit structureChanged();
    });

    // The picker menu becomes the submenu itself rather than having its actions
    // copied out: an action belongs to the menu that created it, and moving
    // them leaves ownership split between two menus.
    QMenu* replace = ui::buildPluginMenu(
        menu, m_controller, /*instruments=*/false,
        [this, channel, insertId](const daw::plugins::PluginDescriptor& descriptor) {
            if (!m_controller->replaceInsert(channel.toStdString(),
                                             insertId.toStdString(), descriptor))
                reportPluginFailure(descriptor);
            emit edited();
            emit structureChanged();
        });
    replace->setTitle(tr("Replace with"));
    menu->addMenu(replace);

    menu->addSeparator();
    QAction* up = menu->addAction(tr("Move Up"));
    up->setEnabled(index > 0);
    connect(up, &QAction::triggered, this, [this, channel, insertId, index] {
        m_controller->moveInsert(channel.toStdString(), insertId.toStdString(),
                                 index - 1);
        emit edited();
        emit structureChanged();
    });
    QAction* down = menu->addAction(tr("Move Down"));
    down->setEnabled(inserts && index + 1 < inserts->size());
    connect(down, &QAction::triggered, this, [this, channel, insertId, index] {
        m_controller->moveInsert(channel.toStdString(), insertId.toStdString(),
                                 index + 1);
        emit edited();
        emit structureChanged();
    });

    menu->addSeparator();
    connect(menu->addAction(tr("Remove")), &QAction::triggered, this,
            [this, channel, insertId] {
                m_controller->removeInsert(channel.toStdString(),
                                           insertId.toStdString());
                emit edited();
                emit structureChanged();
            });
    return menu;
}

QWidget* ChannelStrip::buildInstrument() {
    if (m_master) return nullptr;
    const auto* track =
        m_controller->project().findTrack(m_trackId.toStdString());
    // Only the kinds that carry notes have anything to sound them.
    if (!track || !daw::trackAccepts(track->kind, daw::ClipKind::Midi))
        return nullptr;

    const bool loaded = track->instrument.isLoaded();
    const bool filled = !track->instrument.name.empty();
    // The section header already says INSTRUMENT, so the empty slot only has to
    // say it is empty — "NO INSTRUMENT" elides to "NO IN…UMENT" at strip width.
    auto* slot = makeSlotButton(
        filled ? QString::fromStdString(track->instrument.name) : tr("EMPTY"),
        filled);
    const QString trackId = m_trackId;
    const QString instrumentId = QString::fromStdString(track->instrument.id);

    if (loaded) {
        // Same interaction as a loaded insert: click opens the plugin, right
        // click offers the slot's options.
        slot->setProperty("bypassed", track->instrument.bypassed);
        slot->setToolTip(
            track->instrument.bypassed
                ? tr("%1 — bypassed. Click to edit, right-click for options.")
                      .arg(QString::fromStdString(track->instrument.name))
                : tr("%1 — click to edit, right-click for options.")
                      .arg(QString::fromStdString(track->instrument.name)));
        slot->setPopupMode(QToolButton::DelayedPopup);
        connect(slot, &QToolButton::clicked, this, [this, trackId, instrumentId] {
            emit editorRequested(trackId, instrumentId);
        });
        slot->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(slot, &QWidget::customContextMenuRequested, this,
                [this, slot, trackId, instrumentId](const QPoint& at) {
                    auto* menu = new QMenu(slot);
                    connect(menu->addAction(tr("Open Editor")), &QAction::triggered,
                            this, [this, trackId, instrumentId] {
                                emit editorRequested(trackId, instrumentId);
                            });
                    QMenu* replace = ui::buildPluginMenu(
                        menu, m_controller, /*instruments=*/true,
                        [this, trackId](const daw::plugins::PluginDescriptor& d) {
                            if (!m_controller->setTrackInstrumentPlugin(
                                    trackId.toStdString(), d))
                                reportPluginFailure(d);
                            emit edited();
                            emit structureChanged();
                        });
                    replace->setTitle(tr("Replace with"));
                    menu->addMenu(replace);
                    menu->addSeparator();
                    connect(menu->addAction(tr("Remove Instrument")),
                            &QAction::triggered, this, [this, trackId] {
                                m_controller->setTrackInstrumentPlugin(
                                    trackId.toStdString(), {});
                                emit edited();
                                emit structureChanged();
                            });
                    menu->exec(slot->mapToGlobal(at));
                    menu->deleteLater();
                });
        // Addressed by track id and instrument slot id — the same pair an
        // insert uses, so the hover actions are the ones the inserts have.
        return buildSlotWell(
            tr("Instrument"), nullptr,
            {buildSlotRow(slot, trackId, instrumentId,
                          track->instrument.bypassed, /*instrument=*/true)},
            [](const QString& path) { return ui::isAudioFile(path); },
            [this, trackId](const QString& path) { dropSampleOnInstrument(trackId, path); },
            WellDrag{});
    } else {
        slot->setToolTip(tr("What plays this track's notes — click to load one."));
        slot->setMenu(ui::buildLazyPluginMenu(
            slot, m_controller, /*instruments=*/true,
            [this, trackId](const daw::plugins::PluginDescriptor& descriptor) {
                if (!m_controller->setTrackInstrumentPlugin(trackId.toStdString(),
                                                            descriptor)) {
                    reportPluginFailure(descriptor);
                    return;
                }
                const auto* track =
                    m_controller->project().findTrack(trackId.toStdString());
                if (track && !track->instrument.id.empty())
                    emit editorRequested(
                        trackId, QString::fromStdString(track->instrument.id));
                emit edited();
                emit structureChanged();
            }));
    }

    // Dropping a sample on an empty slot is the fastest way to a sound: the
    // sampler goes in and the file goes into it, in one undoable step.
    return buildSlotWell(
        tr("Instrument"), nullptr, {slot},
        [](const QString& path) { return ui::isAudioFile(path); },
        [this, trackId](const QString& path) { dropSampleOnInstrument(trackId, path); },
        WellDrag{});
}

QWidget* ChannelStrip::buildSends() {
    const auto* track =
        m_controller->project().findTrack(m_trackId.toStdString());
    const size_t sendCount = track ? track->sends.size() : 0;
    const int slotCount = std::max<int>(kMinSendSlots, int(sendCount));

    std::vector<QWidget*> rows;
    for (int i = 0; i < slotCount; ++i) {
        if (track && i < int(sendCount)) {
            const daw::SendModel& send = track->sends[size_t(i)];
            const QString sendId = QString::fromStdString(send.id);
            QString destName = tr("Missing");
            if (const auto* dest =
                    m_controller->project().findTrack(send.destinationTrackId))
                destName = QString::fromStdString(dest->name);

            auto* toggle = makeSlotButton(destName, send.enabled);
            toggle->setPopupMode(QToolButton::DelayedPopup);
            toggle->setProperty("bypassed", !send.enabled);
            toggle->setToolTip(
                (send.enabled ? tr("Send to %1, %2. Click to switch it off; the "
                                   "knob sets how much goes.")
                              : tr("Send to %1, %2 — off. Click to switch it on."))
                    .arg(destName,
                         send.preFader ? tr("pre-fader") : tr("post-fader")));
            connect(toggle, &QToolButton::clicked, this,
                    [this, sendId, enabled = send.enabled] {
                        m_controller->setSendEnabled(m_trackId.toStdString(),
                                                     sendId.toStdString(),
                                                     !enabled);
                        emit edited();
                        emit structureChanged();
                    });

            auto* row = new SlotRow(toggle, this);

            // Same three-across shape as a plugin slot, and for the same
            // reason: these are the three things a send is actually adjusted
            // with, and a menu between the pointer and any of them is a tax.
            auto* power = new ui::IconButton(icons::Glyph::Power,
                                             send.enabled ? tr("Switch this send off")
                                                          : tr("Switch this send on"),
                                             row);
            power->setButtonSize(kActionSide, kActionSide);
            power->setCursor(Qt::PointingHandCursor);
            power->setCheckable(true);
            power->setChecked(!send.enabled);
            power->setActiveColor(Theme::mute());
            connect(power, &QAbstractButton::clicked, this,
                    [this, sendId](bool off) {
                        m_controller->setSendEnabled(m_trackId.toStdString(),
                                                     sendId.toStdString(), !off);
                        emit edited();
                        emit structureChanged();
                    });
            row->addSlotAction(power);

            auto* tap = new QToolButton(row);
            tap->setObjectName("TapButton");
            tap->setCursor(Qt::PointingHandCursor);
            tap->setText(send.preFader ? tr("PRE") : tr("PST"));
            tap->setToolTip(send.preFader
                                ? tr("Pre-fader: the send ignores this channel's "
                                     "fader. Click for post-fader.")
                                : tr("Post-fader: the send follows this channel's "
                                     "fader. Click for pre-fader."));
            connect(tap, &QToolButton::clicked, this,
                    [this, sendId, pre = send.preFader] {
                        m_controller->setSendPreFader(m_trackId.toStdString(),
                                                      sendId.toStdString(), !pre);
                        emit edited();
                        emit structureChanged();
                    });
            row->addSlotAction(tap, kTapWidth);

            auto* remove = new ui::IconButton(icons::Glyph::Close,
                                              tr("Remove this send"), row);
            remove->setButtonSize(kActionSide, kActionSide);
            remove->setCursor(Qt::PointingHandCursor);
            connect(remove, &QAbstractButton::clicked, this, [this, sendId] {
                m_controller->removeSend(m_trackId.toStdString(),
                                         sendId.toStdString());
                emit edited();
                emit structureChanged();
            });
            row->addSlotAction(remove);

            // The amount, as a knob rather than the slider that used to sit on
            // a second line: it reads at a glance, it costs no height, and it
            // is where every console puts a send level. It sits at the head of
            // the row, so a column of sends reads down as "this much, to
            // there" rather than the other way round.
            auto* level = new ui::Knob({}, row);
            level->setBare(kSendKnobSide);
            // Up to +6 dB, like a fader: unity is not enough to drive a quiet
            // source into a reverb without turning the bus up under everything
            // else feeding it.
            level->setRange(0.0, double(daw::EngineController::kMaxSendLevel));
            level->setDefaultValue(0.5);
            level->setValue(send.level);
            level->setFormatter([](double v) { return ui::formatGainDb(v); });
            level->setToolTip(tr("Send amount to %1 — up to +6 dB").arg(destName));
            level->setAutomatable(true);
            connect(level, &ui::Knob::automateRequested, this,
                    [this, sendId] {
                        emit automateSendRequested(m_trackId, sendId);
                    });
            auto levelStart = std::make_shared<std::optional<float>>();
            connect(level, &ui::Knob::valueChanged, this,
                    [this, sendId, levelStart](double v) {
                if (!*levelStart) {
                    if (const auto* track = m_controller->project().findTrack(
                            m_trackId.toStdString())) {
                        for (const auto& current : track->sends) {
                            if (current.id == sendId.toStdString()) {
                                *levelStart = current.level;
                                break;
                            }
                        }
                    }
                }
                m_controller->setSendLevel(m_trackId.toStdString(),
                                           sendId.toStdString(), float(v));
            });
            connect(level, &ui::Knob::editFinished, this,
                    [this, sendId, levelStart] {
                if (*levelStart) {
                    m_controller->commitSendLevelEdit(
                        m_trackId.toStdString(), sendId.toStdString(),
                        **levelStart);
                    levelStart->reset();
                }
                emit edited();
            });
            row->setLeading(level);

            rows.push_back(row);
        } else {
            auto* b = makeSlotButton(tr("SEND %1").arg(i + 1), false);
            auto* menu = new QMenu(b);
            connect(menu, &QMenu::aboutToShow, this,
                    [this, menu] { populateAddSendMenu(menu); });
            b->setMenu(menu);
            rows.push_back(b);
        }
    }

    auto* add = new ui::IconButton(icons::Glyph::Plus, tr("Add send"), this);
    add->setButtonSize(16, 14);
    connect(add, &QAbstractButton::clicked, this, [this, add] {
        QMenu menu(this);
        populateAddSendMenu(&menu);
        menu.exec(add->mapToGlobal(QPoint(0, add->height())));
    });
    WellDrag drag;
    drag.dragMime = kSendsMime;
    drag.dragPayload = m_trackId;
    drag.titleTip = tr("Drag onto another channel to move these sends there. "
                       "Hold Alt to copy them instead.");
    return buildSlotWell(tr("Sends"), add, rows, {}, {}, std::move(drag));
}

void ChannelStrip::populateInputMenu(QMenu* menu) {
    menu->clear();
    QAction* none = menu->addAction(tr("No Input"));
    connect(none, &QAction::triggered, this, [this] {
        m_controller->setTrackInputEnabled(m_trackId.toStdString(), false);
        m_inputButton->setFieldText(tr("No Input"));
        emit edited();
    });
    menu->addSeparator();

    const auto inputDevice = m_controller->currentInputDeviceInfo();
    const int channels = int(inputDevice.inputChannels);
    const auto pick = [this](int first, int count, const QString& label) {
        m_controller->setTrackInputChannel(m_trackId.toStdString(),
                                           uint32_t(first));
        m_controller->setTrackInputChannelCount(m_trackId.toStdString(),
                                                uint32_t(count));
        m_controller->setTrackInputEnabled(m_trackId.toStdString(), true);
        m_inputButton->setFieldText(label);
        emit edited();
    };

    for (int ch = 0; ch < channels; ++ch) {
        const QString label = ch < int(inputDevice.inputChannelNames.size())
            ? QString::fromStdString(inputDevice.inputChannelNames[std::size_t(ch)])
            : tr("Input %1").arg(ch + 1);
        QAction* action = menu->addAction(label);
        connect(action, &QAction::triggered, this,
                [pick, ch, label] { pick(ch, 1, label); });
    }
    // The stereo pairs, spelled out. They used to be what picking a single
    // input silently did — "Input 1" captured 1 and 2 — which is why a mono
    // recording came back with a silent right channel.
    if (channels >= 2) {
        menu->addSeparator();
        for (int ch = 0; ch + 1 < channels; ch += 2) {
            const QString left = ch < int(inputDevice.inputChannelNames.size())
                ? QString::fromStdString(inputDevice.inputChannelNames[std::size_t(ch)])
                : QString::number(ch + 1);
            const QString right = ch + 1 < int(inputDevice.inputChannelNames.size())
                ? QString::fromStdString(
                      inputDevice.inputChannelNames[std::size_t(ch + 1)])
                : QString::number(ch + 2);
            const QString label = tr("Inputs %1 + %2").arg(left, right);
            QAction* action = menu->addAction(label);
            connect(action, &QAction::triggered, this,
                    [pick, ch, label] { pick(ch, 2, label); });
        }
    }
}

void ChannelStrip::populateOutputMenu(QMenu* menu) {
    menu->clear();
    QAction* master = menu->addAction(tr("Master"));
    connect(master, &QAction::triggered, this, [this] {
        m_controller->setTrackOutputBus(m_trackId.toStdString(), {});
        m_outputButton->setFieldText(tr("Master"));
        emit edited();
    });

    bool addedSeparator = false;
    for (const auto& t : m_controller->project().tracks) {
        if (t.id == m_trackId.toStdString()) continue;
        // A summing folder is a destination like any other bus; routing into
        // one by hand is how a track joins a group without being filed in it.
        if (t.kind != daw::TrackKind::Bus && t.kind != daw::TrackKind::Aux &&
            t.kind != daw::TrackKind::Group && !daw::isSummingFolder(t))
            continue;
        if (!addedSeparator) {
            menu->addSeparator();
            addedSeparator = true;
        }
        const QString id = QString::fromStdString(t.id);
        const QString name = QString::fromStdString(t.name);
        QAction* action = menu->addAction(name);
        connect(action, &QAction::triggered, this, [this, id, name] {
            if (!m_controller->setTrackOutputBus(m_trackId.toStdString(),
                                                 id.toStdString())) {
                QToolTip::showText(QCursor::pos(),
                                   tr("That routing would feed back on itself"),
                                   m_outputButton);
                return;
            }
            m_outputButton->setFieldText(name);
            emit edited();
        });
    }
}

void ChannelStrip::populateAddSendMenu(QMenu* menu) {
    menu->clear();
    bool any = false;
    for (const auto& t : m_controller->project().tracks) {
        if (t.id == m_trackId.toStdString()) continue;
        if (t.kind != daw::TrackKind::Bus && t.kind != daw::TrackKind::Aux &&
            !daw::isSummingFolder(t))
            continue;
        any = true;
        const QString id = QString::fromStdString(t.id);
        QAction* action = menu->addAction(QString::fromStdString(t.name));
        connect(action, &QAction::triggered, this, [this, id] {
            m_controller->addSend(m_trackId.toStdString(), id.toStdString());
            emit edited();
            emit structureChanged();
        });
    }
    if (!any) {
        QAction* hint = menu->addAction(tr("Add a bus track to send to"));
        hint->setEnabled(false);
    }
}

QWidget* ChannelStrip::buildFaderRow() {
    auto* box = new QWidget(this);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(7);

    m_meter = new ui::LevelMeter(Qt::Vertical, 2, box);
    m_meter->setMinimumHeight(60);
    m_meter->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_fader = new ui::FaderWidget(box);
    m_fader->setMinimumHeight(60);
    // The dB scale is printed down the fader's left, with the meter on its
    // right — the reading order of every console: numbers, cap, level.
    m_fader->setScaleVisible(true);
    m_fader->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    connect(m_fader, &ui::FaderWidget::gainChanged, this, [this](double g) {
        if (m_master) {
            if (!m_volumeGestureStart)
                m_volumeGestureStart = m_controller->masterVolume();
            m_controller->setMasterVolumeLive(float(g));
        } else {
            const std::string id = m_trackId.toStdString();
            if (!m_volumeGestureStart) {
                if (const auto* track = m_controller->project().findTrack(id))
                    m_volumeGestureStart = track->volume;
            }
            m_controller->setTrackVolumeLive(id, float(g));
        }
        m_gainLabel->setText(ui::formatGainDb(g));
    });
    connect(m_fader, &ui::FaderWidget::editFinished, this, [this] {
        if (m_volumeGestureStart) {
            if (m_master) {
                m_controller->commitMasterVolumeEdit(*m_volumeGestureStart);
            } else {
                m_controller->commitTrackVolumeEdit(
                    {{m_trackId.toStdString(), *m_volumeGestureStart}});
            }
            m_volumeGestureStart.reset();
        }
        emit edited();
    });
    if (!m_master) {
        m_fader->setAutomatable(true);
        connect(m_fader, &ui::FaderWidget::automateRequested, this,
                [this] { emit automateControlRequested(m_trackId, false); });
    }

    row->addStretch(1);
    row->addWidget(m_fader);
    row->addWidget(m_meter);
    row->addStretch(1);
    return box;
}

QWidget* ChannelStrip::buildButtons() {
    auto* box = new QWidget(this);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);

    m_mute = new ui::MsrButton("M", Theme::mute(), tr("Mute"), box);
    if (!m_master) {
        m_mute->setAutomatable(true);
        connect(m_mute, &ui::MsrButton::automateRequested, this,
                [this] { emit automateMuteRequested(m_trackId); });
    }
    connect(m_mute, &QAbstractButton::toggled, this, [this](bool on) {
        if (m_master) return;
        const auto result =
            m_controller->setTrackMuted(m_trackId.toStdString(), on);
        syncFromModel();
        emit edited(daw::collab::marksLocalFileDirty(result));
    });
    m_solo = new ui::MsrButton("S", Theme::solo(), tr("Solo"), box);
    connect(m_solo, &QAbstractButton::toggled, this, [this](bool on) {
        if (m_master) return;
        m_controller->setTrackSoloed(m_trackId.toStdString(), on);
        emit edited();
    });
    row->addWidget(m_mute);
    row->addWidget(m_solo);

    const auto* track = m_master
                            ? nullptr
                            : m_controller->project().findTrack(
                                  m_trackId.toStdString());
    const bool pattern = track && track->kind == daw::TrackKind::Pattern;
    if (pattern) {
        auto* open = new ui::MsrButton("P", colorFromRgb(track->color),
                                       tr("Open Pattern editor"), box);
        open->setAccessibleName(tr("Open Pattern editor"));
        connect(open, &QAbstractButton::clicked, this,
                [this] { emit patternRequested(m_trackId); });
        row->addWidget(open);
    } else if (!m_master) {
        m_monitor = new ui::MsrButton("I", th().accent,
                                      tr("Input monitoring"), box);
        connect(m_monitor, &QAbstractButton::toggled, this, [this](bool on) {
            m_controller->setTrackMonitor(m_trackId.toStdString(), on);
            emit edited();
        });
        row->addWidget(m_monitor);
    }
    if (m_master) {
        m_mute->setEnabled(false);
        m_solo->setEnabled(false);
    }
    return box;
}

QWidget* ChannelStrip::buildNamePlate() {
    QString name = tr("MASTER");
    uint32_t color = 0x888888;
    if (!m_master) {
        if (const auto* t =
                m_controller->project().findTrack(m_trackId.toStdString())) {
            name = QString::fromStdString(t->name).toUpper();
            color = t->color;
        }
    }
    auto* plate = new QLabel(name, this);
    m_namePlate = plate;
    plate->setObjectName("NamePlate");
    plate->setAlignment(Qt::AlignCenter);
    plate->setFixedHeight(22);
    updateNamePlate(name, color);
    return plate;
}

void ChannelStrip::updateNamePlate(const QString& name, std::uint32_t color) {
    if (!m_namePlate) return;
    if (m_namePlate->text() != name) m_namePlate->setText(name);
    const QColor tint = colorFromRgb(color);
    const QColor ink = m_master ? th().accent : th().textPrimary;
    const QString styleKey = tint.name(QColor::HexArgb) + '/' +
                             ink.name(QColor::HexArgb);
    if (m_namePlateStyleKey == styleKey) return;
    m_namePlateStyleKey = styleKey;
    m_namePlate->setStyleSheet(
        QString("background: rgba(%1,%2,%3,40); border-radius: 4px; "
                "font-weight: 700; font-size: 10px; color: %4;")
            .arg(tint.red())
            .arg(tint.green())
            .arg(tint.blue())
            .arg(ink.name()));
}

void ChannelStrip::setStretchable(bool stretchable) {
    m_stretchable = stretchable;
    setSizePolicy(QSizePolicy::Fixed,
                  stretchable ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    setMinimumHeight(m_naturalHeight > 0 ? m_naturalHeight : kFallbackHeight);
    updateGeometry();
}

void ChannelStrip::setSelected(bool selected) {
    if (m_selected == selected) return;
    m_selected = selected;
    update();
}

void ChannelStrip::syncFromModel() {
    // These controls report user edits through their value signals. A value
    // arriving from another view must not echo back into the controller (and
    // recursively rebuild/synchronise the UI again).
    const QSignalBlocker blockFader(m_fader);
    const QSignalBlocker blockPan(m_pan);
    const QSignalBlocker blockMute(m_mute);
    const QSignalBlocker blockSolo(m_solo);
    const std::optional<QSignalBlocker> blockMonitor =
        m_monitor ? std::optional<QSignalBlocker>{std::in_place, m_monitor}
                  : std::nullopt;
    const std::optional<QSignalBlocker> blockMono =
        m_monoButton
            ? std::optional<QSignalBlocker>{std::in_place, m_monoButton}
            : std::nullopt;
    if (m_master) {
        m_fader->setGain(m_controller->masterVolume());
        m_pan->setPan(m_controller->project().masterPan);
        updateNamePlate(tr("MASTER"), 0x888888);
    } else if (const auto* t =
                   m_controller->project().findTrack(m_trackId.toStdString())) {
        updateNamePlate(QString::fromStdString(t->name).toUpper(), t->color);
        m_fader->setGain(t->volume);
        m_pan->setPan(t->pan);
        m_mute->setChecked(t->muted);
        m_solo->setChecked(t->soloed);
        if (m_monitor) m_monitor->setChecked(t->monitor);
        if (m_monoButton) {
            m_monoButton->setChecked(t->mono);
            m_monoButton->setGlyph(t->mono ? icons::Glyph::MonoRing
                                           : icons::Glyph::StereoRings);
            m_monoButton->setToolTip(t->mono ? tr("Mono") : tr("Stereo"));
        }
        if (m_inputButton) {
            m_inputButton->setFieldText(
                !t->inputEnabled ? tr("No Input")
                : t->inputChannelCount >= 2
                    ? tr("Inputs %1+%2")
                          .arg(t->inputChannel + 1)
                          .arg(t->inputChannel + 2)
                    : tr("Input %1").arg(t->inputChannel + 1));
        }
        if (m_outputButton) {
            QString outName = tr("Master");
            if (const auto* bus = m_controller->project().findTrack(t->outputBusId))
                outName = QString::fromStdString(bus->name);
            m_outputButton->setFieldText(outName);
        }
    }
    updateReadouts();
}

void ChannelStrip::refreshAutomationValues() {
    if (m_master || !m_controller) return;
    const auto* track =
        m_controller->project().findTrack(m_trackId.toStdString());
    if (!track) return;

    double gain = track->volume;
    double pan = track->pan;
    if (m_controller->isPlaying()) {
        daw::AutomationTarget volume;
        volume.kind = daw::AutomationTargetKind::TrackVolume;
        volume.channelId = track->id;
        if (const auto value = m_controller->automationValueAtPlayhead(volume))
            gain = *value;

        daw::AutomationTarget panorama;
        panorama.kind = daw::AutomationTargetKind::TrackPan;
        panorama.channelId = track->id;
        if (const auto value = m_controller->automationValueAtPlayhead(panorama))
            pan = *value;
    }

    if (!m_fader->isEditing()) {
        const QSignalBlocker blocker(m_fader);
        m_fader->setGain(gain);
    }
    if (!m_pan->isEditing()) {
        const QSignalBlocker blocker(m_pan);
        m_pan->setPan(pan);
    }
    updateReadouts();
}

void ChannelStrip::updateReadouts() {
    const QString gain = ui::formatGainDb(m_fader->gain());
    const QString pan = panText(m_pan->pan());
    if (m_gainLabel->text() != gain) m_gainLabel->setText(gain);
    if (m_panLabel->text() != pan) m_panLabel->setText(pan);
}

double ChannelStrip::faderGainForTest() const {
    return m_fader ? m_fader->gain() : -1.0;
}

void ChannelStrip::refreshMeter() {
    if (m_master) {
        // The master bus reports separate L/R peaks; channels publish one
        // summed peak, so both bars show the same value there.
        m_meter->setPeaks(m_controller->masterPeakLeft(),
                          m_controller->masterPeakRight());
    } else {
        m_meter->setPeak(m_controller->trackPeak(m_trackId.toStdString()));
    }
}


void ChannelStrip::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton && !m_master)
        emit selectRequested(m_trackId);
    QWidget::mousePressEvent(ev);
}

void ChannelStrip::contextMenuEvent(QContextMenuEvent* ev) {
    if (m_master) return;
    QMenu menu(this);
    QMenu* automation = menu.addMenu(
        icons::icon(icons::Glyph::AutomationCreate, th().textPrimary),
        tr("Create Automation Clip"));
    QAction* volume = automation->addAction(tr("Volume"));
    QAction* pan = automation->addAction(tr("Pan"));
    QAction* mute = automation->addAction(tr("Mute"));
    QHash<QAction*, QString> sends;
    if (const auto* track =
            m_controller->project().findTrack(m_trackId.toStdString())) {
        for (const daw::SendModel& send : track->sends) {
            QString destination = tr("Send");
            if (const auto* target =
                    m_controller->project().findTrack(send.destinationTrackId)) {
                destination = QString::fromStdString(target->name);
            }
            QAction* action = automation->addAction(tr("Send to %1").arg(destination));
            sends.insert(action, QString::fromStdString(send.id));
        }
    }
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("Remove Track"));
    QAction* chosen = menu.exec(ev->globalPos());
    if (chosen == volume) emit automateControlRequested(m_trackId, false);
    else if (chosen == pan) emit automateControlRequested(m_trackId, true);
    else if (chosen == mute) emit automateMuteRequested(m_trackId);
    else if (sends.contains(chosen))
        emit automateSendRequested(m_trackId, sends.value(chosen));
    else if (chosen == remove)
        emit removeRequested(m_trackId);
}

void ChannelStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QColor fill = m_master ? t.surfaceElevated : t.surface;
    if (m_selected) fill = mixColors(fill, t.accent, 0.16);
    // A chain hovering over the strip lights the whole strip: what is about to
    // change is the channel, not the slot the pointer happens to be over.
    if (m_dropHighlight) fill = mixColors(fill, t.accent, 0.3);
    p.setBrush(fill);
    p.setPen(QPen(m_dropHighlight || m_selected ? t.accent : t.separator(),
                  m_dropHighlight ? 2.0 : m_selected ? 1.6 : 1.0));
    p.drawRoundedRect(r, 9, 9);
}

// ── Dragging a whole section from one strip to another ─────────────────────
//
// The strip itself takes these drops rather than any one section: the target is
// a *channel*, and asking the user to land on the same small well they picked
// up from would make an easy gesture fussy.

namespace {
/// What a drag over a strip carries, if anything this strip can take.
const char* stripDropKind(const QMimeData* mime) {
    if (!payloadOf(mime, kChainMime).isEmpty()) return kChainMime;
    if (!payloadOf(mime, kSendsMime).isEmpty()) return kSendsMime;
    return nullptr;
}
}  // namespace

std::optional<daw::plugins::PluginDescriptor> ChannelStrip::pluginFromMime(
    const QMimeData* mime) const {
    int format = 0;
    QString uid;
    if (!ui::decodePluginRef(mime, format, uid)) return std::nullopt;
    return m_controller->pluginManager().find(daw::plugins::Format(format),
                                              uid.toStdString());
}

QString ChannelStrip::presetFromMime(const QMimeData* mime) const {
    if (!mime || !mime->hasUrls()) return {};
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (ui::channelstrippresets::isPresetFile(path)) return path;
    }
    return {};
}

bool ChannelStrip::hasBrowserDrop(const QMimeData* mime) const {
    int format = 0;
    QString uid;
    return ui::decodePluginRef(mime, format, uid) ||
           !presetFromMime(mime).isEmpty();
}

void ChannelStrip::acceptBrowserDrop(const QMimeData* mime) {
    const QString preset = presetFromMime(mime);
    if (!preset.isEmpty()) {
        const audio::Result result = m_controller->applyChannelStripPreset(
            channelId().toStdString(), preset.toStdString());
        if (!result) {
            QToolTip::showText(
                QCursor::pos(),
                tr("%1 could not be applied: %2")
                    .arg(ui::channelstrippresets::displayName(preset),
                         QString::fromStdString(result.message())),
                this);
            return;
        }
        emit edited();
        emit structureChanged();
        return;
    }

    const auto descriptor = pluginFromMime(mime);
    if (!descriptor) {
        QToolTip::showText(
            QCursor::pos(),
            tr("This plugin is no longer available. Rescan plugins and try again."),
            this);
        return;
    }

    bool landed = false;
    QString editorChannel;
    QString editorSlot;
    if (descriptor->isInstrument) {
        const auto* track = m_master
            ? nullptr
            : m_controller->project().findTrack(m_trackId.toStdString());
        if (!track || !daw::trackAccepts(track->kind, daw::ClipKind::Midi)) {
            QToolTip::showText(
                QCursor::pos(),
                tr("%1 is an instrument. Drop it on a MIDI, Instrument, or "
                   "Pattern track.")
                    .arg(QString::fromStdString(descriptor->name)),
                this);
            return;
        }
        landed = m_controller->setTrackInstrumentPlugin(
            m_trackId.toStdString(), *descriptor);
        const auto* loadedTrack = landed
            ? m_controller->project().findTrack(m_trackId.toStdString())
            : nullptr;
        if (loadedTrack && !loadedTrack->instrument.id.empty()) {
            editorChannel = m_trackId;
            editorSlot = QString::fromStdString(loadedTrack->instrument.id);
        }
    } else {
        const QString channel = channelId();
        const std::string id =
            m_controller->addInsert(channel.toStdString(), *descriptor);
        landed = !id.empty();
        if (landed) {
            editorChannel = channel;
            editorSlot = QString::fromStdString(id);
        }
    }

    if (!landed) {
        reportPluginFailure(*descriptor);
        return;
    }
    if (!editorChannel.isEmpty() && !editorSlot.isEmpty())
        emit editorRequested(editorChannel, editorSlot);
    emit edited();
    emit structureChanged();
}

bool ChannelStrip::eventFilter(QObject* watched, QEvent* event) {
    (void)watched;
    if (event->type() == QEvent::DragEnter) {
        auto* drag = static_cast<QDragEnterEvent*>(event);
        if (!hasBrowserDrop(drag->mimeData()))
            return QWidget::eventFilter(watched, event);
        m_browserDropActive = true;
        m_dropHighlight = true;
        update();
        drag->acceptProposedAction();
        return true;
    }
    if (event->type() == QEvent::DragMove && m_browserDropActive) {
        static_cast<QDragMoveEvent*>(event)->acceptProposedAction();
        return true;
    }
    if (event->type() == QEvent::DragLeave && m_browserDropActive) {
        m_browserDropActive = false;
        m_dropHighlight = false;
        update();
        return true;
    }
    if (event->type() == QEvent::Drop) {
        auto* drop = static_cast<QDropEvent*>(event);
        if (!hasBrowserDrop(drop->mimeData()))
            return QWidget::eventFilter(watched, event);
        m_browserDropActive = false;
        m_dropHighlight = false;
        update();
        drop->acceptProposedAction();
        acceptBrowserDrop(drop->mimeData());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void ChannelStrip::dragEnterEvent(QDragEnterEvent* ev) {
    if (hasBrowserDrop(ev->mimeData())) {
        m_browserDropActive = true;
        m_dropHighlight = true;
        update();
        ev->acceptProposedAction();
        return;
    }
    const char* kind = stripDropKind(ev->mimeData());
    if (!kind) return;
    // Sends belong to a track; the master has none to receive.
    if (kind == kSendsMime && m_master) return;
    if (payloadOf(ev->mimeData(), kind) == channelId()) return;   // itself
    m_dropHighlight = true;
    update();
    ev->acceptProposedAction();
}

void ChannelStrip::dragMoveEvent(QDragMoveEvent* ev) {
    if (m_browserDropActive) {
        ev->acceptProposedAction();
        return;
    }
    if (m_dropHighlight) ev->acceptProposedAction();
}

void ChannelStrip::dragLeaveEvent(QDragLeaveEvent*) {
    m_browserDropActive = false;
    m_dropHighlight = false;
    update();
}

void ChannelStrip::dropEvent(QDropEvent* ev) {
    if (hasBrowserDrop(ev->mimeData())) {
        m_browserDropActive = false;
        m_dropHighlight = false;
        update();
        ev->acceptProposedAction();
        acceptBrowserDrop(ev->mimeData());
        return;
    }
    m_dropHighlight = false;
    update();
    const char* kind = stripDropKind(ev->mimeData());
    if (!kind) return;
    const QString source = payloadOf(ev->mimeData(), kind);
    if (source.isEmpty() || source == channelId()) return;
    ev->acceptProposedAction();
    // Alt (Option) copies; a plain drag moves, which is what dragging a thing
    // out of one place and into another means everywhere else.
    const bool copy = ev->modifiers() & Qt::AltModifier;
    if (kind == kChainMime) acceptChainDrop(source, copy);
    else acceptSendsDrop(source, copy);
}

void ChannelStrip::acceptChainDrop(const QString& sourceChannel, bool copy) {
    const auto chain = m_controller->copyChannelStrip(sourceChannel.toStdString(),
                                                      /*withSettings=*/false);
    if (chain.inserts.empty()) return;
    const std::size_t mark = m_controller->undoDepth();
    if (!m_controller->pasteChannelInserts(channelId().toStdString(), chain)) return;
    if (!copy) {
        // Moved, not copied: the chain left the channel it came from — one
        // gesture, so one undo entry covers both halves of it.
        m_controller->pasteChannelInserts(sourceChannel.toStdString(), {});
        m_controller->collapseUndo(mark, tr("Move Plugins").toStdString());
    }
    emit edited();
    emit structureChanged();
}

void ChannelStrip::acceptSendsDrop(const QString& sourceTrackId, bool copy) {
    if (m_master) return;
    if (!m_controller->copySendsTo(sourceTrackId.toStdString(),
                                   m_trackId.toStdString(), !copy))
        return;
    emit edited();
    emit structureChanged();
}

void ChannelStrip::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
QLabel { color: %TEXT2%; font-size: 10px; }
#StripName { color: %TEXT%; font-size: 11px; font-weight: 600; }
/* An I/O plate. Recessed rather than raised: it is a field showing where the
   signal comes from and goes to, not a button you press for an action. The
   padding is what keeps the destination centred between the caption RoutingField
   paints on the left and the caret it paints on the right. */
#RoutingButton {
    background: %RECESS%; border: 1px solid %SEP%; border-radius: 5px;
    color: %TEXT%; font-size: 9px; font-weight: 600; padding: 0 14px;
}
#RoutingButton:hover { background: %WELL%; border-color: %ACCENT_SOFT%; }
#RoutingButton:disabled { color: %TEXT2%; }
#RoutingButton::menu-indicator { image: none; width: 0; }
#SlotWell { background: %WELL%; border: 1px solid %SEP%; border-radius: 7px; }
#SlotButton {
    background: %SLOT%; border: 1px solid %SEP%; border-radius: 4px;
    color: %TEXT2%; font-size: 9px; font-weight: 600; padding: 0 5px;
    text-align: left;
}
#SlotButton[active="true"] { color: %TEXT%; border-color: %ACCENT%; }
/* Bypassed reads as "off" without the pointer on it: the accent border is
   traded for a muted red and the name dims. Listed after [active] so it wins
   — same specificity, later rule. */
#SlotButton[bypassed="true"] { color: %DIM%; border-color: %BYPASS%; }
/* A slot whose plugin is named in the project but is not loaded — uninstalled,
   unlicensed, or moved since the scan. It has to look wrong: it is a hole in
   the chain the user cannot hear. */
#SlotButton[missing="true"] { color: %BYPASS%; border-color: %BYPASS%; }
#SlotButton:hover { background: %HOVER%; }
#SlotButton::menu-indicator { image: none; width: 0; }
#SlotButton:disabled { color: %TEXT2%; }
/* PRE / PST: one of the three actions revealed on a hovered send row, and the
   only one that says its state in letters rather than in a glyph. */
#TapButton {
    background: %SLOT%; border: 1px solid %SEP%; border-radius: 4px;
    color: %ACCENT%; font-size: 8px; font-weight: 700; padding: 0;
}
#TapButton:hover { background: %HOVER%; }
#TapButton::menu-indicator { image: none; width: 0; }
)")
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%WELL%", t.well().name())
        .replace("%RECESS%", mixColors(t.well(), QColor(0, 0, 0),
                                       t.dark ? 0.40 : 0.10).name())
        .replace("%ACCENT_SOFT%", mixColors(t.separator(), t.accent, 0.55).name())
        .replace("%SLOT%", mixColors(t.surfaceElevated, t.background, 0.2).name())
        .replace("%SEP%", t.separator().name())
        .replace("%HOVER%", mixColors(t.surfaceElevated, t.textPrimary, 0.12).name())
        .replace("%DIM%", mixColors(t.textSecondary, t.background, 0.35).name())
        .replace("%BYPASS%", mixColors(Theme::mute(), t.background, 0.45).name())
        .replace("%ACCENT%", t.accent.name()));

    m_namePlateStyleKey.clear();
    if (m_master) {
        updateNamePlate(tr("MASTER"), 0x888888);
    } else if (const auto* track =
                   m_controller->project().findTrack(m_trackId.toStdString())) {
        updateNamePlate(QString::fromStdString(track->name).toUpper(),
                        track->color);
    }
    update();
}
