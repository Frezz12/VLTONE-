#include "ScrollMotion.hpp"
#include "SettingsWindow.hpp"
#include "TrackIcons.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "UiFrameClock.hpp"
#include <QSpinBox>
#include "MixerPreferences.hpp"
#include "AiSettingsPage.hpp"
#include "AccountSettingsPage.hpp"
#include "AccountService.hpp"
#include "BrowserSettingsPage.hpp"
#include "RecoverySettingsPage.hpp"
#include "AudioSettingsPage.hpp"
#include "QuickImportSettingsPage.hpp"
#include "ContextPanelPage.hpp"
#include "LocalizationManager.hpp"
#include "NotebookSettingsPage.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "ThemePackage.hpp"
#include "Controls.hpp"
#include "KnobAppearance.hpp"
#include "Icons.hpp"
#include "TimelineBackgroundPrefs.hpp"
#include "ProjectTemplates.hpp"
#include "StartupProjectPrefs.hpp"
#include "UiConstants.hpp"
#include "WaveformPaint.hpp"
#include "RecordingSettingsPage.hpp"
#include "TransportSettingsPage.hpp"

#include <QApplication>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QCheckBox>
#include <QChildEvent>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSlider>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStyle>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTabBar>
#include <QTimer>
#include <QThreadPool>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {
struct SettingsPageAppearance {
    icons::Glyph icon;
    const char* description;
};

// Indexed by the stable SettingsWindow::Tab IDs, independently of sidebar order.
constexpr SettingsPageAppearance kPageAppearance[] = {
    {icons::Glyph::Headphones, QT_TRANSLATE_NOOP("SettingsWindow", "Audio devices, inputs, outputs and latency.")},
    {icons::Glyph::Import, QT_TRANSLATE_NOOP("SettingsWindow", "Sources and options for importing media.")},
    {icons::Glyph::Play, QT_TRANSLATE_NOOP("SettingsWindow", "Playback controls and transport display.")},
    {icons::Glyph::Record, QT_TRANSLATE_NOOP("SettingsWindow", "Recording modes, takes and MIDI input.")},
    {icons::Glyph::Inspector, QT_TRANSLATE_NOOP("SettingsWindow", "Tools and actions for the current selection.")},
    {icons::Glyph::Folder, QT_TRANSLATE_NOOP("SettingsWindow", "Folders, previews and browser layout.")},
    {icons::Glyph::Notebook, QT_TRANSLATE_NOOP("SettingsWindow", "Notes, fonts and notebook appearance.")},
    {icons::Glyph::Assistant, QT_TRANSLATE_NOOP("SettingsWindow", "Assistant providers, models and access keys.")},
    {icons::Glyph::Users, QT_TRANSLATE_NOOP("SettingsWindow", "Your account and connected services.")},
    {icons::Glyph::Globe, QT_TRANSLATE_NOOP("SettingsWindow", "Interface language and installed translations.")},
    {icons::Glyph::Reload, QT_TRANSLATE_NOOP("SettingsWindow", "Automatic backups and project recovery.")},
    {icons::Glyph::Image, QT_TRANSLATE_NOOP("SettingsWindow", "Choose a palette or manage complete saved themes.")},
    {icons::Glyph::Brush, QT_TRANSLATE_NOOP("SettingsWindow", "Create your own palette with a live preview.")},
    {icons::Glyph::Key, QT_TRANSLATE_NOOP("SettingsWindow", "Find commands and customize keyboard shortcuts.")},
    {icons::Glyph::Gear, QT_TRANSLATE_NOOP("SettingsWindow", "Project startup and everyday editing preferences.")},
    {icons::Glyph::Mixer, QT_TRANSLATE_NOOP("SettingsWindow", "Channel width and console layout.")},
    {icons::Glyph::Image, QT_TRANSLATE_NOOP("SettingsWindow", "Images, video, visibility and blur for the timeline and header.")},
    {icons::Glyph::Play, QT_TRANSLATE_NOOP("SettingsWindow", "Grid, waveforms, clip corners, playhead and track selection.")},
    {icons::Glyph::Brush, QT_TRANSLATE_NOOP("SettingsWindow", "Interface typography and your track icon library.")},
    {icons::Glyph::Gear, QT_TRANSLATE_NOOP("SettingsWindow", "Refresh rate, rendering quality and navigation motion.")},
    {icons::Glyph::Plugin, QT_TRANSLATE_NOOP("SettingsWindow", "Plugin formats, search results and insertion behavior.")},
    {icons::Glyph::Globe, QT_TRANSLATE_NOOP("SettingsWindow", "Home page, bookmarks and the browser start background.")},
};
static_assert(std::size(kPageAppearance) == SettingsWindow::kPageCount);

class SettingsSearchDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem row(option);
        initStyleOption(&row, index);
        const QString title = row.text;
        const QIcon icon = row.icon;
        row.text.clear();
        row.icon = QIcon();
        row.features.setFlag(QStyleOptionViewItem::HasDisplay, false);
        row.features.setFlag(QStyleOptionViewItem::HasDecoration, false);
        const QWidget* widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &row, painter, widget);
        painter->save();
        const QRect bounds = option.rect.adjusted(12, 8, -12, -8);
        icon.paint(painter, QRect(bounds.left(), bounds.top() + 4, 18, 18));
        const QRect textBounds = bounds.adjusted(30, 0, 0, 0);
        QFont titleFont = option.font;
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(th().textPrimary);
        const QFontMetrics titleMetrics(titleFont);
        painter->drawText(textBounds, Qt::AlignTop | Qt::AlignLeft,
                         titleMetrics.elidedText(title, Qt::ElideRight, textBounds.width()));
        painter->setFont(option.font);
        painter->setPen(th().textSecondary);
        const QString path = index.data(Qt::UserRole + 1).toString();
        painter->drawText(textBounds.adjusted(0, titleMetrics.height() + 3, 0, 0), Qt::AlignTop | Qt::AlignLeft,
                         option.fontMetrics.elidedText(path, Qt::ElideRight, textBounds.width()));
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(200, option.fontMetrics.height() * 2 + 22);
    }
};

enum class ColorSection {
    Surfaces,
    Typography,
    Accent,
    Timeline,
};

// The editable colours of a theme, paired with a pointer-to-member so the
// editor can read and write each one generically.
struct ColorField {
    const char* key;
    const char* label;
    QColor Theme::* member;
    ColorSection section;
};
const std::vector<ColorField>& colorFields() {
    static const std::vector<ColorField> fields = {
        {"background", QT_TRANSLATE_NOOP("SettingsWindow", "Background"), &Theme::background, ColorSection::Surfaces},
        {"surface", QT_TRANSLATE_NOOP("SettingsWindow", "Surface"), &Theme::surface, ColorSection::Surfaces},
        {"surfaceElevated", QT_TRANSLATE_NOOP("SettingsWindow", "Surface (elevated)"), &Theme::surfaceElevated, ColorSection::Surfaces},
        {"headerBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Header"), &Theme::headerBackground, ColorSection::Surfaces},
        {"transportBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Transport"), &Theme::transportBackground, ColorSection::Surfaces},
        {"toolbarBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Toolbar"), &Theme::toolbarBackground, ColorSection::Surfaces},
        {"pluginMenuBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Plugin menu background"), &Theme::pluginMenuBackground, ColorSection::Surfaces},
        {"textPrimary", QT_TRANSLATE_NOOP("SettingsWindow", "Text"), &Theme::textPrimary, ColorSection::Typography},
        {"textSecondary", QT_TRANSLATE_NOOP("SettingsWindow", "Text (secondary)"), &Theme::textSecondary, ColorSection::Typography},
        {"accent", QT_TRANSLATE_NOOP("SettingsWindow", "Accent"), &Theme::accent, ColorSection::Accent},
        {"accentHighlight", QT_TRANSLATE_NOOP("SettingsWindow", "Accent (highlight)"), &Theme::accentHighlight, ColorSection::Accent},
        {"selection", QT_TRANSLATE_NOOP("SettingsWindow", "Selection"), &Theme::selection, ColorSection::Accent},
        {"waveform", QT_TRANSLATE_NOOP("SettingsWindow", "Waveform"), &Theme::waveform, ColorSection::Timeline},
        {"cursor", QT_TRANSLATE_NOOP("SettingsWindow", "Playhead"), &Theme::cursor, ColorSection::Timeline},
        {"gridLine", QT_TRANSLATE_NOOP("SettingsWindow", "Grid line"), &Theme::gridLine, ColorSection::Timeline},
        {"gridLineStrong", QT_TRANSLATE_NOOP("SettingsWindow", "Grid line (strong)"), &Theme::gridLineStrong, ColorSection::Timeline},
    };
    return fields;
}

class ThemePreviewWidget final : public QWidget {
public:
    ThemePreviewWidget(std::function<Theme()> themeProvider, QWidget* parent)
        : QWidget(parent), m_themeProvider(std::move(themeProvider)) {
        setMinimumHeight(152);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QCoreApplication::translate(
            "SettingsWindow", "Live theme preview"));
        setAccessibleDescription(QCoreApplication::translate(
            "SettingsWindow",
            "A miniature arrangement that updates with the selected colours."));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const Theme theme = m_themeProvider();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF frame = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        QPainterPath clip;
        clip.addRoundedRect(frame, 10.0, 10.0);
        painter.setClipPath(clip);
        painter.fillPath(clip, theme.background);

        const qreal headerHeight = 28.0;
        const qreal footerHeight = 25.0;
        const qreal sidebarWidth = std::clamp(frame.width() * 0.22, 82.0, 118.0);
        const auto plate = [&](const QRectF& bounds, const QColor& top, const QColor& bottom) {
            QLinearGradient face(bounds.topLeft(), bounds.bottomLeft());
            face.setColorAt(0, top);
            face.setColorAt(1, bottom);
            painter.fillRect(bounds, face);
            painter.setPen(theme.edgeLight(top));
            painter.drawLine(bounds.topLeft(), bounds.topRight());
            painter.setPen(theme.edgeDark(bottom));
            painter.drawLine(bounds.bottomLeft(), bounds.bottomRight());
        };
        plate(QRectF(frame.left(), frame.top(), frame.width(), headerHeight),
              mixColors(theme.headerBackground, theme.surfaceElevated, 0.32), theme.headerBackground);
        painter.fillRect(QRectF(frame.left(), frame.top() + headerHeight,
                                sidebarWidth, frame.height() - headerHeight),
                         theme.surface);
        plate(QRectF(frame.left() + sidebarWidth, frame.bottom() - footerHeight,
                     frame.width() - sidebarWidth, footerHeight), theme.panelTop(), theme.panelBottom());

        const QRectF transport(frame.center().x() - 58.0, frame.top() + 5.0,
                               116.0, 18.0);
        QLinearGradient display(transport.topLeft(), transport.bottomLeft());
        display.setColorAt(0, theme.wellTop());
        display.setColorAt(1, theme.wellBottom());
        painter.setPen(theme.edgeDark(theme.well()));
        painter.setBrush(display);
        painter.drawRoundedRect(transport, 6.0, 6.0);
        painter.setPen(theme.textPrimary);
        painter.drawText(transport, Qt::AlignCenter, QStringLiteral("1.1.000   120"));

        painter.setPen(theme.textSecondary);
        QFont small = font();
        small.setPixelSize(9);
        small.setWeight(QFont::DemiBold);
        painter.setFont(small);
        const qreal laneLeft = frame.left() + sidebarWidth;
        const qreal laneTop = frame.top() + headerHeight;
        const qreal laneRight = frame.right();
        const qreal laneBottom = frame.bottom() - footerHeight;
        painter.save();
        painter.setOpacity(painter.opacity() * ui::gridOpacity() / 100.0);
        for (int line = 1; laneLeft + line * 18.0 < laneRight; ++line) {
            const qreal x = laneLeft + line * 18.0;
            painter.setPen(QPen(line % 4 == 0 ? theme.gridLineStrong : theme.gridLine,
                                ui::gridLineWidth()));
            painter.drawLine(QPointF(x, laneTop), QPointF(x, laneBottom));
        }
        painter.restore();

        const qreal trackHeight = (laneBottom - laneTop) / 3.0;
        const QString trackNames[] = {
            QCoreApplication::translate("SettingsWindow", "Drums"),
            QCoreApplication::translate("SettingsWindow", "Bass"),
            QCoreApplication::translate("SettingsWindow", "Synth"),
        };
        for (int i = 0; i < 3; ++i) {
            const qreal top = laneTop + i * trackHeight;
            if (i == 1)
                painter.fillRect(QRectF(frame.left(), top, frame.width(), trackHeight),
                                 theme.selection);
            painter.setPen(theme.separator());
            painter.drawLine(QPointF(frame.left(), top + trackHeight),
                             QPointF(frame.right(), top + trackHeight));
            painter.setPen(i <= 1 ? theme.textPrimary : theme.textSecondary);
            painter.drawText(QRectF(frame.left() + 10.0, top, sidebarWidth - 20.0,
                                    trackHeight),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             trackNames[i]);
        }

        const QRectF clipRect(laneLeft + 22.0, laneTop + 12.0,
                              std::max(90.0, (laneRight - laneLeft) * 0.48),
                              trackHeight - 20.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.accent);
        painter.drawRoundedRect(clipRect, 5.0, 5.0);
        painter.setPen(QPen(Qt::white, 1.5));
        QPainterPath waveform;
        waveform.moveTo(clipRect.left() + 8.0, clipRect.center().y());
        for (int x = 8; x < int(clipRect.width()) - 8; x += 8) {
            const qreal y = clipRect.center().y() + ((x / 8) % 2 ? -6.0 : 6.0);
            waveform.lineTo(clipRect.left() + x, y);
        }
        painter.drawPath(waveform);

        painter.setPen(QPen(theme.cursor, 2.0));
        const qreal playheadX = laneLeft + (laneRight - laneLeft) * 0.72;
        painter.drawLine(QPointF(playheadX, laneTop),
                         QPointF(playheadX, laneBottom));

        // Show the same raised key and recessed rail used by the full interface.
        const QRectF key(frame.right() - 44, frame.bottom() - footerHeight + 5, 34, 15);
        QLinearGradient keyFace(key.topLeft(), key.bottomLeft());
        keyFace.setColorAt(0, theme.controlTop());
        keyFace.setColorAt(1, theme.controlBottom());
        painter.setBrush(keyFace);
        painter.setPen(theme.edgeDark(theme.controlBottom()));
        painter.drawRoundedRect(key, 3, 3);
        painter.setPen(theme.edgeLight(theme.controlTop()));
        painter.drawLine(key.topLeft() + QPointF(3, 0), key.topRight() - QPointF(3, 0));
        painter.setPen(theme.textPrimary);
        painter.drawText(key, Qt::AlignCenter, QStringLiteral("FX"));
        const QRectF rail(laneLeft + 12, key.center().y() - 2, 50, 4);
        painter.setPen(theme.edgeDark(theme.well()));
        painter.setBrush(theme.wellTop());
        painter.drawRoundedRect(rail, 2, 2);

        painter.setClipping(false);
        painter.setPen(theme.sectionDivider());
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, 10.0, 10.0);
    }

private:
    std::function<Theme()> m_themeProvider;
};

QColor compositeOver(const QColor& foreground, const QColor& background) {
    const double alpha = foreground.alphaF();
    return QColor::fromRgbF(
        foreground.redF() * alpha + background.redF() * (1.0 - alpha),
        foreground.greenF() * alpha + background.greenF() * (1.0 - alpha),
        foreground.blueF() * alpha + background.blueF() * (1.0 - alpha));
}

double relativeLuminance(const QColor& colour) {
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92
                               : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.redF()) +
           0.7152 * channel(colour.greenF()) +
           0.0722 * channel(colour.blueF());
}

double contrastRatio(const QColor& foreground, const QColor& background) {
    const QColor opaqueForeground = compositeOver(foreground, background);
    const double lighter = std::max(relativeLuminance(opaqueForeground),
                                    relativeLuminance(background));
    const double darker = std::min(relativeLuminance(opaqueForeground),
                                   relativeLuminance(background));
    return (lighter + 0.05) / (darker + 0.05);
}

QString presetDisplayName(const Theme& theme) {
    if (theme.id == QLatin1String("dark")) return QCoreApplication::translate(
        "SettingsWindow", "Dark");
    if (theme.id == QLatin1String("monochrome")) return QCoreApplication::translate(
        "SettingsWindow", "Monochrome");
    if (theme.id == QLatin1String("dark-blue")) return QCoreApplication::translate(
        "SettingsWindow", "Dark Blue");
    if (theme.id == QLatin1String("studio-gray")) return QCoreApplication::translate(
        "SettingsWindow", "Studio Gray");
    if (theme.id == QLatin1String("light")) return QCoreApplication::translate(
        "SettingsWindow", "Light");
    if (theme.id == QLatin1String("solarized-light")) return QCoreApplication::translate(
        "SettingsWindow", "Solarized Light");
    if (theme.id == QLatin1String("gruvbox")) return QCoreApplication::translate(
        "SettingsWindow", "Gruvbox");
    return theme.name;
}

// Dense settings rows contain sliders, combo boxes and spin boxes that accept
// Wheel on hover. A vertical gesture over those controls should move the page;
// values remain directly editable by drag, click and keyboard.
class SettingsScrollArea final : public QScrollArea {
public:
    using QScrollArea::QScrollArea;

    void setSettingsWidget(QWidget* page) {
        setWidget(page);
        watch(page);
    }

    static bool checkWheelRoutingForTest() {
        SettingsScrollArea scroll;
        auto* page = new QWidget;
        page->setFixedSize(180, 600);
        auto* slider = new ui::GlassSlider(Qt::Horizontal, page);
        slider->setGeometry(20, 20, 120, 28);
        slider->setRange(0, 100);
        slider->setValue(50);
        scroll.setSettingsWidget(page);
        scroll.resize(200, 140);
        scroll.show();
        QApplication::processEvents();

        const QPointF point(slider->rect().center());
        const QPointF global = slider->mapToGlobal(point);
        QWheelEvent angle(point, global, {}, QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(slider, &angle);
        const int afterAngle = scroll.verticalScrollBar()->value();
        QWheelEvent pixels(point, global, QPoint(0, -24), {}, Qt::NoButton,
                           Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(slider, &pixels);
        const bool scrolled = angle.isAccepted() && pixels.isAccepted() && afterAngle > 0 &&
                              scroll.verticalScrollBar()->value() > afterAngle && slider->value() == 50;
        scroll.resize(200, 800);
        QApplication::processEvents();
        QWheelEvent overShortPage(point, slider->mapToGlobal(point), {}, QPoint(0, -120), Qt::NoButton,
                                 Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(slider, &overShortPage);
        return scrolled && scroll.verticalScrollBar()->maximum() == 0 && slider->value() == 50;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::ChildAdded) {
            auto* childEvent = static_cast<QChildEvent*>(event);
            if (auto* child = qobject_cast<QWidget*>(childEvent->child())) watch(child);
        }
        if (event->type() == QEvent::Wheel && widget()) {
            auto* source = qobject_cast<QWidget*>(watched);
            auto* wheel = static_cast<QWheelEvent*>(event);
            const bool belongsToPage = source &&
                (source == widget() || widget()->isAncestorOf(source));
            const bool vertical = wheel->pixelDelta().y() != 0 ||
                                  wheel->angleDelta().y() != 0;
            bool valueControl = false;
            for (auto* ancestor = source; ancestor && ancestor != widget(); ancestor = ancestor->parentWidget()) {
                // Scrollable lists (palettes, folders, shortcuts) keep their
                // own wheel behavior, including their native edge propagation.
                if (qobject_cast<QAbstractItemView*>(ancestor))
                    return QScrollArea::eventFilter(watched, event);
                valueControl |= qobject_cast<QComboBox*>(ancestor) || qobject_cast<QAbstractSpinBox*>(ancestor) ||
                                qobject_cast<QSlider*>(ancestor);
            }
            if (belongsToPage && vertical && valueControl && verticalScrollBar()->maximum() == 0) {
                event->accept();
                return true;
            }
            if (belongsToPage && vertical &&
                verticalScrollBar()->maximum() > verticalScrollBar()->minimum()) {
                if (wheel->pixelDelta().y() != 0) {
                    verticalScrollBar()->setValue(
                        verticalScrollBar()->value() - wheel->pixelDelta().y());
                    event->accept();
                    return true;
                }
                QWheelEvent forwarded(
                    viewport()->mapFromGlobal(wheel->globalPosition()),
                    wheel->globalPosition(), wheel->pixelDelta(),
                    wheel->angleDelta(), wheel->buttons(), wheel->modifiers(),
                    wheel->phase(), wheel->inverted(), wheel->source(),
                    wheel->pointingDevice());
                forwarded.setTimestamp(wheel->timestamp());
                forwarded.ignore();
                QApplication::sendEvent(viewport(), &forwarded);
                event->setAccepted(forwarded.isAccepted());
                return forwarded.isAccepted();
            }
        }
        return QScrollArea::eventFilter(watched, event);
    }

private:
    void watch(QWidget* branch) {
        if (!branch) return;
        branch->installEventFilter(this);
        const auto descendants = branch->findChildren<QWidget*>();
        for (QWidget* child : descendants) child->installEventFilter(this);
    }
};

/// A settings page owns its natural size, while the dialog owns the viewport.
/// This breaks the size-hint chain that used to let a long page push the whole
/// window beyond the top and bottom of a smaller display.
QScrollArea* scrollablePage(QWidget* page) {
    page->setMinimumSize(0, 0);
    page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // Explanatory copy is the usual source of an enormous page size hint.
    // Let it gain height instead of forcing the whole dialog (or a horizontal
    // scrollbar) wider than the screen. Compact form labels stay single-line.
    for (QLabel* label : page->findChildren<QLabel*>()) {
        if (label->text().size() < 56) continue;
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    auto* scroll = new SettingsScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setMinimumSize(0, 0);
    scroll->setSettingsWidget(page);
    return scroll;
}

using ThemeTask =
    std::function<ui::ThemePackageResult(const ui::ThemePackageProgress&)>;
using ThemeTaskCompletion =
    std::function<void(const ui::ThemePackageResult&)>;

void runThemeTask(SettingsWindow* owner, const QString& label, ThemeTask task,
                  ThemeTaskCompletion completion) {
    auto* dialog = new QProgressDialog(label, QObject::tr("Cancel"), 0, 0,
                                       owner);
    dialog->setWindowTitle(QObject::tr("Theme"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(0);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->show();

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    QObject::connect(dialog, &QProgressDialog::canceled, dialog,
                     [cancelled] { cancelled->store(true); });
    const QPointer<SettingsWindow> ownerGuard(owner);
    const QPointer<QProgressDialog> dialogGuard(dialog);
    QThreadPool::globalInstance()->start(
        [ownerGuard, dialogGuard, cancelled, task = std::move(task),
         completion = std::move(completion)]() mutable {
            int lastPercent = -1;
            const ui::ThemePackageProgress report =
                [cancelled, dialogGuard, &lastPercent](qint64 completed,
                                                       qint64 total) {
                    if (cancelled->load()) return false;
                    const int percent = total > 0
                        ? int(std::clamp(
                              double(completed) / double(total) * 100.0,
                              0.0, 100.0))
                        : 0;
                    if (percent != lastPercent) {
                        lastPercent = percent;
                        QMetaObject::invokeMethod(
                            qApp,
                            [dialogGuard, percent] {
                                if (!dialogGuard) return;
                                dialogGuard->setRange(0, 100);
                                dialogGuard->setValue(percent);
                            },
                            Qt::QueuedConnection);
                    }
                    return !cancelled->load();
                };
            const ui::ThemePackageResult result = task(report);
            QMetaObject::invokeMethod(
                qApp,
                [ownerGuard, dialogGuard, completion = std::move(completion),
                 result] {
                    if (dialogGuard) dialogGuard->deleteLater();
                    if (ownerGuard) completion(result);
                },
                Qt::QueuedConnection);
        });
}
} // namespace

SettingsWindow::SettingsWindow(daw::EngineController* controller,
                               ShortcutManager* shortcuts, QWidget* parent)
    : QDialog(parent, Qt::Widget), m_controller(controller), m_shortcuts(shortcuts) {
    setWindowTitle(tr("Settings — %1").arg(QApplication::applicationDisplayName()));
    setObjectName(QStringLiteral("SettingsWindow"));
    resize(1040, 700);
    setSizeGripEnabled(false);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("SettingsPages"));
    m_pages->setMinimumSize(0, 0);
    m_pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_pageFactories[kAudioTab] = [this] {
        m_audioPage = new AudioSettingsPage(m_controller, this);
        connect(m_audioPage, &AudioSettingsPage::cpuStatusBarVisibilityChanged, this, &SettingsWindow::cpuStatusBarVisibilityChanged);
        return m_audioPage;
    };
    m_pageFactories[kQuickImportTab] = [this] { return m_quickImportPage = new QuickImportSettingsPage(this); };
    m_pageFactories[kTransportTab] = [this] {
        auto* page = new TransportSettingsPage(m_controller, this);
        connect(page, &TransportSettingsPage::panelStyleChanged, this, &SettingsWindow::transportPanelStyleChanged);
        return page;
    };
    m_pageFactories[kRecordingTab] = [this] {
        m_recordingPage = new RecordingSettingsPage(m_controller, this);
        connect(m_recordingPage, &RecordingSettingsPage::recordModeChanged, this, &SettingsWindow::recordModeChanged);
        return m_recordingPage;
    };
    m_pageFactories[kContextPanelTab] = [this] {
        auto* page = new ContextPanelPage(this);
        connect(page, &ContextPanelPage::changed, this, &SettingsWindow::contextPanelSettingsChanged);
        return page;
    };
    m_pageFactories[kBrowserTab] = [this] {
        auto* page = new BrowserSettingsPage(this);
        connect(page, &BrowserSettingsPage::changed, this, &SettingsWindow::browserSettingsChanged);
        return page;
    };
    m_pageFactories[kNotebookTab] = [this] {
        m_notebookPage = new NotebookSettingsPage(this);
        connect(m_notebookPage, &NotebookSettingsPage::changed, this, &SettingsWindow::notebookSettingsChanged);
        return m_notebookPage;
    };
    m_pageFactories[kAiTab] = [this] {
        auto* page = new AiSettingsPage(this);
        connect(page, &AiSettingsPage::changed, this, &SettingsWindow::aiSettingsChanged);
        return page;
    };
    m_pageFactories[kAccountTab] = [this] {
        auto* page = new AccountSettingsPage(this);
        connect(page, &AccountSettingsPage::logoutRequested, this, &SettingsWindow::accountLogoutRequested);
        return page;
    };
    m_pageFactories[kLanguageTab] = [this] { return buildLanguageTab(); };
    m_pageFactories[kRecoveryTab] = [this] { return new RecoverySettingsPage(this); };
    m_pageFactories[kThemesTab] = [this] { return buildThemesTab(); };
    m_pageFactories[kThemeEditorTab] = [this] { return buildThemeEditorTab(); };
    m_pageFactories[kShortcutsTab] = [this] { return buildShortcutsTab(); };
    m_pageFactories[kInterfaceTab] = [this] { return buildInterfaceTab(); };
    m_pageFactories[kMixerTab] = [this] { return buildMixerTab(); };
    m_pageFactories[kBackgroundsTab] = [this] { return buildBackgroundsTab(); };
    m_pageFactories[kTimelineTab] = [this] { return buildTimelineTab(); };
    m_pageFactories[kFontsTab] = [this] { return buildFontsTab(); };
    m_pageFactories[kPerformanceTab] = [this] { return buildPerformanceTab(); };
    m_pageFactories[kPluginsTab] = [this] {
        auto* page = new ContextPanelPage(this, ContextPanelPage::Section::Plugins);
        connect(page, &ContextPanelPage::changed, this, &SettingsWindow::contextPanelSettingsChanged);
        return page;
    };
    m_pageFactories[kWebBrowserTab] = [this] {
        auto* page = new BrowserSettingsPage(this, BrowserSettingsPage::Section::Web);
        connect(page, &BrowserSettingsPage::changed, this, &SettingsWindow::browserSettingsChanged);
        return page;
    };
    for (const auto& factory : m_pageFactories) m_pages->addWidget(new QWidget(m_pages));

    auto* sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("SettingsSidebar"));
    sidebar->setFixedWidth(200);
    auto* sidebarColumn = new QVBoxLayout(sidebar);
    sidebarColumn->setContentsMargins(12, 18, 12, 12);
    sidebarColumn->setSpacing(18);
    auto* title = new QLabel(QApplication::applicationDisplayName(), sidebar);
    title->setObjectName(QStringLiteral("SettingsBrand"));
    title->setContentsMargins(8, 0, 0, 0);
    sidebarColumn->addWidget(title);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("SettingsSearch"));
    m_search->setAccessibleName(tr("Search all settings"));
    m_search->setPlaceholderText(tr("Search settings…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumHeight(30);
    m_search->setMaximumWidth(250);
    m_search->setToolTip(tr("Search all settings (%1)").arg(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)));
    m_search->installEventFilter(this);
    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] { m_search->setFocus(); m_search->selectAll(); });

    m_navigation = new QTreeWidget(sidebar);
    m_navigation->setObjectName(QStringLiteral("SettingsNavigation"));
    m_navigation->setAccessibleName(tr("Settings sections"));
    m_navigation->setHeaderHidden(true);
    m_navigation->setRootIsDecorated(false);
    m_navigation->setItemsExpandable(false);
    m_navigation->setExpandsOnDoubleClick(false);
    m_navigation->setUniformRowHeights(true);
    m_navigation->setIndentation(0);
    m_navigation->setIconSize(QSize(18, 18));
    m_navigation->setMouseTracking(true);
    m_navigation->setFrameShape(QFrame::NoFrame);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarColumn->addWidget(m_navigation, 1);
    const auto addGroup = [this](
                              const QString& title,
                              std::initializer_list<std::pair<QString, Tab>> pages) {
        auto* group = new QTreeWidgetItem(m_navigation, QStringList{title});
        group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren);
        group->setData(0, Qt::UserRole, int(m_categoryPages.size()));
        group->setSizeHint(0, QSize(0, 40));
        QVector<int> category;
        for (const auto& [label, page] : pages) {
            category.push_back(int(page));
            m_pageLabels.insert(int(page), label);
            m_navigationItems.insert(int(page), group);
        }
        group->setToolTip(0, tr(kPageAppearance[category.front()].description));
        group->setData(0, Qt::AccessibleDescriptionRole, group->toolTip(0));
        m_categoryPages.push_back(category);
    };
    addGroup(tr("General"),
             {{tr("General"), kInterfaceTab},
              {tr("Language"), kLanguageTab},
              {tr("Shortcuts"), kShortcutsTab},
              {tr("Performance"), kPerformanceTab},
              {tr("Recovery"), kRecoveryTab}});
    addGroup(tr("Audio"),
             {{tr("Devices"), kAudioTab},
              {tr("Recording"), kRecordingTab},
              {tr("Transport"), kTransportTab}});
    addGroup(tr("Workspace"),
             {{tr("Files"), kBrowserTab},
              {tr("Import"), kQuickImportTab},
              {tr("Plugins"), kPluginsTab},
              {tr("Inspector"), kContextPanelTab},
              {tr("Mixer"), kMixerTab},
              {tr("Notebook"), kNotebookTab},
              {tr("Web"), kWebBrowserTab}});
    addGroup(tr("Appearance"),
             {{tr("Themes"), kThemesTab},
              {tr("Timeline"), kTimelineTab},
              {tr("Backgrounds"), kBackgroundsTab},
              {tr("Fonts & Icons"), kFontsTab},
              {tr("Palette"), kThemeEditorTab}});
    addGroup(tr("AI"), {{tr("Assistant"), kAiTab}});
    m_pageLabels.insert(kAccountTab, tr("Account"));
    connect(m_navigation, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!item) return;
        const int category = item->data(0, Qt::UserRole).toInt();
        showTab(m_categoryPages[category].front());
    });

    m_accountButton = new QPushButton(sidebar);
    m_accountButton->setObjectName(QStringLiteral("SettingsAccount"));
    m_accountButton->setAutoDefault(false);
    m_accountButton->setCheckable(true);
    m_accountButton->setCursor(Qt::PointingHandCursor);
    m_accountButton->setMinimumHeight(58);
    auto* accountRow = new QHBoxLayout(m_accountButton);
    accountRow->setContentsMargins(8, 8, 8, 8);
    accountRow->setSpacing(10);
    m_accountAvatar = new QLabel(m_accountButton);
    m_accountAvatar->setObjectName(QStringLiteral("SettingsAccountAvatar"));
    m_accountAvatar->setFixedSize(30, 30);
    m_accountAvatar->setAlignment(Qt::AlignCenter);
    accountRow->addWidget(m_accountAvatar);
    auto* identity = new QVBoxLayout;
    identity->setSpacing(2);
    m_accountName = new QLabel(m_accountButton);
    m_accountName->setTextFormat(Qt::PlainText);
    m_accountName->setObjectName(QStringLiteral("SettingsAccountName"));
    m_accountName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_accountCaption = new QLabel(tr("Account settings"), m_accountButton);
    m_accountCaption->setObjectName(QStringLiteral("SettingsAccountCaption"));
    m_accountCaption->setProperty("role", "secondary");
    identity->addWidget(m_accountName);
    identity->addWidget(m_accountCaption);
    accountRow->addLayout(identity, 1);
    for (auto* label : m_accountButton->findChildren<QLabel*>())
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    sidebarColumn->addWidget(m_accountButton);
    connect(m_accountButton, &QPushButton::clicked, this, [this] { showTab(kAccountTab); });
    if (auto* service = account::Service::instance()) {
        connect(service, &account::Service::snapshotChanged, this, &SettingsWindow::refreshAccount);
        connect(service, &account::Service::authenticatedChanged, this, &SettingsWindow::refreshAccount);
    }
    refreshAccount();

    auto* topBar = new QWidget(this);
    topBar->setObjectName(QStringLiteral("SettingsTopBar"));
    auto* topRow = new QHBoxLayout(topBar);
    topRow->setContentsMargins(24, 10, 24, 10);
    topRow->setSpacing(10);
    auto* settingsIcon = new QLabel(topBar);
    settingsIcon->setObjectName(QStringLiteral("SettingsHeaderIcon"));
    settingsIcon->setFixedSize(18, 18);
    topRow->addWidget(settingsIcon);
    auto* settingsTitle = new QLabel(tr("Settings"), topBar);
    topRow->addWidget(settingsTitle);
    topRow->addStretch();
    topRow->addWidget(m_search, 1);
    m_subtabs = new QTabBar(this);
    m_subtabs->setObjectName(QStringLiteral("SettingsSubtabs"));
    m_subtabs->setAccessibleName(tr("Settings pages"));
    m_subtabs->setExpanding(false);
    m_subtabs->setDrawBase(false);
    m_subtabs->setUsesScrollButtons(true);
    m_subtabs->setElideMode(Qt::ElideNone);
    m_subtabs->setIconSize(QSize(16, 16));
    connect(m_subtabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0) showTab(m_subtabs->tabData(index).toInt());
    });

    auto* pageHeader = new QWidget(this);
    pageHeader->setObjectName(QStringLiteral("SettingsPageHeader"));
    auto* headerRow = new QHBoxLayout(pageHeader);
    headerRow->setContentsMargins(0, 0, 0, 6);
    auto* heading = new QVBoxLayout;
    heading->setSpacing(3);
    m_pageTitle = new QLabel(pageHeader);
    m_pageTitle->setObjectName(QStringLiteral("SettingsPageTitle"));
    m_pageTitle->setProperty("role", "pageTitle");
    m_pageTitle->setWordWrap(true);
    m_pageDescription = new QLabel(pageHeader);
    m_pageDescription->setProperty("role", "secondary");
    m_pageDescription->setWordWrap(true);
    m_pageDescription->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading->addWidget(m_pageTitle);
    heading->addWidget(m_pageDescription);
    headerRow->addLayout(heading, 1);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &SettingsWindow::refreshNavigationAppearance);

    const auto markThemeModified = [this] {
        if (m_applyingInstalledTheme) return;
        QSettings().remove(QStringLiteral("ui/activeThemePackage"));
        refreshThemeLibrary();
    };
    connect(this, &SettingsWindow::themeBackgroundSettingsChanged, this,
            markThemeModified);
    connect(this, &SettingsWindow::notebookSettingsChanged, this,
            markThemeModified);
    connect(this, &SettingsWindow::selectionTintChanged, this,
            markThemeModified);
    connect(&ThemeManager::instance(), &ThemeManager::fontChanged, this,
            markThemeModified);
    connect(&ThemeManager::instance(), &ThemeManager::trackIconsChanged, this,
            markThemeModified);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto* normalPage = new QWidget(this);
    auto* content = new QVBoxLayout(normalPage);
    content->setContentsMargins(32, 22, 28, 0);
    content->setSpacing(14);
    content->addWidget(pageHeader);
    content->addWidget(m_pages, 1);
    m_content = new QStackedWidget(this);
    m_content->addWidget(normalPage);
    auto* searchPage = new QWidget(this);
    auto* searchColumn = new QVBoxLayout(searchPage);
    searchColumn->setContentsMargins(32, 22, 28, 0);
    auto* searchTitle = new QLabel(tr("Search results"), searchPage);
    searchTitle->setProperty("role", "pageTitle");
    searchColumn->addWidget(searchTitle);
    m_searchStatus = new QLabel(searchPage);
    m_searchStatus->setObjectName(QStringLiteral("SettingsSearchStatus"));
    m_searchStatus->setProperty("role", "secondary");
    m_searchStatus->setWordWrap(true);
    searchColumn->addWidget(m_searchStatus);
    m_searchResults = new QTreeWidget(searchPage);
    m_searchResults->setObjectName(QStringLiteral("SettingsSearchResults"));
    m_searchResults->setAccessibleName(tr("Matching settings"));
    m_searchResults->setHeaderHidden(true);
    m_searchResults->setItemDelegate(new SettingsSearchDelegate(m_searchResults));
    m_searchResults->setRootIsDecorated(false);
    m_searchResults->setAlternatingRowColors(false);
    m_searchResults->setFrameShape(QFrame::NoFrame);
    m_searchResults->setUniformRowHeights(true);
    m_searchResults->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_searchResults->installEventFilter(this);
    searchColumn->addWidget(m_searchResults, 1);
    connect(m_searchResults, &QTreeWidget::itemClicked, this, &SettingsWindow::activateSearchResult);
    connect(m_searchResults, &QTreeWidget::itemActivated, this, &SettingsWindow::activateSearchResult);
    m_content->addWidget(searchPage);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(120);
    connect(m_searchTimer, &QTimer::timeout, this, &SettingsWindow::updateSearch);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.trimmed().isEmpty()) {
            m_searchTimer->stop();
            m_content->setCurrentIndex(0);
            refreshNavigationAppearance();
        } else {
            m_searchTimer->start();
        }
    });

    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("SettingsFooter"));
    auto* footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(22, 10, 22, 12);
    m_pageHint = new QLabel(footer);
    m_pageHint->setProperty("role", "secondary");
    m_pageHint->setWordWrap(true);
    footerRow->addWidget(m_pageHint, 1);
    // The footer owns this action for the lifetime of the settings window.
    // Lazy page creation must never add another button or leave an orphan.
    m_audioApply = new QPushButton(tr("Apply"), footer);
    m_audioApply->setObjectName(QStringLiteral("ApplyAudioSettings"));
    m_audioApply->setProperty("accentAction", true);
    m_audioApply->setAutoDefault(false);
    m_audioApply->setAccessibleName(tr("Apply audio device settings"));
    m_audioApply->hide();
    connect(m_audioApply, &QPushButton::clicked, this, [this] {
        ensurePage(kAudioTab);
        if (m_audioPage) m_audioPage->apply();
    });
    footerRow->addWidget(m_audioApply);
    footerRow->addWidget(buttons);
    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    right->addWidget(topBar);
    right->addWidget(m_subtabs);
    right->addWidget(m_content, 1);
    right->addWidget(footer);
    auto* body = new QHBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(sidebar);
    body->addLayout(right, 1);

    const int savedPage = QSettings().value(
        QStringLiteral("ui/settingsPage"), int(kInterfaceTab)).toInt();
    showTab(std::clamp(savedPage, 0, int(kPageCount) - 1));

    // The workspace supplies the final bounds after this dialog is embedded.
    // Standalone dialogs are constrained in showEvent once their host is known.
}

void SettingsWindow::refreshNavigationAppearance() {
    const int currentPage = m_pages->currentIndex();
    for (int i = 0; i < m_navigation->topLevelItemCount(); ++i) {
        auto* group = m_navigation->topLevelItem(i);
        const bool selected = group == m_navigationItems.value(currentPage);
        group->setForeground(0, selected ? th().textPrimary : th().textSecondary);
        group->setIcon(0, icons::icon(kPageAppearance[m_categoryPages[i].front()].icon,
                                    selected ? th().accent : th().textSecondary));
        QFont font = group->font(0);
        font.setBold(selected);
        group->setFont(0, font);
    }
    m_pageTitle->setText(currentPage == kAudioTab ? tr("Audio") : m_pageLabels.value(currentPage));
    m_pageDescription->setText(tr(kPageAppearance[currentPage].description));
    for (int i = 0; i < m_subtabs->count(); ++i)
        m_subtabs->setTabIcon(i, icons::icon(kPageAppearance[m_subtabs->tabData(i).toInt()].icon,
                                         i == m_subtabs->currentIndex() ? th().accent : th().textSecondary, 16));
    auto* headerIcon = findChild<QLabel*>(QStringLiteral("SettingsHeaderIcon"));
    headerIcon->setPixmap(icons::icon(icons::Glyph::Gear, th().textSecondary)
                              .pixmap(QSize(18, 18), devicePixelRatioF()));
    QString hint = tr("Changes apply as you edit.");
    if (currentPage == kAudioTab)
        hint = tr("Use Apply for device changes. The CPU display updates immediately.");
    else if (currentPage == kAiTab)
        hint = tr("Use Save model for connections. Other options apply as you edit.");
    else if (currentPage == kLanguageTab || currentPage == kRecoveryTab)
        hint = tr("Changes are saved for the next application launch.");
    else if (currentPage == kPerformanceTab)
        hint = tr("Changes apply immediately. Switching GPU rendering requires a restart.");
    else if (currentPage == kThemesTab)
        hint = tr("Palettes apply on selection. Use Apply for a saved theme.");
    else if (currentPage == kAccountTab)
        hint = tr("Account information is synchronized with VLTONE.");
    if (m_pageHint) m_pageHint->setText(hint);
    if (m_audioApply) m_audioApply->setVisible(currentPage == kAudioTab && m_content->currentIndex() == 0);
}

void SettingsWindow::refreshAccount() {
    auto* service = account::Service::instance();
    QString name = service && service->authenticated() ? service->snapshot().nickname : QString();
    if (name.isEmpty() && service && service->authenticated()) name = service->snapshot().email;
    if (name.isEmpty()) name = tr("Not signed in");
    m_accountName->setText(name);
    m_accountAvatar->setText(name.left(1).toUpper());
    m_accountButton->setAccessibleName(tr("Account settings: %1").arg(name));
    m_accountButton->setToolTip(name + QLatin1Char('\n') + tr("Account settings"));
}

void SettingsWindow::refreshSubtabs(int page) {
    const QSignalBlocker blocker(m_subtabs);
    auto* category = m_navigationItems.value(page);
    const int categoryIndex = category ? category->data(0, Qt::UserRole).toInt() : -1;
    if (m_subtabs->property("category").toInt() != categoryIndex || !m_subtabs->count()) {
        while (m_subtabs->count()) m_subtabs->removeTab(0);
        const QVector<int> pages = category ? m_categoryPages[categoryIndex] : QVector<int>{kAccountTab};
        for (int id : pages) {
            const int tab = m_subtabs->addTab(m_pageLabels.value(id));
            m_subtabs->setTabData(tab, id);
            m_subtabs->setTabToolTip(tab, tr(kPageAppearance[id].description));
        }
        m_subtabs->setProperty("category", categoryIndex);
    }
    for (int i = 0; i < m_subtabs->count(); ++i)
        if (m_subtabs->tabData(i).toInt() == page) m_subtabs->setCurrentIndex(i);
}

void SettingsWindow::preparePage(QWidget* page) {
    auto* column = qobject_cast<QVBoxLayout*>(page->layout());
    if (column) {
        // Existing module pages use sectionLabel. Give those sections the same
        // native group containers as the other settings, without rebinding controls.
        QList<QLayoutItem*> items;
        while (column->count()) items.push_back(column->takeAt(0));
        QVBoxLayout* section = column;
        for (auto* item : items) {
            QWidget* widget = item->widget();
            auto* label = qobject_cast<QLabel*>(widget);
            if (label && label->property("role") == QLatin1String("section")) {
                auto* group = new QGroupBox(label->text(), page);
                column->addWidget(group);
                section = new QVBoxLayout(group);
                delete label;
                delete item;
            } else if (widget && widget->minimumHeight() == 1 && widget->maximumHeight() == 1) {
                delete widget;
                delete item;
            } else if (auto* layout = item->layout()) {
                layout->setParent(nullptr);
                section->addLayout(layout);
            } else if (widget) {
                section->addWidget(widget);
                delete item;
            } else if (item->spacerItem() && item->expandingDirections().testFlag(Qt::Vertical)) {
                column->addItem(item);
            } else {
                section->addItem(item);
            }
        }
        column->setContentsMargins(0, 0, 4, 16);
        column->setSpacing(16);
    }
    for (auto* group : page->findChildren<QGroupBox*>()) {
        group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        if (group->layout()) {
            group->layout()->setContentsMargins(16, 12, 16, 16);
            group->layout()->setSpacing(12);
        }
    }
    for (auto* form : page->findChildren<QFormLayout*>()) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(12);
        for (int row = 0; row < form->rowCount(); ++row) {
            auto* labelItem = form->itemAt(row, QFormLayout::LabelRole);
            auto* fieldItem = form->itemAt(row, QFormLayout::FieldRole);
            auto* label = labelItem ? qobject_cast<QLabel*>(labelItem->widget()) : nullptr;
            QWidget* field = fieldItem ? fieldItem->widget() : nullptr;
            if (field && (!labelItem || (label && label->text().isEmpty())) &&
                (qobject_cast<QCheckBox*>(field) || qobject_cast<QRadioButton*>(field))) {
                const auto rowItems = form->takeRow(row);
                if (rowItems.labelItem) { delete rowItems.labelItem->widget(); delete rowItems.labelItem; }
                delete rowItems.fieldItem;
                form->insertRow(row, field);
                continue;
            }
            if (label && field) {
                label->setWordWrap(true);
                label->setBuddy(field);
                if (field->accessibleName().isEmpty()) field->setAccessibleName(label->text());
            }
        }
    }
    for (auto* label : page->findChildren<QLabel*>()) {
        if (!label->pixmap().isNull()) continue;
        // Long explanatory copy must reflow instead of setting the window width.
        if (label->text().size() > 90) {
            label->setWordWrap(true);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            if (!label->property("role").isValid() &&
                !label->textInteractionFlags().testFlag(Qt::TextSelectableByMouse))
                label->setProperty("role", "secondary");
        }
    }
    for (auto* button : page->findChildren<QPushButton*>()) button->setAutoDefault(false);
    for (auto* spin : page->findChildren<QAbstractSpinBox*>()) {
        spin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        spin->setMinimumWidth(110);
    }
    for (auto* combo : page->findChildren<QComboBox*>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(16);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
}

namespace {
QString settingsSearchText(QString text) {
    return text.remove(QLatin1Char('&')).replace(QRegularExpression(QStringLiteral("<[^>]+>")), QString())
        .toCaseFolded().replace(QChar(0x451), QChar(0x435)).simplified();
}
}

void SettingsWindow::buildSearchIndex() {
    if (!m_searchIndex.isEmpty()) return;
    const int selected = m_pages->currentIndex();
    for (int page = 0; page < kPageCount; ++page) {
        ensurePage(page);
        auto* scroll = qobject_cast<QScrollArea*>(m_pages->widget(page));
        auto* navigation = m_navigationItems.value(page);
        const QString pagePath = (navigation ? navigation->text(0) + QStringLiteral(" / ") : QString()) + m_pageLabels.value(page);
        const auto add = [&](const QString& title, QWidget* target, const QString& extra = QString()) {
            if (title.trimmed().isEmpty()) return;
            QString section;
            for (auto* parent = target; parent && parent != scroll->widget(); parent = parent->parentWidget()) {
                if (auto* group = qobject_cast<QGroupBox*>(parent)) { section = group->title(); break; }
            }
            const QString path = pagePath + (section.isEmpty() ? QString() : QStringLiteral(" / ") + section);
            const QString terms = settingsSearchText(title + QLatin1Char(' ') + path + QLatin1Char(' ') + extra);
            m_searchIndex.push_back({page, title, path, terms, target});
        };
        add(m_pageLabels.value(page), scroll->widget(), tr(kPageAppearance[page].description) + QLatin1Char(' ') + QLatin1String(kPageAppearance[page].description));
        QSet<QWidget*> indexed;
        for (auto* label : scroll->widget()->findChildren<QLabel*>()) {
            if (!label->buddy()) continue;
            QWidget* field = label->buddy();
            add(label->text(), field, field->accessibleName() + QLatin1Char(' ') + field->toolTip() + QLatin1Char(' ') + field->objectName());
            indexed.insert(field);
        }
        for (auto* group : scroll->widget()->findChildren<QGroupBox*>()) add(group->title(), group);
        for (auto* button : scroll->widget()->findChildren<QAbstractButton*>()) {
            if (button->text().isEmpty() || indexed.contains(button)) continue;
            add(button->text(), button, button->toolTip());
        }
        // Index names, never current values: URLs, credentials and user text
        // are not search terms. This also covers non-form sliders and lists.
        for (auto* control : scroll->widget()->findChildren<QWidget*>()) {
            if (indexed.contains(control) || control->accessibleName().isEmpty() ||
                qobject_cast<QAbstractButton*>(control) || qobject_cast<QLabel*>(control)) continue;
            add(control->accessibleName(), control, control->objectName());
        }
    }
    m_pages->setCurrentIndex(selected);
}

void SettingsWindow::updateSearch() {
    const QString query = settingsSearchText(m_search->text());
    if (query.isEmpty()) return;
    buildSearchIndex();
    m_searchResults->clear();
    const QStringList words = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QVector<std::pair<int, int>> matches;
    for (int i = 0; i < m_searchIndex.size(); ++i) {
        const auto& entry = m_searchIndex[i];
        if (!std::all_of(words.begin(), words.end(), [&](const QString& word) { return entry.terms.contains(word); })) continue;
        const QString title = settingsSearchText(entry.title);
        matches.push_back({title == query ? 0 : title.contains(query) ? 1 : 2, i});
    }
    std::stable_sort(matches.begin(), matches.end(), [](auto a, auto b) { return a.first < b.first; });
    for (const auto& match : matches) {
        const auto& entry = m_searchIndex[match.second];
        auto* item = new QTreeWidgetItem(m_searchResults, {entry.title});
        item->setData(0, Qt::UserRole, match.second);
        item->setData(0, Qt::UserRole + 1, entry.path);
        item->setData(0, Qt::AccessibleDescriptionRole, entry.path);
        item->setIcon(0, icons::icon(kPageAppearance[entry.page].icon, th().textSecondary));
        item->setToolTip(0, entry.title + QLatin1Char('\n') + entry.path);
    }
    m_searchStatus->setText(matches.isEmpty()
        ? tr("No settings found. Try a shorter word or a section name.")
        : tr("%1 results. Select a setting to open its page.").arg(matches.size()));
    if (!matches.isEmpty()) m_searchResults->setCurrentItem(m_searchResults->topLevelItem(0));
    m_content->setCurrentIndex(1);
    if (m_audioApply) m_audioApply->hide();
    m_pageHint->setText(tr("Enter opens a result · Esc clears search"));
}

void SettingsWindow::activateSearchResult(QTreeWidgetItem* item) {
    if (!item) return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= m_searchIndex.size()) return;
    const auto entry = m_searchIndex[index];
    showTab(entry.page);
    revealSetting(entry.target);
}

void SettingsWindow::revealSetting(QWidget* target) {
    if (!target) return;
    const QPointer<QWidget> guarded(target);
    QTimer::singleShot(0, this, [this, guarded] {
        if (!guarded) return;
        auto* scroll = qobject_cast<QScrollArea*>(m_pages->currentWidget());
        if (!scroll) return;
        if (guarded == scroll->widget()) { scroll->verticalScrollBar()->setValue(0); return; }
        if (!scroll->widget()->isAncestorOf(guarded)) return;
        QWidget* visible = guarded;
        while (visible && !visible->isVisibleTo(scroll->widget())) visible = visible->parentWidget();
        if (!visible) return;
        if (qobject_cast<QGroupBox*>(visible))
            scroll->verticalScrollBar()->setValue(visible->mapTo(scroll->widget(), QPoint()).y() - 12);
        else
            scroll->ensureWidgetVisible(visible, 0, 24);
        QWidget* focus = visible;
        if (focus->focusPolicy() == Qt::NoFocus)
            for (auto* child : visible->findChildren<QWidget*>())
                if (child->isVisible() && child->isEnabled() && child->focusPolicy() != Qt::NoFocus) { focus = child; break; }
        if (focus->isEnabled() && focus->focusPolicy() != Qt::NoFocus) focus->setFocus(Qt::OtherFocusReason);
        QWidget* highlight = visible;
        for (auto* parent = visible; parent != scroll->widget() && parent; parent = parent->parentWidget())
            if (qobject_cast<QGroupBox*>(parent)) { highlight = parent; break; }
        highlight->setProperty("searchMatch", true);
        highlight->style()->unpolish(highlight);
        highlight->style()->polish(highlight);
        highlight->update();
        QTimer::singleShot(1800, highlight, [highlight] {
            highlight->setProperty("searchMatch", false);
            highlight->style()->unpolish(highlight);
            highlight->style()->polish(highlight);
            highlight->update();
        });
    });
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_search || watched == m_searchResults) &&
        event->type() == QEvent::ShortcutOverride && !m_search->text().trimmed().isEmpty()) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape ||
            (watched == m_search && key == Qt::Key_Down)) {
            // Search navigation takes precedence over workspace shortcuts.
            event->accept();
            return true;
        }
    }
    if ((watched == m_search || watched == m_searchResults) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && !m_search->text().isEmpty()) {
            m_search->clear();
            m_search->setFocus();
            return true;
        }
        if (watched == m_search && !m_search->text().trimmed().isEmpty()) {
            if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                if (m_searchTimer->isActive()) { m_searchTimer->stop(); updateSearch(); }
                if (key->key() == Qt::Key_Down) m_searchResults->setFocus();
                else activateSearchResult(m_searchResults->currentItem());
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsWindow::refreshTimelineBackgroundSource() {
    if (!m_timelineBackgroundPath) return;
    const QString display = QDir::toNativeSeparators(
        ui::timelinebackgroundprefs::path());
    m_timelineBackgroundPath->setText(display);
    m_timelineBackgroundPath->setToolTip(display);
    m_clearTimelineBackground->setEnabled(!display.isEmpty());
    const QSignalBlocker blocker(m_enableTimelineBackground);
    m_enableTimelineBackground->setChecked(ui::timelinebackgroundprefs::enabled());
}

void SettingsWindow::showEvent(QShowEvent* event) {
    refreshCurrentPage();
    QDialog::showEvent(event);
    constrainToScreen();
    // Native frame margins become reliable only after the first show. Clamp a
    // second time so a window manager cannot leave the title bar off-screen.
    QTimer::singleShot(0, this, &SettingsWindow::constrainToScreen);
}

void SettingsWindow::showQuickImportError(const QString& message) {
    showTab(kQuickImportTab);
    if (m_quickImportPage) m_quickImportPage->showConfigurationError(message);
}

void SettingsWindow::constrainToScreen() {
    if (!isWindow()) return;
    QScreen* target = parentWidget() ? parentWidget()->screen() : screen();
    if (!target) target = QApplication::primaryScreen();
    if (!target) return;

    constexpr int kScreenInset = 20;
    constexpr int kPreferredHeight = 700;
    constexpr int kAbsoluteHeightCap = 760;
    const QRect available =
        target->availableGeometry().adjusted(kScreenInset, kScreenInset,
                                             -kScreenInset, -kScreenInset);
    if (available.isEmpty()) return;

    const int heightCap = std::max(1, std::min(kAbsoluteHeightCap,
                                               available.height()));
    setMaximumHeight(heightCap);
    const int targetHeight = std::min({height(), kPreferredHeight, heightCap});
    resize(std::min(width(), available.width()),
           targetHeight);

    // Keep the whole shell, including its title bar, reachable. The contents
    // stay available through the per-tab vertical scrollbar.
    QRect frame = frameGeometry();
    int x = frame.x();
    int y = frame.y();
    if (frame.right() > available.right())
        x -= frame.right() - available.right();
    if (frame.bottom() > available.bottom())
        y -= frame.bottom() - available.bottom();
    x = std::max(available.left(), x);
    y = std::max(available.top(), y);
    move(x, y);
}

void SettingsWindow::ensurePage(int index) {
    if (index < 0 || index >= int(m_pageFactories.size()) || !m_pageFactories[index]) return;
    // A moved-from std::function is not required to be empty.
    auto create = std::exchange(m_pageFactories[index], {});
    QWidget* placeholder = m_pages->widget(index);
    QWidget* body = create();
    preparePage(body);
    QWidget* page = scrollablePage(body);
    m_pages->removeWidget(placeholder);
    m_pages->insertWidget(index, page);
    placeholder->deleteLater();
}

void SettingsWindow::refreshCurrentPage() {
    if (!m_pages) return;
    switch (m_pages->currentIndex()) {
    case kThemesTab: refreshThemeLibrary(); refreshThemeControls(); break;
    case kThemeEditorTab:
    case kTimelineTab:
    case kBackgroundsTab:
    case kFontsTab: refreshThemeControls(); break;
    case kInterfaceTab: refreshStartupTemplateOptions(); break;
    case kNotebookTab: if (m_notebookPage) m_notebookPage->refresh(); break;
    case kQuickImportTab: if (m_quickImportPage) m_quickImportPage->refresh(); break;
    default: break;
    }
}

void SettingsWindow::showTab(int index) {
    if (!m_pages || index < 0 || index >= m_pages->count()) return;
    m_search->clear();
    ensurePage(index);
    m_pages->setCurrentIndex(index);
    {
        const QSignalBlocker blocker(m_navigation);
        QTreeWidgetItem* item = m_navigationItems.value(index);
        m_navigation->setCurrentItem(item);
        if (!item) m_navigation->clearSelection();
        if (item) m_navigation->scrollToItem(item);
    }
    m_accountButton->setChecked(index == kAccountTab);
    refreshSubtabs(index);
    setWindowTitle(tr("Settings — %1 · %2")
                       .arg(QApplication::applicationDisplayName(), m_pageLabels.value(index)));
    QSettings().setValue(QStringLiteral("ui/settingsPage"), index);
    refreshCurrentPage();
    refreshNavigationAppearance();
}

void SettingsWindow::reloadRecordingPage() {
    if (m_recordingPage) m_recordingPage->reload();
}

bool SettingsWindow::checkAudioPageForTest() const {
    const_cast<SettingsWindow*>(this)->ensurePage(kAudioTab);
    return m_audioPage && m_audioPage->checkForTest();
}

bool SettingsWindow::checkWheelRoutingForTest() {
    return SettingsScrollArea::checkWheelRoutingForTest();
}

bool SettingsWindow::checkOrganizationForTest() {
    const auto fail = [this](const char* stage) {
        std::fprintf(stderr, "settings organization failed: %s\n", stage);
        const QString screenshot = qEnvironmentVariable("DAW_SETTINGS_SCREENSHOT");
        if (!screenshot.isEmpty()) grab().save(screenshot + QStringLiteral("-failure.png"));
        return false;
    };
    const int originalPage = m_pages->currentIndex();
    const QString theme = ThemeManager::instance().themeId();
    if (m_navigation->topLevelItemCount() != 5 || !m_accountButton || !m_subtabs)
        return fail("primary navigation");
    QSet<int> navigationPages{kAccountTab};
    for (int category = 0; category < m_navigation->topLevelItemCount(); ++category) {
        auto* item = m_navigation->topLevelItem(category);
        if (item->childCount()) return fail("flat sidebar");
        if (m_navigation->currentItem() == item) showTab(m_categoryPages[category].front());
        else m_navigation->setCurrentItem(item);
        if (m_pages->currentIndex() != m_categoryPages[category].front() ||
            m_subtabs->count() != m_categoryPages[category].size()) return fail("category landing page");
        for (int i = 0; i < m_subtabs->count(); ++i) {
            const int id = m_subtabs->tabData(i).toInt();
            if (navigationPages.contains(id)) return fail("duplicate page");
            navigationPages.insert(id);
            m_subtabs->setCurrentIndex(i);
            if (m_pages->currentIndex() != id || m_navigation->currentItem() != item)
                return fail("subtab navigation");
        }
    }
    if (navigationPages.size() != kPageCount) return fail("page coverage");
    m_accountButton->click();
    QApplication::processEvents();
    if (m_pages->currentIndex() != kAccountTab || !m_accountButton->isChecked() ||
        !m_navigation->selectedItems().isEmpty() || m_subtabs->count() != 1 ||
        m_accountButton->mapTo(this, QPoint()).y() < height() / 2 ||
        m_accountName->text().isEmpty()) return fail("account footer navigation");
    for (auto* label : m_accountButton->findChildren<QLabel*>()) {
        if (!m_accountButton->rect().contains(QRect(label->mapTo(m_accountButton, QPoint()), label->size())))
            return fail("account card clipping");
    }
    showTab(kAudioTab);
    m_subtabs->setFocus();
    QKeyEvent nextTab(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(m_subtabs, &nextTab);
    if (m_pages->currentIndex() != kRecordingTab) return fail("keyboard subtab navigation");
    const auto pageContains = [this](int page, const char* object) {
        ensurePage(page);
        return m_pages->widget(page)->findChild<QWidget*>(QLatin1String(object)) != nullptr;
    };
    if (!pageContains(kPerformanceTab, "UiFrameMode") || pageContains(kInterfaceTab, "UiFrameMode") ||
        !pageContains(kTimelineTab, "TimelineGridOpacity") || pageContains(kThemesTab, "TimelineGridOpacity") ||
        !pageContains(kFontsTab, "CustomTrackIcons") || pageContains(kThemesTab, "CustomTrackIcons") ||
        !pageContains(kWebBrowserTab, "WebBrowserHome") || pageContains(kBrowserTab, "WebBrowserHome") ||
        !pageContains(kPluginsTab, "PluginSetting.contextPanel/pluginInsertPosition") ||
        pageContains(kContextPanelTab, "PluginSetting.contextPanel/pluginInsertPosition")) return fail("page ownership");

    const int opacity = m_gridOpacity->value();
    m_search->setText(QStringLiteral("TimelineGridOpacity"));
    m_searchTimer->stop();
    updateSearch();
    if (m_searchResults->topLevelItemCount() != 1) return fail("unique search result");
    activateSearchResult(m_searchResults->topLevelItem(0));
    QApplication::processEvents();
    if (m_pages->currentIndex() != kTimelineTab || !m_search->text().isEmpty() ||
        m_gridOpacity->value() != opacity || ThemeManager::instance().themeId() != theme) return fail("search navigation or preference preservation");
    auto* scroll = qobject_cast<QScrollArea*>(m_pages->currentWidget());
    if (!scroll || !scroll->viewport()->rect().intersects(QRect(m_gridOpacity->mapTo(scroll->viewport(), QPoint()), m_gridOpacity->size()))) return fail("search target visibility");

    m_search->setText(QStringLiteral("no-such-setting-937416"));
    m_searchTimer->stop();
    updateSearch();
    if (m_searchResults->topLevelItemCount() != 0 || m_content->currentIndex() != 1) return fail("empty results");
    m_search->setFocus();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(m_search, &escape);
    if (!m_search->text().isEmpty() || m_content->currentIndex() != 0) return fail("escape search");

    // Native keyboard navigation opens a matching setting without changing it.
    m_search->setFocus();
    m_search->setText(QStringLiteral("ScrollMotionSpeed"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(m_search, &enter);
    if (m_pages->currentIndex() != kPerformanceTab) return fail("keyboard activation");

    showTab(kAudioTab);
    QApplication::processEvents();
    if (!m_audioApply || !m_audioApply->isVisible() || m_pages->isAncestorOf(m_audioApply)) return fail("audio footer action");
    for (const auto* name : {"AsioInputConfig", "AsioOutputConfig"}) {
        auto* button = m_audioPage->findChild<QPushButton*>(QLatin1String(name));
        if (button && button->isVisible() && !button->parentWidget()->rect().contains(button->geometry()))
            return fail("audio channel button clipping");
    }
    showTab(kInterfaceTab);
    if (m_audioApply->isVisible()) return fail("audio action page scope");

    showTab(kBackgroundsTab);
    const auto groups = m_pages->currentWidget()->findChildren<QGroupBox*>();
    if (groups.size() != 2) return fail("background sections");
    revealSetting(groups.back());
    QApplication::processEvents();
    scroll = qobject_cast<QScrollArea*>(m_pages->currentWidget());
    if (!scroll || scroll->verticalScrollBar()->value() == 0) return fail("section reveal scroll");

    const QString screenshot = qEnvironmentVariable("DAW_SETTINGS_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        const QList<std::pair<Tab, QString>> examples = {
            {kInterfaceTab, QStringLiteral("general")}, {kAudioTab, QStringLiteral("audio")},
            {kThemesTab, QStringLiteral("themes")}, {kTimelineTab, QStringLiteral("timeline")},
            {kPerformanceTab, QStringLiteral("performance")}, {kPluginsTab, QStringLiteral("plugins")},
            {kBrowserTab, QStringLiteral("files")}, {kRecordingTab, QStringLiteral("recording")},
            {kAiTab, QStringLiteral("ai")}, {kWebBrowserTab, QStringLiteral("web")},
            {kAccountTab, QStringLiteral("account")}};
        for (const auto& [page, name] : examples) {
            showTab(page);
            QApplication::processEvents();
            if (auto* area = qobject_cast<QScrollArea*>(m_pages->currentWidget())) area->verticalScrollBar()->setValue(0);
            grab().save(screenshot + QLatin1Char('-') + name + QStringLiteral(".png"));
        }
        m_search->setText(tr("Background"));
        m_searchTimer->stop();
        updateSearch();
        QApplication::processEvents();
        grab().save(screenshot + QStringLiteral("-search.png"));
    }
    showTab(originalPage);
    return true;
}

QWidget* SettingsWindow::buildLanguageTab() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    auto* languageGroup = new QGroupBox(tr("Application language"), page);
    auto* languageColumn = new QVBoxLayout(languageGroup);
    column->addWidget(languageGroup);

    auto* intro = new QLabel(
        tr("Choose the application language. A full workspace language change "
           "takes effect after restart."), page);
    intro->setWordWrap(true);
    languageColumn->addWidget(intro);

    m_languageList = new QComboBox(page);
    m_languageList->setObjectName(QStringLiteral("ApplicationLanguage"));
    m_languageList->setAccessibleName(tr("Application language"));
    languageColumn->addWidget(m_languageList);

    m_languageStatus = new QLabel(page);
    m_languageStatus->setWordWrap(true);
    m_languageStatus->setObjectName(QStringLiteral("SettingsHint"));
    languageColumn->addWidget(m_languageStatus);

    auto* translations = new QGroupBox(tr("Translation files"), page);
    auto* translationColumn = new QVBoxLayout(translations);
    column->addWidget(translations);

    auto* help = new QLabel(
        tr("To make another language, export the JSON template, fill in the "
           "empty translations in any text editor, and import the file."), page);
    help->setWordWrap(true);
    translationColumn->addWidget(help);

    auto* importButton = new QPushButton(tr("Import Language…"), page);
    auto* exportButton = new QPushButton(tr("Export Template…"), page);
    m_removeLanguage = new QPushButton(tr("Remove Language"), page);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(importButton);
    buttons->addWidget(exportButton);
    buttons->addStretch(1);
    buttons->addWidget(m_removeLanguage);
    translationColumn->addLayout(buttons);
    column->addStretch(1);

    connect(&ui::LocalizationManager::instance(),
            &ui::LocalizationManager::languagesChanged, this,
            &SettingsWindow::refreshLanguages);

    connect(m_languageList, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (index < 0) return;
                const QString locale =
                    m_languageList->itemData(index).toString();
                if (locale.isEmpty()) return;

                QString error;
                if (!ui::LocalizationManager::instance().setPreferredLocale(
                        locale, &error)) {
                    QMessageBox::warning(this, tr("Language change failed"), error);
                    refreshLanguages();
                    return;
                }
                refreshLanguages();
                if (locale == ui::LocalizationManager::instance().activeLocale())
                    return;

                QMessageBox box(QMessageBox::Question,
                                tr("Restart required"),
                                tr("Restart VLTONE now to apply the new "
                                   "language?"),
                                QMessageBox::Yes | QMessageBox::No, this);
                box.button(QMessageBox::Yes)->setText(tr("Restart Now"));
                box.button(QMessageBox::No)->setText(tr("Later"));
                if (box.exec() == QMessageBox::Yes) emit restartRequested();
            });

    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Language"), QString(),
            tr("VLTONE Language Pack (*.vltlang.json *.json);;All Files (*)"));
        if (path.isEmpty()) return;

        auto result =
            ui::LocalizationManager::instance().importLanguagePack(path);
        if (result.alreadyExists) {
            const auto answer = QMessageBox::question(
                this, tr("Replace Language"),
                tr("A language pack for %1 already exists. Replace it?")
                    .arg(result.locale),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes) {
                result = ui::LocalizationManager::instance().importLanguagePack(
                    path, true);
            }
        }
        if (!result.ok) {
            if (!result.alreadyExists)
                QMessageBox::warning(this, tr("Import failed"), result.error);
            return;
        }
        refreshLanguages();
        QMessageBox::information(
            this, tr("Language installed"),
            tr("%1 was installed. Translated: %2; missing: %3; unknown: %4.")
                .arg(result.languageName)
                .arg(result.translated)
                .arg(result.missing)
                .arg(result.unknown));
    });

    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Language Template"),
            QStringLiteral("vlt-language-template.vltlang.json"),
            tr("VLTONE Language Pack (*.vltlang.json *.json)"));
        if (path.isEmpty()) return;
        QString error;
        if (!ui::LocalizationManager::instance().exportTemplate(path, &error)) {
            QMessageBox::warning(this, tr("Export failed"), error);
            return;
        }
        QMessageBox::information(this, tr("Template exported"),
                                 tr("The language template was saved to %1.")
                                     .arg(path));
    });

    connect(m_removeLanguage, &QPushButton::clicked, this, [this] {
        const int index = m_languageList->currentIndex();
        if (index < 0) return;
        const QString locale = m_languageList->itemData(index).toString();
        const QString name = m_languageList->itemText(index);
        if (QMessageBox::question(
                this, tr("Remove Language"),
                tr("Remove the installed language %1?").arg(name),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        QString error;
        if (!ui::LocalizationManager::instance().removeLanguagePack(locale,
                                                                     &error)) {
            QMessageBox::warning(this, tr("Remove failed"), error);
            return;
        }
        refreshLanguages();
    });

    refreshLanguages();
    return page;
}

void SettingsWindow::refreshLanguages() {
    if (!m_languageList) return;
    const QString preferred =
        ui::LocalizationManager::instance().preferredLocale();
    const QString active = ui::LocalizationManager::instance().activeLocale();

    QString activeName = active;
    QString preferredName = preferred;
    QSignalBlocker blocker(m_languageList);
    m_languageList->clear();
    for (const ui::LanguageInfo& language :
         ui::LocalizationManager::instance().languages()) {
        const QString label = language.author.isEmpty()
                                  ? QStringLiteral("%1 (%2)")
                                        .arg(language.languageName,
                                             language.locale)
                                  : QStringLiteral("%1 (%2) — %3")
                                        .arg(language.languageName,
                                             language.locale, language.author);
        m_languageList->addItem(label, language.locale);
        if (language.locale == active) activeName = language.languageName;
        if (language.locale == preferred) preferredName = language.languageName;
    }
    const int selected = m_languageList->findData(preferred);
    m_languageList->setCurrentIndex(selected >= 0 ? selected : 0);
    const QString selectedLocale = m_languageList->currentData().toString();
    m_removeLanguage->setEnabled(
        !ui::LocalizationManager::instance().isBuiltIn(selectedLocale));

    m_languageStatus->setText(
        active == preferred
            ? tr("Current language: %1.").arg(activeName)
            : tr("Current language: %1. Next launch: %2.")
                  .arg(activeName, preferredName));
}

QWidget* SettingsWindow::buildThemesTab() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->addWidget(new QLabel(
        tr("Choose a built-in palette, or apply a complete saved theme.")));

    auto* knobGroup = new QGroupBox(tr("Knob design"), page);
    knobGroup->setObjectName(QStringLiteral("KnobDesignGroup"));
    auto* knobColumn = new QVBoxLayout(knobGroup);
    auto* knobRow = new QHBoxLayout;
    auto* knobDesign = new QComboBox(knobGroup);
    knobDesign->setObjectName(QStringLiteral("KnobDesignSelector"));
    knobDesign->setAccessibleName(tr("Knob design"));
    knobDesign->addItem(tr("Classic"), int(ui::KnobDesign::Classic));
    knobDesign->addItem(tr("Brushed metal"), int(ui::KnobDesign::BrushedMetal));
    knobDesign->setCurrentIndex(knobDesign->findData(int(ui::knobDesign())));
    connect(knobDesign, &QComboBox::currentIndexChanged, this, [knobDesign] {
        ui::setKnobDesign(static_cast<ui::KnobDesign>(knobDesign->currentData().toInt()));
    });
    knobRow->addWidget(knobDesign, 1);
    knobRow->addSpacing(16);
    auto* panPreview = new ui::PanKnob(knobGroup);
    panPreview->setAccessibleName(tr("Pan preview"));
    panPreview->setPan(.35);
    knobRow->addWidget(panPreview);
    auto* knobPreview = new ui::Knob({}, knobGroup);
    knobPreview->setBare(52);
    knobPreview->setRange(0, 1);
    knobPreview->setValue(.65);
    knobPreview->setAccessibleName(tr("Knob preview"));
    knobRow->addWidget(knobPreview);
    knobColumn->addLayout(knobRow);
    auto* knobHint = new QLabel(tr("Applies to round controls throughout VLTONE. Colored value rings keep their function."), knobGroup);
    knobHint->setWordWrap(true);
    knobColumn->addWidget(knobHint);
    col->addWidget(knobGroup);

    auto* faderGroup = new QGroupBox(tr("Timeline volume faders"), page);
    faderGroup->setObjectName(QStringLiteral("TrackFaderDesignGroup"));
    auto* faderColumn = new QVBoxLayout(faderGroup);
    auto* faderRow = new QHBoxLayout;
    auto* faderDesign = new QComboBox(faderGroup);
    faderDesign->setObjectName(QStringLiteral("TrackFaderDesignSelector"));
    faderDesign->setAccessibleName(tr("Timeline volume faders"));
    using FaderDesign = ui::FaderWidget::TrackHeaderDesign;
    faderDesign->addItem(tr("Classic"), int(FaderDesign::Classic));
    faderDesign->addItem(tr("Recessed"), int(FaderDesign::Recessed));
    faderDesign->setCurrentIndex(faderDesign->findData(int(ui::FaderWidget::trackHeaderDesign())));
    connect(faderDesign, &QComboBox::currentIndexChanged, this, [faderDesign] {
        ui::FaderWidget::setTrackHeaderDesign(
            static_cast<FaderDesign>(faderDesign->currentData().toInt()));
    });
    faderRow->addWidget(faderDesign, 1);
    faderRow->addSpacing(16);
    auto* faderPreview = new ui::FaderWidget(Qt::Horizontal, faderGroup);
    faderPreview->setObjectName(QStringLiteral("TrackFaderPreview"));
    faderPreview->setTrackHeaderStyle(true);
    faderPreview->setFixedWidth(140);
    faderPreview->setGain(.8);
    faderPreview->setAccessibleName(tr("Volume fader preview"));
    faderRow->addWidget(faderPreview);
    faderColumn->addLayout(faderRow);
    auto* faderHint = new QLabel(tr("Choose the volume fader design for timeline track headers."), faderGroup);
    faderHint->setWordWrap(true);
    faderColumn->addWidget(faderHint);
    col->addWidget(faderGroup);

    m_themeList = new QListWidget(page);
    m_themeList->setObjectName(QStringLiteral("ThemePaletteGallery"));
    m_themeList->setAccessibleName(tr("Built-in palettes"));
    m_themeList->setViewMode(QListView::IconMode);
    m_themeList->setResizeMode(QListView::Adjust);
    m_themeList->setMovement(QListView::Static);
    m_themeList->setIconSize(QSize(132, 62));
    m_themeList->setGridSize(QSize(158, 102));
    m_themeList->setSpacing(4);
    m_themeList->setMinimumHeight(230);
    m_themeList->setMaximumHeight(340);
    {
        // Populated with the signal blocked, and the handler connected only
        // afterwards: `currentItemChanged` *persists* a theme, and no window
        // should be able to change the user's palette by being opened. That
        // matters most when the active theme is a custom one — no row matches
        // it, so any current-row the view picks for itself would be a silent
        // switch to a preset.
        QSignalBlocker block(m_themeList);
        for (const Theme& preset : ThemeManager::instance().presets()) {
            auto* item = new QListWidgetItem(presetDisplayName(preset),
                                             m_themeList);
            item->setData(Qt::UserRole, preset.id);
            QPixmap thumbnail(264, 124);
            thumbnail.setDevicePixelRatio(2.0);
            thumbnail.fill(preset.background);
            QPainter painter(&thumbnail);
            painter.fillRect(QRect(0, 0, 132, 12), preset.headerBackground);
            painter.fillRect(QRect(0, 12, 28, 50), preset.surface);
            for (int lane = 0; lane < 3; ++lane) {
                painter.fillRect(QRect(4, 17 + lane * 14, 19, 4), preset.textSecondary);
                painter.fillRect(QRect(35 + lane * 9, 16 + lane * 14, 68 - lane * 8, 9),
                                 lane == 1 ? preset.waveform : preset.accent);
            }
            painter.setPen(preset.cursor);
            painter.drawLine(70, 12, 70, 61);
            painter.end();
            item->setIcon(QIcon(thumbnail));
            item->setToolTip(presetDisplayName(preset));
            if (preset.id == ThemeManager::instance().themeId())
                m_themeList->setCurrentItem(item);
        }
    }
    connect(m_themeList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* cur, QListWidgetItem*) {
                if (cur) {
                    QSettings().remove(QStringLiteral("ui/activeThemePackage"));
                    ThemeManager::instance().setThemeId(
                        cur->data(Qt::UserRole).toString());
                    refreshThemeLibrary();
                }
            });
    // Keep the selection in step if the theme changes elsewhere (e.g. undo of a
    // future settings action, or another surface switching it).
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        const QString id = ThemeManager::instance().themeId();
        bool found = false;
        for (int i = 0; i < m_themeList->count(); ++i) {
            if (m_themeList->item(i)->data(Qt::UserRole).toString() == id) {
                QSignalBlocker block(m_themeList);
                m_themeList->setCurrentRow(i);
                found = true;
                break;
            }
        }
        if (!found) {
            QSignalBlocker block(m_themeList);
            m_themeList->setCurrentRow(-1);
        }
    });

    col->addWidget(new QLabel(tr("Built-in palettes")));
    col->addWidget(m_themeList, 1);

    auto* libraryGroup = new QGroupBox(tr("Saved Themes"), page);
    auto* libraryColumn = new QVBoxLayout(libraryGroup);
    auto* libraryHint = new QLabel(
        tr("A .vlttheme file keeps the palette, backgrounds and fonts together. "
           "Imported resources are copied into VLTONE so the original file can "
           "be deleted."),
        libraryGroup);
    libraryHint->setWordWrap(true);
    libraryColumn->addWidget(libraryHint);

    m_savedThemeList = new QListWidget(libraryGroup);
    m_savedThemeList->setAccessibleName(tr("Saved themes"));
    m_savedThemeList->setMinimumHeight(110);
    libraryColumn->addWidget(m_savedThemeList);

    auto* saveTheme = new QPushButton(tr("Save Theme…"), libraryGroup);
    auto* importTheme = new QPushButton(tr("Import…"), libraryGroup);
    m_applySavedTheme = new QPushButton(tr("Apply"), libraryGroup);
    m_exportSavedTheme = new QPushButton(tr("Export Current…"), libraryGroup);
    auto* openThemesFolder = new QPushButton(tr("Open Themes Folder"), libraryGroup);
    m_applySavedTheme->setEnabled(false);
    auto* primaryLibraryButtons = new QHBoxLayout;
    primaryLibraryButtons->addWidget(saveTheme);
    primaryLibraryButtons->addWidget(importTheme);
    primaryLibraryButtons->addWidget(m_applySavedTheme);
    primaryLibraryButtons->addStretch(1);
    libraryColumn->addLayout(primaryLibraryButtons);
    auto* secondaryLibraryButtons = new QHBoxLayout;
    secondaryLibraryButtons->addWidget(m_exportSavedTheme);
    secondaryLibraryButtons->addWidget(openThemesFolder);
    secondaryLibraryButtons->addStretch(1);
    libraryColumn->addLayout(secondaryLibraryButtons);
    col->addWidget(libraryGroup);

    connect(saveTheme, &QPushButton::clicked, this,
            &SettingsWindow::saveCurrentThemeToLibrary);
    connect(importTheme, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Theme"), QString(),
            tr("VLTONE Theme (*.vlttheme);;Legacy Theme (*.json *.dawtheme.json);;All Files (*)"));
        if (!path.isEmpty()) importThemeFile(path);
    });
    connect(m_applySavedTheme, &QPushButton::clicked, this,
            &SettingsWindow::applySelectedSavedTheme);
    connect(m_exportSavedTheme, &QPushButton::clicked, this,
            &SettingsWindow::exportCurrentTheme);
    connect(m_savedThemeList, &QListWidget::itemActivated, this,
            [this](QListWidgetItem*) { applySelectedSavedTheme(); });
    connect(m_savedThemeList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current) {
                m_applySavedTheme->setEnabled(current != nullptr);
            });
    connect(openThemesFolder, &QPushButton::clicked, this, [] {
        QDir().mkpath(ui::ThemePackage::libraryDirectory());
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(ui::ThemePackage::libraryDirectory()));
    });

    QDir().mkpath(ui::ThemePackage::libraryDirectory());
    m_themeLibraryWatcher = new QFileSystemWatcher(this);
    m_themeLibraryWatcher->addPath(ui::ThemePackage::libraryDirectory());
    connect(m_themeLibraryWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { refreshThemeLibrary(); });
    refreshThemeLibrary();

    col->addStretch();
    return page;
}

QWidget* SettingsWindow::buildFontsTab() {
    auto* page = new QWidget(this);
    auto* col = new QVBoxLayout(page);
    m_trackIconGroup = new QGroupBox(tr("Track Icons"), page);
    m_trackIconGroup->setObjectName(QStringLiteral("TrackIconSettings"));
    auto* iconColumn = new QVBoxLayout(m_trackIconGroup);
    auto* iconHint = new QLabel(tr("Add your own images to the track icon library. Images are optimized to 96 × 96 and saved with your theme settings. Projects opened without an icon use the default track icon."), m_trackIconGroup);
    iconHint->setWordWrap(true);
    iconColumn->addWidget(iconHint);
    m_trackIconList = new QListWidget(m_trackIconGroup);
    m_trackIconList->setObjectName(QStringLiteral("CustomTrackIcons"));
    m_trackIconList->setAccessibleName(tr("Custom track icons"));
    m_trackIconList->setViewMode(QListView::IconMode);
    m_trackIconList->setResizeMode(QListView::Adjust);
    m_trackIconList->setMovement(QListView::Static);
    m_trackIconList->setIconSize(QSize(32, 32));
    m_trackIconList->setGridSize(QSize(92, 68));
    m_trackIconList->setWordWrap(true);
    m_trackIconList->setFixedHeight(150);
    iconColumn->addWidget(m_trackIconList);
    auto* iconButtons = new QHBoxLayout;
    auto* addIcon = new QPushButton(tr("Add icon…"), m_trackIconGroup);
    addIcon->setObjectName(QStringLiteral("ImportTrackIcon"));
    auto* removeIcon = new QPushButton(tr("Remove icon"), m_trackIconGroup);
    removeIcon->setEnabled(false);
    removeIcon->setToolTip(tr("Tracks using this icon will display their default icon."));
    iconButtons->addWidget(addIcon);
    iconButtons->addWidget(removeIcon);
    iconButtons->addStretch(1);
    iconColumn->addLayout(iconButtons);
    col->addWidget(m_trackIconGroup);
    connect(addIcon, &QPushButton::clicked, this, [this] { importTrackIconFile(); });
    connect(m_trackIconList, &QListWidget::currentItemChanged, this,
            [removeIcon](QListWidgetItem* item) { removeIcon->setEnabled(item != nullptr); });
    connect(removeIcon, &QPushButton::clicked, this, [this] {
        auto* item = m_trackIconList->currentItem();
        if (!item) return;
        QString error;
        if (!ui::trackicons::removeCustom(item->data(Qt::UserRole).toString(), &error))
            QMessageBox::warning(this, tr("Track Icons"), error);
    });
    connect(&ThemeManager::instance(), &ThemeManager::trackIconsChanged, this,
            &SettingsWindow::refreshTrackIcons);
    refreshTrackIcons();

    auto* fontGroup = new QGroupBox(tr("Interface Font"), page);
    auto* fontColumn = new QVBoxLayout(fontGroup);
    auto* fontHelp = new QLabel(
        tr("Import a TTF, OTF, TTC, or OTC font. A private copy is stored in "
           "the application data folder and applied immediately."),
        fontGroup);
    fontHelp->setWordWrap(true);
    fontColumn->addWidget(fontHelp);

    m_fontStatus = new QLabel(fontGroup);
    m_fontStatus->setObjectName(QStringLiteral("InterfaceFontStatus"));
    m_fontStatus->setAccessibleName(tr("Current interface font"));
    m_fontStatus->setWordWrap(true);
    fontColumn->addWidget(m_fontStatus);

    auto* preview = new QLabel(
        tr("Preview: Music, rhythm, automation — 123 BPM"), fontGroup);
    preview->setObjectName(QStringLiteral("InterfaceFontPreview"));
    preview->setWordWrap(true);
    fontColumn->addWidget(preview);

    auto* importFont = new QPushButton(tr("Import Font…"), fontGroup);
    m_resetFont = new QPushButton(tr("Use Default Font"), fontGroup);
    auto* fontButtons = new QHBoxLayout;
    fontButtons->addWidget(importFont);
    fontButtons->addWidget(m_resetFont);
    fontButtons->addStretch(1);
    fontColumn->addLayout(fontButtons);
    col->addWidget(fontGroup);

    connect(importFont, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Font"), QString(),
            tr("Font Files (*.ttf *.otf *.ttc *.otc);;All Files (*)"));
        if (path.isEmpty()) return;
        QString error;
        if (!ThemeManager::instance().importFont(path, &error)) {
            QMessageBox::warning(this, tr("Font import failed"), error);
            return;
        }
        refreshFontStatus();
    });
    connect(m_resetFont, &QPushButton::clicked, this, [] {
        ThemeManager::instance().resetFont();
    });
    connect(&ThemeManager::instance(), &ThemeManager::fontChanged, this,
            &SettingsWindow::refreshFontStatus);
    refreshFontStatus();

    col->addStretch();
    return page;
}

QWidget* SettingsWindow::buildBackgroundsTab() {
    auto* page = new QWidget(this);
    auto* col = new QVBoxLayout(page);
    auto* backgroundGroup = new QGroupBox(tr("Timeline Background"), page);
    auto* backgroundColumn = new QVBoxLayout(backgroundGroup);
    auto* backgroundHint = new QLabel(
        tr("Choose a local photo, animated GIF or video, or send a video from "
           "the browser to the timeline background. Backgrounds are appearance "
           "settings and are not saved in the project."),
        backgroundGroup);
    backgroundHint->setWordWrap(true);
    backgroundColumn->addWidget(backgroundHint);

    auto* enableTimelineBackground = new QCheckBox(
        tr("Enable custom timeline background"), backgroundGroup);
    m_enableTimelineBackground = enableTimelineBackground;
    enableTimelineBackground->setChecked(
        ui::timelinebackgroundprefs::enabled());
    enableTimelineBackground->setAccessibleName(
        tr("Custom timeline background enabled"));
    backgroundColumn->addWidget(enableTimelineBackground);

    auto* backgroundForm = new QFormLayout;
    backgroundForm->setSpacing(8);
    auto* fileRow = new QWidget(backgroundGroup);
    m_timelineFileRow = fileRow;
    auto* fileLayout = new QHBoxLayout(fileRow);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    fileLayout->setSpacing(6);
    auto* backgroundPath = new QLineEdit(fileRow);
    m_timelineBackgroundPath = backgroundPath;
    backgroundPath->setReadOnly(true);
    backgroundPath->setPlaceholderText(tr("Theme colour only"));
    backgroundPath->setAccessibleName(tr("Timeline background source"));
    auto* chooseBackground = new QPushButton(tr("Choose…"), fileRow);
    chooseBackground->setAccessibleName(tr("Choose timeline background"));
    auto* clearBackground = new QPushButton(tr("Clear"), fileRow);
    m_clearTimelineBackground = clearBackground;
    fileLayout->addWidget(backgroundPath, 1);
    fileLayout->addWidget(chooseBackground);
    fileLayout->addWidget(clearBackground);
    backgroundForm->addRow(tr("Media"), fileRow);

    auto* timelinePlacement = new QComboBox(backgroundGroup);
    m_timelinePlacement = timelinePlacement;
    using BackgroundPlacement = ui::timelinebackgroundprefs::Placement;
    timelinePlacement->addItem(tr("Fill frame (crop to fit)"),
                               int(BackgroundPlacement::Fill));
    timelinePlacement->addItem(tr("Stretch to frame"),
                               int(BackgroundPlacement::Stretch));
    timelinePlacement->addItem(tr("Tile at original size"),
                               int(BackgroundPlacement::Tile));
    timelinePlacement->addItem(tr("Original size, centred"),
                               int(BackgroundPlacement::Center));
    timelinePlacement->setCurrentIndex(std::max(
        0, timelinePlacement->findData(
               int(ui::timelinebackgroundprefs::placement()))));
    timelinePlacement->setAccessibleName(
        tr("Timeline background placement"));
    timelinePlacement->setToolTip(
        tr("Fill automatically adapts to every window and screen size."));
    connect(timelinePlacement, &QComboBox::activated, this,
            [this, timelinePlacement](int index) {
                ui::timelinebackgroundprefs::setPlacement(
                    BackgroundPlacement(
                        timelinePlacement->itemData(index).toInt()));
                emit themeBackgroundSettingsChanged();
            });
    backgroundForm->addRow(tr("Layout"), timelinePlacement);

    const auto refreshBackgroundPath = [this] { refreshTimelineBackgroundSource(); };
    connect(chooseBackground, &QPushButton::clicked, this,
            [this, refreshBackgroundPath] {
                const QString selected = QFileDialog::getOpenFileName(
                    this, tr("Choose a timeline background"),
                    ui::timelinebackgroundprefs::path(),
                    tr("Background media (*.png *.jpg *.jpeg *.webp *.bmp "
                       "*.gif *.mp4 *.m4v *.webm *.ogv *.mov *.mkv *.avi);;Images "
                       "(*.png *.jpg *.jpeg *.webp *.bmp *.gif);;Videos "
                       "(*.mp4 *.m4v *.webm *.ogv *.mov *.mkv *.avi)"));
                if (selected.isEmpty()) return;
                if (!ui::timelinebackgroundprefs::setPath(selected)) {
                    QMessageBox::warning(
                        this, tr("Unsupported background"),
                        tr("Choose a supported image, GIF or video file."));
                    return;
                }
                m_enableTimelineBackground->setChecked(true);
                refreshBackgroundPath();
                emit themeBackgroundSettingsChanged();
            });
    connect(clearBackground, &QPushButton::clicked, this,
            [this, refreshBackgroundPath] {
                ui::timelinebackgroundprefs::clear();
                refreshBackgroundPath();
                emit themeBackgroundSettingsChanged();
            });

    const auto addPercentSlider =
        [backgroundGroup, backgroundForm](const QString& label, int value,
                                          const QString& accessibleName) {
            auto* row = new QWidget(backgroundGroup);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(8);
            auto* slider = new ui::GlassSlider(Qt::Horizontal, row);
            slider->setRange(0, 100);
            slider->setValue(value);
            slider->setAccessibleName(accessibleName);
            auto* output = new QLabel(QStringLiteral("%1%").arg(value), row);
            output->setMinimumWidth(40);
            output->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(slider, 1);
            layout->addWidget(output);
            backgroundForm->addRow(label, row);
            QObject::connect(slider, &QSlider::valueChanged, output,
                             [output](int current) {
                                 output->setText(
                                     QStringLiteral("%1%").arg(current));
                             });
            return slider;
        };
    auto* visibility = addPercentSlider(
        tr("Visibility"), ui::timelinebackgroundprefs::visibility(),
        tr("Timeline background visibility"));
    m_timelineVisibility = visibility;
    m_timelineVisibilityValue =
        visibility->parentWidget()->findChild<QLabel*>();
    connect(visibility, &QSlider::valueChanged, this, [this](int value) {
        ui::timelinebackgroundprefs::setVisibility(value);
        emit themeBackgroundSettingsChanged();
    });

    auto* blurRow = new QWidget(backgroundGroup);
    m_timelineBlurRow = blurRow;
    auto* blurLayout = new QHBoxLayout(blurRow);
    blurLayout->setContentsMargins(0, 0, 0, 0);
    blurLayout->setSpacing(8);
    auto* blur = new ui::GlassSlider(Qt::Horizontal, blurRow);
    m_timelineBlur = blur;
    blur->setRange(0, 32);
    blur->setValue(ui::timelinebackgroundprefs::blurRadius());
    blur->setAccessibleName(tr("Timeline background blur"));
    auto* blurValue = new QLabel(
        tr("%1 px").arg(ui::timelinebackgroundprefs::blurRadius()), blurRow);
    m_timelineBlurValue = blurValue;
    blurValue->setMinimumWidth(52);
    blurValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    blurLayout->addWidget(blur, 1);
    blurLayout->addWidget(blurValue);
    backgroundForm->addRow(tr("Blur"), blurRow);
    auto* blurDebounce = new QTimer(backgroundGroup);
    blurDebounce->setSingleShot(true);
    blurDebounce->setInterval(45);
    connect(blur, &QSlider::valueChanged, this,
            [blurValue, blurDebounce](int pixels) {
                blurValue->setText(tr("%1 px").arg(pixels));
                ui::timelinebackgroundprefs::setBlurRadius(pixels);
                blurDebounce->start();
            });
    connect(blurDebounce, &QTimer::timeout, this,
            &SettingsWindow::themeBackgroundSettingsChanged);

    auto* animateBackground = new QCheckBox(
        tr("Play GIF and video backgrounds"), backgroundGroup);
    m_timelineAnimate = animateBackground;
    animateBackground->setChecked(
        ui::timelinebackgroundprefs::animatedBackgroundsEnabled());
    animateBackground->setToolTip(
        tr("Reduce Motion freezes animated backgrounds on a still frame."));
    connect(animateBackground, &QCheckBox::toggled, this, [this](bool enabled) {
        ui::timelinebackgroundprefs::setAnimatedBackgroundsEnabled(enabled);
        emit themeBackgroundSettingsChanged();
    });
    backgroundForm->addRow(QString(), animateBackground);
    backgroundColumn->addLayout(backgroundForm);
    col->addWidget(backgroundGroup);
    refreshBackgroundPath();
    const auto syncTimelineBackgroundEnabled =
        [fileRow, timelinePlacement, visibility, blurRow,
         animateBackground](bool enabled) {
            fileRow->setEnabled(enabled);
            timelinePlacement->setEnabled(enabled);
            visibility->setEnabled(enabled);
            blurRow->setEnabled(enabled);
            animateBackground->setEnabled(enabled);
        };
    syncTimelineBackgroundEnabled(enableTimelineBackground->isChecked());
    connect(enableTimelineBackground, &QCheckBox::toggled, this,
            [this, syncTimelineBackgroundEnabled](bool enabled) {
                ui::timelinebackgroundprefs::setEnabled(enabled);
                syncTimelineBackgroundEnabled(enabled);
                emit themeBackgroundSettingsChanged();
            });

    auto* headerBackgroundGroup =
        new QGroupBox(tr("Header Background"), page);
    auto* headerBackgroundColumn = new QVBoxLayout(headerBackgroundGroup);
    auto* headerBackgroundHint = new QLabel(
        tr("Add a local photo, animated GIF or video behind the top controls. "
           "A low visibility keeps every control readable."),
        headerBackgroundGroup);
    headerBackgroundHint->setWordWrap(true);
    headerBackgroundColumn->addWidget(headerBackgroundHint);

    auto* enableHeaderBackground = new QCheckBox(
        tr("Enable custom header background"), headerBackgroundGroup);
    m_enableHeaderBackground = enableHeaderBackground;
    enableHeaderBackground->setChecked(ui::headerbackgroundprefs::enabled());
    enableHeaderBackground->setAccessibleName(
        tr("Custom header background enabled"));
    headerBackgroundColumn->addWidget(enableHeaderBackground);

    auto* headerBackgroundForm = new QFormLayout;
    headerBackgroundForm->setSpacing(8);
    auto* headerFileRow = new QWidget(headerBackgroundGroup);
    m_headerFileRow = headerFileRow;
    auto* headerFileLayout = new QHBoxLayout(headerFileRow);
    headerFileLayout->setContentsMargins(0, 0, 0, 0);
    headerFileLayout->setSpacing(6);
    auto* headerPath = new QLineEdit(headerFileRow);
    m_headerBackgroundPath = headerPath;
    headerPath->setReadOnly(true);
    headerPath->setPlaceholderText(tr("Theme colour only"));
    headerPath->setAccessibleName(tr("Header background file"));
    auto* chooseHeaderBackground = new QPushButton(tr("Choose..."), headerFileRow);
    chooseHeaderBackground->setAccessibleName(tr("Choose header background"));
    auto* clearHeaderBackground = new QPushButton(tr("Clear"), headerFileRow);
    m_clearHeaderBackground = clearHeaderBackground;
    headerFileLayout->addWidget(headerPath, 1);
    headerFileLayout->addWidget(chooseHeaderBackground);
    headerFileLayout->addWidget(clearHeaderBackground);
    headerBackgroundForm->addRow(tr("Media"), headerFileRow);

    const auto refreshHeaderBackgroundPath =
        [headerPath, clearHeaderBackground] {
            const QString path = ui::headerbackgroundprefs::path();
            headerPath->setText(QDir::toNativeSeparators(path));
            headerPath->setToolTip(path);
            clearHeaderBackground->setEnabled(!path.isEmpty());
        };
    connect(chooseHeaderBackground, &QPushButton::clicked, this,
            [this, refreshHeaderBackgroundPath] {
                const QString selected = QFileDialog::getOpenFileName(
                    this, tr("Choose a header background"),
                    ui::headerbackgroundprefs::path(),
                    tr("Background media (*.png *.jpg *.jpeg *.webp *.bmp "
                       "*.gif *.mp4 *.m4v *.webm *.ogv *.mov *.mkv *.avi);;Images "
                       "(*.png *.jpg *.jpeg *.webp *.bmp *.gif);;Videos "
                       "(*.mp4 *.m4v *.webm *.ogv *.mov *.mkv *.avi)"));
                if (selected.isEmpty()) return;
                if (!ui::headerbackgroundprefs::setPath(selected)) {
                    QMessageBox::warning(
                        this, tr("Unsupported background"),
                        tr("Choose a supported image, GIF or video file."));
                    return;
                }
                m_enableHeaderBackground->setChecked(true);
                refreshHeaderBackgroundPath();
                emit themeBackgroundSettingsChanged();
            });
    connect(clearHeaderBackground, &QPushButton::clicked, this,
            [this, refreshHeaderBackgroundPath] {
                ui::headerbackgroundprefs::clear();
                refreshHeaderBackgroundPath();
                emit themeBackgroundSettingsChanged();
            });

    auto* headerPlacement = new QComboBox(headerBackgroundGroup);
    m_headerPlacement = headerPlacement;
    headerPlacement->addItem(tr("Fill frame (crop to fit)"),
                             int(BackgroundPlacement::Fill));
    headerPlacement->addItem(tr("Stretch to frame"),
                             int(BackgroundPlacement::Stretch));
    headerPlacement->addItem(tr("Tile at original size"),
                             int(BackgroundPlacement::Tile));
    headerPlacement->addItem(tr("Original size, centred"),
                             int(BackgroundPlacement::Center));
    headerPlacement->setCurrentIndex(std::max(
        0, headerPlacement->findData(
               int(ui::headerbackgroundprefs::placement()))));
    headerPlacement->setAccessibleName(tr("Header background placement"));
    headerPlacement->setToolTip(
        tr("Fill automatically adapts to every window and screen size."));
    connect(headerPlacement, &QComboBox::activated, this,
            [this, headerPlacement](int index) {
                ui::headerbackgroundprefs::setPlacement(
                    BackgroundPlacement(
                        headerPlacement->itemData(index).toInt()));
                emit themeBackgroundSettingsChanged();
            });
    headerBackgroundForm->addRow(tr("Layout"), headerPlacement);

    const auto addHeaderPercentSlider =
        [headerBackgroundGroup, headerBackgroundForm](
            const QString& label, int value, const QString& accessibleName) {
            auto* row = new QWidget(headerBackgroundGroup);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(8);
            auto* slider = new ui::GlassSlider(Qt::Horizontal, row);
            slider->setRange(0, 100);
            slider->setValue(value);
            slider->setAccessibleName(accessibleName);
            auto* output = new QLabel(QStringLiteral("%1%").arg(value), row);
            output->setMinimumWidth(40);
            output->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(slider, 1);
            layout->addWidget(output);
            headerBackgroundForm->addRow(label, row);
            QObject::connect(slider, &QSlider::valueChanged, output,
                             [output](int current) {
                                 output->setText(
                                     QStringLiteral("%1%").arg(current));
                             });
            return slider;
        };
    auto* headerVisibility = addHeaderPercentSlider(
        tr("Visibility"), ui::headerbackgroundprefs::visibility(),
        tr("Header background visibility"));
    m_headerVisibility = headerVisibility;
    m_headerVisibilityValue =
        headerVisibility->parentWidget()->findChild<QLabel*>();
    connect(headerVisibility, &QSlider::valueChanged, this,
            [this](int value) {
                ui::headerbackgroundprefs::setVisibility(value);
                emit themeBackgroundSettingsChanged();
            });

    auto* headerBlurRow = new QWidget(headerBackgroundGroup);
    m_headerBlurRow = headerBlurRow;
    auto* headerBlurLayout = new QHBoxLayout(headerBlurRow);
    headerBlurLayout->setContentsMargins(0, 0, 0, 0);
    headerBlurLayout->setSpacing(8);
    auto* headerBlur = new ui::GlassSlider(Qt::Horizontal, headerBlurRow);
    m_headerBlur = headerBlur;
    headerBlur->setRange(0, 32);
    headerBlur->setValue(ui::headerbackgroundprefs::blurRadius());
    headerBlur->setAccessibleName(tr("Header background blur"));
    auto* headerBlurValue = new QLabel(
        tr("%1 px").arg(ui::headerbackgroundprefs::blurRadius()),
        headerBlurRow);
    m_headerBlurValue = headerBlurValue;
    headerBlurValue->setMinimumWidth(52);
    headerBlurValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    headerBlurLayout->addWidget(headerBlur, 1);
    headerBlurLayout->addWidget(headerBlurValue);
    headerBackgroundForm->addRow(tr("Blur"), headerBlurRow);
    auto* headerBlurDebounce = new QTimer(headerBackgroundGroup);
    headerBlurDebounce->setSingleShot(true);
    headerBlurDebounce->setInterval(45);
    connect(headerBlur, &QSlider::valueChanged, this,
            [headerBlurValue, headerBlurDebounce](int pixels) {
                headerBlurValue->setText(tr("%1 px").arg(pixels));
                ui::headerbackgroundprefs::setBlurRadius(pixels);
                headerBlurDebounce->start();
            });
    connect(headerBlurDebounce, &QTimer::timeout, this,
            &SettingsWindow::themeBackgroundSettingsChanged);

    auto* animateHeaderBackground = new QCheckBox(
        tr("Play GIF and video backgrounds"), headerBackgroundGroup);
    m_headerAnimate = animateHeaderBackground;
    animateHeaderBackground->setChecked(
        ui::headerbackgroundprefs::animatedBackgroundsEnabled());
    animateHeaderBackground->setToolTip(
        tr("Reduce Motion freezes animated backgrounds on a still frame."));
    connect(animateHeaderBackground, &QCheckBox::toggled, this,
            [this](bool enabled) {
                ui::headerbackgroundprefs::setAnimatedBackgroundsEnabled(
                    enabled);
                emit themeBackgroundSettingsChanged();
            });
    headerBackgroundForm->addRow(QString(), animateHeaderBackground);
    headerBackgroundColumn->addLayout(headerBackgroundForm);
    col->addWidget(headerBackgroundGroup);
    refreshHeaderBackgroundPath();

    const auto syncHeaderBackgroundEnabled =
        [headerFileRow, headerPlacement, headerVisibility, headerBlurRow,
         animateHeaderBackground](bool enabled) {
            headerFileRow->setEnabled(enabled);
            headerPlacement->setEnabled(enabled);
            headerVisibility->setEnabled(enabled);
            headerBlurRow->setEnabled(enabled);
            animateHeaderBackground->setEnabled(enabled);
        };
    syncHeaderBackgroundEnabled(enableHeaderBackground->isChecked());
    connect(enableHeaderBackground, &QCheckBox::toggled, this,
            [this, syncHeaderBackgroundEnabled](bool enabled) {
                ui::headerbackgroundprefs::setEnabled(enabled);
                syncHeaderBackgroundEnabled(enabled);
                emit themeBackgroundSettingsChanged();
            });

    col->addStretch();
    return page;
}

QWidget* SettingsWindow::buildTimelineTab() {
    auto* page = new QWidget(this);
    auto* col = new QVBoxLayout(page);
    auto* preview = new ThemePreviewWidget([] { return th(); }, page);
    preview->setObjectName(QStringLiteral("TimelineAppearancePreview"));
    col->addWidget(preview);
    connect(this, &SettingsWindow::timelineAppearanceChanged, preview, qOverload<>(&QWidget::update));
    connect(&ThemeManager::instance(), &ThemeManager::changed, preview, qOverload<>(&QWidget::update));
    auto* gridGroup = new QGroupBox(tr("Timeline grid"), page);
    auto* gridCol = new QVBoxLayout(gridGroup);
    auto* gridWidthRow = new QHBoxLayout;
    auto* gridWidthLabel = new QLabel(tr("Line thickness"), gridGroup);
    m_gridWidth = new ui::GlassSlider(Qt::Horizontal, gridGroup);
    m_gridWidth->setObjectName(QStringLiteral("TimelineGridWidth"));
    m_gridWidth->setAccessibleName(tr("Grid line thickness in pixels"));
    m_gridWidth->setRange(int(std::lround(ui::kGridLineWidthMin * 10.0)),
                         int(std::lround(ui::kGridLineWidthMax * 10.0)));
    m_gridWidth->setPageStep(5);
    m_gridWidth->setValue(int(std::lround(ui::gridLineWidth() * 10.0)));
    gridWidthLabel->setBuddy(m_gridWidth);
    m_gridWidthValue = new QLabel(gridGroup);
    m_gridWidthValue->setMinimumWidth(48);
    m_gridWidthValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const auto showGridWidth = [this] {
        m_gridWidthValue->setText(tr("%1 px").arg(ui::gridLineWidth(), 0, 'f', 1));
    };
    showGridWidth();
    gridWidthRow->addWidget(gridWidthLabel);
    gridWidthRow->addWidget(m_gridWidth, 1);
    gridWidthRow->addWidget(m_gridWidthValue);
    gridCol->addLayout(gridWidthRow);

    auto* gridOpacityRow = new QHBoxLayout;
    auto* gridOpacityLabel = new QLabel(tr("Opacity"), gridGroup);
    m_gridOpacity = new ui::GlassSlider(Qt::Horizontal, gridGroup);
    m_gridOpacity->setObjectName(QStringLiteral("TimelineGridOpacity"));
    m_gridOpacity->setAccessibleName(tr("Grid opacity in percent"));
    m_gridOpacity->setRange(0, 100);
    m_gridOpacity->setPageStep(10);
    m_gridOpacity->setValue(ui::gridOpacity());
    gridOpacityLabel->setBuddy(m_gridOpacity);
    m_gridOpacityValue = new QLabel(gridGroup);
    m_gridOpacityValue->setMinimumWidth(48);
    m_gridOpacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const auto showGridOpacity = [this] {
        m_gridOpacityValue->setText(QStringLiteral("%1%").arg(ui::gridOpacity()));
    };
    showGridOpacity();
    gridOpacityRow->addWidget(gridOpacityLabel);
    gridOpacityRow->addWidget(m_gridOpacity, 1);
    gridOpacityRow->addWidget(m_gridOpacityValue);
    gridCol->addLayout(gridOpacityRow);
    auto* gridHint = new QLabel(tr("0% hides the grid. Snapping stays unchanged."), gridGroup);
    gridHint->setWordWrap(true);
    gridCol->addWidget(gridHint);
    col->addWidget(gridGroup);

    auto* clipsGroup = new QGroupBox(tr("Timeline clips"), page);
    auto* clipsColumn = new QVBoxLayout(clipsGroup);
    auto* roundedClips = new QCheckBox(tr("Rounded clip corners"), clipsGroup);
    roundedClips->setObjectName(QStringLiteral("RoundedClipCorners"));
    roundedClips->setChecked(ui::roundedClipCorners());
    roundedClips->setToolTip(tr("Applies to audio, MIDI, automation and Pattern clips, including recording previews."));
    clipsColumn->addWidget(roundedClips);
    auto* waveformRow = new QHBoxLayout;
    auto* waveformLabel = new QLabel(tr("Waveform style"), clipsGroup);
    auto* waveformStyle = new QComboBox(clipsGroup);
    waveformStyle->setObjectName(QStringLiteral("WaveformStyle"));
    waveformStyle->setAccessibleName(tr("Waveform style"));
    waveformStyle->addItem(tr("Crisp"), int(ui::WaveformStyle::Crisp));
    waveformStyle->addItem(tr("Smooth"), int(ui::WaveformStyle::Smooth));
    waveformStyle->setCurrentIndex(waveformStyle->findData(int(ui::waveformStyle())));
    waveformStyle->setToolTip(tr("Crisp keeps visible steps; Smooth draws a soft, continuous outline. Applies to the timeline, Warp and audio previews."));
    waveformLabel->setBuddy(waveformStyle);
    waveformRow->addWidget(waveformLabel);
    waveformRow->addWidget(waveformStyle, 1);
    clipsColumn->addLayout(waveformRow);
    connect(waveformStyle, &QComboBox::currentIndexChanged, this, [this, waveformStyle] {
        ui::setWaveformStyle(ui::WaveformStyle(waveformStyle->currentData().toInt()));
        emit timelineAppearanceChanged();
    });
    col->addWidget(clipsGroup);
    connect(roundedClips, &QCheckBox::toggled, this, [this](bool enabled) {
        ui::setRoundedClipCorners(enabled);
        emit timelineAppearanceChanged();
    });

    // The playhead. Thickness is a real preference rather than a default worth
    // defending: the same hairline that is right on a sparse arrangement is
    // invisible over dense waveforms on a high-density screen.
    auto* headGroup = new QGroupBox(tr("Playhead"), page);
    auto* headCol = new QVBoxLayout(headGroup);
    auto* widthRow = new QHBoxLayout;
    auto* widthLabel = new QLabel(tr("Line thickness"), headGroup);
    auto* widthSlider = new ui::GlassSlider(Qt::Horizontal, headGroup);
    m_playheadWidth = widthSlider;
    // Tenths of a pixel: the default is 1.6, and whole steps would take that
    // choice away.
    widthSlider->setRange(int(std::lround(ui::kPlayheadWidthMin * 10.0)),
                          int(std::lround(ui::kPlayheadWidthMax * 10.0)));
    widthSlider->setSingleStep(1);
    widthSlider->setPageStep(5);
    widthSlider->setValue(int(std::lround(ui::playheadWidth() * 10.0)));
    widthSlider->setAccessibleName(tr("Playhead line thickness in pixels"));
    auto* widthValue = new QLabel(headGroup);
    m_playheadWidthValue = widthValue;
    widthValue->setMinimumWidth(48);
    widthValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const auto showWidth = [widthValue](double pixels) {
        widthValue->setText(tr("%1 px").arg(pixels, 0, 'f', 1));
    };
    showWidth(ui::playheadWidth());
    widthRow->addWidget(widthLabel);
    widthRow->addWidget(widthSlider, 1);
    widthRow->addWidget(widthValue);
    headCol->addLayout(widthRow);

    auto* trail = new QCheckBox(tr("Leave a glowing trail while it moves"),
                                headGroup);
    m_playheadTrail = trail;
    trail->setChecked(ui::playheadTrail());
    trail->setAccessibleName(tr("Playhead motion trail"));
    headCol->addWidget(trail);
    col->addWidget(headGroup);

    // How a selected track is washed. It belongs beside the palette because it
    // is a palette decision, and it is a real choice rather than a default:
    // the track's own colour tells you *which* track at a glance, while a
    // neutral wash never argues with the colour it is sitting on.
    auto* tintGroup = new QGroupBox(tr("Selected tracks"), page);
    auto* tintCol = new QVBoxLayout(tintGroup);
    auto* byColour = new QRadioButton(tr("Tint with the track's own colour"),
                                      tintGroup);
    auto* neutral = new QRadioButton(tr("Tint with a neutral wash"), tintGroup);
    m_trackColourTint = byColour;
    m_neutralTint = neutral;
    (ui::selectionTint() == ui::SelectionTint::Neutral ? neutral : byColour)
        ->setChecked(true);
    const auto storeTint = [this](ui::SelectionTint tint) {
        ui::setSelectionTint(tint);
        // Nothing owns "the selection colour" as a widget property; every
        // surface reads it while painting. A theme change is the signal they
        // all already listen to, so it is the honest way to say "repaint".
        ThemeManager::instance().apply();
        emit selectionTintChanged();
    };
    connect(byColour, &QRadioButton::toggled, this, [storeTint](bool on) {
        if (on) storeTint(ui::SelectionTint::TrackColour);
    });
    connect(neutral, &QRadioButton::toggled, this, [storeTint](bool on) {
        if (on) storeTint(ui::SelectionTint::Neutral);
    });
    tintCol->addWidget(byColour);
    tintCol->addWidget(neutral);
    col->addWidget(tintGroup);


    // Repaint the arrangement without resetting application styles on every
    // slider step. Its retained grid geometry must be invalidated as well.
    const auto repaintSurfaces = [this] {
        if (m_themePreview) m_themePreview->update();
        emit timelineAppearanceChanged();
    };
    connect(m_gridWidth, &QSlider::valueChanged, this,
            [showGridWidth, repaintSurfaces](int tenths) {
                ui::setGridLineWidth(double(tenths) / 10.0);
                showGridWidth();
                repaintSurfaces();
            });
    connect(m_gridOpacity, &QSlider::valueChanged, this,
            [showGridOpacity, repaintSurfaces](int percent) {
                ui::setGridOpacity(percent);
                showGridOpacity();
                repaintSurfaces();
            });
    connect(widthSlider, &QSlider::valueChanged, this,
            [showWidth, repaintSurfaces](int tenths) {
                const double pixels = double(tenths) / 10.0;
                ui::setPlayheadWidth(pixels);
                showWidth(ui::playheadWidth());
                repaintSurfaces();
            });
    connect(trail, &QCheckBox::toggled, this,
            [repaintSurfaces](bool on) {
                ui::setPlayheadTrail(on);
                repaintSurfaces();
            });
    col->addStretch();
    return page;
}

void SettingsWindow::refreshTrackIcons() {
    if (!m_trackIconList) return;
    const QString selected = m_trackIconList->currentItem()
        ? m_trackIconList->currentItem()->data(Qt::UserRole).toString() : QString();
    m_trackIconList->clear();
    for (const auto& entry : ui::trackicons::customIcons()) {
        const QIcon image = ui::trackicons::icon(entry.id, th().textPrimary);
        auto* item = new QListWidgetItem(image.isNull() ? icons::icon(icons::Glyph::Image, th().textSecondary) : image,
                                        entry.name, m_trackIconList);
        item->setData(Qt::UserRole, entry.id);
        item->setToolTip(entry.name);
        if (entry.id == selected) m_trackIconList->setCurrentItem(item);
    }
}

QString SettingsWindow::importTrackIconFile() {
    showTab(kFontsTab);
    if (auto* scroll = qobject_cast<QScrollArea*>(m_pages->widget(kFontsTab)))
        scroll->ensureWidgetVisible(m_trackIconGroup, 0, 20);
    const QString path = QFileDialog::getOpenFileName(this, tr("Add track icon"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.svg *.ico);;All files (*)"));
    if (path.isEmpty()) return {};
    QString error;
    const QString id = ui::trackicons::importFile(path, &error);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Track Icons"), error);
        return {};
    }
    for (int i = 0; i < m_trackIconList->count(); ++i)
        if (m_trackIconList->item(i)->data(Qt::UserRole).toString() == id) {
            m_trackIconList->setCurrentRow(i);
            m_trackIconList->scrollToItem(m_trackIconList->item(i));
            break;
        }
    return id;
}

void SettingsWindow::refreshFontStatus() {
    if (!m_fontStatus || !m_resetFont) return;
    const ThemeManager& manager = ThemeManager::instance();
    if (manager.hasCustomFont()) {
        m_fontStatus->setText(
            tr("Custom font: %1 (%2)")
                .arg(manager.customFontFamily(), manager.customFontFileName()));
    } else {
        m_fontStatus->setText(
            tr("Default font: %1").arg(manager.defaultFontFamily()));
    }
    m_resetFont->setEnabled(manager.hasCustomFont());
}

void SettingsWindow::refreshThemeLibrary() {
    if (!m_savedThemeList) return;
    const QString selectedPath = m_savedThemeList->currentItem()
        ? m_savedThemeList->currentItem()->data(Qt::UserRole).toString()
        : QString();
    const QString activePackage =
        QSettings().value(QStringLiteral("ui/activeThemePackage")).toString();
    const QVector<ui::ThemeLibraryEntry> entries =
        ui::ThemePackage::libraryEntries();
    QHash<QString, int> nameCounts;
    for (const auto& entry : entries) ++nameCounts[entry.name.toCaseFolded()];

    QSignalBlocker blocker(m_savedThemeList);
    m_savedThemeList->clear();
    int selectedRow = -1;
    for (const ui::ThemeLibraryEntry& entry : entries) {
        QString label = entry.name;
        if (nameCounts.value(entry.name.toCaseFolded()) > 1)
            label += QStringLiteral(" · %1").arg(entry.packageId.left(8));
        auto* item = new QListWidgetItem(label, m_savedThemeList);
        item->setData(Qt::UserRole, entry.filePath);
        item->setData(Qt::UserRole + 1, entry.packageId);
        item->setToolTip(QDir::toNativeSeparators(entry.filePath));
        if ((!selectedPath.isEmpty() && entry.filePath == selectedPath) ||
            (selectedPath.isEmpty() && entry.packageId == activePackage))
            selectedRow = m_savedThemeList->count() - 1;
    }
    m_savedThemeList->setCurrentRow(selectedRow);
    if (m_applySavedTheme)
        m_applySavedTheme->setEnabled(selectedRow >= 0);
}

void SettingsWindow::refreshThemeControls() {
    refreshTimelineBackgroundSource();
    refreshFontStatus();

    const auto setCheck = [](QCheckBox* box, bool checked) {
        if (!box) return;
        const QSignalBlocker blocker(box);
        box->setChecked(checked);
    };
    const auto setSlider = [](QSlider* slider, int value) {
        if (!slider) return;
        const QSignalBlocker blocker(slider);
        slider->setValue(value);
    };
    const auto setCombo = [](QComboBox* combo, int value) {
        if (!combo) return;
        const QSignalBlocker blocker(combo);
        const int index = combo->findData(value);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    };

    setCheck(m_enableTimelineBackground,
             ui::timelinebackgroundprefs::enabled());
    setCombo(m_timelinePlacement,
             int(ui::timelinebackgroundprefs::placement()));
    setSlider(m_timelineVisibility,
              ui::timelinebackgroundprefs::visibility());
    setSlider(m_timelineBlur, ui::timelinebackgroundprefs::blurRadius());
    setCheck(m_timelineAnimate,
             ui::timelinebackgroundprefs::animatedBackgroundsEnabled());
    if (m_timelineVisibilityValue)
        m_timelineVisibilityValue->setText(
            QStringLiteral("%1%").arg(ui::timelinebackgroundprefs::visibility()));
    if (m_timelineBlurValue)
        m_timelineBlurValue->setText(
            tr("%1 px").arg(ui::timelinebackgroundprefs::blurRadius()));
    const bool timelineEnabled = ui::timelinebackgroundprefs::enabled();
    for (QWidget* widget : QList<QWidget*>{
             m_timelineFileRow, m_timelinePlacement, m_timelineVisibility,
             m_timelineBlurRow, m_timelineAnimate})
        if (widget) widget->setEnabled(timelineEnabled);

    const QString headerPath = ui::headerbackgroundprefs::path();
    if (m_headerBackgroundPath) {
        m_headerBackgroundPath->setText(QDir::toNativeSeparators(headerPath));
        m_headerBackgroundPath->setToolTip(headerPath);
    }
    if (m_clearHeaderBackground)
        m_clearHeaderBackground->setEnabled(!headerPath.isEmpty());
    setCheck(m_enableHeaderBackground, ui::headerbackgroundprefs::enabled());
    setCombo(m_headerPlacement, int(ui::headerbackgroundprefs::placement()));
    setSlider(m_headerVisibility, ui::headerbackgroundprefs::visibility());
    setSlider(m_headerBlur, ui::headerbackgroundprefs::blurRadius());
    setCheck(m_headerAnimate,
             ui::headerbackgroundprefs::animatedBackgroundsEnabled());
    if (m_headerVisibilityValue)
        m_headerVisibilityValue->setText(
            QStringLiteral("%1%").arg(ui::headerbackgroundprefs::visibility()));
    if (m_headerBlurValue)
        m_headerBlurValue->setText(
            tr("%1 px").arg(ui::headerbackgroundprefs::blurRadius()));
    const bool headerEnabled = ui::headerbackgroundprefs::enabled();
    for (QWidget* widget : QList<QWidget*>{
             m_headerFileRow, m_headerPlacement, m_headerVisibility,
             m_headerBlurRow, m_headerAnimate})
        if (widget) widget->setEnabled(headerEnabled);

    setSlider(m_gridWidth, int(std::lround(ui::gridLineWidth() * 10.0)));
    setSlider(m_gridOpacity, ui::gridOpacity());
    if (m_gridWidthValue)
        m_gridWidthValue->setText(tr("%1 px").arg(ui::gridLineWidth(), 0, 'f', 1));
    if (m_gridOpacityValue)
        m_gridOpacityValue->setText(QStringLiteral("%1%").arg(ui::gridOpacity()));
    setSlider(m_playheadWidth,
              int(std::lround(ui::playheadWidth() * 10.0)));
    setCheck(m_playheadTrail, ui::playheadTrail());
    if (m_playheadWidthValue)
        m_playheadWidthValue->setText(
            tr("%1 px").arg(ui::playheadWidth(), 0, 'f', 1));
    if (m_trackColourTint && m_neutralTint) {
        const QSignalBlocker trackBlocker(m_trackColourTint);
        const QSignalBlocker neutralBlocker(m_neutralTint);
        const bool neutral =
            ui::selectionTint() == ui::SelectionTint::Neutral;
        m_trackColourTint->setChecked(!neutral);
        m_neutralTint->setChecked(neutral);
    }
}

bool SettingsWindow::applyInstalledTheme(const QString& filePath,
                                         const QString& storageId) {
    m_applyingInstalledTheme = true;
    const ui::ThemePackageResult result =
        ui::ThemePackage::apply(filePath, storageId);
    if (!result.ok) {
        m_applyingInstalledTheme = false;
        QMessageBox::warning(this, tr("Theme could not be applied"), result.error);
        return false;
    }
    m_editTheme = ThemeManager::instance().theme();
    if (m_themeNameEdit) m_themeNameEdit->setText(m_editTheme.name);
    refreshSwatches();
    refreshThemeControls();
    if (m_notebookPage) m_notebookPage->refresh();
    refreshThemeLibrary();
    emit themeBackgroundSettingsChanged();
    emit notebookSettingsChanged();
    emit selectionTintChanged();
    m_applyingInstalledTheme = false;
    return true;
}

void SettingsWindow::saveCurrentThemeToLibrary() {
    QString name;
    if (m_pages && m_pages->currentIndex() == kThemeEditorTab &&
        m_themeNameEdit) {
        name = m_themeNameEdit->text().trimmed();
        if (name.isEmpty()) {
            m_themeNameEdit->setFocus();
            return;
        }
    } else {
        bool accepted = false;
        const QString initial = ThemeManager::instance().theme().name.isEmpty()
            ? tr("Custom Theme") : ThemeManager::instance().theme().name;
        name = QInputDialog::getText(
            this, tr("Save Theme"), tr("Theme name"), QLineEdit::Normal,
            initial, &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
    }

    ui::ThemePackageSnapshot snapshot;
    QString error;
    if (!ui::ThemePackage::captureCurrent(name, snapshot, &error)) {
        QMessageBox::warning(this, tr("Theme could not be saved"), error);
        return;
    }
    runThemeTask(
        this, tr("Saving theme…"),
        [snapshot](const ui::ThemePackageProgress& progress) {
            return ui::ThemePackage::saveToLibrary(snapshot, progress);
        },
        [this](const ui::ThemePackageResult& result) {
            if (result.cancelled) return;
            if (!result.ok) {
                QMessageBox::warning(this, tr("Theme could not be saved"),
                                     result.error);
                return;
            }
            QSettings().setValue(
                QStringLiteral("ui/activeThemePackage"),
                result.manifest.value(QStringLiteral("packageId")).toString());
            refreshThemeLibrary();
            QMessageBox::information(this, tr("Theme Saved"),
                                     tr("The theme is now in your VLTONE library."));
        });
}

void SettingsWindow::exportCurrentTheme() {
    const QString baseName = ThemeManager::instance().theme().name.isEmpty()
        ? tr("Custom Theme") : ThemeManager::instance().theme().name;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Theme"), baseName + QStringLiteral(".vlttheme"),
        tr("VLTONE Theme (*.vlttheme)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".vlttheme"), Qt::CaseInsensitive))
        path += QStringLiteral(".vlttheme");

    ui::ThemePackageSnapshot snapshot;
    QString error;
    if (!ui::ThemePackage::captureCurrent(baseName, snapshot, &error)) {
        QMessageBox::warning(this, tr("Theme could not be exported"), error);
        return;
    }
    runThemeTask(
        this, tr("Exporting theme…"),
        [snapshot, path](const ui::ThemePackageProgress& progress) {
            return ui::ThemePackage::write(snapshot, path, progress);
        },
        [this](const ui::ThemePackageResult& result) {
            if (result.cancelled) return;
            if (!result.ok) {
                QMessageBox::warning(this, tr("Theme could not be exported"),
                                     result.error);
                return;
            }
            QMessageBox::information(
                this, tr("Theme Exported"),
                tr("The .vlttheme file includes the theme's media and fonts."));
        });
}

void SettingsWindow::applySelectedSavedTheme() {
    if (!m_savedThemeList || !m_savedThemeList->currentItem()) return;
    const QString path =
        m_savedThemeList->currentItem()->data(Qt::UserRole).toString();
    runThemeTask(
        this, tr("Preparing theme…"),
        [path](const ui::ThemePackageProgress& progress) {
            return ui::ThemePackage::install(path, progress);
        },
        [this](const ui::ThemePackageResult& result) {
            if (result.cancelled) return;
            if (!result.ok) {
                QMessageBox::warning(this, tr("Theme could not be applied"),
                                     result.error);
                return;
            }
            applyInstalledTheme(result.filePath, result.storageId);
        });
}

void SettingsWindow::importThemeFile(const QString& path) {
    const QFileInfo info(path);
    if (info.suffix().compare(QStringLiteral("json"),
                              Qt::CaseInsensitive) == 0) {
        QFile file(path);
        const QJsonDocument document = file.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
        if (!document.isObject()) {
            QMessageBox::warning(this, tr("Import failed"),
                                 tr("%1 is not a valid legacy theme file.")
                                     .arg(QDir::toNativeSeparators(path)));
            return;
        }
        m_editTheme = ThemeManager::fromJson(
            document.object(), ThemeManager::instance().theme());
        ThemeManager::instance().applyCustomTheme(m_editTheme, true);
        QSettings().remove(QStringLiteral("ui/activeThemePackage"));
        if (m_themeNameEdit) m_themeNameEdit->setText(m_editTheme.name);
        refreshSwatches();
        refreshThemeLibrary();
        return;
    }

    runThemeTask(
        this, tr("Importing theme…"),
        [path](const ui::ThemePackageProgress& progress) {
            return ui::ThemePackage::install(path, progress);
        },
        [this](const ui::ThemePackageResult& result) {
            if (result.cancelled) return;
            if (!result.ok) {
                QMessageBox::warning(this, tr("Import failed"), result.error);
                return;
            }
            if (applyInstalledTheme(result.filePath, result.storageId)) {
                QMessageBox::information(
                    this, tr("Theme Imported"),
                    tr("The theme and its resources are now stored inside VLTONE. "
                       "You can delete the original .vlttheme file."));
            }
        });
}

QWidget* SettingsWindow::buildThemeEditorTab() {
    m_editTheme = ThemeManager::instance().theme();   // start from the active one
    if (m_editTheme.id != QLatin1String("custom"))
        m_editTheme.name = tr("My Theme");

    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("ThemeEditorPage"));
    auto* col = new QVBoxLayout(page);
    col->setSpacing(12);

    auto* title = new QLabel(tr("Create your theme"), page);
    title->setProperty("role", "pageTitle");
    col->addWidget(title);
    auto* introduction = new QLabel(
        tr("Start with a familiar palette, then make it yours. The preview "
           "updates as you choose colours, and accepted changes appear "
           "throughout VLTONE immediately."),
        page);
    introduction->setProperty("role", "secondary");
    introduction->setWordWrap(true);
    col->addWidget(introduction);

    auto* detailsGroup = new QGroupBox(tr("Theme details"), page);
    auto* detailsForm = new QFormLayout(detailsGroup);
    detailsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    detailsForm->setHorizontalSpacing(14);
    detailsForm->setVerticalSpacing(9);

    m_themeNameEdit = new QLineEdit(
        m_editTheme.name.isEmpty() ? tr("My Theme") : m_editTheme.name,
        detailsGroup);
    m_themeNameEdit->setObjectName(QStringLiteral("ThemeEditorName"));
    m_themeNameEdit->setAccessibleName(tr("Theme name"));
    m_themeNameEdit->setPlaceholderText(tr("My studio theme"));
    m_themeNameEdit->setMaxLength(80);
    detailsForm->addRow(tr("Name"), m_themeNameEdit);

    auto* starterRow = new QWidget(detailsGroup);
    auto* starterLayout = new QHBoxLayout(starterRow);
    starterLayout->setContentsMargins(0, 0, 0, 0);
    starterLayout->setSpacing(8);
    auto* starter = new QComboBox(starterRow);
    starter->setObjectName(QStringLiteral("ThemeEditorStarter"));
    starter->setAccessibleName(tr("Starting palette"));
    int currentPreset = -1;
    for (const Theme& preset : ThemeManager::instance().presets()) {
        starter->addItem(presetDisplayName(preset), preset.id);
        if (preset.id == ThemeManager::instance().themeId())
            currentPreset = starter->count() - 1;
    }
    if (currentPreset >= 0) starter->setCurrentIndex(currentPreset);
    auto* useStarter = new QPushButton(tr("Use palette"), starterRow);
    useStarter->setToolTip(tr("Replace the colours below with this palette"));
    useStarter->setEnabled(starter->count() > 0);
    starterLayout->addWidget(starter, 1);
    starterLayout->addWidget(useStarter);
    detailsForm->addRow(tr("Start from"), starterRow);

    auto* starterHint = new QLabel(
        tr("Your current appearance is already loaded. A starter palette "
           "replaces only the colours; your theme name stays the same."),
        detailsGroup);
    starterHint->setProperty("role", "secondary");
    starterHint->setWordWrap(true);
    detailsForm->addRow(QString(), starterHint);
    col->addWidget(detailsGroup);

    auto* previewGroup = new QGroupBox(tr("Live preview"), page);
    auto* previewColumn = new QVBoxLayout(previewGroup);
    auto* previewHeader = new QHBoxLayout;
    auto* previewHint = new QLabel(
        tr("Check surfaces, text, clips, grid lines and the playhead together."),
        previewGroup);
    previewHint->setProperty("role", "secondary");
    previewHint->setWordWrap(true);
    m_themeLiveStatus = new QLabel(tr("● Applied live"), previewGroup);
    m_themeLiveStatus->setAccessibleName(tr("Theme changes are applied live"));
    previewHeader->addWidget(previewHint, 1);
    previewHeader->addWidget(m_themeLiveStatus, 0, Qt::AlignTop);
    previewColumn->addLayout(previewHeader);
    m_themePreview = new ThemePreviewWidget([this] { return m_editTheme; },
                                            previewGroup);
    m_themePreview->setObjectName(QStringLiteral("ThemeEditorPreview"));
    previewColumn->addWidget(m_themePreview);
    m_themeContrastStatus = new QLabel(previewGroup);
    m_themeContrastStatus->setObjectName(QStringLiteral("ThemeContrastStatus"));
    m_themeContrastStatus->setWordWrap(true);
    previewColumn->addWidget(m_themeContrastStatus);
    col->addWidget(previewGroup);

    struct SectionInfo {
        ColorSection section;
        const char* title;
        const char* hint;
    };
    const SectionInfo sections[] = {
        {ColorSection::Surfaces,
         QT_TRANSLATE_NOOP("SettingsWindow", "Surfaces"),
         QT_TRANSLATE_NOOP("SettingsWindow", "Window, panels and toolbars.")},
        {ColorSection::Typography,
         QT_TRANSLATE_NOOP("SettingsWindow", "Text"),
         QT_TRANSLATE_NOOP("SettingsWindow", "Primary and supporting labels.")},
        {ColorSection::Accent,
         QT_TRANSLATE_NOOP("SettingsWindow", "Accent & selection"),
         QT_TRANSLATE_NOOP("SettingsWindow", "Actions, highlights and selected content.")},
        {ColorSection::Timeline,
         QT_TRANSLATE_NOOP("SettingsWindow", "Timeline"),
         QT_TRANSLATE_NOOP("SettingsWindow", "Waveforms, playhead and grid lines.")},
    };

    m_swatches.clear();
    for (const SectionInfo& section : sections) {
        auto* group = new QGroupBox(
            QCoreApplication::translate("SettingsWindow", section.title), page);
        auto* groupColumn = new QVBoxLayout(group);
        auto* hint = new QLabel(
            QCoreApplication::translate("SettingsWindow", section.hint), group);
        hint->setProperty("role", "secondary");
        hint->setWordWrap(true);
        groupColumn->addWidget(hint);

        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 2, 0, 0);
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(7);
        int sectionIndex = 0;
        for (const ColorField& field : colorFields()) {
            if (field.section != section.section) continue;
            const QString fieldName = QCoreApplication::translate(
                "SettingsWindow", field.label);
            auto* row = new QWidget(group);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            auto* label = new QLabel(fieldName, row);
            auto* swatch = new QPushButton(row);
            swatch->setObjectName(QStringLiteral("ThemeColour_%1").arg(field.key));
            swatch->setMinimumSize(122, 30);
            swatch->setCursor(Qt::PointingHandCursor);
            swatch->setAccessibleName(tr("Choose %1 colour").arg(fieldName));
            swatch->setToolTip(tr("Choose %1 colour").arg(fieldName));
            rowLayout->addWidget(label, 1);
            rowLayout->addWidget(swatch);
            m_swatches.insert(field.key, swatch);

            QColor Theme::* member = field.member;
            connect(swatch, &QPushButton::clicked, this, [this, member] {
                const QColor start = m_editTheme.*member;
                QColorDialog dialog(start, this);
                dialog.setWindowTitle(tr("Choose colour"));
                dialog.setOption(QColorDialog::ShowAlphaChannel);
                connect(&dialog, &QColorDialog::currentColorChanged, this,
                        [this, member](const QColor& colour) {
                            if (!colour.isValid()) return;
                            m_editTheme.*member = colour;
                            refreshSwatches();
                        });
                if (dialog.exec() == QDialog::Accepted) {
                    m_editTheme.*member = dialog.selectedColor();
                    applyEditTheme();
                } else {
                    m_editTheme.*member = start;
                    refreshSwatches();
                }
            });

            grid->addWidget(row, sectionIndex / 2, sectionIndex % 2);
            ++sectionIndex;
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        groupColumn->addLayout(grid);
        col->addWidget(group);
    }

    connect(m_themeNameEdit, &QLineEdit::textEdited, this,
            [this](const QString& text) {
                m_editTheme.name = text.trimmed();
                if (m_themeSaveButton)
                    m_themeSaveButton->setEnabled(!m_editTheme.name.isEmpty());
            });
    connect(useStarter, &QPushButton::clicked, this, [this, starter] {
        const QString presetId = starter->currentData().toString();
        const auto& presets = ThemeManager::instance().presets();
        const auto found = std::find_if(
            presets.cbegin(), presets.cend(),
            [&presetId](const Theme& preset) { return preset.id == presetId; });
        if (found == presets.cend()) return;
        const QString name = m_themeNameEdit->text().trimmed();
        m_editTheme = *found;
        m_editTheme.name = name.isEmpty() ? tr("Custom") : name;
        m_themeNameEdit->setText(m_editTheme.name);
        applyEditTheme();
    });

    // Import/export are supporting actions. Saving is the clear completion of
    // the editor flow and therefore gets the single accented button.
    auto* exportBtn = new QPushButton(tr("Export…"));
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        applyEditTheme();
        exportCurrentTheme();
    });
    auto* importBtn = new QPushButton(tr("Import…"));
    connect(importBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Theme"), QString(),
            tr("VLTONE Theme (*.vlttheme);;Legacy Theme (*.json *.dawtheme.json);;All Files (*)"));
        if (!path.isEmpty()) importThemeFile(path);
    });
    m_themeSaveButton = new QPushButton(tr("Save to Library"));
    m_themeSaveButton->setObjectName(QStringLiteral("ThemeEditorSave"));
    m_themeSaveButton->setProperty("accentAction", true);
    m_themeSaveButton->setAccessibleDescription(
        tr("Save this theme so it can be applied again later"));
    m_themeSaveButton->setEnabled(!m_themeNameEdit->text().trimmed().isEmpty());
    connect(m_themeSaveButton, &QPushButton::clicked, this, [this] {
        m_editTheme.name = m_themeNameEdit->text().trimmed();
        applyEditTheme();
        saveCurrentThemeToLibrary();
    });

    auto* actions = new QHBoxLayout;
    actions->addWidget(importBtn);
    actions->addWidget(exportBtn);
    actions->addStretch(1);
    actions->addWidget(m_themeSaveButton);
    col->addLayout(actions);
    col->addStretch();

    refreshSwatches();
    return page;
}

void SettingsWindow::applyEditTheme() {
    ThemeManager::instance().applyCustomTheme(m_editTheme, /*persist=*/true);
    QSettings().remove(QStringLiteral("ui/activeThemePackage"));
    refreshThemeLibrary();
    refreshSwatches();
    // Keep the preset list from highlighting a stale row now that "custom" won.
    if (m_themeList) {
        QSignalBlocker block(m_themeList);
        m_themeList->setCurrentRow(-1);
    }
}

void SettingsWindow::refreshSwatches() {
    for (const ColorField& f : colorFields()) {
        auto* swatch = m_swatches.value(f.key);
        if (!swatch) continue;
        const QColor c = m_editTheme.*(f.member);
        QPixmap sample(24, 16);
        sample.fill(Qt::transparent);
        QPainter painter(&sample);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(mixColors(c, m_editTheme.textPrimary, 0.35), 1.0));
        painter.setBrush(c);
        painter.drawRoundedRect(QRectF(0.5, 0.5, 23.0, 15.0), 4.0, 4.0);
        swatch->setIcon(QIcon(sample));
        swatch->setIconSize(sample.size());
        const QString hex = c.name(QColor::HexRgb).toUpper();
        swatch->setText(c.alpha() == 255
                            ? hex
                            : tr("%1 · %2%").arg(hex).arg(c.alphaF() * 100.0,
                                                        0, 'f', 0));
        swatch->setAccessibleDescription(
            tr("Current colour %1").arg(swatch->text()));
    }

    if (m_themePreview) m_themePreview->update();
    if (m_themeLiveStatus) {
        m_themeLiveStatus->setStyleSheet(
            QStringLiteral("color: %1; font-weight: 600;")
                .arg(m_editTheme.accentHighlight.name(QColor::HexArgb)));
    }
    if (m_themeContrastStatus) {
        const double weakest = std::min(
            {contrastRatio(m_editTheme.textPrimary, m_editTheme.background),
             contrastRatio(m_editTheme.textPrimary, m_editTheme.surface),
             contrastRatio(m_editTheme.textSecondary, m_editTheme.background),
             contrastRatio(m_editTheme.textSecondary, m_editTheme.surface),
             contrastRatio(m_editTheme.textPrimary, m_editTheme.controlTop()),
             contrastRatio(m_editTheme.textPrimary, m_editTheme.controlBottom()),
             contrastRatio(m_editTheme.textSecondary, m_editTheme.panelTop()),
             contrastRatio(m_editTheme.textSecondary, m_editTheme.panelBottom()),
             contrastRatio(m_editTheme.textSecondary, m_editTheme.wellTop()),
             contrastRatio(m_editTheme.textSecondary, m_editTheme.wellBottom()),
             contrastRatio(m_editTheme.accentText(), m_editTheme.accent)});
        if (weakest >= 4.5) {
            m_themeContrastStatus->setText(
                tr("✓ Text contrast looks good — weakest pairing is %1:1.")
                    .arg(weakest, 0, 'f', 2));
        } else {
            m_themeContrastStatus->setText(
                tr("⚠ Low text contrast — weakest pairing is %1:1. Aim for "
                   "4.5:1 or higher.")
                    .arg(weakest, 0, 'f', 2));
        }
    }
    if (m_themeSaveButton && m_themeNameEdit)
        m_themeSaveButton->setEnabled(!m_themeNameEdit->text().trimmed().isEmpty());
}

QWidget* SettingsWindow::buildShortcutsTab() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->addWidget(new QLabel(
        tr("Click a shortcut field and press the new keys. If a key is already "
           "used you'll be asked before reassigning it. Shortcuts follow the "
           "physical key, so they work in every keyboard layout. The same "
           "binding is shown as Command on macOS and Ctrl on Windows/Linux.")));

    auto* filter = new QLineEdit(page);
    filter->setObjectName(QStringLiteral("ShortcutFilter"));
    filter->setAccessibleName(tr("Search keyboard shortcuts"));
    filter->setPlaceholderText(tr("Find a command or category…"));
    filter->setClearButtonEnabled(true);
    col->addWidget(filter);
    auto* table = new QTableWidget(page);
    table->setObjectName(QStringLiteral("ShortcutTable"));
    table->setMinimumHeight(300);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels(
        {tr("Command"), tr("Category"), tr("Shortcut")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    auto commands = m_shortcuts->commands();   // copy so we can sort for display
    std::sort(commands.begin(), commands.end(),
              [](const ShortcutManager::Command& a,
                 const ShortcutManager::Command& b) {
                  if (a.category != b.category) return a.category < b.category;
                  return a.label < b.label;
              });

    table->setRowCount(int(commands.size()));
    for (int r = 0; r < int(commands.size()); ++r) {
        const ShortcutManager::Command& c = commands[r];
        table->setItem(r, 0, new QTableWidgetItem(c.label));
        table->setItem(r, 1, new QTableWidgetItem(c.category));

        auto* edit = new QKeySequenceEdit(m_shortcuts->shortcut(c.id));
        edit->setMaximumSequenceLength(1);
        m_editors.insert(c.id, edit);
        const QString id = c.id;
        connect(edit, &QKeySequenceEdit::editingFinished, this, [this, id, edit] {
            const QKeySequence seq =
                m_shortcuts->canonicalShortcut(edit->keySequence());
            if (edit->keySequence() != seq) {
                QSignalBlocker block(edit);
                edit->setKeySequence(seq);
            }
            if (seq == m_shortcuts->shortcut(id)) return;   // no real change

            const QString other = m_shortcuts->conflict(id, seq);
            if (!other.isEmpty()) {
                const auto res = QMessageBox::question(
                    this, tr("Shortcut in use"),
                    tr("\"%1\" is already assigned to \"%2\".\n\n"
                       "Reassign it to \"%3\"?")
                        .arg(seq.toString(QKeySequence::NativeText),
                             m_shortcuts->label(other), m_shortcuts->label(id)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (res != QMessageBox::Yes) {
                    QSignalBlocker block(edit);
                    edit->setKeySequence(m_shortcuts->shortcut(id));
                    return;
                }
                m_shortcuts->setShortcut(other, QKeySequence());
                if (auto* e = m_editors.value(other)) {
                    QSignalBlocker block(e);
                    e->setKeySequence(QKeySequence());
                }
            }
            m_shortcuts->setShortcut(id, seq);
            // A Cyrillic key captured by QKeySequenceEdit is stored as its
            // physical Latin position. Reflect that canonical form at once so
            // the field and the menu show the same cross-layout binding.
            QSignalBlocker block(edit);
            edit->setKeySequence(m_shortcuts->shortcut(id));
        });
        table->setCellWidget(r, 2, edit);
    }

    connect(filter, &QLineEdit::textChanged, table, [table](const QString& query) {
        const QString needle = query.trimmed();
        for (int row = 0; row < table->rowCount(); ++row)
            table->setRowHidden(row, !table->item(row, 0)->text().contains(needle, Qt::CaseInsensitive) &&
                                    !table->item(row, 1)->text().contains(needle, Qt::CaseInsensitive));
    });
    auto* resetButton = new QPushButton(tr("Reset All to Defaults"));
    connect(resetButton, &QPushButton::clicked, this, [this] {
        for (auto it = m_editors.constBegin(); it != m_editors.constEnd(); ++it)
            m_shortcuts->resetToDefault(it.key());
        refreshShortcutEditors();
    });
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(resetButton);

    col->addWidget(table, 1);
    col->addLayout(buttonRow);
    return page;
}

void SettingsWindow::refreshShortcutEditors() {
    for (auto it = m_editors.constBegin(); it != m_editors.constEnd(); ++it) {
        QSignalBlocker block(it.value());
        it.value()->setKeySequence(m_shortcuts->shortcut(it.key()));
    }
}

QWidget* SettingsWindow::buildMixerTab() {
    auto& preferences = ui::MixerPreferences::instance();
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* group = new QGroupBox(tr("Channel strips"), page);
    auto* form = new QFormLayout(group);
    auto* view = new QComboBox(group);
    view->setObjectName(QStringLiteral("MixerView"));
    view->setAccessibleName(tr("Mixer view"));
    view->addItem(tr("Classic", "mixer view"));
    view->addItem(tr("Compact"));
    view->setCurrentIndex(preferences.view() == ui::MixerPreferences::View::Compact ? 1 : 0);
    form->addRow(tr("Mixer view"), view);
    auto* compactWidth = new QComboBox(group);
    compactWidth->setObjectName(QStringLiteral("MixerCompactWidth"));
    compactWidth->setAccessibleName(tr("Compact channel width"));
    for (int width : {24, 32, 40, 48}) compactWidth->addItem(tr("%1 px").arg(width), width);
    compactWidth->setCurrentIndex(compactWidth->findData(preferences.compactWidth()));
    form->addRow(tr("Compact channel width"), compactWidth);
    auto* controls = new QWidget(group);
    auto* row = new QHBoxLayout(controls);
    row->setContentsMargins(0, 0, 0, 0);
    auto* slider = new ui::GlassSlider(Qt::Horizontal, controls);
    slider->setObjectName(QStringLiteral("MixerChannelWidthSlider"));
    slider->setAccessibleName(tr("Channel width"));
    slider->setRange(ui::MixerPreferences::kMinimumWidth, ui::MixerPreferences::kMaximumWidth);
    slider->setPageStep(5);
    auto* value = new QSpinBox(controls);
    value->setObjectName(QStringLiteral("MixerChannelWidthValue"));
    value->setAccessibleName(tr("Channel width"));
    value->setRange(slider->minimum(), slider->maximum());
    value->setSuffix(tr(" px"));
    value->setKeyboardTracking(false);
    row->addWidget(slider, 1);
    row->addWidget(value);
    form->addRow(tr("Classic channel width"), controls);
    auto* hint = new QLabel(tr("Narrow channels show more tracks. Wider channels leave more room "
                              "for names and controls. Changes apply immediately and are saved "
                              "for every project."), group);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto* reset = new QPushButton(tr("Reset to %1 px").arg(ui::MixerPreferences::kDefaultWidth), group);
    reset->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    reset->setObjectName(QStringLiteral("MixerChannelWidthReset"));
    form->addRow(reset);
    const auto reload = [slider, value, reset, &preferences](int width) {
        const QSignalBlocker a(slider), b(value);
        slider->setValue(width);
        value->setValue(width);
        reset->setEnabled(preferences.view() == ui::MixerPreferences::View::Classic &&
                          width != ui::MixerPreferences::kDefaultWidth);
    };
    reload(preferences.channelWidth());
    connect(slider, &QSlider::valueChanged, &preferences, &ui::MixerPreferences::setChannelWidth);
    connect(value, &QSpinBox::valueChanged, &preferences, &ui::MixerPreferences::setChannelWidth);
    connect(reset, &QPushButton::clicked, group, [&preferences] {
        preferences.setChannelWidth(ui::MixerPreferences::kDefaultWidth);
    });
    connect(&preferences, &ui::MixerPreferences::channelWidthChanged, page, reload);
    const auto updateView = [view, compactWidth, controls, reset, &preferences](ui::MixerPreferences::View mode) {
        const QSignalBlocker block(view);
        const bool compact = mode == ui::MixerPreferences::View::Compact;
        view->setCurrentIndex(compact ? 1 : 0);
        compactWidth->setEnabled(compact);
        controls->setEnabled(!compact);
        reset->setEnabled(!compact && preferences.channelWidth() != ui::MixerPreferences::kDefaultWidth);
    };
    updateView(preferences.view());
    connect(view, &QComboBox::currentIndexChanged, page, [&preferences](int index) {
        preferences.setView(index == 1 ? ui::MixerPreferences::View::Compact : ui::MixerPreferences::View::Classic);
    });
    connect(&preferences, &ui::MixerPreferences::viewChanged, page, updateView);
    connect(compactWidth, &QComboBox::currentIndexChanged, page, [compactWidth, &preferences] {
        preferences.setCompactWidth(compactWidth->currentData().toInt());
    });
    connect(&preferences, &ui::MixerPreferences::compactWidthChanged, page, [compactWidth](int width) {
        const QSignalBlocker block(compactWidth);
        compactWidth->setCurrentIndex(compactWidth->findData(width));
    });
    auto* detailsHint = new QLabel(tr("In Compact view, the selected channel's routing, plugins, mini modules "
                                      "and sends appear on the right. Drag the divider to resize that panel."), group);
    detailsHint->setWordWrap(true);
    form->addRow(detailsHint);
    layout->addWidget(group);

    auto* colorGroup = new QGroupBox(tr("Mini modules"), page);
    auto* colorForm = new QFormLayout(colorGroup);
    auto* colorToggle = new QCheckBox(tr("Show mini modules on channel strips"), colorGroup);
    colorToggle->setObjectName(QStringLiteral("MixerColorSectionToggle"));
    colorToggle->setAccessibleName(tr("Show mini modules on channel strips"));
    colorToggle->setChecked(preferences.colorVisible());
    colorForm->addRow(colorToggle);
    auto* colorHint = new QLabel(
        tr("Each audio channel can hold three mini modules. Hiding this section "
           "keeps its processing active. Individual cards can be collapsed "
           "independently; faders remain aligned."),
        colorGroup);
    colorHint->setWordWrap(true);
    colorForm->addRow(colorHint);
    connect(colorToggle, &QCheckBox::toggled, &preferences,
            &ui::MixerPreferences::setColorVisible);
    layout->addWidget(colorGroup);
    auto* heightHint = new QLabel(tr("Drag the top edge of the mixer to change its height. "
                                    "Faders use the available height; a short mixer scrolls "
                                    "to keep every control accessible."), page);
    heightHint->setWordWrap(true);
    layout->addWidget(heightHint);
    layout->addStretch();
    return page;
}

QWidget* SettingsWindow::buildInterfaceTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* startupGroup = new QGroupBox(tr("Startup"), page);
    auto* startupForm = new QFormLayout(startupGroup);
    m_startupTemplate = new QComboBox(startupGroup);
    m_startupTemplate->setObjectName(QStringLiteral("StartupProjectTemplate"));
    m_startupTemplate->setAccessibleName(tr("Default project at launch"));
    m_startupTemplate->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_startupTemplate->setMinimumContentsLength(24);
    refreshStartupTemplateOptions();
    connect(m_startupTemplate,
            qOverload<int>(&QComboBox::currentIndexChanged), startupGroup,
            [this] {
                ui::startupproject::setTemplatePath(
                    m_startupTemplate->currentData().toString());
            });
    startupForm->addRow(tr("Project opened at launch:"), m_startupTemplate);
    auto* startupHint = new QLabel(
        tr("Applies only when VLTONE starts normally. Opening a project, "
           "restoring work after a crash, and File > New Project take "
           "priority over this template."),
        startupGroup);
    startupHint->setWordWrap(true);
    startupForm->addRow(startupHint);
    layout->addWidget(startupGroup);

    auto* editingGroup = new QGroupBox(tr("Editing"), page);
    auto* editingLayout = new QVBoxLayout(editingGroup);
    auto* duplicateClips = new QCheckBox(
        tr("Include clips when duplicating tracks and folders"),
        editingGroup);
    duplicateClips->setObjectName(QStringLiteral("DuplicateTrackClips"));
    duplicateClips->setAccessibleName(
        tr("Include clips when duplicating tracks and folders"));
    duplicateClips->setChecked(ui::duplicateTrackClips());
    auto* duplicateHint = new QLabel(
        tr("When off, Duplicate keeps the track hierarchy, instruments, "
           "plugins, mixer settings and routing, but creates empty lanes."),
        editingGroup);
    duplicateHint->setWordWrap(true);
    connect(duplicateClips, &QCheckBox::toggled, editingGroup,
            [](bool enabled) { ui::setDuplicateTrackClips(enabled); });
    editingLayout->addWidget(duplicateClips);
    editingLayout->addWidget(duplicateHint);
    layout->addWidget(editingGroup);
    layout->addStretch();
    return page;
}

QWidget* SettingsWindow::buildPerformanceTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* group = new QGroupBox(tr("Refresh rate"), page);
    auto* form = new QFormLayout(group);
    auto* mode = new QComboBox(group);
    mode->setObjectName("UiFrameMode");
    mode->setAccessibleName(tr("Refresh rate"));
    for (int fps : {30, 60, 75, 90, 120, 144, 165, 240, 360})
        mode->addItem(tr("%1 FPS").arg(fps), fps);
    mode->addItem(tr("Custom"), -1);
    mode->addItem(tr("Follow display"), -2);
    mode->addItem(tr("Unlimited"), -3);
    auto* limit = new QSpinBox(group);
    limit->setObjectName("UiFrameLimit");
    limit->setRange(1, 1000);
    limit->setSuffix(tr(" FPS"));
    limit->setAccessibleName(tr("Custom refresh rate"));
    auto& clock = ui::FrameClock::instance();
    const auto reload = [mode, limit, form, &clock] {
        const QSignalBlocker a(mode), b(limit);
        limit->setValue(clock.limit());
        const int data = clock.mode() == ui::FrameMode::Display ? -2 :
                         clock.mode() == ui::FrameMode::Unlimited ? -3 : clock.limit();
        const int index = mode->findData(data);
        mode->setCurrentIndex(index >= 0 ? index : mode->findData(-1));
        limit->setEnabled(mode->currentData().toInt() == -1);
        if (form->rowCount() > 1) form->setRowVisible(limit, limit->isEnabled());
    };
    reload();
    form->addRow(tr("Refresh rate"), mode);
    form->addRow(tr("Custom refresh rate"), limit);
    form->setRowVisible(limit, limit->isEnabled());
    const auto apply = [mode, limit, form, &clock] {
        const int choice = mode->currentData().toInt();
        limit->setEnabled(choice == -1);
        form->setRowVisible(limit, choice == -1);
        clock.setPreference(choice == -2 ? ui::FrameMode::Display :
                            choice == -3 ? ui::FrameMode::Unlimited : ui::FrameMode::Fixed,
                            choice > 0 ? choice : limit->value());
    };
    connect(mode, &QComboBox::currentIndexChanged, page, apply);
    connect(limit, &QSpinBox::valueChanged, page, apply);
    connect(&clock, &ui::FrameClock::preferenceChanged, page, reload);
    auto* explanation = new QLabel(tr("Applies immediately to the application's visual updates. "
        "Unlimited removes the application limit; the actual rate depends on your display and system. "
        "Third-party plugin windows control their own refresh rate."), group);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    layout->addWidget(group);

    auto* motionGroup=new QGroupBox(tr("Navigation motion"),page);
    motionGroup->setObjectName("NavigationMotionSettings");
    auto* motionForm=new QFormLayout(motionGroup);
    auto* smooth=new QCheckBox(tr("Smooth scrolling"),motionGroup);
    smooth->setObjectName("SmoothScrollingEnabled");smooth->setAccessibleName(tr("Smooth scrolling"));
    auto* speed=new QSpinBox(motionGroup);speed->setRange(50,200);speed->setSuffix(tr(" %"));
    speed->setObjectName("ScrollMotionSpeed");speed->setAccessibleName(tr("Scroll animation speed"));
    auto* softness=new QSlider(Qt::Horizontal,motionGroup);softness->setRange(0,100);
    softness->setObjectName("ScrollMotionStrength");softness->setAccessibleName(tr("Scroll smoothing"));
    auto* softnessValue=new QLabel(motionGroup);softnessValue->setMinimumWidth(42);
    auto* softnessRow=new QWidget(motionGroup);auto* softnessLayout=new QHBoxLayout(softnessRow);
    softnessLayout->setContentsMargins(0,0,0,0);softnessLayout->addWidget(softness);softnessLayout->addWidget(softnessValue);
    auto* reduced=new QCheckBox(tr("Reduce motion"),motionGroup);
    reduced->setObjectName("NavigationReduceMotion");reduced->setAccessibleName(tr("Reduce motion"));
    auto& motion=ui::ScrollPreferences::instance();
    const auto reloadMotion=[smooth,speed,softness,softnessValue,reduced,&motion] {
        const QSignalBlocker a(smooth),b(speed),c(softness),d(reduced);
        smooth->setChecked(motion.enabled());speed->setValue(motion.speed());softness->setValue(motion.strength());
        softnessValue->setText(QString::number(motion.strength())+QLatin1Char('%'));
        reduced->setChecked(motion.reducedMotion());
        speed->setEnabled(motion.enabled()&&!motion.reducedMotion());
        softness->setEnabled(motion.enabled()&&!motion.reducedMotion());
    };
    reloadMotion();
    connect(smooth,&QCheckBox::toggled,page,[&motion](bool value){motion.setEnabled(value);});
    connect(speed,&QSpinBox::valueChanged,page,[&motion](int value){motion.setSpeed(value);});
    connect(softness,&QSlider::valueChanged,page,[&motion](int value){motion.setStrength(value);});
    connect(reduced,&QCheckBox::toggled,page,[&motion](bool value){motion.setReducedMotion(value);});
    connect(&motion,&ui::ScrollPreferences::changed,page,reloadMotion);
    motionForm->addRow(smooth);motionForm->addRow(tr("Speed"),speed);
    motionForm->addRow(tr("Smoothing"),softnessRow);motionForm->addRow(reduced);
    auto* motionHint=new QLabel(tr("Applies immediately to scrolling in the browser, lists, mixer and editors. "
        "Higher speed stops sooner; more smoothing gives a softer finish. "
        "Touchpad gestures, keyboard commands and editing follow your input directly."),motionGroup);
    motionHint->setWordWrap(true);motionForm->addRow(motionHint);layout->addWidget(motionGroup);

    auto* graphicsGroup = new QGroupBox(tr("Graphics quality"), page);
    auto* graphicsForm = new QFormLayout(graphicsGroup);
    auto* gpu = new QCheckBox(tr("GPU rendering (experimental)"), graphicsGroup);
    gpu->setObjectName("GpuWorkspaceEnabled");
    const bool runningGpuMode = ui::graphics::gpuWorkspaceEnabled();
    gpu->setChecked(QSettings().value("ui/gpuWorkspace", true).toBool());
    auto* restartGpu = new QPushButton(tr("Restart VLTONE"), graphicsGroup);
    restartGpu->setObjectName(QStringLiteral("GpuRestartButton"));
    restartGpu->setProperty("accentAction", true);
    restartGpu->setAccessibleName(tr("Restart VLTONE to apply rendering mode"));
    restartGpu->setToolTip(
        tr("Restart VLTONE and reopen the current project"));
    restartGpu->setVisible(gpu->isChecked() != runningGpuMode);
    connect(gpu, &QCheckBox::toggled, page,
            [restartGpu, runningGpuMode](bool enabled) {
        QSettings().setValue("ui/gpuWorkspace", enabled);
        restartGpu->setVisible(enabled != runningGpuMode);
    });
    connect(restartGpu, &QPushButton::clicked, this,
            &SettingsWindow::restartRequested);
    graphicsForm->addRow(gpu);
    auto* gpuHint = new QLabel(tr("Takes effect after restarting VLTONE. Accelerates the workspace, "
        "editors and media. Turn this off to use compatibility rendering."), graphicsGroup);
    gpuHint->setWordWrap(true);
    graphicsForm->addRow(gpuHint);
    graphicsForm->addRow(restartGpu);
    auto* quality = new QComboBox(graphicsGroup);
    quality->setObjectName("GraphicsQuality");
    quality->setAccessibleName(tr("Graphics quality"));
    using ui::graphics::Quality;
    for (const auto& entry : {std::pair{tr("Auto"), Quality::Automatic},
            std::pair{tr("Maximum"), Quality::Maximum}, std::pair{tr("Medium"), Quality::Medium},
            std::pair{tr("Low"), Quality::Low}})
        quality->addItem(entry.first, int(entry.second));
    auto& graphics = ui::graphics::GraphicsPreferences::instance();
    quality->setCurrentIndex(quality->findData(int(graphics.quality())));
    connect(quality, &QComboBox::currentIndexChanged, page, [quality, &graphics] {
        graphics.setQuality(Quality(quality->currentData().toInt()));
    });
    connect(&graphics, &ui::graphics::GraphicsPreferences::qualityChanged, page, [quality, &graphics] {
        const QSignalBlocker blocker(quality);
        quality->setCurrentIndex(quality->findData(int(graphics.quality())));
    });
    graphicsForm->addRow(tr("Quality"), quality);
    auto* qualityHint = new QLabel(tr("Controls decorative backgrounds only. Maximum keeps full resolution; "
        "Medium uses 75% resolution and up to 30 FPS; Low uses 50% and up to 15 FPS. "
        "Auto adapts to graphics load. Text, notes, waveforms and editing stay sharp. "
        "Your blur setting is independent of graphics quality."), graphicsGroup);
    qualityHint->setWordWrap(true);
    graphicsForm->addRow(qualityHint);
    layout->addWidget(graphicsGroup);

    layout->addStretch();
    return page;
}

void SettingsWindow::refreshStartupTemplateOptions() {
    if (!m_startupTemplate) return;
    const QString selected = ui::startupproject::templatePath();
    const QSignalBlocker blocker(m_startupTemplate);
    m_startupTemplate->clear();
    m_startupTemplate->addItem(tr("Empty project"), QString());
    for (const QString& path : ui::projecttemplates::files()) {
        m_startupTemplate->addItem(
            ui::projecttemplates::displayName(path), path);
    }
    int index = m_startupTemplate->findData(selected);
    if (index < 0 && !selected.isEmpty()) {
        m_startupTemplate->addItem(
            tr("Missing: %1").arg(QFileInfo(selected).fileName()), selected);
        index = m_startupTemplate->count() - 1;
    }
    m_startupTemplate->setCurrentIndex(std::max(0, index));
}
