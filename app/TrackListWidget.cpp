#include "TrackListWidget.hpp"
#include "CompLayout.hpp"
#include "Controls.hpp"
#include "ChannelStripPresets.hpp"
#include "ProjectTemplates.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "FileTypes.hpp"
#include "UiConstants.hpp"

#include "EngineController.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineF>
#include <QSignalBlocker>
#include <QLinearGradient>
#include <QMenu>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
/// Horizontal offset per folder level.
constexpr int kIndentStep = 14;
/// How far past its right edge a row's card is drawn before being clipped. Only
/// has to exceed the corner radius: everything past the edge is thrown away,
/// and what is left is a square side that meets the timeline with no seam.
constexpr double kRightOverhang = 12.0;

/// The kind icon's tile. Big enough to be the row's landmark — it is what the
/// eye finds a track by when the names are all "Audio 4" — and tall enough to
/// stand beside both the name and the chip strip under it.
constexpr int kIconSize = 34;

/// The chips: four of them plus the fader and the pan on one line, so they are
/// deliberately smaller than the mixer's.
constexpr int kChipW = 19;
constexpr int kChipH = 15;
constexpr int kChipGap = 3;

/// What each part of a row needs before it is worth showing at all.
constexpr int kFaderMin = 68;
constexpr int kPanWidth = 28;
constexpr int kPanHeight = 28;
constexpr int kPartGap = 6;
constexpr int kFullChipStrip = 4 * kChipW + 3 * kChipGap;

/// Full-height row geometry stays aligned to the timeline, while the painted
/// inset surface gives each channel a modern card silhouette and breathing
/// room. This avoids the old stack of edge-to-edge grey rectangles.
class TrackRowSurface final : public QWidget {
public:
    explicit TrackRowSurface(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMouseTracking(true);
    }

    /// `primary` is the one row of a multi-row selection that the single-track
    /// panels are showing. Without it a selection of six looks like six equally
    /// current tracks, and nothing says which one the inspector is about.
    void setVisualState(bool selected, bool primary, bool folder) {
        if (m_selected == selected && m_primary == primary && m_folder == folder)
            return;
        m_selected = selected;
        m_primary = primary;
        m_folder = folder;
        update();
    }

    /// The row's own colour, which is what a selection is washed with unless
    /// the user has asked for the neutral one.
    void setTrackColor(const QColor& color) {
        if (m_color == color) return;
        m_color = color;
        update();
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Enter) {
            m_hovered = true;
            update();
        } else if (event->type() == QEvent::Leave) {
            m_hovered = false;
            update();
        }
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();

        p.fillRect(rect(), mixColors(t.background, t.surface, 0.18));
        // Rounded on the left, square and flush on the right: the header is one
        // half of a lane whose other half is the timeline, and a gap with two
        // more corners in it would read as two separate things. The card is
        // drawn past the right edge and clipped, which is what leaves that side
        // square without hand-building a path for four corners.
        //
        // Vertically it takes all but a hairline: the rows are a stack, not a
        // list of cards, and the space between them was height the lanes could
        // have been using.
        const QRectF card = QRectF(rect()).adjusted(4.5, 0.5, kRightOverhang, -0.5);
        p.setClipRect(rect());

        QColor base = m_folder ? mixColors(t.surface, t.background, 0.40)
                               : mixColors(t.surface, t.surfaceElevated, 0.10);
        const QColor wash = ui::selectionWash(m_color);
        if (m_selected) {
            base = mixColors(base, wash,
                             (t.dark ? 0.20 : 0.14) * (m_primary ? 1.0 : 0.6));
        }
        if (m_hovered && !m_selected)
            base = mixColors(base, t.textPrimary, t.dark ? 0.045 : 0.025);

        QLinearGradient fill(card.topLeft(), card.bottomLeft());
        fill.setColorAt(0.0, mixColors(base, t.surfaceElevated, 0.25));
        fill.setColorAt(1.0, mixColors(base, t.background, 0.05));
        p.setBrush(fill);

        QColor edge = m_selected
                          ? mixColors(t.sectionDivider(), wash,
                                      m_primary ? 0.75 : 0.45)
                          : mixColors(t.separator(), t.surfaceElevated, 0.20);
        p.setPen(QPen(edge, m_selected ? 1.35 : 1.0));
        p.drawRoundedRect(card, 8.0, 8.0);

        if (m_primary) {
            QColor shine = mixColors(wash, t.textPrimary, 0.25);
            shine.setAlpha(t.dark ? 80 : 56);
            p.setPen(QPen(shine, 1.0));
            p.drawLine(QPointF(card.left() + 9.0, card.top() + 1.0),
                       QPointF(double(width()), card.top() + 1.0));
        }
    }

private:
    bool m_selected = false;
    bool m_primary = false;
    bool m_folder = false;
    bool m_hovered = false;
    QColor m_color;
};

class TrackColorRail final : public QWidget {
public:
    TrackColorRail(const QColor& color, QWidget* parent)
        : QWidget(parent), m_color(color) {
        setFixedWidth(6);
    }

    void setTrackColor(const QColor& color) {
        if (m_color == color) return;
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient gradient(0, 5, 0, height() - 5);
        gradient.setColorAt(0.0, m_color.lighter(122));
        gradient.setColorAt(1.0, m_color.darker(112));
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 5, -1, -5), 2, 2);
    }

private:
    QColor m_color;
};

/// The row's landmark: a tinted tile carrying the glyph for what kind of track
/// this is. On a folder it is also the disclosure control — a folder's icon is
/// the obvious thing to click to open it, and it is a far bigger target than a
/// chevron the row has no width for.
class TrackIcon final : public QAbstractButton {
public:
    TrackIcon(icons::Glyph glyph, const QColor& color, QWidget* parent)
        : QAbstractButton(parent), m_glyph(glyph), m_color(color) {
        setFixedSize(kIconSize, kIconSize);
        setFocusPolicy(Qt::NoFocus);
        // Not a button unless something makes it one; a plain track's icon is
        // decoration and must not eat the click that selects the row.
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    void setTrackColor(const QColor& color) {
        if (m_color == color) return;
        m_color = color;
        update();
    }

    void makeDisclosure(bool expanded) {
        m_disclosure = true;
        m_expanded = expanded;
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();

        const QRectF tile = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QColor fill = m_color;
        fill.setAlpha(t.dark ? 46 : 34);
        p.setPen(QPen(mixColors(m_color, t.background, t.dark ? 0.55 : 0.35), 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(tile, 8.0, 8.0);

        // The glyphs are drawn on a 24-unit grid; give this one the tile minus
        // its padding so a bigger tile means a bigger icon, not more air.
        icons::paint(p, m_glyph, tile.adjusted(5, 5, -5, -5),
                     mixColors(m_color, t.textPrimary, 0.30));

        if (!m_disclosure) return;
        // A chevron tucked into the corner: this tile opens and closes. It sits
        // on a disc of the row's own colour, so it stays a legible badge over
        // whatever part of the folder glyph happens to be under it.
        const QPointF centre(tile.right() - 5.5, tile.bottom() - 5.0);
        p.setPen(Qt::NoPen);
        p.setBrush(mixColors(t.surface, m_color, 0.16));
        p.drawEllipse(centre, 6.0, 6.0);
        QPen pen(mixColors(m_color, t.textPrimary, 0.55), 1.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const double dy = m_expanded ? 1.6 : -1.6;
        p.drawPolyline(QPolygonF({centre + QPointF(-3.2, -dy), centre,
                                  centre + QPointF(3.2, -dy)}));
    }

private:
    icons::Glyph m_glyph;
    QColor m_color;
    bool m_disclosure = false;
    bool m_expanded = false;
};

/// Drop feedback for a drag. It has to be its own child widget raised above the
/// rows: anything the list paints itself would be covered by the opaque row
/// containers on top of it.
class DropIndicator : public QWidget {
public:
    explicit DropIndicator(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        hide();
    }

    void showLine(const QRect& area) {
        m_folderMode = false;
        setGeometry(area);
        raise();
        show();
        update();
    }

    void showFolder(const QRect& area) {
        m_folderMode = true;
        setGeometry(area);
        raise();
        show();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor accent = th().accent;
        if (m_folderMode) {
            p.setPen(QPen(accent, 2));
            p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 50));
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
            return;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(rect()), 1.5, 1.5);
    }

private:
    bool m_folderMode = false;
};

QString panText(double pan) {
    if (std::abs(pan) < 0.01) return QStringLiteral("C");
    return (pan < 0 ? QStringLiteral("L") : QStringLiteral("R")) +
           QString::number(int(std::round(std::abs(pan) * 100.0)));
}

/// The track's kind in words, for the icon's tooltip. The document's own
/// `toString` is the on-disk spelling ("midi", "bus") and is not for reading.
QString trackKindName(daw::TrackKind kind) {
    switch (kind) {
        case daw::TrackKind::Audio:      return QObject::tr("Audio track");
        case daw::TrackKind::Midi:       return QObject::tr("MIDI track");
        case daw::TrackKind::Instrument: return QObject::tr("MIDI track");
        case daw::TrackKind::Pattern:    return QObject::tr("Pattern track");
        case daw::TrackKind::Automation: return QObject::tr("Automation lane");
        case daw::TrackKind::Bus:        return QObject::tr("Bus");
        case daw::TrackKind::Aux:        return QObject::tr("Send track");
        case daw::TrackKind::Group:      return QObject::tr("Group");
        case daw::TrackKind::Master:     return QObject::tr("Master");
        case daw::TrackKind::Folder:     return QObject::tr("Folder");
    }
    return {};
}

/// What a track looks like at a glance. One mapping, so the header, and
/// anything else that grows an icon later, cannot disagree about what a bus is.
icons::Glyph glyphForTrack(const daw::TrackModel& track) {
    switch (track.kind) {
        case daw::TrackKind::Audio:      return icons::Glyph::Waveform;
        case daw::TrackKind::Midi:       return icons::Glyph::MidiKeys;
        case daw::TrackKind::Instrument: return icons::Glyph::MidiKeys;
        case daw::TrackKind::Pattern:    return icons::Glyph::Grid;
        case daw::TrackKind::Automation: return icons::Glyph::Automation;
        case daw::TrackKind::Bus:
        case daw::TrackKind::Group:      return icons::Glyph::Mixer;
        case daw::TrackKind::Aux:        return icons::Glyph::ArrowRight;
        case daw::TrackKind::Master:     return icons::Glyph::Headphones;
        case daw::TrackKind::Folder:
            return track.summing ? icons::Glyph::FolderSum : icons::Glyph::Folder;
    }
    return icons::Glyph::Waveform;
}
} // namespace

TrackListWidget::TrackListWidget(daw::EngineController* controller,
                                 QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("TrackHeaders");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(ui::kTrackHeaderWidth);
    setMouseTracking(true);
    setAcceptDrops(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* ruler = new QWidget(this);
    ruler->setObjectName("TrackListRuler");
    ruler->setFixedHeight(ui::kRulerHeight);
    auto* rulerRow = new QHBoxLayout(ruler);
    rulerRow->setContentsMargins(11, 0, 11, 0);
    rulerRow->setSpacing(7);
    // Where the word "TRACKS" used to be: the two chips that lift a mute or a
    // solo anywhere in the project. They are the same chips the rows carry —
    // and they light for the same reason, because *something* down the list is
    // in that state. A label saying "tracks" over a column of tracks was the
    // least useful thing that could occupy this corner.
    m_clearMutes = new ui::MsrButton("M", Theme::mute(),
                                     tr("Unmute every track"), ruler);
    m_clearSolos = new ui::MsrButton("S", Theme::solo(),
                                     tr("Clear every solo"), ruler);
    // They light like the row chips do, but they are not a state of their own:
    // whatever the click leaves the project in, `refreshGlobalChips` puts the
    // lamp back in step with it.
    connect(m_clearMutes, &QAbstractButton::clicked, this, [this] {
        const auto result = m_controller->clearAllMutes();
        syncTrackValues();
        refreshGlobalChips();
        emit tracksChanged(daw::collab::marksLocalFileDirty(result));
    });
    connect(m_clearSolos, &QAbstractButton::clicked, this, [this] {
        m_controller->clearAllSolos();
        refreshGlobalChips();
        emit tracksChanged();
    });

    auto* hint = new QLabel(tr("ARRANGEMENT"), ruler);
    hint->setObjectName("TrackListHint");
    rulerRow->addWidget(m_clearMutes);
    rulerRow->addWidget(m_clearSolos);
    rulerRow->addStretch(1);
    rulerRow->addWidget(hint);
    outer->addWidget(ruler);

    // The rows live on a host widget inside a fixed-height viewport rather than
    // in the column's own layout: a project with thirty tracks has to *scroll*,
    // not grow the window until the arrangement no longer fits on the screen.
    // The host is moved by the timeline's scroll offset, so the two columns
    // cannot drift apart.
    m_viewport = new QWidget(this);
    m_viewport->setObjectName("TrackListViewport");
    m_viewport->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    m_viewport->setMinimumHeight(0);
    outer->addWidget(m_viewport, 1);

    m_rowsHost = new QWidget(m_viewport);
    m_rowsLayout = new QVBoxLayout(m_rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);

    m_indicator = new DropIndicator(this);

    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        applyTheme();
        applyHighlight();
    });
    applyTheme();
}

void TrackListWidget::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#TrackHeaders { background: %BG%; }
#TrackHeaders { border-right: 1px solid %SECTION%; }
#TrackListRuler { background: %RULER%; border-bottom: 1px solid %SECTION%; }
#TrackHeaders QLineEdit { background: transparent; border: none; color: %TEXT%;
                          font-size: 12px; font-weight: 600; padding: 0; }
#TrackHeaders QLineEdit:focus { background: %WELL%; border-radius: 6px; }
#TrackHeaders QLabel { color: %TEXT2%; font-size: 9px; }
#TrackListTitle { color: %TEXT%; font-size: 10px; font-weight: 700;
                  letter-spacing: 0.8px; }
#TrackListHint { color: %TEXT2%; font-size: 8px; font-weight: 600;
                 letter-spacing: 0.5px; }
#FolderCount { color: %TEXT2%; font-size: 9px; font-weight: 600; }
)")
        .replace("%BG%", mixColors(t.background, t.surface, 0.18).name())
        .replace("%RULER%", mixColors(t.surface, t.toolbarBackground, 0.40).name())
        .replace("%SECTION%", t.sectionDivider().name())
        .replace("%WELL%", t.well().name())
        .replace("%SEP%", t.separator().name())
        .replace("%TEXT2%", t.textSecondary.name())
        .replace("%TEXT%", t.textPrimary.name()));
}

QWidget* TrackListWidget::buildRow(const daw::TrackModel& track, int number,
                                   int depth) {
    (void)number;
    const QString id = QString::fromStdString(track.id);
    const bool folder = daw::isFolder(track);
    const bool pattern = track.kind == daw::TrackKind::Pattern;
    const bool automationLane = daw::isAutomationLane(track);
    const bool recordable = daw::acceptsRecording(track);
    // A plain folder is a container: no fader, no pan, no meter, nothing in the
    // mixer. A summing folder is a bus, and wears a bus's controls.
    const bool channel = daw::carriesAudio(track);
    const QColor color = colorFromRgb(track.color);

    auto* container = new TrackRowSurface(this);
    container->setTrackColor(color);
    container->setObjectName("TrackRow");
    container->setFixedHeight(ui::laneHeightForTrack(track));
    container->setProperty("trackId", id);
    container->setProperty("isFolder", folder);
    container->setMouseTracking(true);   // for the resize-edge hover cursor
    container->installEventFilter(this);

    auto* colorBar = new TrackColorRail(color, container);

    auto* icon = new TrackIcon(glyphForTrack(track), color, container);
    if (folder) {
        icon->makeDisclosure(track.expanded);
        icon->setToolTip(track.expanded ? tr("Collapse folder")
                                        : tr("Expand folder"));
        // Look the folder up when clicked: the document vector moves under us
        // as tracks are added and reordered, so a captured reference dangles.
        connect(icon, &QAbstractButton::clicked, this, [this, id] {
            const auto* f = m_controller->project().findTrack(id.toStdString());
            if (!f) return;
            m_controller->setFolderExpanded(id.toStdString(), !f->expanded);
            emit orderChanged();
        });
    } else {
        icon->setToolTip(trackKindName(track.kind));
    }

    auto* name = new ui::InlineNameEdit(QString::fromStdString(track.name),
                                        container);
    name->setProperty("trackId", id);
    name->setMinimumWidth(40);
    name->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    connect(name, &QLineEdit::editingFinished, this, [this, id, name] {
        const auto result = m_controller->renameTrack(
            id.toStdString(), name->text().toStdString());
        if (const auto* track =
                m_controller->project().findTrack(id.toStdString())) {
            const QSignalBlocker blocker(name);
            name->setText(QString::fromStdString(track->name));
        }
        syncTrackValues();
        emit tracksChanged(daw::collab::marksLocalFileDirty(result));
    });

    const auto chip = [&](const QString& letter, const QColor& active,
                          const QString& tip) {
        auto* b = new ui::MsrButton(letter, active, tip, container);
        b->setChipSize(kChipW, kChipH);
        return b;
    };

    ui::MsrButton* mute = nullptr;
    ui::MsrButton* solo = nullptr;
    if (!automationLane) {
        mute = chip("M", Theme::mute(),
                    folder ? tr("Mute folder") : tr("Mute"));
        mute->setProperty("trackButtonPaintRole", QStringLiteral("mute"));
        mute->setProperty("trackButtonPaintId", id);
        mute->installEventFilter(this);
        mute->setChecked(track.muted);
        if (channel) {
            mute->setAutomatable(true);
            connect(mute, &ui::MsrButton::automateRequested, this,
                    [this, id] { emit automateMuteRequested(id); });
        }
        connect(mute, &QAbstractButton::toggled, this,
                [this, id](bool on) {
                    applyMuteToGroup(id, on);
                });

        solo = chip("S", Theme::solo(), tr("Solo"));
        solo->setProperty("trackButtonPaintRole", QStringLiteral("solo"));
        solo->setProperty("trackButtonPaintId", id);
        solo->installEventFilter(this);
        solo->setChecked(track.soloed);
        connect(solo, &QAbstractButton::toggled, this, [this, id](bool on) {
            applyToGroup(id, [&](const std::string& target) {
                m_controller->setTrackSoloed(target, on);
            });
        });
    }

    ui::MsrButton* monitor = nullptr;
    ui::MsrButton* record = nullptr;
    if (recordable) {
        monitor = chip("I", th().accent, tr("Input monitoring"));
        monitor->setChecked(track.monitor);
        monitor->setAutoMark(track.monitorAuto);
        monitor->setToolTip(track.monitorAuto
                                ? tr("Input monitoring — set automatically. "
                                     "Click to take over.")
                                : tr("Input monitoring"));
        connect(monitor, &QAbstractButton::toggled, this,
                [this, id, monitor](bool on) {
                    applyToGroup(id, [&](const std::string& target) {
                        m_controller->setTrackMonitor(target, on);
                    });
                    // A click is the user taking the button back, so the mark
                    // goes with it.
                    monitor->setAutoMark(false);
                    monitor->setToolTip(tr("Input monitoring"));
                });

        // Only ever seen while there is a recording to be had — see
        // `applyRecordChips`. It says "this take lands here", and because the
        // targets *are* the selection, clicking it adds or removes the track
        // from that selection rather than setting some second kind of arm.
        record = chip("R", Theme::record(),
                      tr("Record onto this track. Pinning one track — or "
                         "several — overrides the selection until they are all "
                         "un-pinned."));
        record->hide();
        connect(record, &QAbstractButton::clicked, this, [this, id](bool on) {
            emit recordPinToggled(id, on);
        });
    }

    ui::MsrButton* patternButton = nullptr;
    if (pattern) {
        patternButton = chip("P", color, tr("Open Pattern editor"));
        patternButton->setAccessibleName(tr("Open Pattern editor"));
        connect(patternButton, &QAbstractButton::clicked, this,
                [this, id] { emit openPatternRequested(id); });
    }

    auto* chips = new QWidget(container);
    auto* chipRow = new QHBoxLayout(chips);
    chipRow->setContentsMargins(0, 0, 0, 0);
    chipRow->setSpacing(kChipGap);
    if (mute) chipRow->addWidget(mute);
    if (solo) chipRow->addWidget(solo);
    if (patternButton) chipRow->addWidget(patternButton);
    if (monitor) chipRow->addWidget(monitor);
    if (record) chipRow->addWidget(record);
    chipRow->addStretch(1);

    ui::FaderWidget* fader = nullptr;
    ui::PanKnob* pan = nullptr;
    ui::LevelMeter* meter = nullptr;
    if (channel) {
        fader = new ui::FaderWidget(Qt::Horizontal, container);
        fader->setGain(track.volume);
        fader->setMinimumWidth(kFaderMin);
        fader->setToolTip(tr("Level  %1").arg(ui::formatGainDb(track.volume)));
        connect(fader, &ui::FaderWidget::gainChanged, this,
                [this, id, fader](double gain) {
                    // The read-out lives in the tooltip now: the row is one
                    // line, and a permanent "0.0 dB" label was the widest thing
                    // on it that nobody reads except while dragging.
                    fader->setToolTip(tr("Level  %1").arg(ui::formatGainDb(gain)));
                    applyGroupGain(id, float(gain));
                });
        connect(fader, &ui::FaderWidget::editFinished, this, [this] {
            m_controller->commitTrackVolumeEdit(m_gainGesture.start);
            m_gainGesture.clear();
            emit tracksChanged();
        });
        // A plain double-click resets it; Alt/Option+double-click or the
        // toolbar's latched mode creates automation.
        fader->setAutomatable(true);
        connect(fader, &ui::FaderWidget::automateRequested, this,
                [this, id] { emit automateControlRequested(id, false); });

        pan = new ui::PanKnob(container);
        pan->setFixedSize(kPanWidth, kPanHeight);
        pan->setPan(track.pan);
        pan->setToolTip(tr("Pan  %1").arg(panText(track.pan)));
        connect(pan, &ui::PanKnob::panChanged, this, [this, id, pan](double v) {
            pan->setToolTip(tr("Pan  %1").arg(panText(v)));
            applyGroupPan(id, float(v));
        });
        connect(pan, &ui::PanKnob::editFinished, this, [this] {
            m_controller->commitTrackPanEdit(m_panGesture.start);
            m_panGesture.clear();
            emit tracksChanged();
        });
        pan->setAutomatable(true);
        connect(pan, &ui::PanKnob::automateRequested, this,
                [this, id] { emit automateControlRequested(id, true); });

        // One strip, flush against the row's right edge and running its whole
        // height. Two thin bars with a gap read as a mistake at this width, and
        // the level being shown is one peak either way.
        meter = new ui::LevelMeter(Qt::Vertical, 1, container);
        meter->setMeterStyle(ui::LevelMeter::Style::Rail);
        meter->setFixedWidth(5);
    }

    // The name over the chips, with the icon standing beside both of them —
    // one line for the whole channel instead of the old two-row stack.
    auto* stack = new QVBoxLayout;
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(2);
    stack->addStretch(1);
    stack->addWidget(name);
    if (!automationLane && (!folder || channel)) stack->addWidget(chips);
    stack->addStretch(1);

    // The controls live in a band of their own, centred in the row: a lane can
    // be dragged taller or grown by an open take stack, and the channel's name
    // and fader belong in the middle of whatever height it ends up with. The
    // colour rail and the meter stay outside the band, so they run the row's
    // full height and mark the lane rather than the controls.
    auto* band = new QWidget(container);
    band->setMaximumHeight(ui::kLaneHeight);
    auto* h = new QHBoxLayout(band);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(kPartGap);
    h->addWidget(icon, 0, Qt::AlignVCenter);
    h->addLayout(stack, 1);
    if (folder && !channel) {
        // Nothing to fade and nothing to pan: the two chips take the place the
        // fader would have had, pinned right and centred on the row.
        auto* count = new QLabel(band);
        count->setObjectName("FolderCount");
        count->setText(QString::number(
            daw::subtreeOf(m_controller->project(), track.id).size()));
        count->setToolTip(tr("Tracks in this folder"));
        h->addWidget(count, 0, Qt::AlignVCenter);
        h->addWidget(chips, 0, Qt::AlignVCenter);
        const int chipCount = patternButton ? 3 : 2;
        chips->setFixedWidth(chipCount * kChipW +
                             (chipCount - 1) * kChipGap);
    } else if (fader && pan) {
        h->addWidget(fader, 2);
        h->addWidget(pan, 0, Qt::AlignVCenter);
    } else {
        // An automation lane: no channel, so no fader and no pan to lay out.
        // What it drives is its name. Mute and Solo are channel states and have
        // no meaning on a curve lane, so the header deliberately ends cleanly.
        h->addStretch(1);
        chips->hide();
    }

    auto* bandColumn = new QVBoxLayout;
    bandColumn->setContentsMargins(0, 0, 0, 0);
    bandColumn->setSpacing(0);
    bandColumn->addStretch(1);
    bandColumn->addWidget(band);
    bandColumn->addStretch(1);

    // No right margin and no bottom margin under the meter: it is a rail
    // welded to the row's edge, top to bottom.
    auto* outer = new QHBoxLayout(container);
    outer->setContentsMargins(4, 0, 0, 0);
    outer->setSpacing(kPartGap);
    outer->addSpacing(depth * kIndentStep);
    outer->addWidget(colorBar);
    outer->addLayout(bandColumn, 1);
    if (meter) outer->addWidget(meter);

    m_rows.push_back({track.id, folder, channel, depth, container, colorBar,
                      icon, fader, pan, meter, mute, solo, monitor, record,
                      patternButton, name});
    applyRowAdaptivity(m_rows.back());
    return container;
}

void TrackListWidget::syncTrackValues() {
    for (const Row& row : m_rows) {
        const daw::TrackModel* t = m_controller->project().findTrack(row.id);
        if (!t) continue;
        const QColor color = colorFromRgb(t->color);
        static_cast<TrackRowSurface*>(row.container)->setTrackColor(color);
        static_cast<TrackColorRail*>(row.colorRail)->setTrackColor(color);
        static_cast<TrackIcon*>(row.icon)->setTrackColor(color);
        if (auto* name = qobject_cast<QLineEdit*>(row.nameEdit);
            name && !name->hasFocus() && name->text() != QString::fromStdString(t->name)) {
            QSignalBlocker block(name);
            name->setText(QString::fromStdString(t->name));
        }
        // Blocked: these setters are how the row *reports* an edit, and a value
        // arriving from elsewhere must not be echoed back as one.
        if (row.fader) {
            QSignalBlocker block(row.fader);
            row.fader->setGain(t->volume);
            row.fader->setToolTip(tr("Level  %1").arg(ui::formatGainDb(t->volume)));
        }
        if (row.pan) {
            QSignalBlocker block(row.pan);
            row.pan->setPan(t->pan);
            row.pan->setToolTip(tr("Pan  %1").arg(panText(t->pan)));
        }
        if (row.mute) {
            QSignalBlocker block(row.mute);
            row.mute->setChecked(t->muted);
        }
        if (row.solo) {
            QSignalBlocker block(row.solo);
            row.solo->setChecked(t->soloed);
        }
        if (row.monitor) {
            QSignalBlocker block(row.monitor);
            row.monitor->setChecked(t->monitor);
            row.monitor->setAutoMark(t->monitorAuto);
        }
    }
    applyRecordChips();
    refreshGlobalChips();
}

void TrackListWidget::refreshAutomationValues() {
    if (!isVisible() || !m_viewport) return;
    const QRect visible = m_viewport->rect();
    for (const Row& row : m_rows) {
        if (!row.container->isVisible()) continue;
        const QRect rowRect(row.container->mapTo(m_viewport, QPoint{}),
                            row.container->size());
        if (!visible.intersects(rowRect)) continue;
        const daw::TrackModel* track = m_controller->project().findTrack(row.id);
        if (!track) continue;
        double gain = track->volume;
        double pan = track->pan;
        if (m_controller->isPlaying()) {
            daw::AutomationTarget volume;
            volume.kind = daw::AutomationTargetKind::TrackVolume;
            volume.channelId = row.id;
            if (const auto value = m_controller->automationValueAtPlayhead(volume))
                gain = *value;

            daw::AutomationTarget panorama;
            panorama.kind = daw::AutomationTargetKind::TrackPan;
            panorama.channelId = row.id;
            if (const auto value =
                    m_controller->automationValueAtPlayhead(panorama)) {
                pan = *value;
            }
        }
        if (row.fader && !row.fader->isEditing() &&
            std::abs(row.fader->gain() - gain) > 1.0e-7) {
            const QSignalBlocker blocker(row.fader);
            row.fader->setGain(gain);
            row.fader->setToolTip(
                tr("Level  %1").arg(ui::formatGainDb(gain)));
        }
        if (row.pan && !row.pan->isEditing() &&
            std::abs(row.pan->pan() - pan) > 1.0e-7) {
            const QSignalBlocker blocker(row.pan);
            row.pan->setPan(pan);
            row.pan->setToolTip(tr("Pan  %1").arg(panText(pan)));
        }
    }
}

void TrackListWidget::refreshGlobalChips() {
    // Lit means "there is something here to clear", which is exactly the state
    // a row's own chip shows — one chip standing for the whole column.
    if (m_clearMutes) {
        QSignalBlocker block(m_clearMutes);
        m_clearMutes->setChecked(m_controller->anyMuted());
    }
    if (m_clearSolos) {
        QSignalBlocker block(m_clearSolos);
        m_clearSolos->setChecked(m_controller->anySoloed());
    }
}

void TrackListWidget::setRecordState(bool engaged, const QStringList& targets) {
    if (m_recordEngaged == engaged && m_recordTargets == targets) return;
    m_recordEngaged = engaged;
    m_recordTargets = targets;
    applyRecordChips();
}

void TrackListWidget::applyRecordChips() {
    for (const Row& row : m_rows) {
        if (!row.record) continue;
        const QString id = QString::fromStdString(row.id);
        row.record->setVisible(m_recordEngaged);
        QSignalBlocker block(row.record);
        row.record->setChecked(m_recordTargets.contains(id));
        applyRowAdaptivity(row);
    }
}

void TrackListWidget::rebuild() {
    for (auto& row : m_rows) {
        if (!row.container) continue;
        // Out of the layout **now**, not when it is finally deleted.
        // `deleteLater` keeps the widget alive until the next turn of the event
        // loop — it has to, since a rebuild is often triggered from inside a
        // click on one of these very rows — and a layout still holding the old
        // rows lays itself out for both sets at once. That is what put the new
        // rows hundreds of pixels down the column and made every track look
        // squeezed the moment another one was added.
        m_rowsLayout->removeWidget(row.container);
        row.container->hide();
        row.container->deleteLater();
    }
    m_rows.clear();

    const auto& project = m_controller->project();
    int number = 0;
    for (const auto& visible : daw::visibleTracks(project)) {
        const daw::TrackModel& track = project.tracks[visible.index];
        if (!daw::isFolder(track)) ++number;
        m_rowsLayout->addWidget(buildRow(track, number, visible.depth));
    }

    // Anything that has been deleted, or is hidden inside a collapsed folder,
    // drops out of the selection — a row nobody can see must not still be the
    // thing an edit lands on.
    QStringList surviving;
    for (const QString& id : std::as_const(m_selectedIds)) {
        if (rowIndexOf(id) >= 0) surviving.push_back(id);
    }
    const bool lost = surviving != m_selectedIds;
    m_selectedIds = surviving;
    if (!m_selectedIds.contains(m_selectedId)) {
        m_selectedId = m_selectedIds.isEmpty() ? QString() : m_selectedIds.back();
    }
    if (m_selectedIds.isEmpty() && !m_rows.empty()) {
        m_selectedId = QString::fromStdString(m_rows.front().id);
        m_selectedIds = {m_selectedId};
        m_anchorId = m_selectedId;
        emitSelection();
    } else if (lost) {
        emitSelection();
    }
    applyRecordChips();
    refreshGlobalChips();
    applyHighlight();
    layoutRows();
}

void TrackListWidget::refreshMeters() {
    if (!isVisible() || !m_viewport) return;
    const QRect visible = m_viewport->rect();
    for (const auto& row : m_rows) {
        if (!row.meter || !row.container->isVisible()) continue;
        const QRect rowRect(row.container->mapTo(m_viewport, QPoint{}),
                            row.container->size());
        if (visible.intersects(rowRect))
            row.meter->setPeak(m_controller->trackPeak(row.id));
    }
}

void TrackListWidget::syncRowHeights() { layoutRows(); }

void TrackListWidget::applyRowAdaptivity(const Row& row) {
    if (!row.fader) return;
    // Everything on the row that cannot give up a pixel, so what is left is
    // what the name, the fader and the pan have to share.
    const int width = m_rowsHost && m_rowsHost->width() > 0
                          ? m_rowsHost->width()
                          : ui::kTrackHeaderWidth;
    const int fixed = 4 /*left margin*/ + row.depth * kIndentStep +
                      6 /*colour rail*/ + kIconSize +
                      (row.meter ? row.meter->width() : 0) + 4 * kPartGap;
    const int flexible = width - fixed;

    int chipCount = 0;
    for (const QWidget* chip : {static_cast<QWidget*>(row.mute),
                                static_cast<QWidget*>(row.solo),
                                static_cast<QWidget*>(row.pattern),
                                static_cast<QWidget*>(row.monitor),
                                static_cast<QWidget*>(row.record)}) {
        if (chip && !chip->isHidden()) ++chipCount;
    }
    const int stackMin = std::max(
        40, chipCount > 0 ? chipCount * kChipW + (chipCount - 1) * kChipGap
                          : 0);

    // When the normal throw no longer fits, level and pan both stay reachable
    // as a compact pair of round controls.
    constexpr int kCompactFaderSide = 24;
    const bool normalFader =
        flexible >= kFullChipStrip + kPartGap + kFaderMin;
    const bool showFader = normalFader ||
        flexible >= kFullChipStrip + kPartGap + kCompactFaderSide;
    const bool showPan =
        normalFader
            ? flexible >= kFullChipStrip + 2 * kPartGap + kFaderMin + kPanWidth
            : flexible >= stackMin + 2 * kPartGap +
                              kCompactFaderSide + kPanWidth;
    row.fader->setCompactKnob(showFader && !normalFader);
    row.fader->setVisible(showFader);
    if (row.pan) row.pan->setVisible(showPan);
}

/// A row's rectangle in the column's own coordinates. The rows sit on a host
/// widget that the scroll offset moves, so their own geometry is relative to
/// that host and every hit test has to add where the host currently is.
QRect TrackListWidget::rowGeometry(size_t index) const {
    if (index >= m_rows.size() || !m_rows[index].container) return {};
    return m_rows[index].container->geometry().translated(
        m_rowsHost ? m_rowsHost->mapTo(const_cast<TrackListWidget*>(this),
                                       QPoint(0, 0))
                   : QPoint(0, 0));
}

std::pair<bool, bool> TrackListWidget::rowControlsForTest(
    const QString& trackId) const {
    for (const Row& row : m_rows) {
        if (QString::fromStdString(row.id) != trackId) continue;
        return {row.fader && row.fader->isVisibleTo(row.container),
                row.pan && row.pan->isVisibleTo(row.container)};
    }
    return {false, false};
}

ui::MsrButton* TrackListWidget::rowChipForTest(const QString& trackId,
                                              const QString& letter) const {
    for (const Row& row : m_rows) {
        if (QString::fromStdString(row.id) != trackId) continue;
        if (letter == QLatin1String("M")) return row.mute;
        if (letter == QLatin1String("S")) return row.solo;
        if (letter == QLatin1String("I")) return row.monitor;
        if (letter == QLatin1String("R")) return row.record;
    }
    return nullptr;
}

ui::FaderWidget* TrackListWidget::rowFaderForTest(const QString& trackId) const {
    for (const Row& row : m_rows) {
        if (QString::fromStdString(row.id) == trackId) return row.fader;
    }
    return nullptr;
}

QRect TrackListWidget::rowRectForTest(int index) const {
    if (index < 0 || size_t(index) >= m_rows.size()) return {};
    return rowGeometry(size_t(index));
}

void TrackListWidget::setVerticalScroll(int y) {
    if (y == m_scrollY) return;
    m_scrollY = y;
    positionRowsHost();
}

void TrackListWidget::positionRowsHost() {
    if (!m_rowsHost) return;
    const QPoint wanted(0, -m_scrollY);
    if (m_rowsHost->pos() != wanted) m_rowsHost->move(wanted);
}

void TrackListWidget::layoutRows() {
    if (!m_rowsHost || !m_viewport) return;
    // Each row's height re-read from the document — the very number the
    // timeline draws its lane with — and the host sized to their sum.
    //
    // Both halves have to happen here, together. The host is positioned by
    // hand, and a QVBoxLayout holding nothing but fixed-height widgets
    // *centres* them when it is given more room than they need and *squeezes*
    // them when it is given less: either way, a host whose height is not
    // exactly the sum of its rows puts every header out of step with its lane.
    // Asking the layout for that sum is no good — its cached hint is still the
    // old total in the same call stack that added a row to it, which is what
    // made every track shrink the moment a new one was added.
    int total = 0;
    for (const auto& row : m_rows) {
        if (!row.container) continue;
        if (const auto* track = m_controller->project().findTrack(row.id)) {
            const int height = ui::laneHeightForTrack(*track);
            if (row.container->minimumHeight() != height) {
                row.container->setFixedHeight(height);
            }
        }
        total += row.container->minimumHeight();
    }

    m_rowsHost->setGeometry(0, -m_scrollY, m_viewport->width(), total);
    for (const auto& row : m_rows) applyRowAdaptivity(row);
    // The rows take their new places now rather than on the next event loop
    // turn: the caller is usually mid-rebuild and about to be painted.
    if (m_rowsLayout) m_rowsLayout->activate();
}

void TrackListWidget::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    layoutRows();
}

void TrackListWidget::wheelEvent(QWheelEvent* ev) {
    // The headers scroll with the lanes, and the lanes own the offset — so the
    // wheel over this column asks for the same movement the timeline would
    // have made.
    emit verticalScrollRequested(-ev->angleDelta().y() / 2);
    ev->accept();
}

int TrackListWidget::rowResizeEdgeAt(const QPoint& posInList) const {
    constexpr int kZone = 5;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (!m_rows[i].container) continue;
        const QRect g = rowGeometry(i);
        if (std::abs(posInList.y() - g.bottom()) <= kZone) return int(i);
    }
    return -1;
}

// ── Acting on a whole selection ────────────────────────────────────────────

std::vector<std::string> TrackListWidget::actionTargets(const QString& id) const {
    std::vector<std::string> targets;
    if (!m_selectedIds.contains(id)) {
        targets.push_back(id.toStdString());
        return targets;
    }
    targets.reserve(size_t(m_selectedIds.size()));
    for (const QString& selected : m_selectedIds)
        targets.push_back(selected.toStdString());
    return targets;
}

void TrackListWidget::applyToGroup(
    const QString& id, const std::function<void(const std::string&)>& act) {
    if (m_applyingGroup) return;
    m_applyingGroup = true;
    for (const std::string& target : actionTargets(id)) act(target);
    m_applyingGroup = false;
    // The other rows' chips are showing the old state until they are re-read.
    syncTrackValues();
    refreshGlobalChips();
    emit tracksChanged();
}

void TrackListWidget::applyMuteToGroup(const QString& id, bool muted) {
    if (m_applyingGroup) return;
    m_applyingGroup = true;
    const std::vector<std::string> targets = actionTargets(id);
    const auto result = m_controller->setTracksMuted(targets, muted);
    m_applyingGroup = false;
    // Submitted projection and blocked rollback both come from the document;
    // never leave the initiating chip's transient checked state as a shadow
    // copy of it.
    syncTrackValues();
    refreshGlobalChips();
    emit tracksChanged(daw::collab::marksLocalFileDirty(result));
}

void TrackListWidget::applyGroupGain(const QString& id, float gain) {
    if (m_applyingGroup) return;
    const auto* driver = m_controller->project().findTrack(id.toStdString());
    const GroupGesture& gesture = beginGroupGesture(
        m_gainGesture, id, driver ? driver->volume : 1.0f,
        [](const daw::TrackModel& t) { return t.volume; });

    m_applyingGroup = true;
    if (gesture.driverStart > 1e-4f) {
        const float ratio = gain / gesture.driverStart;
        for (const auto& [target, from] : gesture.start) {
            m_controller->setTrackVolumeLive(
                target, target == id.toStdString() ? gain : from * ratio);
        }
    } else {
        // Dragging up from silence has no ratio to preserve; everything the
        // drag covers simply follows it.
        for (const auto& [target, from] : gesture.start) {
            (void)from;
            m_controller->setTrackVolumeLive(target, gain);
        }
    }
    m_applyingGroup = false;
    if (gesture.start.size() > 1) syncTrackValues();
}

void TrackListWidget::applyGroupPan(const QString& id, float pan) {
    if (m_applyingGroup) return;
    const auto* driver = m_controller->project().findTrack(id.toStdString());
    const GroupGesture& gesture = beginGroupGesture(
        m_panGesture, id, driver ? driver->pan : 0.0f,
        [](const daw::TrackModel& t) { return t.pan; });

    const float delta = pan - gesture.driverStart;
    m_applyingGroup = true;
    for (const auto& [target, from] : gesture.start) {
        m_controller->setTrackPanLive(
            target, target == id.toStdString()
                        ? pan
                        : std::clamp(from + delta, -1.0f, 1.0f));
    }
    m_applyingGroup = false;
    if (gesture.start.size() > 1) syncTrackValues();
}

const TrackListWidget::GroupGesture& TrackListWidget::beginGroupGesture(
    GroupGesture& gesture, const QString& id, float driverStart,
    float (*read)(const daw::TrackModel&)) {
    if (gesture.coversDrag(id)) return gesture;
    gesture.clear();
    gesture.driverId = id;
    gesture.driverStart = driverStart;
    for (const std::string& target : actionTargets(id)) {
        if (const auto* track = m_controller->project().findTrack(target))
            gesture.start.emplace_back(target, read(*track));
    }
    return gesture;
}

// ── Selection ──────────────────────────────────────────────────────────────

collab::SemanticPoint TrackListWidget::collaborationPresenceAt(
    const QPointF& position) const {
    collab::SemanticPoint point;
    point.surface = {collab::SurfaceKind::TrackList, QStringLiteral("main"),
                     {}};
    point.normalized = collab::normalizedSurfacePoint(position, size());
    const QRect rowRect =
        rowRectForTrack(trackIdAt(position.toPoint()));
    if (rowRect.isNull() || rowRect.height() <= 0) {
        // Above the rows, or past the last one: report the chrome so the
        // pointer still shows without claiming a track it is not over.
        point.targetId = QStringLiteral("column_chrome");
        return point;
    }
    point.trackId = trackIdAt(position.toPoint());
    point.targetId = QStringLiteral("row");
    point.laneFraction =
        std::clamp((position.y() - rowRect.top()) / double(rowRect.height()),
                   0.0, 1.0);
    return point;
}

std::optional<QPointF> TrackListWidget::collaborationPositionFor(
    const collab::SemanticPoint& point) const {
    if (point.surface.kind != collab::SurfaceKind::TrackList)
        return std::nullopt;
    const double x = point.normalized.x() >= 0.0
                         ? point.normalized.x() * width()
                         : width() * 0.5;
    if (point.trackId.isEmpty()) {
        // Chrome, or a pointer sent before this column knew the track.
        if (point.normalized.y() < 0.0) return std::nullopt;
        return QPointF(x, point.normalized.y() * height());
    }
    const QRect rowRect = rowRectForTrack(point.trackId);
    // The track is not in this column at all — scrolled far away, filtered out,
    // or already deleted here. Hiding is correct: a fallback would put their
    // pointer on somebody else's row.
    if (rowRect.isNull() || rowRect.height() <= 0) return std::nullopt;
    const double fraction =
        point.laneFraction >= 0.0 ? point.laneFraction : 0.5;
    return QPointF(x, rowRect.top() + fraction * rowRect.height());
}

bool TrackListWidget::checkCollaborationPresenceForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    daw::EngineController controller;
    controller.initialize(48000.0, 512, false);
    const QString first =
        QString::fromStdString(controller.addTrack(daw::TrackKind::Audio, "A"));
    const QString second =
        QString::fromStdString(controller.addTrack(daw::TrackKind::Audio, "B"));
    if (first.isEmpty() || second.isEmpty())
        return fail(QStringLiteral("track list presence fixture has no tracks"));

    TrackListWidget list(&controller);
    list.resize(240, 600);
    list.rebuild();
    list.setVerticalScroll(0);
    QRect row = list.rowRectForTrack(second);
    if (row.isNull() || row.height() <= 0)
        return fail(QStringLiteral("track list presence fixture has no rows"));

    const QPointF source(120.0, row.top() + row.height() * 0.25);
    const collab::SemanticPoint semantic = list.collaborationPresenceAt(source);
    if (semantic.trackId != second ||
        semantic.targetId != QLatin1String("row") ||
        std::abs(semantic.laneFraction - 0.25) > 1e-6) {
        return fail(QStringLiteral("track list presence lost its track context"));
    }

    // The whole point of the semantic form: after a scroll the same packet must
    // land on the same track, not on the pixel it originally came from.
    list.setVerticalScroll(40);
    row = list.rowRectForTrack(second);
    const auto remapped = list.collaborationPositionFor(semantic);
    if (row.isNull() || !remapped ||
        std::abs(remapped->y() - (row.top() + row.height() * 0.25)) > 1e-6) {
        return fail(QStringLiteral("track list presence did not follow scroll"));
    }

    collab::SemanticPoint absent = semantic;
    absent.trackId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    if (list.collaborationPositionFor(absent))
        return fail(QStringLiteral("track list presence mapped an unknown track"));
    return true;
}

bool TrackListWidget::checkButtonPaintForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    daw::EngineController controller;
    controller.initialize(48000.0, 512, false);
    const QString first = QString::fromStdString(
        controller.addTrack(daw::TrackKind::Audio, "Paint A"));
    const QString second = QString::fromStdString(
        controller.addTrack(daw::TrackKind::Audio, "Paint B"));
    const QString third = QString::fromStdString(
        controller.addTrack(daw::TrackKind::Audio, "Paint C"));
    if (first.isEmpty() || second.isEmpty() || third.isEmpty())
        return fail(QStringLiteral("track button paint fixture has no tracks"));

    TrackListWidget list(&controller);
    list.resize(240, 600);
    list.rebuild();
    list.setSelectedTracks({first, second, third}, first);
    list.show();
    QApplication::processEvents();

    const auto paint = [&](const QString& role, const QString& fromId,
                           const QString& toId) {
        auto* source = list.rowChipForTest(fromId, role);
        auto* target = list.rowChipForTest(toId, role);
        if (!source || !target) return false;
        const QPoint from = source->rect().center();
        const QPoint globalFrom = source->mapToGlobal(from);
        const QPoint globalTo = target->mapToGlobal(target->rect().center());
        const QPoint to = source->mapFromGlobal(globalTo);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(from),
                          QPointF(globalFrom), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(source, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(globalTo),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(source, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to),
                            QPointF(globalTo), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(source, &release);
        QApplication::processEvents();
        return true;
    };

    if (!paint(QStringLiteral("M"), first, second))
        return fail(QStringLiteral("mute paint controls are missing"));
    const auto* a = controller.project().findTrack(first.toStdString());
    const auto* b = controller.project().findTrack(second.toStdString());
    const auto* c = controller.project().findTrack(third.toStdString());
    if (!a || !b || !c || !a->muted || !b->muted || c->muted)
        return fail(QStringLiteral("mute paint escaped the crossed rows"));
    const std::vector<std::string> muteReset{
        first.toStdString(), second.toStdString()};
    controller.setTracksMuted(muteReset, false);
    list.syncTrackValues();

    if (!paint(QStringLiteral("S"), second, third))
        return fail(QStringLiteral("solo paint controls are missing"));
    a = controller.project().findTrack(first.toStdString());
    b = controller.project().findTrack(second.toStdString());
    c = controller.project().findTrack(third.toStdString());
    if (!a || !b || !c || a->soloed || !b->soloed || !c->soloed)
        return fail(QStringLiteral("solo paint escaped the crossed rows"));
    controller.setTrackSoloed(second.toStdString(), false);
    controller.setTrackSoloed(third.toStdString(), false);
    a = controller.project().findTrack(first.toStdString());
    b = controller.project().findTrack(second.toStdString());
    c = controller.project().findTrack(third.toStdString());
    if (!a || !b || !c || a->muted || b->muted || c->muted ||
        a->soloed || b->soloed || c->soloed) {
        return fail(QStringLiteral("track paint fixture did not reset"));
    }
    return true;
}

QString TrackListWidget::trackIdAt(const QPoint& position) const {
    const int row = rowAtPosition(position);
    if (row < 0 || std::size_t(row) >= m_rows.size()) return {};
    return QString::fromStdString(m_rows[std::size_t(row)].id);
}

QRect TrackListWidget::rowRectForTrack(const QString& trackId) const {
    const int index = rowIndexOf(trackId);
    return index < 0 ? QRect() : rowGeometry(std::size_t(index));
}

int TrackListWidget::rowIndexOf(const QString& id) const {
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (QString::fromStdString(m_rows[i].id) == id) return int(i);
    }
    return -1;
}

void TrackListWidget::setSelectedTrack(const QString& id) {
    setSelectedTracks(id.isEmpty() ? QStringList() : QStringList{id}, id);
}

void TrackListWidget::setSelectedTracks(const QStringList& ids,
                                        const QString& primary) {
    const QString lead = primary.isEmpty()
                             ? (ids.isEmpty() ? QString() : ids.back())
                             : primary;
    if (ids == m_selectedIds && lead == m_selectedId) return;
    m_selectedIds = ids;
    m_selectedId = lead;
    if (m_anchorId.isEmpty() || !ids.contains(m_anchorId)) m_anchorId = lead;
    applyHighlight();
    applyRecordChips();
}

void TrackListWidget::clickSelect(const QString& id,
                                  Qt::KeyboardModifiers modifiers) {
    const int clicked = rowIndexOf(id);
    if (clicked < 0) return;

    // Cmd on macOS arrives as ControlModifier, so one test covers both
    // platforms' "add this one to what I already have".
    if (modifiers & Qt::ControlModifier) {
        if (m_selectedIds.contains(id)) {
            m_selectedIds.removeAll(id);
            if (m_selectedId == id) {
                m_selectedId =
                    m_selectedIds.isEmpty() ? QString() : m_selectedIds.back();
            }
        } else {
            m_selectedIds.push_back(id);
            m_selectedId = id;
        }
        m_anchorId = id;
    } else if ((modifiers & Qt::ShiftModifier) && !m_anchorId.isEmpty() &&
               rowIndexOf(m_anchorId) >= 0) {
        // Everything between the anchor and here, in display order — the rows
        // as they are drawn, so a folder's children come with it exactly as
        // they look on screen.
        const int anchor = rowIndexOf(m_anchorId);
        const int from = std::min(anchor, clicked);
        const int to = std::max(anchor, clicked);
        m_selectedIds.clear();
        for (int i = from; i <= to; ++i) {
            m_selectedIds.push_back(QString::fromStdString(m_rows[size_t(i)].id));
        }
        m_selectedId = id;   // the anchor stays put for the next shift-click
    } else {
        if (m_selectedIds == QStringList{id} && m_selectedId == id) return;
        m_selectedIds = {id};
        m_selectedId = id;
        m_anchorId = id;
    }
    applyHighlight();
    applyRecordChips();
    emitSelection();
}

void TrackListWidget::emitSelection() {
    emit selectionSetChanged(m_selectedIds);
    emit selectionChanged(m_selectedId);
}

void TrackListWidget::applyHighlight() {
    for (const auto& row : m_rows) {
        const QString id = QString::fromStdString(row.id);
        static_cast<TrackRowSurface*>(row.container)
            ->setVisualState(m_selectedIds.contains(id), id == m_selectedId,
                             row.isFolder);
    }
}

// ── Drag to reorder ────────────────────────────────────────────────────────

int TrackListWidget::rowAtPosition(const QPoint& pos) const {
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const QRect geometry = rowGeometry(i);
        if (pos.y() >= geometry.top() && pos.y() <= geometry.bottom()) {
            return int(i);
        }
    }
    return -1;
}

void TrackListWidget::updateDropTarget(const QPoint& pos) {
    m_dropIntoFolder = false;
    m_dropFolderRow = -1;

    const int over = rowAtPosition(pos);
    if (over < 0) {
        // Above the first row or below the last one.
        m_dropRow = pos.y() < 0 ? 0 : int(m_rows.size());
        showDropFeedback();
        return;
    }

    const QRect geometry = rowGeometry(size_t(over));
    const double fraction = double(pos.y() - geometry.top()) / geometry.height();

    // The middle of a folder row means "put it inside"; the outer thirds mean
    // "insert above/below", so a track can still be placed next to a folder.
    if (m_rows[size_t(over)].isFolder && fraction > 0.3 && fraction < 0.7) {
        m_dropIntoFolder = true;
        m_dropFolderRow = over;
        m_dropRow = over + 1;
    } else {
        m_dropRow = fraction < 0.5 ? over : over + 1;
    }
    showDropFeedback();
}

void TrackListWidget::showDropFeedback() {
    auto* indicator = static_cast<DropIndicator*>(m_indicator);
    if (!m_dragging) {
        indicator->hide();
        return;
    }
    if (m_dropIntoFolder && m_dropFolderRow >= 0 &&
        m_dropFolderRow < int(m_rows.size())) {
        indicator->showFolder(rowGeometry(size_t(m_dropFolderRow)));
        return;
    }
    int y = ui::kRulerHeight;
    if (m_dropRow >= int(m_rows.size())) {
        if (!m_rows.empty()) y = rowGeometry(m_rows.size() - 1).bottom();
    } else if (m_dropRow >= 0) {
        y = rowGeometry(size_t(m_dropRow)).top();
    }
    indicator->showLine(QRect(4, y - 1, width() - 8, 3));
}

void TrackListWidget::finishDrag() {
    const int from = m_dragRow;
    const int to = m_dropRow;
    const bool intoFolder = m_dropIntoFolder;
    const int folderRow = m_dropFolderRow;
    cancelDrag();
    if (from < 0 || from >= int(m_rows.size()) || to < 0) return;

    const auto& project = m_controller->project();
    const std::string movedId = m_rows[size_t(from)].id;

    std::string parentId;
    size_t targetIndex = 0;

    if (intoFolder && folderRow >= 0 && folderRow < int(m_rows.size())) {
        parentId = m_rows[size_t(folderRow)].id;
        // Drop at the end of the folder's contents.
        targetIndex = project.indexOf(parentId) + 1 +
                      daw::subtreeOf(project, parentId).size();
    } else if (to >= int(m_rows.size())) {
        targetIndex = project.tracks.size();
        parentId.clear();
    } else {
        const std::string neighbourId = m_rows[size_t(to)].id;
        const auto* neighbour = project.findTrack(neighbourId);
        parentId = neighbour ? neighbour->parentId : std::string{};
        targetIndex = project.indexOf(neighbourId);
    }

    if (m_controller->moveTrack(movedId, targetIndex, parentId)) {
        emit orderChanged();
    }
}

void TrackListWidget::cancelDrag() {
    m_pressing = false;
    m_dragging = false;
    m_dragRow = -1;
    m_dropRow = -1;
    m_dropIntoFolder = false;
    m_dropFolderRow = -1;
    unsetCursor();
    static_cast<DropIndicator*>(m_indicator)->hide();
}

// ── A plugin dropped from the browser ──────────────────────────────────────

int TrackListWidget::pluginDropRowAt(const QPoint& posInList,
                                    bool instrument) const {
    const int row = rowAtPosition(posInList);
    if (row < 0) return -1;
    const auto* track = m_controller->project().findTrack(m_rows[size_t(row)].id);
    if (!track) return -1;
    // An instrument goes at the head of a chain that plays notes; an effect
    // goes into any chain there is. A plain folder has neither. Deciding it
    // here means the row simply does not light up for a drop it would refuse,
    // which is a better answer than a message after the fact.
    return instrument ? ((track->kind == daw::TrackKind::Pattern ||
                          daw::trackAccepts(track->kind, daw::ClipKind::Midi))
                             ? row : -1)
                      : (daw::carriesAudio(*track) ? row : -1);
}

std::optional<daw::plugins::PluginDescriptor> TrackListWidget::pluginFromMime(
    const QMimeData* mime) const {
    int format = 0;
    QString uid;
    if (!ui::decodePluginRef(mime, format, uid)) return std::nullopt;
    return m_controller->pluginManager().find(daw::plugins::Format(format),
                                              uid.toStdString());
}

namespace {
QStringList audioFilesFromMime(const QMimeData* mime) {
    QStringList files;
    if (!mime || !mime->hasUrls()) return files;
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (ui::isAudioFile(path)) files.push_back(path);
    }
    return files;
}

QString channelStripPresetFromMime(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return {};
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (ui::channelstrippresets::isPresetFile(path)) return path;
    }
    return {};
}

QString projectTemplateFromMime(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return {};
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (ui::projecttemplates::isTemplatePackage(path)) return path;
    }
    return {};
}
} // namespace

bool TrackListWidget::updateBrowserDropTarget(const QPoint& posInList,
                                              const QMimeData* mime) {
    m_projectTemplateDropPath.clear();
    const auto plugin = pluginFromMime(mime);
    if (plugin) {
        m_pluginDropRow = pluginDropRowAt(posInList, plugin->isInstrument);
    } else if (const QString templ = projectTemplateFromMime(mime);
               !templ.isEmpty()) {
        m_projectTemplateDropPath = templ;
        m_pluginDropRow = -1;
    } else if (!channelStripPresetFromMime(mime).isEmpty()) {
        const int row = rowAtPosition(posInList);
        const auto* track = row >= 0
            ? m_controller->project().findTrack(m_rows[size_t(row)].id)
            : nullptr;
        m_pluginDropRow = track && daw::carriesAudio(*track) ? row : -1;
    } else if (!audioFilesFromMime(mime).isEmpty()) {
        const int row = rowAtPosition(posInList);
        const auto* track = row >= 0
            ? m_controller->project().findTrack(m_rows[size_t(row)].id)
            : nullptr;
        m_pluginDropRow = track && track->kind == daw::TrackKind::Pattern
                              ? row : -1;
    } else {
        m_pluginDropRow = -1;
        return false;
    }

    auto* indicator = static_cast<DropIndicator*>(m_indicator);
    if (!m_projectTemplateDropPath.isEmpty()) {
        int y = ui::kRulerHeight;
        if (!m_rows.empty()) y = rowGeometry(m_rows.size() - 1).bottom();
        indicator->showLine(QRect(4, y - 1, width() - 8, 3));
    } else if (m_pluginDropRow < 0) {
        indicator->hide();
    } else {
        indicator->showFolder(rowGeometry(size_t(m_pluginDropRow)));
    }
    return true;
}

void TrackListWidget::dragEnterEvent(QDragEnterEvent* ev) {
    if (!updateBrowserDropTarget(ev->position().toPoint(), ev->mimeData()))
        return;
    // Accept here even when this particular row would refuse: without it Qt
    // stops sending move events and the drag can never find a row that fits.
    ev->acceptProposedAction();
}

void TrackListWidget::dragMoveEvent(QDragMoveEvent* ev) {
    if (!updateBrowserDropTarget(ev->position().toPoint(), ev->mimeData()))
        return;
    if (m_pluginDropRow < 0 && m_projectTemplateDropPath.isEmpty()) {
        ev->ignore();
        return;
    }
    ev->acceptProposedAction();
}

void TrackListWidget::dragLeaveEvent(QDragLeaveEvent*) {
    m_pluginDropRow = -1;
    m_projectTemplateDropPath.clear();
    static_cast<DropIndicator*>(m_indicator)->hide();
}

void TrackListWidget::dropEvent(QDropEvent* ev) {
    // Do not trust the last move event. A fast drop can arrive directly after
    // enter, and the rows may also have rebuilt while the pointer was held.
    updateBrowserDropTarget(ev->position().toPoint(), ev->mimeData());
    const int row = m_pluginDropRow;
    const QString projectTemplate = m_projectTemplateDropPath;
    m_pluginDropRow = -1;
    m_projectTemplateDropPath.clear();
    static_cast<DropIndicator*>(m_indicator)->hide();

    if (!projectTemplate.isEmpty()) {
        ev->acceptProposedAction();
        emit projectTemplateTracksRequested(projectTemplate);
        return;
    }

    if (row < 0 || row >= int(m_rows.size())) return;

    const std::string trackId = m_rows[size_t(row)].id;
    const QString preset = channelStripPresetFromMime(ev->mimeData());
    if (!preset.isEmpty()) {
        const audio::Result result = m_controller->applyChannelStripPreset(
            trackId, preset.toStdString());
        ev->acceptProposedAction();
        if (!result) {
            QToolTip::showText(
                QCursor::pos(),
                tr("%1 could not be applied: %2")
                    .arg(ui::channelstrippresets::displayName(preset),
                         QString::fromStdString(result.message())),
                this);
            return;
        }
        emit tracksChanged();
        emit orderChanged();
        return;
    }

    const QStringList files = audioFilesFromMime(ev->mimeData());
    if (!files.isEmpty()) {
        bool landed = false;
        for (const QString& file : files) {
            landed |= !m_controller
                            ->addPatternSample(trackId, file.toStdString())
                            .empty();
        }
        ev->acceptProposedAction();
        if (landed) {
            emit tracksChanged();
            emit orderChanged();
        }
        return;
    }

    const auto descriptor = pluginFromMime(ev->mimeData());
    if (!descriptor) return;
    // An instrument belongs at the head of the chain, not in an insert slot.
    bool landed = false;
    QString editorChannel;
    QString editorSlot;
    if (descriptor->isInstrument) {
        const auto* target = m_controller->project().findTrack(trackId);
        if (target && target->kind == daw::TrackKind::Pattern) {
            const std::string child =
                m_controller->addPatternInstrument(trackId, *descriptor);
            landed = !child.empty();
            const auto* childTrack = landed
                ? m_controller->project().findTrack(child)
                : nullptr;
            if (childTrack && !childTrack->instrument.id.empty()) {
                editorChannel = QString::fromStdString(child);
                editorSlot = QString::fromStdString(childTrack->instrument.id);
            }
        } else {
            landed = m_controller->setTrackInstrumentPlugin(trackId, *descriptor);
            const auto* loadedTrack = landed
                ? m_controller->project().findTrack(trackId)
                : nullptr;
            if (loadedTrack && !loadedTrack->instrument.id.empty()) {
                editorChannel = QString::fromStdString(trackId);
                editorSlot = QString::fromStdString(
                    loadedTrack->instrument.id);
            }
        }
    } else {
        const std::string id = m_controller->addInsert(trackId, *descriptor);
        landed = !id.empty();
        if (landed) {
            editorChannel = QString::fromStdString(trackId);
            editorSlot = QString::fromStdString(id);
        }
    }
    ev->acceptProposedAction();
    if (!landed) return;
    if (!editorChannel.isEmpty() && !editorSlot.isEmpty())
        emit pluginEditorRequested(editorChannel, editorSlot);
    emit tracksChanged();
    emit orderChanged();
}

bool TrackListWidget::eventFilter(QObject* obj, QEvent* ev) {
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w) return QWidget::eventFilter(obj, ev);

    const QString paintRole = w->property("trackButtonPaintRole").toString();
    if (!paintRole.isEmpty()) {
        auto* button = qobject_cast<QAbstractButton*>(w);
        auto* mouse = dynamic_cast<QMouseEvent*>(ev);
        if (button && ev->type() == QEvent::MouseButtonPress && mouse &&
            mouse->button() == Qt::LeftButton) {
            m_trackButtonPaintPending = true;
            m_trackButtonPainting = false;
            m_trackButtonPaintTarget = !button->isChecked();
            m_trackButtonPaintLocalFileDirty = false;
            m_trackButtonPaintRole = paintRole;
            m_trackButtonPaintPressGlobal = mouse->globalPosition().toPoint();
            m_trackButtonPaintLastGlobal = m_trackButtonPaintPressGlobal;
            m_trackButtonPainted.clear();
            return QWidget::eventFilter(obj, ev);
        }
        if (button && ev->type() == QEvent::MouseMove && mouse &&
            m_trackButtonPaintPending &&
            paintRole == m_trackButtonPaintRole &&
            (mouse->buttons() & Qt::LeftButton)) {
            const QPoint now = mouse->globalPosition().toPoint();
            if (!m_trackButtonPainting &&
                (now - m_trackButtonPaintPressGlobal).manhattanLength() <
                    QApplication::startDragDistance()) {
                return QWidget::eventFilter(obj, ev);
            }
            m_trackButtonPainting = true;
            button->setDown(false);
            applyTrackButtonPaintAlong(m_trackButtonPaintLastGlobal, now);
            m_trackButtonPaintLastGlobal = now;
            return true;
        }
        if (button && ev->type() == QEvent::MouseButtonRelease && mouse &&
            mouse->button() == Qt::LeftButton && m_trackButtonPaintPending &&
            paintRole == m_trackButtonPaintRole) {
            if (m_trackButtonPainting) {
                button->setDown(false);
                applyTrackButtonPaintAlong(
                    m_trackButtonPaintLastGlobal,
                    mouse->globalPosition().toPoint());
                finishTrackButtonPaint();
                return true;
            }

            // No drag: let the normal toggled handler keep its existing
            // selected-track group semantics.
            m_trackButtonPaintPending = false;
            m_trackButtonPaintRole.clear();
            return QWidget::eventFilter(obj, ev);
        }
        return QWidget::eventFilter(obj, ev);
    }

    const QVariant id = w->property("trackId");
    if (!id.isValid()) return QWidget::eventFilter(obj, ev);

    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            const QPoint posInList = w->mapTo(this, me->position().toPoint());
            // Grabbing a row's bottom edge resizes its height instead of
            // selecting / starting a reorder drag.
            const int edge = rowResizeEdgeAt(posInList);
            if (edge >= 0) {
                m_resizingHeight = true;
                m_resizeId = QString::fromStdString(m_rows[size_t(edge)].id);
                m_resizeStartY = me->globalPosition().toPoint().y();
                // The pointer changes the track's base height. The container
                // can be taller because an open comp editor contributes take
                // rows; feeding that total back as the new base makes the lane
                // jump by the comp height again on the first mouse move.
                m_resizeStartH = ui::kLaneHeight;
                if (const auto* track =
                        m_controller->project().findTrack(
                            m_resizeId.toStdString())) {
                    m_resizeStartH = ui::laneHeightFor(track->height);
                }
                m_resizeStart.clear();
                m_resizeUndoStart.clear();
                for (const std::string& target : actionTargets(m_resizeId)) {
                    if (const auto* t = m_controller->project().findTrack(target)) {
                        m_resizeStart.emplace_back(target,
                                                   double(ui::laneHeightFor(t->height)));
                        m_resizeUndoStart.emplace_back(target, t->height);
                    }
                }
                w->setCursor(Qt::SizeVerCursor);
                return true;
            }
            clickSelect(id.toString(), me->modifiers());
            m_pressing = true;
            m_pressPos = posInList;
            m_dragRow = rowAtPosition(m_pressPos);
        }
        return false; // children still get the click
    }
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (m_resizingHeight) {
            const int dy = me->globalPosition().toPoint().y() - m_resizeStartY;
            const int h = std::clamp(m_resizeStartH + dy, ui::kMinLaneHeight,
                                     ui::kMaxLaneHeight);
            // Everything selected grows with the row being dragged, each from
            // the height it started at — so a stack of tracks dragged taller
            // keeps whatever differences it had rather than snapping level.
            for (const auto& [target, from] : m_resizeStart) {
                m_controller->setTrackHeight(
                    target, target == m_resizeId.toStdString()
                                ? double(h)
                                : double(std::clamp(int(from) + dy,
                                                    ui::kMinLaneHeight,
                                                    ui::kMaxLaneHeight)));
            }
            // Re-read every row through laneHeightForTrack instead of forcing
            // this header to the base height. An open comp editor contributes
            // extra height to the lane; dropping that part here made this row
            // and every row below it diverge from the timeline during resize.
            // Laying out the whole stack synchronously also moves all following
            // rows on the same mouse frame, so no stale/cached geometry leaks
            // into hit-testing or painting.
            layoutRows();
            emit trackHeightChanged();
            return true;
        }
        // Hover feedback: a resize cursor near a row's bottom edge.
        if (!m_pressing) {
            const QPoint posInList = w->mapTo(this, me->position().toPoint());
            if (rowResizeEdgeAt(posInList) >= 0) w->setCursor(Qt::SizeVerCursor);
            else w->unsetCursor();
            return false;
        }
        if (!(me->buttons() & Qt::LeftButton)) return false;
        const QPoint pos = w->mapTo(this, me->position().toPoint());
        if (!m_dragging &&
            (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            return false;
        }
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        updateDropTarget(pos);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (m_resizingHeight) {
            m_resizingHeight = false;
            m_controller->commitTrackHeightEdit(m_resizeUndoStart);
            m_resizeStart.clear();
            m_resizeUndoStart.clear();
            w->unsetCursor();
            emit trackHeightChanged();
            return true;
        }
        if (m_dragging) {
            finishDrag();
            return true;
        }
        m_pressing = false;
        return false;
    }
    case QEvent::ContextMenu: {
        auto* ce = static_cast<QContextMenuEvent*>(ev);
        // Right-clicking inside a selection acts on the whole of it; right-
        // clicking outside one is a new, single selection. Anything else and
        // "make a folder of these six" would collapse to one track the moment
        // the menu was opened.
        if (!m_selectedIds.contains(id.toString())) {
            clickSelect(id.toString(), Qt::NoModifier);
        }
        showTrackContextMenu(id.toString(), ce->globalPos());
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(obj, ev);
}

void TrackListWidget::applyTrackButtonPaint(QAbstractButton* button) {
    if (!button) return;
    const QString role = button->property("trackButtonPaintRole").toString();
    const QString id = button->property("trackButtonPaintId").toString();
    if (role != m_trackButtonPaintRole || id.isEmpty() ||
        m_trackButtonPainted.contains(id)) {
        return;
    }

    m_trackButtonPainted.insert(id);
    if (role == QStringLiteral("mute")) {
        const std::vector<std::string> target{id.toStdString()};
        const auto result = m_controller->setTracksMuted(
            target, m_trackButtonPaintTarget);
        m_trackButtonPaintLocalFileDirty =
            m_trackButtonPaintLocalFileDirty ||
            daw::collab::marksLocalFileDirty(result);
    } else {
        m_controller->setTrackSoloed(id.toStdString(),
                                     m_trackButtonPaintTarget);
        m_trackButtonPaintLocalFileDirty = true;
    }

    const auto* track = m_controller->project().findTrack(id.toStdString());
    const bool modelState = track &&
        (role == QStringLiteral("mute") ? track->muted : track->soloed);
    const QSignalBlocker blocker(button);
    button->setChecked(modelState);
}

void TrackListWidget::applyTrackButtonPaintAlong(const QPoint& fromGlobal,
                                                 const QPoint& toGlobal) {
    const QLineF path(fromGlobal, toGlobal);
    for (const Row& row : m_rows) {
        QAbstractButton* button = m_trackButtonPaintRole == QStringLiteral("mute")
                                      ? row.mute
                                      : row.solo;
        if (!button || !button->isVisible()) continue;
        const QRectF hit(QRect(button->mapToGlobal(QPoint{}), button->size()));
        bool crossed = hit.contains(fromGlobal) || hit.contains(toGlobal);
        if (!crossed) {
            const QLineF edges[] = {
                QLineF(hit.topLeft(), hit.topRight()),
                QLineF(hit.topRight(), hit.bottomRight()),
                QLineF(hit.bottomRight(), hit.bottomLeft()),
                QLineF(hit.bottomLeft(), hit.topLeft())};
            QPointF intersection;
            for (const QLineF& edge : edges) {
                if (path.intersects(edge, &intersection) ==
                    QLineF::BoundedIntersection) {
                    crossed = true;
                    break;
                }
            }
        }
        if (crossed) applyTrackButtonPaint(button);
    }
}

void TrackListWidget::finishTrackButtonPaint() {
    if (!m_trackButtonPaintPending) return;
    const bool changed = !m_trackButtonPainted.isEmpty();
    const bool localFileDirty = m_trackButtonPaintLocalFileDirty;
    m_trackButtonPaintPending = false;
    m_trackButtonPainting = false;
    m_trackButtonPaintRole.clear();
    m_trackButtonPainted.clear();

    if (changed) {
        syncTrackValues();
        emit tracksChanged(localFileDirty);
    }
}

void TrackListWidget::contextMenuEvent(QContextMenuEvent* ev) {
    // Only reached when the click missed every row — the rows' own filter
    // handles those and returns true.
    QMenu menu(this);
    const auto trackKinds = ui::addTrackKindItems(menu);
    QAction* chosen = menu.exec(ev->globalPos());
    if (!chosen) return;
    if (const auto spec = trackKinds.constFind(chosen);
        spec != trackKinds.constEnd()) {
        spec->create(*m_controller);
        emit orderChanged();
    }
}

void TrackListWidget::showTrackContextMenu(const QString& id,
                                           const QPoint& globalPos) {
    const auto* track = m_controller->project().findTrack(id.toStdString());
    if (!track) return;
    const bool isFolder = daw::isFolder(*track);
    const bool isPattern = track->kind == daw::TrackKind::Pattern;
    const bool channel = daw::carriesAudio(*track);
    const int selected = int(m_selectedIds.size());

    QMenu menu(this);
    QAction* automationVolume = nullptr;
    QAction* automationPan = nullptr;
    QAction* automationMute = nullptr;
    QHash<QAction*, QString> automationSends;
    if (channel) {
        QMenu* automation = menu.addMenu(
            icons::icon(icons::Glyph::AutomationCreate, th().textPrimary),
            tr("Create Automation Clip"));
        automationVolume = automation->addAction(tr("Volume"));
        automationPan = automation->addAction(tr("Pan"));
        automationMute = automation->addAction(tr("Mute"));
        for (const daw::SendModel& send : track->sends) {
            QString destination = tr("Send");
            if (const auto* target =
                    m_controller->project().findTrack(send.destinationTrackId)) {
                destination = QString::fromStdString(target->name);
            }
            QAction* action =
                automation->addAction(tr("Send to %1").arg(destination));
            automationSends.insert(action, QString::fromStdString(send.id));
        }
        menu.addSeparator();
    }
    QAction* colour = menu.addAction(
        isFolder ? tr("Folder Colour…") : tr("Track Colour…"));
    colour->setToolTip(isFolder
                           ? tr("Recolours every track inside the folder too")
                           : QString());
    menu.addSeparator();

    QAction* dup = nullptr;
    QAction* dupNoFx = nullptr;
    if (!isFolder || isPattern) {
        dup = menu.addAction(isPattern ? tr("Duplicate Pattern")
                                       : tr("Duplicate Track"));
        if (!isPattern) {
            dupNoFx =
                menu.addAction(tr("Duplicate Track (without plugins)"));
        }
        menu.addSeparator();
    }

    QAction* summing = nullptr;
    if (isFolder && !isPattern) {
        summing = menu.addAction(tr("Sum Into a Bus"));
        summing->setCheckable(true);
        summing->setChecked(track->summing);
        summing->setToolTip(
            tr("Give the folder a channel of its own and route everything "
               "inside it through that channel"));
        menu.addSeparator();
    }

    // Recording mode, the explicit way — the header chip cycles, this names the
    // three states and shows which one is in force.
    QAction* modeGlobal = nullptr;
    QAction* modeOverwrite = nullptr;
    QAction* modeLayers = nullptr;
    if (daw::acceptsRecording(*track)) {
        QMenu* modes = menu.addMenu(tr("Track Recording Mode"));
        const auto add = [&](const QString& text, daw::TrackRecordMode m) {
            QAction* a = modes->addAction(text);
            a->setCheckable(true);
            a->setChecked(track->recordMode == m);
            return a;
        };
        modeGlobal = add(tr("Use Global (%1)")
                             .arg(m_controller->recordMode() ==
                                          daw::RecordMode::Layers
                                      ? tr("Layer recording")
                                      : tr("Overwrite")),
                         daw::TrackRecordMode::UseGlobal);
        modeOverwrite = add(tr("Overwrite"), daw::TrackRecordMode::Overwrite);
        modeLayers = add(tr("Layer Recording"), daw::TrackRecordMode::Layers);
        menu.addSeparator();
    }

    // Folder from the selection. Its wording carries the count, because
    // "these" has to be unambiguous when it means six rows and not the one
    // under the pointer.
    QAction* packPlain = menu.addAction(
        selected > 1 ? tr("Move %1 Tracks into a Folder").arg(selected)
                     : tr("Move into a Folder"));
    QAction* packSumming = menu.addAction(
        selected > 1 ? tr("Move %1 Tracks into a Summing Folder").arg(selected)
                     : tr("Move into a Summing Folder"));
    menu.addSeparator();

    QAction* del = menu.addAction(
        selected > 1 ? tr("Delete %1 Tracks").arg(selected)
                     : (isPattern ? tr("Delete Pattern")
                                  : isFolder ? tr("Delete Folder")
                                             : tr("Delete Track")));
    // The creation items belong here too: right-clicking a track is the
    // obvious place to look for "add another one", and hunting for a patch of
    // empty column to click is not a thing anyone should have to do.
    menu.addSeparator();
    const auto trackKinds = ui::addTrackKindItems(menu);

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == automationVolume || chosen == automationPan) {
        emit automateControlRequested(id, chosen == automationPan);
        return;
    }
    if (chosen == automationMute) {
        emit automateMuteRequested(id);
        return;
    }
    if (automationSends.contains(chosen)) {
        emit automateSendRequested(id, automationSends.value(chosen));
        return;
    }
    if (chosen == modeGlobal || chosen == modeOverwrite || chosen == modeLayers) {
        m_controller->setTrackRecordMode(
            id.toStdString(), chosen == modeOverwrite
                                  ? daw::TrackRecordMode::Overwrite
                              : chosen == modeLayers
                                  ? daw::TrackRecordMode::Layers
                                  : daw::TrackRecordMode::UseGlobal);
        rebuild();
        emit tracksChanged();
        return;
    }
    if (chosen == colour) {
        const QColor picked = QColorDialog::getColor(
            colorFromRgb(track->color), this,
            isFolder ? tr("Folder Colour") : tr("Track Colour"));
        if (!picked.isValid()) return;
        const uint32_t rgb = (uint32_t(picked.red()) << 16) |
                             (uint32_t(picked.green()) << 8) |
                             uint32_t(picked.blue());
        // Every selected row, so recolouring six tracks is one gesture — and a
        // folder among them takes its contents with it.
        for (const QString& target : std::as_const(m_selectedIds))
            m_controller->setTrackColor(target.toStdString(), rgb);
        rebuild();
        emit tracksChanged();
        return;
    }
    if (chosen == packPlain || chosen == packSumming) {
        emit packRequested(chosen == packSumming);
        return;
    }
    if (summing && chosen == summing) {
        m_controller->setFolderSumming(id.toStdString(), !track->summing);
        emit orderChanged();
        return;
    }
    if (chosen == del) {
        // A copy: removing tracks rebuilds the column, which rewrites the very
        // list being walked.
        const QStringList doomed = m_selectedIds;
        for (const QString& target : doomed)
            m_controller->removeTrack(target.toStdString());
        m_selectedIds.clear();
        m_selectedId.clear();
        m_anchorId.clear();
    } else if (dup && chosen == dup) {
        if (isPattern)
            m_controller->duplicatePattern(id.toStdString());
        else
            m_controller->duplicateTrack(id.toStdString(),
                                         /*withInserts=*/true);
    } else if (dupNoFx && chosen == dupNoFx) {
        m_controller->duplicateTrack(id.toStdString(), /*withInserts=*/false);
    } else if (const auto spec = trackKinds.constFind(chosen);
               spec != trackKinds.constEnd()) {
        spec->create(*m_controller);
    } else {
        return;
    }
    // The shell rebuilds the header immediately, then refreshes channel strips
    // after Qt has had a chance to paint the new structure.
    emit orderChanged();
}
