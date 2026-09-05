#include "SettingsWindow.hpp"
#include "AiSettingsPage.hpp"
#include "AccountSettingsPage.hpp"
#include "BrowserSettingsPage.hpp"
#include "RecoverySettingsPage.hpp"
#include "AudioSettingsPage.hpp"
#include "ContextPanelPage.hpp"
#include "LocalizationManager.hpp"
#include "NotebookSettingsPage.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "TimelineBackgroundPrefs.hpp"
#include "UiConstants.hpp"
#include "RecordingSettingsPage.hpp"
#include "TransportSettingsPage.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QCheckBox>
#include <QRadioButton>
#include <QSlider>
#include <QScrollArea>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {
// The editable colours of a theme, paired with a pointer-to-member so the
// editor can read and write each one generically.
struct ColorField {
    const char* key;
    const char* label;
    QColor Theme::* member;
};
const std::vector<ColorField>& colorFields() {
    static const std::vector<ColorField> fields = {
        {"background", QT_TRANSLATE_NOOP("SettingsWindow", "Background"), &Theme::background},
        {"surface", QT_TRANSLATE_NOOP("SettingsWindow", "Surface"), &Theme::surface},
        {"surfaceElevated", QT_TRANSLATE_NOOP("SettingsWindow", "Surface (elevated)"), &Theme::surfaceElevated},
        {"headerBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Header"), &Theme::headerBackground},
        {"transportBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Transport"), &Theme::transportBackground},
        {"toolbarBackground", QT_TRANSLATE_NOOP("SettingsWindow", "Toolbar"), &Theme::toolbarBackground},
        {"textPrimary", QT_TRANSLATE_NOOP("SettingsWindow", "Text"), &Theme::textPrimary},
        {"textSecondary", QT_TRANSLATE_NOOP("SettingsWindow", "Text (secondary)"), &Theme::textSecondary},
        {"accent", QT_TRANSLATE_NOOP("SettingsWindow", "Accent"), &Theme::accent},
        {"accentHighlight", QT_TRANSLATE_NOOP("SettingsWindow", "Accent (highlight)"), &Theme::accentHighlight},
        {"waveform", QT_TRANSLATE_NOOP("SettingsWindow", "Waveform"), &Theme::waveform},
        {"cursor", QT_TRANSLATE_NOOP("SettingsWindow", "Playhead"), &Theme::cursor},
        {"gridLine", QT_TRANSLATE_NOOP("SettingsWindow", "Grid line"), &Theme::gridLine},
        {"gridLineStrong", QT_TRANSLATE_NOOP("SettingsWindow", "Grid line (strong)"), &Theme::gridLineStrong},
        {"selection", QT_TRANSLATE_NOOP("SettingsWindow", "Selection"), &Theme::selection},
    };
    return fields;
}

QString presetDisplayName(const Theme& theme) {
    if (theme.id == QLatin1String("logic")) return QCoreApplication::translate(
        "SettingsWindow", "Logic Graphite");
    if (theme.id == QLatin1String("logic-light")) return QCoreApplication::translate(
        "SettingsWindow", "Logic Light");
    if (theme.id == QLatin1String("dark")) return QCoreApplication::translate(
        "SettingsWindow", "Dark");
    if (theme.id == QLatin1String("light")) return QCoreApplication::translate(
        "SettingsWindow", "Light");
    if (theme.id == QLatin1String("midnight")) return QCoreApplication::translate(
        "SettingsWindow", "Midnight");
    if (theme.id == QLatin1String("carbon")) return QCoreApplication::translate(
        "SettingsWindow", "Carbon");
    if (theme.id == QLatin1String("dracula")) return QCoreApplication::translate(
        "SettingsWindow", "Dracula");
    if (theme.id == QLatin1String("solarized-dark")) return QCoreApplication::translate(
        "SettingsWindow", "Solarized Dark");
    if (theme.id == QLatin1String("solarized-light")) return QCoreApplication::translate(
        "SettingsWindow", "Solarized Light");
    if (theme.id == QLatin1String("nord")) return QCoreApplication::translate(
        "SettingsWindow", "Nord");
    if (theme.id == QLatin1String("gruvbox")) return QCoreApplication::translate(
        "SettingsWindow", "Gruvbox");
    return theme.name;
}

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
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setMinimumSize(0, 0);
    scroll->setWidget(page);
    return scroll;
}
} // namespace

SettingsWindow::SettingsWindow(daw::EngineController* controller,
                               ShortcutManager* shortcuts, QWidget* parent)
    : QDialog(parent, Qt::Widget), m_controller(controller), m_shortcuts(shortcuts) {
    setWindowTitle(tr("Settings — %1").arg(QApplication::applicationName()));
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
    addPage(new TransportSettingsPage(m_controller, this), tr("Transport"));
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
    auto* notebookPage = new NotebookSettingsPage(this);
    connect(notebookPage, &NotebookSettingsPage::changed, this,
            &SettingsWindow::notebookSettingsChanged);
    addPage(notebookPage, tr("Notebook"));
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

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto* col = new QVBoxLayout(this);
    col->addWidget(m_tabs, 1);
    col->addWidget(buttons);

    constrainToScreen();
}

void SettingsWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    constrainToScreen();
    // Native frame margins become reliable only after the first show. Clamp a
    // second time so a window manager cannot leave the title bar off-screen.
    QTimer::singleShot(0, this, &SettingsWindow::constrainToScreen);
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
                                tr("Restart VLT Studio Pro now to apply the new "
                                   "language?"),
                                QMessageBox::Yes | QMessageBox::No, this);
                box.button(QMessageBox::Yes)->setText(tr("Restart Now"));
                box.button(QMessageBox::No)->setText(tr("Later"));
                if (box.exec() == QMessageBox::Yes) emit restartRequested();
            });

    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Language"), QString(),
            tr("VLT Language Pack (*.vltlang.json *.json);;All Files (*)"));
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
            tr("VLT Language Pack (*.vltlang.json *.json)"));
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
    col->addWidget(new QLabel(tr("Choose a colour theme — it applies immediately.")));

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
            [](QListWidgetItem* cur, QListWidgetItem*) {
                if (cur)
                    ThemeManager::instance().setThemeId(
                        cur->data(Qt::UserRole).toString());
            });
    // Keep the selection in step if the theme changes elsewhere (e.g. undo of a
    // future settings action, or another surface switching it).
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        const QString id = ThemeManager::instance().themeId();
        for (int i = 0; i < m_themeList->count(); ++i) {
            if (m_themeList->item(i)->data(Qt::UserRole).toString() == id) {
                QSignalBlocker block(m_themeList);
                m_themeList->setCurrentRow(i);
                break;
            }
        }
    });

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
    fontColumn->addWidget(preview);

    auto* importFont = new QPushButton(tr("Import Font…"), fontGroup);
    m_resetFont = new QPushButton(tr("Use System Font"), fontGroup);
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

    col->addWidget(m_themeList, 1);

    auto* backgroundGroup = new QGroupBox(tr("Timeline Background"), page);
    auto* backgroundColumn = new QVBoxLayout(backgroundGroup);
    auto* backgroundHint = new QLabel(
        tr("Choose a local photo, animated GIF or video for the arrangement "
           "grid. The file stays on this computer and is not saved in the "
           "project."),
        backgroundGroup);
    backgroundHint->setWordWrap(true);
    backgroundColumn->addWidget(backgroundHint);

    auto* enableTimelineBackground = new QCheckBox(
        tr("Enable custom timeline background"), backgroundGroup);
    enableTimelineBackground->setChecked(
        ui::timelinebackgroundprefs::enabled());
    enableTimelineBackground->setAccessibleName(
        tr("Custom timeline background enabled"));
    backgroundColumn->addWidget(enableTimelineBackground);

    auto* backgroundForm = new QFormLayout;
    backgroundForm->setSpacing(8);
    auto* fileRow = new QWidget(backgroundGroup);
    auto* fileLayout = new QHBoxLayout(fileRow);
    fileLayout->setContentsMargins(0, 0, 0, 0);
    fileLayout->setSpacing(6);
    auto* backgroundPath = new QLineEdit(fileRow);
    backgroundPath->setReadOnly(true);
    backgroundPath->setPlaceholderText(tr("Theme colour only"));
    backgroundPath->setAccessibleName(tr("Timeline background file"));
    auto* chooseBackground = new QPushButton(tr("Choose…"), fileRow);
    chooseBackground->setAccessibleName(tr("Choose timeline background"));
    auto* clearBackground = new QPushButton(tr("Clear"), fileRow);
    fileLayout->addWidget(backgroundPath, 1);
    fileLayout->addWidget(chooseBackground);
    fileLayout->addWidget(clearBackground);
    backgroundForm->addRow(tr("Media"), fileRow);

    auto* timelinePlacement = new QComboBox(backgroundGroup);
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

    const auto refreshBackgroundPath = [backgroundPath, clearBackground] {
        const QString path = ui::timelinebackgroundprefs::path();
        backgroundPath->setText(QDir::toNativeSeparators(path));
        backgroundPath->setToolTip(path);
        clearBackground->setEnabled(!path.isEmpty());
    };
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
            auto* slider = new QSlider(Qt::Horizontal, row);
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
    connect(visibility, &QSlider::valueChanged, this, [this](int value) {
        ui::timelinebackgroundprefs::setVisibility(value);
        emit themeBackgroundSettingsChanged();
    });

    auto* blurRow = new QWidget(backgroundGroup);
    auto* blurLayout = new QHBoxLayout(blurRow);
    blurLayout->setContentsMargins(0, 0, 0, 0);
    blurLayout->setSpacing(8);
    auto* blur = new QSlider(Qt::Horizontal, blurRow);
    blur->setRange(0, 32);
    blur->setValue(ui::timelinebackgroundprefs::blurRadius());
    blur->setAccessibleName(tr("Timeline background blur"));
    auto* blurValue = new QLabel(
        tr("%1 px").arg(ui::timelinebackgroundprefs::blurRadius()), blurRow);
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
    enableHeaderBackground->setChecked(ui::headerbackgroundprefs::enabled());
    enableHeaderBackground->setAccessibleName(
        tr("Custom header background enabled"));
    headerBackgroundColumn->addWidget(enableHeaderBackground);

    auto* headerBackgroundForm = new QFormLayout;
    headerBackgroundForm->setSpacing(8);
    auto* headerFileRow = new QWidget(headerBackgroundGroup);
    auto* headerFileLayout = new QHBoxLayout(headerFileRow);
    headerFileLayout->setContentsMargins(0, 0, 0, 0);
    headerFileLayout->setSpacing(6);
    auto* headerPath = new QLineEdit(headerFileRow);
    headerPath->setReadOnly(true);
    headerPath->setPlaceholderText(tr("Theme colour only"));
    headerPath->setAccessibleName(tr("Header background file"));
    auto* chooseHeaderBackground = new QPushButton(tr("Choose..."), headerFileRow);
    chooseHeaderBackground->setAccessibleName(tr("Choose header background"));
    auto* clearHeaderBackground = new QPushButton(tr("Clear"), headerFileRow);
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
            auto* slider = new QSlider(Qt::Horizontal, row);
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
    connect(headerVisibility, &QSlider::valueChanged, this,
            [this](int value) {
                ui::headerbackgroundprefs::setVisibility(value);
                emit themeBackgroundSettingsChanged();
            });

    auto* headerBlurRow = new QWidget(headerBackgroundGroup);
    auto* headerBlurLayout = new QHBoxLayout(headerBlurRow);
    headerBlurLayout->setContentsMargins(0, 0, 0, 0);
    headerBlurLayout->setSpacing(8);
    auto* headerBlur = new QSlider(Qt::Horizontal, headerBlurRow);
    headerBlur->setRange(0, 32);
    headerBlur->setValue(ui::headerbackgroundprefs::blurRadius());
    headerBlur->setAccessibleName(tr("Header background blur"));
    auto* headerBlurValue = new QLabel(
        tr("%1 px").arg(ui::headerbackgroundprefs::blurRadius()),
        headerBlurRow);
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
    auto* widthSlider = new QSlider(Qt::Horizontal, headGroup);
    // Tenths of a pixel: the default is 1.6, and whole steps would take that
    // choice away.
    widthSlider->setRange(int(std::lround(ui::kPlayheadWidthMin * 10.0)),
                          int(std::lround(ui::kPlayheadWidthMax * 10.0)));
    widthSlider->setSingleStep(1);
    widthSlider->setPageStep(5);
    widthSlider->setValue(int(std::lround(ui::playheadWidth() * 10.0)));
    widthSlider->setAccessibleName(tr("Playhead line thickness in pixels"));
    auto* widthValue = new QLabel(headGroup);
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
            tr("System font: %1").arg(manager.systemFontFamily()));
    }
    m_resetFont->setEnabled(manager.hasCustomFont());
}

QWidget* SettingsWindow::buildThemeEditorTab() {
    m_editTheme = ThemeManager::instance().theme();   // start from the active one

    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->addWidget(new QLabel(
        tr("Design your own palette — changes apply live. Export it to share, "
           "or import a theme file.")));

    // Name row.
    auto* nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("Name")));
    m_themeNameEdit = new QLineEdit(m_editTheme.name.isEmpty() ? tr("Custom")
                                                              : m_editTheme.name);
    connect(m_themeNameEdit, &QLineEdit::textEdited, this, [this](const QString& s) {
        m_editTheme.name = s;
    });
    nameRow->addWidget(m_themeNameEdit, 1);
    col->addLayout(nameRow);

    // A scrollable grid of colour swatches.
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* gridingHost = new QWidget;
    auto* grid = new QGridLayout(gridingHost);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(7);

    const auto& fields = colorFields();
    for (int i = 0; i < int(fields.size()); ++i) {
        const ColorField& f = fields[size_t(i)];
        const int rowN = i / 2;
        const int colN = (i % 2) * 2;

        auto* swatch = new QPushButton;
        swatch->setFixedSize(46, 22);
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setToolTip(tr("Pick a colour"));
        m_swatches.insert(f.key, swatch);
        QColor Theme::* member = f.member;
        connect(swatch, &QPushButton::clicked, this, [this, member] {
            const QColor start = m_editTheme.*member;
            const QColor picked = QColorDialog::getColor(
                start, this, tr("Choose colour"),
                QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return;
            m_editTheme.*member = picked;
            applyEditTheme();
        });

        grid->addWidget(new QLabel(
            QCoreApplication::translate("SettingsWindow", f.label)), rowN,
            colN);
        grid->addWidget(swatch, rowN, colN + 1);
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(2, 1);
    scroll->setWidget(gridingHost);
    col->addWidget(scroll, 1);

    // Actions: reseed from the active theme, export and import.
    auto* fromCurrent = new QPushButton(tr("Start From Active Theme"));
    connect(fromCurrent, &QPushButton::clicked, this, [this] {
        m_editTheme = ThemeManager::instance().theme();
        m_editTheme.name = tr("Custom");
        if (m_themeNameEdit) m_themeNameEdit->setText(m_editTheme.name);
        refreshSwatches();
        applyEditTheme();
    });
    auto* exportBtn = new QPushButton(tr("Export…"));
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export Theme"),
            m_editTheme.name + ".dawtheme.json",
            tr("VLT Studio Pro Theme (*.json *.dawtheme.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, tr("Export failed"),
                                 tr("Could not write %1").arg(path));
            return;
        }
        file.write(QJsonDocument(ThemeManager::toJson(m_editTheme))
                       .toJson(QJsonDocument::Indented));
    });
    auto* importBtn = new QPushButton(tr("Import…"));
    connect(importBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Import Theme"), QString(),
            tr("VLT Studio Pro Theme (*.json *.dawtheme.json);;All Files (*)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) {
            QMessageBox::warning(this, tr("Import failed"),
                                 tr("%1 is not a valid theme file").arg(path));
            return;
        }
        m_editTheme = ThemeManager::fromJson(doc.object(),
                                             ThemeManager::instance().theme());
        if (m_themeNameEdit) m_themeNameEdit->setText(m_editTheme.name);
        refreshSwatches();
        applyEditTheme();
    });

    auto* actions = new QHBoxLayout;
    actions->addWidget(fromCurrent);
    actions->addStretch(1);
    actions->addWidget(importBtn);
    actions->addWidget(exportBtn);
    col->addLayout(actions);

    refreshSwatches();
    return page;
}

void SettingsWindow::applyEditTheme() {
    ThemeManager::instance().applyCustomTheme(m_editTheme, /*persist=*/true);
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
        // A checkerboard-free preview: solid fill with a hairline border.
        swatch->setStyleSheet(
            QString("background: %1; border: 1px solid rgba(128,128,128,0.6); "
                    "border-radius: 5px;")
                .arg(c.name(QColor::HexArgb)));
    }
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
