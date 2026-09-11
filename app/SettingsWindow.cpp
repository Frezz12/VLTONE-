#include "SettingsWindow.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "UiFrameClock.hpp"
#include <QSpinBox>
#include "AiSettingsPage.hpp"
#include "AccountSettingsPage.hpp"
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
#include "TimelineBackgroundPrefs.hpp"
#include "ProjectTemplates.hpp"
#include "StartupProjectPrefs.hpp"
#include "UiConstants.hpp"
#include "RecordingSettingsPage.hpp"
#include "TransportSettingsPage.hpp"

#include <QApplication>
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
#include <QSlider>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace {
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
        painter.fillRect(QRectF(frame.left(), frame.top(), frame.width(), headerHeight),
                         theme.headerBackground);
        painter.fillRect(QRectF(frame.left(), frame.top() + headerHeight,
                                sidebarWidth, frame.height() - headerHeight),
                         theme.surface);
        painter.fillRect(QRectF(frame.left() + sidebarWidth,
                                frame.bottom() - footerHeight,
                                frame.width() - sidebarWidth, footerHeight),
                         theme.toolbarBackground);

        const QRectF transport(frame.center().x() - 58.0, frame.top() + 5.0,
                               116.0, 18.0);
        painter.setPen(theme.separator());
        painter.setBrush(theme.transportBackground);
        painter.drawRoundedRect(transport, 6.0, 6.0);
        painter.setPen(theme.accentHighlight);
        painter.drawText(transport, Qt::AlignCenter, QStringLiteral("1.1.000   120"));

        painter.setPen(theme.textSecondary);
        QFont small = font();
        small.setPixelSize(9);
        small.setWeight(QFont::DemiBold);
        painter.setFont(small);
        painter.drawText(QRectF(frame.left() + 10.0, frame.top() + 38.0,
                                sidebarWidth - 20.0, 16.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QCoreApplication::translate("SettingsWindow", "Tracks"));

        const qreal laneLeft = frame.left() + sidebarWidth;
        const qreal laneTop = frame.top() + headerHeight;
        const qreal laneRight = frame.right();
        const qreal laneBottom = frame.bottom() - footerHeight;
        painter.setPen(theme.gridLine);
        for (qreal x = laneLeft + 18.0; x < laneRight; x += 18.0)
            painter.drawLine(QPointF(x, laneTop), QPointF(x, laneBottom));
        painter.setPen(theme.gridLineStrong);
        for (qreal x = laneLeft + 72.0; x < laneRight; x += 72.0)
            painter.drawLine(QPointF(x, laneTop), QPointF(x, laneBottom));

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
            painter.setPen(i == 0 ? theme.textPrimary : theme.textSecondary);
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
        painter.setPen(QPen(theme.waveform, 1.5));
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
        return angle.isAccepted() && pixels.isAccepted() && afterAngle > 0 &&
               scroll.verticalScrollBar()->value() > afterAngle &&
               slider->value() == 50;
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
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    resize(640, 560);
    setSizeGripEnabled(false);

    m_tabs = new QTabWidget(this);
    m_tabs->setMinimumSize(0, 0);
    m_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    const auto addPage = [this](QWidget* page, const QString& label) {
        m_tabs->addTab(scrollablePage(page), label);
    };

    m_audioPage = new AudioSettingsPage(m_controller, this);
    connect(m_audioPage, &AudioSettingsPage::cpuStatusBarVisibilityChanged,
            this, &SettingsWindow::cpuStatusBarVisibilityChanged);
    addPage(m_audioPage, tr("Audio"));
    m_quickImportPage = new QuickImportSettingsPage(this);
    addPage(m_quickImportPage, tr("Quick Import"));
    auto* transportPage = new TransportSettingsPage(m_controller, this);
    addPage(transportPage, tr("Transport"));
    connect(transportPage, &TransportSettingsPage::panelStyleChanged, this,
            &SettingsWindow::transportPanelStyleChanged);
    m_recordingPage = new RecordingSettingsPage(m_controller, this);
    connect(m_recordingPage, &RecordingSettingsPage::recordModeChanged, this,
            &SettingsWindow::recordModeChanged);
    addPage(m_recordingPage, tr("Recording"));
    auto* contextPage = new ContextPanelPage(this);
    connect(contextPage, &ContextPanelPage::changed, this,
            &SettingsWindow::contextPanelSettingsChanged);
    addPage(contextPage, tr("Context Panel"));
    auto* browserPage = new BrowserSettingsPage(this);
    connect(browserPage, &BrowserSettingsPage::changed, this,
            &SettingsWindow::browserSettingsChanged);
    addPage(browserPage, tr("Browser"));
    m_notebookPage = new NotebookSettingsPage(this);
    connect(m_notebookPage, &NotebookSettingsPage::changed, this,
            &SettingsWindow::notebookSettingsChanged);
    addPage(m_notebookPage, tr("Notebook"));
    auto* aiPage = new AiSettingsPage(this);
    connect(aiPage, &AiSettingsPage::changed, this,
            &SettingsWindow::aiSettingsChanged);
    addPage(aiPage, tr("AI"));
    auto* accountPage = new AccountSettingsPage(this);
    connect(accountPage, &AccountSettingsPage::logoutRequested, this,
            &SettingsWindow::accountLogoutRequested);
    addPage(accountPage, tr("Account"));
    addPage(buildLanguageTab(), tr("Language"));
    addPage(new RecoverySettingsPage(this), tr("Recovery"));
    addPage(buildThemesTab(), tr("Themes"));
    addPage(buildThemeEditorTab(), tr("Theme Editor"));
    addPage(buildShortcutsTab(), tr("Keyboard Shortcuts"));
    addPage(buildInterfaceTab(), tr("Interface"));

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

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto* col = new QVBoxLayout(this);
    col->addWidget(m_tabs, 1);
    col->addWidget(buttons);

    constrainToScreen();
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
    refreshTimelineBackgroundSource();
    refreshStartupTemplateOptions();
    refreshThemeLibrary();
    refreshThemeControls();
    if (m_notebookPage) m_notebookPage->refresh();
    if (m_quickImportPage) m_quickImportPage->refresh();
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
    constexpr int kPreferredHeight = 560;
    constexpr int kAbsoluteHeightCap = 680;
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

void SettingsWindow::showTab(int index) {
    if (m_tabs) m_tabs->setCurrentIndex(index);
}

void SettingsWindow::reloadRecordingPage() {
    if (m_recordingPage) m_recordingPage->reload();
}

bool SettingsWindow::checkAudioPageForTest() const {
    return m_audioPage && m_audioPage->checkForTest();
}

bool SettingsWindow::checkWheelRoutingForTest() {
    return SettingsScrollArea::checkWheelRoutingForTest();
}

QWidget* SettingsWindow::buildLanguageTab() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);

    auto* intro = new QLabel(
        tr("Choose the application language. A full workspace language change "
           "takes effect after restart."), page);
    intro->setWordWrap(true);
    column->addWidget(intro);

    m_languageList = new QComboBox(page);
    m_languageList->setObjectName(QStringLiteral("ApplicationLanguage"));
    m_languageList->setAccessibleName(tr("Application language"));
    column->addWidget(m_languageList);

    m_languageStatus = new QLabel(page);
    m_languageStatus->setWordWrap(true);
    m_languageStatus->setObjectName(QStringLiteral("SettingsHint"));
    column->addWidget(m_languageStatus);

    auto* help = new QLabel(
        tr("To make another language, export the JSON template, fill in the "
           "empty translations in any text editor, and import the file."), page);
    help->setWordWrap(true);
    column->addWidget(help);

    auto* importButton = new QPushButton(tr("Import Language…"), page);
    auto* exportButton = new QPushButton(tr("Export Template…"), page);
    m_removeLanguage = new QPushButton(tr("Remove Language"), page);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(importButton);
    buttons->addWidget(exportButton);
    buttons->addStretch(1);
    buttons->addWidget(m_removeLanguage);
    column->addLayout(buttons);
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

    m_themeList = new QListWidget(page);
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


    // Nothing owns the playhead as a widget property either; the arrangement
    // reads these while painting, so a theme refresh is again the honest way to
    // ask every surface to repaint.
    const auto repaintSurfaces = [this] {
        ThemeManager::instance().apply();
        emit selectionTintChanged();
    };
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
    return page;
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
    if (m_tabs && m_tabs->currentIndex() == kThemeEditorTab &&
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
             contrastRatio(m_editTheme.textSecondary, m_editTheme.surface)});
        if (weakest >= 4.5) {
            m_themeContrastStatus->setText(
                tr("✓ Text contrast looks good — weakest pairing is %1:1.")
                    .arg(weakest, 0, 'f', 1));
        } else {
            m_themeContrastStatus->setText(
                tr("⚠ Low text contrast — weakest pairing is %1:1. Aim for "
                   "4.5:1 or higher.")
                    .arg(weakest, 0, 'f', 1));
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

    auto* table = new QTableWidget(page);
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
    const auto reload = [mode, limit, &clock] {
        const QSignalBlocker a(mode), b(limit);
        limit->setValue(clock.limit());
        const int data = clock.mode() == ui::FrameMode::Display ? -2 :
                         clock.mode() == ui::FrameMode::Unlimited ? -3 : clock.limit();
        const int index = mode->findData(data);
        mode->setCurrentIndex(index >= 0 ? index : mode->findData(-1));
        limit->setEnabled(mode->currentData().toInt() == -1);
    };
    reload();
    form->addRow(tr("Refresh rate"), mode);
    form->addRow(tr("Custom refresh rate"), limit);
    const auto apply = [mode, limit, &clock] {
        const int choice = mode->currentData().toInt();
        limit->setEnabled(choice == -1);
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

    auto* graphicsGroup = new QGroupBox(tr("Graphics quality"), page);
    auto* graphicsForm = new QFormLayout(graphicsGroup);
    auto* gpu = new QCheckBox(tr("GPU rendering (experimental)"), graphicsGroup);
    gpu->setObjectName("GpuWorkspaceEnabled");
    const bool runningGpuMode = ui::graphics::gpuWorkspaceEnabled();
    gpu->setChecked(QSettings().value("ui/gpuWorkspace", false).toBool());
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
